#pragma once

#ifndef __linux__

#include "src/stirling/source_connector.h"

namespace pl {
namespace stirling {

DUMMY_SOURCE_CONNECTOR(SocketTraceConnector);

}  // namespace stirling
}  // namespace pl

#else

#include <bcc/BPF.h>

#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <absl/synchronization/mutex.h>

#include "demos/applications/hipster_shop/reflection.h"
#include "src/common/grpcutils/service_descriptor_database.h"
#include "src/common/system/socket_info.h"
#include "src/stirling/bpf_tools/bcc_wrapper.h"
#include "src/stirling/cass_table.h"
#include "src/stirling/common/socket_trace.h"
#include "src/stirling/conn_stats_table.h"
#include "src/stirling/connection_tracker.h"
#include "src/stirling/http/utils.h"
#include "src/stirling/http_table.h"
#include "src/stirling/mysql_table.h"
#include "src/stirling/pgsql/types.h"
#include "src/stirling/pgsql_table.h"
#include "src/stirling/socket_trace_bpf_tables.h"
#include "src/stirling/source_connector.h"
#include "src/stirling/utils/proc_tracker.h"

DECLARE_bool(stirling_enable_parsing_protobufs);
DECLARE_uint32(stirling_socket_trace_sampling_period_millis);
DECLARE_string(perf_buffer_events_output_path);
DECLARE_bool(stirling_enable_http_tracing);
DECLARE_bool(stirling_enable_grpc_kprobe_tracing);
DECLARE_bool(stirling_enable_grpc_uprobe_tracing);
DECLARE_bool(stirling_enable_mysql_tracing);
DECLARE_bool(stirling_disable_self_tracing);
DECLARE_bool(stirling_use_packaged_headers);
DECLARE_string(stirling_role_to_trace);
DECLARE_string(stirling_cluster_cidr);

namespace pl {
namespace stirling {

class SocketTraceConnector : public SourceConnector, public bpf_tools::BCCWrapper {
 public:
  // Used in ReadPerfBuffers to drain the relevant perf buffers.
  static constexpr auto kPerfBuffers =
      MakeArray<std::string_view>("socket_control_events", "socket_data_events",
                                  "go_grpc_header_events", "go_grpc_data_events");

  static constexpr auto kTables =
      MakeArray(kConnStatsTable, kHTTPTable, kMySQLTable, kCQLTable, kPGSQLTable);
  static constexpr uint32_t kConnStatsTableNum =
      SourceConnector::TableNum(kTables, kConnStatsTable);
  static constexpr uint32_t kHTTPTableNum = SourceConnector::TableNum(kTables, kHTTPTable);
  static constexpr uint32_t kMySQLTableNum = SourceConnector::TableNum(kTables, kMySQLTable);
  static constexpr uint32_t kCQLTableNum = SourceConnector::TableNum(kTables, kCQLTable);
  static constexpr uint32_t kPGSQLTableNum = SourceConnector::TableNum(kTables, kPGSQLTable);

  static constexpr std::chrono::milliseconds kDefaultPushPeriod{1000};

  static std::unique_ptr<SourceConnector> Create(std::string_view name) {
    return std::unique_ptr<SourceConnector>(new SocketTraceConnector(name));
  }

  Status InitImpl() override;
  Status StopImpl() override;
  void TransferDataImpl(ConnectorContext* ctx, uint32_t table_num, DataTable* data_table) override;

  // Updates control map value for protocol, which specifies which role(s) to trace for the given
  // protocol's traffic.
  Status UpdateProtocolTraceRole(TrafficProtocol protocol, EndpointRole role_to_trace);
  Status TestOnlySetTargetPID(int64_t pid);
  Status DisableSelfTracing();

  /**
   * @brief Number of active ConnectionTrackers.
   *
   * Note: Multiple ConnectionTrackers on same TGID+FD are counted as 1.
   */
  size_t NumActiveConnections() const { return connection_trackers_.size(); }

  /**
   * @brief Gets a pointer to a ConnectionTracker by conn_id.
   *
   * @param connid The connection to get.
   * @return Pointer to the ConnectionTracker, or nullptr if it does not exist.
   */
  const ConnectionTracker* GetConnectionTracker(struct conn_id_t conn_id) const;

 private:
  // ReadPerfBuffers poll callback functions (must be static).
  // These are used by the static variables below, and have to be placed here.
  static void HandleDataEvent(void* cb_cookie, void* data, int data_size);
  static void HandleDataEventsLoss(void* cb_cookie, uint64_t lost);
  static void HandleControlEvent(void* cb_cookie, void* data, int data_size);
  static void HandleControlEventsLoss(void* cb_cookie, uint64_t lost);
  static void HandleHTTP2HeaderEvent(void* cb_cookie, void* data, int data_size);
  static void HandleHTTP2HeaderEventLoss(void* cb_cookie, uint64_t lost);
  static void HandleHTTP2Data(void* cb_cookie, void* data, int data_size);
  static void HandleHTTP2DataLoss(void* cb_cookie, uint64_t lost);

