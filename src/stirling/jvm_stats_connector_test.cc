#include "src/stirling/jvm_stats_connector.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "src/common/base/test_utils.h"
#include "src/common/exec/subprocess.h"
#include "src/common/testing/test_environment.h"
#include "src/stirling/jvm_stats_table.h"
#include "src/stirling/testing/common.h"

namespace pl {
namespace stirling {

using ::pl::stirling::testing::ColWrapperSizeIs;
using ::pl::stirling::testing::FindRecordIdxMatchesPID;
using ::pl::testing::TestFilePath;
using ::testing::Each;
using ::testing::SizeIs;

struct JavaHelloWorld : SubProcess {
  inline static const std::string kClassPath =
      TestFilePath("src/stirling/testing/java/HelloWorld.jar");

  ~JavaHelloWorld() {
    Kill();
    EXPECT_EQ(9, Wait());
  }

  Status Start() {
    auto status = SubProcess::Start({"java", "-cp", kClassPath, "-Xms1m", "-Xmx4m", "HelloWorld"});
    // Sleep 2 seconds for the process to create the data file.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return status;
  }
};

class JVMStatsConnectorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    connector_ = JVMStatsConnector::Create("jvm_stats_connector");
    ASSERT_OK(connector_->Init());
    ctx_ = std::make_unique<StandaloneContext>();
  }

  void TearDown() override { EXPECT_OK(connector_->Stop()); }

  std::unique_ptr<SourceConnector> connector_;
  std::unique_ptr<StandaloneContext> ctx_;
  DataTable data_table_{kJVMStatsTable};
};

// NOTE: This test will likely break under --runs_per_tests=100 or higher because of limitations of
// Bazel's sandboxing.
//
// Bazel uses PID namespace, so the PID of the java subprocess is often the same in different test
// runs. However, Bazel does not uses chroot, or other mechanisms of isolating filesystems. So the
// Java subprocesses all writes to the same memory mapped file with the same path, which causes data
// corruption and test failures.
//
// Tests that java processes are detected and data is collected.
TEST_F(JVMStatsConnectorTest, CaptureData) {
  JavaHelloWorld hello_world1;
  ASSERT_OK(hello_world1.Start());

  connector_->TransferData(ctx_.get(), JVMStatsConnector::kTableNum, &data_table_);
  const types::ColumnWrapperRecordBatch& record_batch = *data_table_.ActiveRecordBatch();
  auto idxes = FindRecordIdxMatchesPID(record_batch, kUPIDIdx, hello_world1.child_pid());
  ASSERT_THAT(idxes, SizeIs(1));

  auto idx = idxes[0];

  md::UPID upid(record_batch[kUPIDIdx]->Get<types::UInt128Value>(idx).val);
  std::filesystem::path proc_pid_path =
      std::filesystem::path("/proc") / std::to_string(hello_world1.child_pid());
  md::UPID expected_upid(/* asid */ 0, hello_world1.child_pid(),
                         system::GetPIDStartTimeTicks(proc_pid_path));
  EXPECT_EQ(upid, expected_upid);

  EXPECT_GE(record_batch[kYoungGCTimeIdx]->Get<types::Duration64NSValue>(idx), 0);
  EXPECT_GE(record_batch[kFullGCTimeIdx]->Get<types::Duration64NSValue>(idx), 0);
  EXPECT_GE(record_batch[kUsedHeapSizeIdx]->Get<types::Int64Value>(idx).val, 0);
  EXPECT_GE(record_batch[kTotalHeapSizeIdx]->Get<types::Int64Value>(idx).val, 0);
  // This is derived from -Xmx4m. But we don't know how to control total_heap_size.
  EXPECT_GE(record_batch[kMaxHeapSizeIdx]->Get<types::Int64Value>(idx).val, 4 * 1024 * 1024);

  JavaHelloWorld hello_world2;
  ASSERT_OK(hello_world2.Start());
  std::this_thread::sleep_for(std::chrono::seconds(2));

  connector_->TransferData(ctx_.get(), JVMStatsConnector::kTableNum, &data_table_);
  EXPECT_THAT(FindRecordIdxMatchesPID(record_batch, kUPIDIdx, hello_world2.child_pid()), SizeIs(1));
  // Make sure the previous processes were scanned as well.
  EXPECT_THAT(FindRecordIdxMatchesPID(record_batch, kUPIDIdx, hello_world1.child_pid()), SizeIs(2));
}

}  // namespace stirling
}  // namespace pl
