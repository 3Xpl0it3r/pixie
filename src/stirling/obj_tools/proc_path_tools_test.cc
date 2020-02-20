#include "src/stirling/obj_tools/proc_path_tools.h"

#include "src/common/testing/test_environment.h"
#include "src/common/testing/test_utils/test_container.h"
#include "src/common/testing/testing.h"

namespace pl {
namespace stirling {
namespace obj_tools {

using ::testing::Contains;
using ::testing::EndsWith;
using ::testing::StartsWith;

TEST(ObjToolsContainerTest, ResolveFunctions) {
  DummyTestContainer container;
  ASSERT_OK(container.Run());

  std::filesystem::path proc_pid = absl::Substitute("/proc/$0", container.process_pid());

  ASSERT_OK_AND_ASSIGN(std::filesystem::path root_dir, ResolveProcessRootDir(proc_pid));
  EXPECT_THAT(root_dir, StartsWith("/var/lib/docker/overlay2/"));
  EXPECT_THAT(root_dir, EndsWith("/merged"));

  ASSERT_OK_AND_ASSIGN(std::filesystem::path process_path,
                       ResolveProcessPath(proc_pid, "/app/foo"));
  EXPECT_THAT(process_path, StartsWith("/var/lib/docker/overlay2/"));
  EXPECT_THAT(process_path, EndsWith("/merged/app/foo"));

  ASSERT_OK_AND_ASSIGN(std::filesystem::path proc_exe, ResolveProcExe(proc_pid));
  EXPECT_THAT(proc_exe, StartsWith("/var/lib/docker/overlay2/"));
  EXPECT_THAT(proc_exe, EndsWith("/merged/usr/local/bin/python3.7"));

  // Stop the container (even though destructor will also take care of this).
  container.Stop();
}

// Disabled because on Jenkins, proc_path_tools discovers the Jenkins container,
// and this test fails. This test should only be run locally outside a container.
// TODO(oazizi): Investigate a fix.
TEST(ObjToolsNonContainerTest, DISABLED_ResolveFunctions) {
  std::filesystem::path proc_pid = "/proc/self";

  ASSERT_OK_AND_ASSIGN(std::filesystem::path root_dir, ResolveProcessRootDir(proc_pid));
  EXPECT_EQ(root_dir, "");

  ASSERT_OK_AND_ASSIGN(std::filesystem::path process_path,
                       ResolveProcessPath(proc_pid, "/app/foo"));
  EXPECT_EQ(process_path, "/app/foo");

  ASSERT_OK_AND_ASSIGN(std::filesystem::path proc_exe, ResolveProcExe(proc_pid));
  EXPECT_THAT(proc_exe, EndsWith("src/stirling/obj_tools/proc_path_tools_test"));
}

}  // namespace obj_tools
}  // namespace stirling
}  // namespace pl