  static constexpr auto kProbeSpecs = MakeArray<bpf_tools::KProbeSpec>({
      {"connect", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_connect"},
      {"connect", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_connect"},
      {"accept", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_accept"},
      {"accept", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_accept"},
      {"accept4", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_accept4"},
      {"accept4", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_accept4"},
      {"open", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_open"},
      {"creat", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_open"},
      {"openat", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_open"},
      {"write", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_write"},
      {"write", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_write"},
      {"writev", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_writev"},
      {"writev", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_writev"},
      {"send", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_send"},
      {"send", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_send"},
      {"sendto", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_sendto"},
      {"sendto", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_sendto"},
      {"sendmsg", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_sendmsg"},
      {"sendmsg", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_sendmsg"},
      {"read", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_read"},
      {"read", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_read"},
      {"readv", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_readv"},
      {"readv", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_readv"},
      {"recv", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_recv"},
      {"recv", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_recv"},
      {"recvfrom", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_recv"},
      {"recvfrom", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_recv"},
      {"recvmsg", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_recvmsg"},
      {"recvmsg", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_recvmsg"},
      {"close", bpf_tools::BPFProbeAttachType::kEntry, "syscall__probe_entry_close"},
      {"close", bpf_tools::BPFProbeAttachType::kReturn, "syscall__probe_ret_close"},
  });

  inline static constexpr auto kHTTP2UProbeTmpls = MakeArray<bpf_tools::UProbeTmpl>({
      // Probes on Golang net/http2 library.
      bpf_tools::UProbeTmpl{
          .symbol = "google.golang.org/grpc/internal/transport.(*http2Client).operateHeaders",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_client_operate_headers",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "google.golang.org/grpc/internal/transport.(*http2Server).operateHeaders",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_server_operate_headers",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "google.golang.org/grpc/internal/transport.(*loopyWriter).writeHeader",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_loopy_writer_write_header",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "golang.org/x/net/http2.(*Framer).WriteDataPadded",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_framer_write_data",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "golang.org/x/net/http2.(*Framer).checkFrameOrder",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http2_framer_check_frame_order",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },

      // Probes on Golang net/http's implementation of http2.
      bpf_tools::UProbeTmpl{
          .symbol = "net/http.(*http2Framer).WriteDataPadded",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2framer_write_data",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "net/http.(*http2Framer).checkFrameOrder",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2framer_check_frame_order",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "net/http.(*http2writeResHeaders).writeFrame",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2writeResHeaders_write_frame",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "golang.org/x/net/http2/hpack.(*Encoder).WriteField",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_hpack_header_encoder",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
      bpf_tools::UProbeTmpl{
          .symbol = "net/http.(*http2serverConn).processHeaders",
          .match_type = elf_tools::SymbolMatchType::kSuffix,
          .probe_fn = "probe_http_http2serverConn_processHeaders",
          .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
      },
  });

  inline static const auto kOpenSSLUProbes = MakeArray<bpf_tools::UProbeSpec>(
      {// A probe on entry of SSL_write
       bpf_tools::UProbeSpec{
           .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
           .symbol = "SSL_write",
           .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
           .probe_fn = "probe_entry_SSL_write",
       },

       // A probe on return of SSL_write
       bpf_tools::UProbeSpec{
           .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
           .symbol = "SSL_write",
           .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
           .probe_fn = "probe_ret_SSL_write",
       },

       // A probe on entry of SSL_read
       bpf_tools::UProbeSpec{
           .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
           .symbol = "SSL_read",
           .attach_type = bpf_tools::BPFProbeAttachType::kEntry,
           .probe_fn = "probe_entry_SSL_read",
       },

       // A probe on return of SSL_read
       bpf_tools::UProbeSpec{
           .binary_path = "/usr/lib/x86_64-linux-gnu/libssl.so.1.1",
           .symbol = "SSL_read",
           .attach_type = bpf_tools::BPFProbeAttachType::kReturn,
           .probe_fn = "probe_ret_SSL_read",
       }});

  // TODO(oazizi): Remove send and recv probes once we are confident that they don't trace anything.
  //               Note that send/recv are not in the syscall table
  //               (https://filippo.io/linux-syscall-table/), but are defined as SYSCALL_DEFINE4 in
  //               https://elixir.bootlin.com/linux/latest/source/net/socket.c.

  static constexpr auto kPerfBufferSpecs = MakeArray<bpf_tools::PerfBufferSpec>({
      // For data events. The order must be consistent with output tables.
      {"socket_data_events", HandleDataEvent, HandleDataEventsLoss},
      // For non-data events. Must not mix with the above perf buffers for data events.
      {"socket_control_events", HandleControlEvent, HandleControlEventsLoss},
      {"go_grpc_header_events", HandleHTTP2HeaderEvent, HandleHTTP2HeaderEventLoss},
      {"go_grpc_data_events", HandleHTTP2Data, HandleHTTP2DataLoss},
  });

  // TODO(yzhao): We will remove this once finalized the mechanism of lazy protobuf parse.
  inline static ::pl::grpc::ServiceDescriptorDatabase grpc_desc_db_{
      demos::hipster_shop::GetFileDescriptorSet()};

  explicit SocketTraceConnector(std::string_view source_name);

  // Helper functions for dynamically deploying uprobes:

  // Find new PIDs since the last call, grouped into a map by the binary path.
  // The new PIDs may require the http2_symaddrs_map BPF map to be updated (even if the binary is
  // already being traced).
  std::map<std::string, std::vector<int32_t> > FindNewPIDs();

  bool UpdateHTTP2SymAddrs(
      elf_tools::ElfReader* elf_reader, const std::vector<int32_t>& pids,
      ebpf::BPFHashTable<uint32_t, struct conn_symaddrs_t>* http2_symaddrs_map);

  StatusOr<int> AttachUProbeTmpl(const ArrayView<bpf_tools::UProbeTmpl>& probe_tmpls,
                                 const std::string& binary, elf_tools::ElfReader* elf_reader);

  StatusOr<int> AttachHTTP2UProbes(
      const std::string& binary, elf_tools::ElfReader* elf_reader,
      const std::vector<int32_t>& new_pids,
      ebpf::BPFHashTable<uint32_t, struct conn_symaddrs_t>* http2_symaddrs_map);

  StatusOr<int> AttachOpenSSLUProbes(const std::string& binary,
                                     const std::vector<int32_t>& new_pids);

  // Scans binaries and deploys uprobes for all purposes (HTTP2, OpenSSL, etc.) on new processes.
  void DeployUProbes();

  // Wraps DeployUProbes() in a loop. Stops when this SocketTraceConnector is stopped.
  // Used for creating a background thread to attach uprobes for newly-created processes.
  void AttachUProbesLoop();

  // This function causes the perf buffer to be read, and triggers callbacks per message.
  void ReadPerfBuffers();

  // Events from BPF.
  // TODO(oazizi/yzhao): These all operate based on pass-by-value, which copies.
  //                     The Handle* functions should call make_unique() of new corresponding
  //                     objects, and these functions should take unique_ptrs.
  void AcceptDataEvent(std::unique_ptr<SocketDataEvent> event);
  void AcceptControlEvent(socket_control_event_t event);
  void AcceptHTTP2Header(std::unique_ptr<HTTP2HeaderEvent> event);
  void AcceptHTTP2Data(std::unique_ptr<HTTP2DataEvent> event);

  // Transfer of messages to the data table.
  void TransferStreams(ConnectorContext* ctx, uint32_t table_num, DataTable* data_table);

  template <typename TProtocolTraits>
  void TransferStream(ConnectorContext* ctx, ConnectionTracker* tracker, DataTable* data_table) {
    VLOG(3) << absl::StrCat("Connection\n", DebugString<TProtocolTraits>(*tracker, ""));

    if (tracker->state() == ConnectionTracker::State::kTransferring) {
      // ProcessToRecords() parses raw events and produces messages in format that are expected by
      // table store. But those messages are not cached inside ConnectionTracker.
      //
      // TODO(yzhao): Consider caching produced messages if they are not transferred.
      auto result = tracker->ProcessToRecords<TProtocolTraits>();
      for (auto& msg : result) {
        AppendMessage(ctx, *tracker, std::move(msg), data_table);
      }
    }
  }

  template <typename TRecordType>
  static void AppendMessage(ConnectorContext* ctx, const ConnectionTracker& conn_tracker,
                            TRecordType record, DataTable* data_table);

  absl::flat_hash_set<md::UPID> get_mds_upids() {
    absl::MutexLock lock(&mds_upids_lock_);
    return mds_upids_;
  }

  void set_mds_upids(absl::flat_hash_set<md::UPID> upids) {
    absl::MutexLock lock(&mds_upids_lock_);
    mds_upids_ = std::move(upids);
  }

  // Returns vector representing currently known cluster (pod and service) CIDRs.
  std::vector<CIDRBlock> ClusterCIDRs(ConnectorContext* ctx);

  // Setups output file stream object writing to the input file path.
  void SetupOutput(const std::filesystem::path& file);
  // Writes data event to the specified output file.
  void WriteDataEvent(const SocketDataEvent& event);

  // TODO(oazizi/yzhao): Change to use std::unique_ptr.

  // Note that the inner map cannot be a vector, because there is no guaranteed order
  // in which events are read from perf buffers.
  // Inner map could be a priority_queue, but benchmarks showed better performance with a std::map.
  // Key is {PID, FD} for outer map (see GetStreamId()), and tsid for inner map.
  std::unordered_map<uint64_t, std::map<uint64_t, ConnectionTracker> > connection_trackers_;

  struct TransferSpec {
    uint32_t table_num;
    std::function<void(SocketTraceConnector&, ConnectorContext*, ConnectionTracker*, DataTable*)>
        transfer_fn = nullptr;
    bool enabled = false;
    // TODO(yzhao): Consider removing this if protocol-specific trace role is not needed.
    // Given protocol_transfer_specs_ is already here, it makes sense to not add another member
    // variable.
    EndpointRole role_to_trace = kRoleClient;
  };

  // This map controls how each protocol is processed and transferred.
  // The table num identifies which data the collected data is transferred.
  // The transfer_fn defines which function is called to process the data for transfer.
  std::map<TrafficProtocol, TransferSpec> protocol_transfer_specs_ = {
      {kProtocolHTTP, {kHTTPTableNum, &SocketTraceConnector::TransferStream<http::ProtocolTraits>}},
      {kProtocolHTTP2,
       {kHTTPTableNum, &SocketTraceConnector::TransferStream<http2::ProtocolTraits>}},
      {kProtocolHTTP2Uprobe,
       {kHTTPTableNum, &SocketTraceConnector::TransferStream<http2u::ProtocolTraits>}},
      {kProtocolMySQL,
       {kMySQLTableNum, &SocketTraceConnector::TransferStream<mysql::ProtocolTraits>}},
      {kProtocolCQL, {kCQLTableNum, &SocketTraceConnector::TransferStream<cass::ProtocolTraits>}},
      {kProtocolPGSQL,
       {kPGSQLTableNum, &SocketTraceConnector::TransferStream<pgsql::ProtocolTraits>}},
      // Unknown protocols attached to HTTP table so that they run their cleanup functions,
      // but the use of nullptr transfer_fn means it won't actually transfer data to the HTTP table.
      {kProtocolUnknown, {kHTTPTableNum, nullptr}},
  };

  // If not a nullptr, writes the events received from perf buffers to this stream.
  std::unique_ptr<std::ofstream> perf_buffer_events_output_stream_;
  enum class OutputFormat {
    kTxt,
    kBin,
  };
  OutputFormat perf_buffer_events_output_format_ = OutputFormat::kTxt;

  // Portal to query for connections, by pid and inode.
  std::unique_ptr<system::SocketInfoManager> socket_info_mgr_;

  std::unique_ptr<system::ProcParser> proc_parser_;

  // Used to periodically attach uprobes.
  std::thread attach_uprobes_thread_;

  // TODO(yzhao): To have a simple synchronization model, uses Mutex + copy.
  absl::Mutex mds_upids_lock_;
  absl::flat_hash_set<md::UPID> mds_upids_ GUARDED_BY(mds_upids_lock_);

  ProcTracker proc_tracker_;

  // Records the binaries that have been attached uprobes.
  absl::flat_hash_set<std::string> http2_probed_binaries_;
  absl::flat_hash_set<std::string> openssl_probed_binaries_;

  std::shared_ptr<SocketTraceBPFTableManager> bpf_table_info_;

  // Manual cluster CIDR provided through flags.
  std::optional<CIDRBlock> cluster_cidr_override_;

  FRIEND_TEST(SocketTraceConnectorTest, AppendNonContiguousEvents);
  FRIEND_TEST(SocketTraceConnectorTest, NoEvents);
  FRIEND_TEST(SocketTraceConnectorTest, HTTPContentType);
  FRIEND_TEST(SocketTraceConnectorTest, UPIDCheck);
  FRIEND_TEST(SocketTraceConnectorTest, RequestResponseMatching);
  FRIEND_TEST(SocketTraceConnectorTest, MissingEventInStream);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInOrder);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupOutOfOrder);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupMissingDataEvent);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupOldGenerations);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInactiveDead);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInactiveAlive);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupNoProtocol);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLPrepareExecuteClose);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLQuery);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLMultipleCommands);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLQueryWithLargeResultset);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLMultiResultset);
  FRIEND_TEST(SocketTraceConnectorTest, CQLQuery);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2ClientTest);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2ServerTest);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2PartialStream);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2ResponseOnly);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2SpanAcrossTransferData);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2SequentialStreams);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2ParallelStreams);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2StreamSandwich);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2StreamIDRace);
  FRIEND_TEST(SocketTraceConnectorTest, HTTP2OldStream);
};

}  // namespace stirling
}  // namespace pl

#endif
