#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include <absl/strings/numbers.h>

#include "src/common/base/base.h"
#include "src/common/system/proc_parser.h"
#include "src/common/system/socket_info.h"
#include "src/common/system/tcp_socket.h"
#include "src/common/testing/testing.h"

namespace pl {
namespace system {

using ::pl::testing::TestFilePath;
using ::testing::Contains;
using ::testing::Not;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

// Keep two versions of AddrPortStr, in case the host machine is using IPv6.
std::string AddrPortStr(struct in6_addr in_addr, in_port_t in_port) {
  std::string addr;
  int port;

  Status s = IPv6AddrToString(in_addr, &addr);
  CHECK(s.ok());
  port = ntohs(in_port);

  return absl::StrCat(addr, ":", port);
}

std::string AddrPortStr(struct in_addr in_addr, in_port_t in_port) {
  std::string addr;
  int port;

  Status s = IPv4AddrToString(in_addr, &addr);
  CHECK(s.ok());
  port = ntohs(in_port);

  return absl::StrCat(addr, ":", port);
}

MATCHER_P(HasLocalIPEndpoint, endpoint, "") {
  switch (arg.second.family) {
    case AF_INET:
      return AddrPortStr(std::get<struct in_addr>(arg.second.local_addr), arg.second.local_port) ==
             endpoint;
    case AF_INET6:
      return AddrPortStr(std::get<struct in6_addr>(arg.second.local_addr), arg.second.local_port) ==
             endpoint;
    default:
      return false;
  }
}

MATCHER_P(HasLocalUnixEndpoint, endpoint, "") {
  switch (arg.second.family) {
    case AF_UNIX:
      return endpoint == absl::Substitute("socket:[$0]", arg.second.local_port);
    default:
      return false;
  }
}

TEST(NetlinkSocketProberTest, EstablishedInetConnection) {
  TCPSocket client;
  TCPSocket server;

  // A bind and connect is sufficient to establish a connection.
  server.BindAndListen();
  client.Connect(server);

  std::string client_endpoint = AddrPortStr(client.addr(), client.port());
  std::string server_endpoint = AddrPortStr(server.addr(), server.port());

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<NetlinkSocketProber> socket_prober,
                       NetlinkSocketProber::Create());
  std::map<int, SocketInfo> socket_info_entries;
  ASSERT_OK(socket_prober->InetConnections(&socket_info_entries));

  EXPECT_THAT(socket_info_entries, Contains(HasLocalIPEndpoint(client_endpoint)));

  client.Close();
  server.Close();
}

TEST(NetlinkSocketProberTest, EstablishedUnixConnection) {
  Status s;
  int retval;

  // Create client and server, and connect them together.
  struct sockaddr_un server_addr = {AF_UNIX, ""};
  int server_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_NE(-1, server_listen_fd);

  retval =
      bind(server_listen_fd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
  ASSERT_EQ(0, retval) << absl::Substitute("bind() failed with errno=$0", errno);

  retval = listen(server_listen_fd, 2);
  ASSERT_EQ(0, retval) << absl::Substitute("listen() failed with errno=$0", errno);

  int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_NE(-1, client_fd) << absl::Substitute("socket() failed with errno=$0", errno);

  retval =
      connect(client_fd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
  ASSERT_EQ(0, retval);

  struct sockaddr_un client_addr;
  socklen_t len = sizeof(client_addr);
  int server_accept_fd =
      accept(server_listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &len);
  ASSERT_NE(-1, server_accept_fd) << absl::Substitute("accept() failed with errno=$0", errno);

  // Extract inode numbers.
  auto proc_parser = std::make_unique<system::ProcParser>(system::Config::GetInstance());
  std::string server_socket_id;
  ASSERT_OK(proc_parser->ReadProcPIDFDLink(getpid(), server_accept_fd, &server_socket_id));

  std::string client_socket_id;
  ASSERT_OK(proc_parser->ReadProcPIDFDLink(getpid(), client_fd, &client_socket_id));

  // Now begin the test of NetlinkSocketProber.
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<NetlinkSocketProber> socket_prober,
                       NetlinkSocketProber::Create());
  std::map<int, SocketInfo> socket_info_entries;
  ASSERT_OK(socket_prober->UnixConnections(&socket_info_entries));

  EXPECT_THAT(socket_info_entries, Contains(HasLocalUnixEndpoint(client_socket_id)));
  EXPECT_THAT(socket_info_entries, Contains(HasLocalUnixEndpoint(server_socket_id)));

  close(client_fd);
  close(server_accept_fd);
  close(server_listen_fd);
}

TEST(NetlinkSocketProberTest, ListeningInetConnection) {
  TCPSocket server;

  // A bind and connect is sufficient to establish a connection.
  server.BindAndListen();

  std::string server_endpoint = AddrPortStr(server.addr(), server.port());

  // Should not find the server endpoint in established state.
  {
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<NetlinkSocketProber> socket_prober,
                         NetlinkSocketProber::Create());
    std::map<int, SocketInfo> socket_info_entries;
    ASSERT_OK(socket_prober->InetConnections(&socket_info_entries, kTCPEstablishedState));
    EXPECT_THAT(socket_info_entries, Not(Contains(HasLocalIPEndpoint(server_endpoint))));
  }

  // Should find the server endpoint in listening state.
  {
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<NetlinkSocketProber> socket_prober,
                         NetlinkSocketProber::Create());
    std::map<int, SocketInfo> socket_info_entries;
    ASSERT_OK(socket_prober->InetConnections(&socket_info_entries, kTCPListeningState));
    EXPECT_THAT(socket_info_entries, Contains(HasLocalIPEndpoint(server_endpoint)));
  }

