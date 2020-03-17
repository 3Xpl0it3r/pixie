#pragma once

#include <bcc/BPF.h>

#include <string>

#include "src/stirling/bcc_bpf_interface/socket_trace.h"

namespace pl {
namespace stirling {

class SocketTraceBPFTableManager {
 public:
  explicit SocketTraceBPFTableManager(ebpf::BPF* bpf)
      : conn_info_map_(bpf->get_hash_table<uint64_t, struct conn_info_t>("conn_info_map")) {}

  void ReleaseResources(struct conn_id_t conn_id) {
    uint64_t key = id(conn_id);
    std::string conn_str = ToString(conn_id);

    // There is some risk that because the sequence below is not executed atomically,
    // we may have a race condition with BPF.
    // For example, an FD may be reused for a new connection, replacing the BPF map entry.
    // But if this happens after we call get_value(), but before we call remove_value(),
    // we may have caused data to be lost.
    // Then the connection would have to be re-discovered by BPF.

    struct conn_info_t conn_info_tmp;
    if (conn_info_map_.get_value(key, conn_info_tmp).code() == 0) {
      // Make sure we're accessing the same generation/timestamp ID of connection tracker.
      if (conn_info_tmp.conn_id.tsid == conn_id.tsid) {
        VLOG(2) << absl::Substitute("$0 Removing conn_info_map.", conn_str);
        if (conn_info_map_.remove_value(key).code() != 0) {
          VLOG(1) << absl::Substitute("$0 Removing conn_info_map entry failed.", conn_str);
        }
      }
    }
  }

 private:
  ebpf::BPFHashTable<uint64_t, struct conn_info_t> conn_info_map_;

  // TODO(oazizi): Can we share this with the similar function in socket_trace.c?
  uint64_t id(struct conn_id_t conn_id) const {
    return (static_cast<uint64_t>(conn_id.upid.tgid) << 32) | conn_id.fd;
  }
};

}  // namespace stirling
}  // namespace pl
