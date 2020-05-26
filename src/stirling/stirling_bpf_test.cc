#include <ctime>
#include <functional>
#include <iomanip>
#include <thread>
#include <utility>

#include "src/common/base/base.h"
#include "src/common/testing/testing.h"
#include "src/stirling/info_class_manager.h"
#include "src/stirling/socket_trace_connector.h"
#include "src/stirling/source_registry.h"
#include "src/stirling/stirling.h"
#include "src/stirling/types.h"

using pl::stirling::AgentMetadataType;
using pl::stirling::SocketTraceConnector;
using pl::stirling::SourceRegistry;
using pl::stirling::Stirling;

using pl::types::ColumnWrapperRecordBatch;
using pl::types::TabletID;

using pl::ArrayView;

class StirlingBPFTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::unique_ptr<SourceRegistry> registry = std::make_unique<SourceRegistry>();
    registry->RegisterOrDie<SocketTraceConnector>("socket_trace_connector");

    // Make Stirling.
    stirling_ = Stirling::Create(std::move(registry));

    // Set dummy callbacks for agent function.
    stirling_->RegisterDataPushCallback(std::bind(&StirlingBPFTest::AppendData, this,
                                                  std::placeholders::_1, std::placeholders::_2,
                                                  std::placeholders::_3));
    stirling_->RegisterAgentMetadataCallback(
        std::bind(&StirlingBPFTest::DummyMetadataCallback, this));
  }

  void AppendData(uint64_t table_id, TabletID tablet_id,
                  std::unique_ptr<ColumnWrapperRecordBatch> record_batch) {
    PL_UNUSED(table_id);
    PL_UNUSED(tablet_id);
    PL_UNUSED(record_batch);
    // A black hole.
  }

  AgentMetadataType DummyMetadataCallback() { return nullptr; }

  std::unique_ptr<Stirling> stirling_;
};

// Stop Stirling. Meant to be called asynchronously, via a thread.
void AsyncKill(Stirling* stirling) {
  if (stirling != nullptr) {
    stirling->Stop();
  }
}

TEST_F(StirlingBPFTest, CleanupTest) {
  ASSERT_OK(stirling_->RunAsThread());

  // Wait for thread to initialize.
  // TODO(oazizi): This is not good. How do we know how much time is enough?
  std::this_thread::sleep_for(std::chrono::seconds(1));

  EXPECT_GT(SocketTraceConnector::num_attached_probes(), 0);
  EXPECT_GT(SocketTraceConnector::num_open_perf_buffers(), 0);

  std::thread killer_thread = std::thread(&AsyncKill, stirling_.get());

  ASSERT_TRUE(killer_thread.joinable());
  killer_thread.join();

  EXPECT_EQ(SocketTraceConnector::num_attached_probes(), 0);
  EXPECT_EQ(SocketTraceConnector::num_open_perf_buffers(), 0);
}