  // Test with multiple states specified.
  {
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<NetlinkSocketProber> socket_prober,
                         NetlinkSocketProber::Create());
    std::map<int, SocketInfo> socket_info_entries;
    ASSERT_OK(socket_prober->InetConnections(&socket_info_entries,
                                             kTCPEstablishedState | kTCPListeningState));
    EXPECT_THAT(socket_info_entries, Contains(HasLocalIPEndpoint(server_endpoint)));
  }

  server.Close();
}

TEST(NetlinkSocketProberTest, ClosedInetConnection) {
  TCPSocket client;
  TCPSocket server;

  // A bind and connect is sufficient to establish a connection.
  server.BindAndListen();
  client.Connect(server);

  std::string client_endpoint = AddrPortStr(client.addr(), client.port());

  client.Close();
  server.Close();

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<NetlinkSocketProber> socket_prober,
                       NetlinkSocketProber::Create());
  std::map<int, SocketInfo> socket_info_entries;
  ASSERT_OK(socket_prober->InetConnections(&socket_info_entries));

  EXPECT_THAT(socket_info_entries, Not(Contains(HasLocalIPEndpoint(client_endpoint))));
}

class NetNamespaceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::filesystem::path testdata_path = TestFilePath("src/common/system/testdata");

    // Bazel doesn't copy symlink testdata as symlinks, so we create the missing symlink testdata
    // here.
    ASSERT_OK(fs::CreateSymlinkIfNotExists("net:[10001]", testdata_path / "proc/123/ns/net"));
    ASSERT_OK(fs::CreateSymlinkIfNotExists("net:[10002]", testdata_path / "proc/456/ns/net"));
    ASSERT_OK(fs::CreateSymlinkIfNotExists("net:[10002]", testdata_path / "proc/789/ns/net"));

    proc_path_ = TestFilePath("src/common/system/testdata/proc");
  }

  std::string proc_path_;
};

TEST_F(NetNamespaceTest, NetNamespace) {
  ASSERT_OK_AND_EQ(NetNamespace(proc_path_, 123), 10001);
  ASSERT_OK_AND_EQ(NetNamespace(proc_path_, 456), 10002);
  ASSERT_NOT_OK(NetNamespace(proc_path_, 111));
}

TEST_F(NetNamespaceTest, PIDsByNetNamespace) {
  std::map<uint32_t, std::vector<int>> pids_by_net_ns = PIDsByNetNamespace(proc_path_);

  ASSERT_EQ(pids_by_net_ns.size(), 2);

  EXPECT_THAT(pids_by_net_ns, Contains(Pair(10001, UnorderedElementsAre(123))));
  EXPECT_THAT(pids_by_net_ns, Contains(Pair(10002, UnorderedElementsAre(456, 789))));
}

}  // namespace system
}  // namespace pl
