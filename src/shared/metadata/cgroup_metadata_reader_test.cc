#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include "src/common/system/config_mock.h"
#include "src/common/testing/testing.h"
#include "src/shared/metadata/cgroup_metadata_reader.h"

namespace pl {
namespace md {

constexpr char kTestDataBasePath[] = "src/shared/metadata";

using ::testing::Return;

namespace {
std::string GetPathToTestDataFile(const std::string& fname) {
  return TestEnvironment::PathToTestDataFile(std::string(kTestDataBasePath) + "/" + fname);
}
}  // namespace

class CGroupMetadataReaderTest : public ::testing::Test {
 protected:
  void SetupMetadataReader(system::MockConfig* sysconfig, const std::string& proc_path,
                           const std::string& sysfs_path) {
    EXPECT_CALL(*sysconfig, HasConfig()).WillRepeatedly(Return(true));
    EXPECT_CALL(*sysconfig, PageSize()).WillRepeatedly(Return(4096));
    EXPECT_CALL(*sysconfig, KernelTicksPerSecond()).WillRepeatedly(Return(10000000));
    EXPECT_CALL(*sysconfig, ClockRealTimeOffset()).WillRepeatedly(Return(128));

    EXPECT_CALL(*sysconfig, proc_path()).WillRepeatedly(Return(GetPathToTestDataFile(proc_path)));
    EXPECT_CALL(*sysconfig, sysfs_path()).WillRepeatedly(Return(GetPathToTestDataFile(sysfs_path)));
  }

  void SetUp() {
    system::MockConfig sysconfig;
    SetupMetadataReader(&sysconfig, "testdata/proc1", "testdata/sysfs1");
    md_reader_.reset(new CGroupMetadataReader(sysconfig));
  }

  void AlternateSetUp() {
    // Use a different sys/fs with the alternate cgroup naming scheme.
    system::MockConfig sysconfig;
    SetupMetadataReader(&sysconfig, "testdata/proc1", "testdata/sysfs2");
    md_reader_.reset(new CGroupMetadataReader(sysconfig));
  }

  std::unique_ptr<CGroupMetadataReader> md_reader_;
};

TEST_F(CGroupMetadataReaderTest, read_pid_list) {
  absl::flat_hash_set<uint32_t> pid_set;
  ASSERT_OK(md_reader_->ReadPIDs(PodQOSClass::kBestEffort, "abcd", "c123", &pid_set));
  EXPECT_THAT(pid_set, ::testing::UnorderedElementsAre(123, 456, 789));
}

TEST_F(CGroupMetadataReaderTest, read_pid_start_time_ticks) {
  EXPECT_EQ(80019809, md_reader_->ReadPIDStartTimeTicks(32391));
}

TEST_F(CGroupMetadataReaderTest, read_pid_cmdline) {
  EXPECT_THAT("/usr/lib/slack/slack --force-device-scale-factor=1.5 --high-dpi-support=1",
              md_reader_->ReadPIDCmdline(32391));
}

TEST_F(CGroupMetadataReaderTest, read_pid_metadata_null) {
  EXPECT_THAT("/usr/lib/at-spi2-core/at-spi2-registryd --use-gnome-session",
              md_reader_->ReadPIDCmdline(79690));
}

TEST_F(CGroupMetadataReaderTest, cgroup_proc_file_path) {
  EXPECT_EQ(GetPathToTestDataFile(
                "testdata/sysfs1/cgroup/cpu,cpuacct/kubepods/burstable/podabcd/c123/cgroup.procs"),
            md_reader_->CGroupProcFilePath(PodQOSClass::kBurstable, "abcd", "c123"));

  EXPECT_EQ(GetPathToTestDataFile(
                "testdata/sysfs1/cgroup/cpu,cpuacct/kubepods/besteffort/podabcd/c123/cgroup.procs"),
            md_reader_->CGroupProcFilePath(PodQOSClass::kBestEffort, "abcd", "c123"));

  EXPECT_EQ(GetPathToTestDataFile(
                "testdata/sysfs1/cgroup/cpu,cpuacct/kubepods/podabcd/c123/cgroup.procs"),
            md_reader_->CGroupProcFilePath(PodQOSClass::kGuaranteed, "abcd", "c123"));
}

TEST_F(CGroupMetadataReaderTest, cgroup_pod_exists) {
  PodInfo pod_info("abcd", "namespace", "pod-name", PodQOSClass::kBestEffort);
  EXPECT_TRUE(md_reader_->PodDirExists(pod_info));
}

TEST_F(CGroupMetadataReaderTest, cgroup_proc_file_path_alternate) {
  AlternateSetUp();

  EXPECT_EQ(GetPathToTestDataFile(
                "testdata/sysfs2/cgroup/cpu,cpuacct/kubepods.slice/kubepods-burstable.slice/"
                "kubepods-burstable-pod5a1d1140_a486_478c_afae_bbc975ff9c3b.slice/"
                "docker-2b41fe4bb7a365960f1e7ed6c09651252b29387b44c9e14ad17e3bc392e7c640.scope/"
                "cgroup.procs"),
            md_reader_->CGroupProcFilePath(
                PodQOSClass::kBurstable, "5a1d1140-a486-478c-afae-bbc975ff9c3b",
                "2b41fe4bb7a365960f1e7ed6c09651252b29387b44c9e14ad17e3bc392e7c640"));

  EXPECT_EQ(GetPathToTestDataFile(
                "testdata/sysfs2/cgroup/cpu,cpuacct/kubepods.slice/kubepods-besteffort.slice/"
                "kubepods-besteffort-pod15b6301f_94d0_44ac_a2a8_6816c7a3fa32.slice/"
                "docker-159757ef9efdc09be13490c8615f1402c170cdd406dad6053ebe0df2db89fcaa.scope/"
                "cgroup.procs"),
            md_reader_->CGroupProcFilePath(
                PodQOSClass::kBestEffort, "15b6301f-94d0-44ac-a2a8-6816c7a3fa32",
                "159757ef9efdc09be13490c8615f1402c170cdd406dad6053ebe0df2db89fcaa"));

  EXPECT_EQ(GetPathToTestDataFile(
                "testdata/sysfs2/cgroup/cpu,cpuacct/kubepods.slice/"
                "kubepods-pod8dbc5577_d0e2_4706_8787_57d52c03ddf2.slice/"
                "docker-b9055cee13e1f37ecb63030593b27f4adc43cbd6629aa7781ffdf53fbaecfa46.scope/"
                "cgroup.procs"),
            md_reader_->CGroupProcFilePath(
                PodQOSClass::kGuaranteed, "8dbc5577-d0e2-4706-8787-57d52c03ddf2",
                "b9055cee13e1f37ecb63030593b27f4adc43cbd6629aa7781ffdf53fbaecfa46"));
}

TEST_F(CGroupMetadataReaderTest, read_pid_list_alternate) {
  AlternateSetUp();

  absl::flat_hash_set<uint32_t> pid_set;
  ASSERT_OK(md_reader_->ReadPIDs(PodQOSClass::kBestEffort, "d7f73b62_b4bf_41dc_a50d_5bdf3b02d3e5",
                                 "37d1600d21282ce1bc32ebbaf99bf2241f66ca096109a4cb5484e2aa305514b4",
                                 &pid_set));
  EXPECT_THAT(pid_set, ::testing::UnorderedElementsAre(123, 456, 789));
}

}  // namespace md
}  // namespace pl
