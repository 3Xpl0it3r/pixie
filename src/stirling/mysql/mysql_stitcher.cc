#include <deque>
#include <memory>
#include <string>
#include <utility>

#include "src/stirling/mysql/mysql.h"
#include "src/stirling/mysql/mysql_handler.h"
#include "src/stirling/mysql/mysql_stitcher.h"

namespace pl {
namespace stirling {
namespace mysql {

// This function looks for unsynchronized req/resp packet queues.
// This could happen for a number of reasons:
//  - lost events
//  - previous unhandled case resulting in a bad state.
// Currently handles the case where an apparently missing request has left dangling responses,
// in which case those requests are popped off.
// TODO(oazizi): Also handle cases where responses should match to a later request (in which case
// requests should be popped off).
// TODO(oazizi): Should also consider sequence IDs in this function.
void SyncRespQueue(const Packet& req_packet, std::deque<Packet>* resp_packets) {
  // This handles the case where there are responses that pre-date a request.
  while (!resp_packets->empty()) {
    Packet& resp_packet = resp_packets->front();

    if (resp_packet.timestamp_ns > req_packet.timestamp_ns) {
      break;
    }

    LOG(WARNING) << absl::Substitute(
        "Dropping response packet that pre-dates request. Size=$0 [OK=$1 ERR=$2 EOF=$3]",
        resp_packet.msg.size(), IsOKPacket(resp_packet), IsErrPacket(resp_packet),
        IsEOFPacket(resp_packet));
    resp_packets->pop_front();
  }
}

/**
 * Returns a read-only view of packets that correspond to the request packet at the head of
 * the request packets, which can then be sent for further processing as a contained bundle.
 *
 * The creation of the response packet bundle is done using timestamps and sequence numbers.
 * Any request with a timestamp that occurs after the timestamp of the 2nd request is not included.
 * Sequence numbers are also checked to be contiguous. Any gap results in sealing the bundle.
 *
 *
 * @param req_packets Deque of all received request packets (some may be missing).
 * @param resp_packets Dequeue of all received response packets (some may be missing).
 * @return View into the "bundle" of response packets that correspond to the first request packet.
 */
DequeView<Packet> GetRespView(const std::deque<Packet>& req_packets,
                              const std::deque<Packet>& resp_packets) {
  DCHECK(!req_packets.empty());

  int count = 0;

  for (const auto& resp_packet : resp_packets) {
    if (req_packets.size() > 1 && resp_packet.timestamp_ns > req_packets[1].timestamp_ns) {
      break;
    }

    uint8_t expected_seq_id = count + 1;
    if (resp_packet.sequence_id != expected_seq_id) {
      LOG(WARNING) << absl::Substitute(
          "Found packet with unexpected sequence ID [expected=$0 actual=$1]", expected_seq_id,
          resp_packet.sequence_id);
      break;
    }
    ++count;
  }

  return DequeView<Packet>(resp_packets, 0, count);
}

std::vector<Record> ProcessMySQLPackets(std::deque<Packet>* req_packets,
                                        std::deque<Packet>* resp_packets, mysql::State* state) {
  std::vector<Record> entries;

  // Process one request per loop iteration. Each request may consume 0, 1 or 2+ response packets.
  // The actual work is forked off to a helper function depending on the command type.
  // There are three possible outcomes for each request:
  //  1) Success. We continue to the next command.
  //  2) Needs more data: Not enough resp packets. We stop processing.
  //     We are still in a good state, and this is not considered an error.
  //  3) Error: An unexpected packet that indicates we have lost sync on the connection.
  //     This is communicated through the StatusOr mechanism.
  //     Recovery is the responsibility of the caller (i.e. ConnectionTracker).
  while (!req_packets->empty()) {
    Packet& req_packet = req_packets->front();

    // Command is the first byte.
    char command = req_packet.msg[0];

    VLOG(2) << absl::StrFormat("command=%x msg=%s", command, req_packet.msg.substr(1));

    // For safety, make sure we have no stale response packets.
    SyncRespQueue(req_packet, resp_packets);

    DequeView<Packet> resp_packets_view = GetRespView(*req_packets, *resp_packets);

    VLOG(2) << absl::Substitute("req_packets=$0 resp_packets=$1 resp_view_size=$2",
                                req_packets->size(), resp_packets->size(),
                                resp_packets_view.size());

    // TODO(oazizi): Also try to sync if responses appear to be for the second request in the queue.
    // (i.e. dropped responses).

    StatusOr<ParseState> s;
    entries.emplace_back();
    Record& entry = entries.back();

    switch (DecodeCommand(command)) {
      // Internal commands with response: ERR_Packet.
      case MySQLEventType::kConnect:
      case MySQLEventType::kConnectOut:
      case MySQLEventType::kTime:
      case MySQLEventType::kDelayedInsert:
      case MySQLEventType::kDaemon:
        s = ProcessRequestWithBasicResponse(req_packet, /* string_req */ false, resp_packets_view,
                                            &entry);
        break;

      case MySQLEventType::kInitDB:
      case MySQLEventType::kCreateDB:
      case MySQLEventType::kDropDB:
        s = ProcessRequestWithBasicResponse(req_packet, /* string_req */ true, resp_packets_view,
                                            &entry);
        break;

      // Basic Commands with response: OK_Packet or ERR_Packet
      case MySQLEventType::kSleep:
      case MySQLEventType::kRegisterSlave:
      case MySQLEventType::kResetConnection:
      case MySQLEventType::kProcessKill:
      case MySQLEventType::kRefresh:  // Deprecated.
      case MySQLEventType::kPing:     // COM_PING can't actually send ERR_Packet.
        s = ProcessRequestWithBasicResponse(req_packet, /* string_req */ false, resp_packets_view,
                                            &entry);
        break;

      case MySQLEventType::kQuit:  // Response: OK_Packet or a connection close.
        s = ProcessRequestWithBasicResponse(req_packet, /* string_req */ false, resp_packets_view,
                                            &entry);
        break;

      // Basic Commands with response: EOF_Packet or ERR_Packet.
      case MySQLEventType::kShutdown:  // Deprecated.
      case MySQLEventType::kSetOption:
      case MySQLEventType::kDebug:
        s = ProcessRequestWithBasicResponse(req_packet, /* string_req */ false, resp_packets_view,
                                            &entry);
        break;

      // COM_FIELD_LIST has its own COM_FIELD_LIST meta response (ERR_Packet or one or more Column
      // Definition packets and a closing EOF_Packet).
      case MySQLEventType::kFieldList:  // Deprecated.
        s = ProcessFieldList(req_packet, resp_packets_view, &entry);
        break;

      // COM_QUERY has its own COM_QUERY meta response (ERR_Packet, OK_Packet,
      // Protocol::LOCAL_INFILE_Request, or ProtocolText::Resultset).
      case MySQLEventType::kQuery:
        s = ProcessQuery(req_packet, resp_packets_view, &entry);
        break;

      // COM_STMT_PREPARE returns COM_STMT_PREPARE_OK on success, ERR_Packet otherwise.
      case MySQLEventType::kStmtPrepare:
        s = ProcessStmtPrepare(req_packet, resp_packets_view, state, &entry);
        break;

      // COM_STMT_SEND_LONG_DATA has no response.
      case MySQLEventType::kStmtSendLongData:
        s = ProcessStmtSendLongData(req_packet, resp_packets_view, state, &entry);
        break;

      // COM_STMT_EXECUTE has its own COM_STMT_EXECUTE meta response (OK_Packet, ERR_Packet or a
      // resultset: Binary Protocol Resultset).
      case MySQLEventType::kStmtExecute:
        s = ProcessStmtExecute(req_packet, resp_packets_view, state, &entry);
        break;

      // COM_CLOSE has no response.
      case MySQLEventType::kStmtClose:
        s = ProcessStmtClose(req_packet, resp_packets_view, state, &entry);
        break;

      // COM_STMT_RESET response is OK_Packet if the statement could be reset, ERR_Packet if not.
      case MySQLEventType::kStmtReset:
        s = ProcessStmtReset(req_packet, resp_packets_view, state, &entry);
        break;

      // COM_STMT_FETCH has a meta response (multi-resultset, or ERR_Packet).
      case MySQLEventType::kStmtFetch:
        s = ProcessStmtFetch(req_packet, resp_packets_view, state, &entry);
        break;

      case MySQLEventType::kProcessInfo:     // a ProtocolText::Resultset or ERR_Packet
      case MySQLEventType::kChangeUser:      // Authentication Method Switch Request Packet or
                                             // ERR_Packet
      case MySQLEventType::kBinlogDumpGTID:  // binlog network stream, ERR_Packet or EOF_Packet
      case MySQLEventType::kBinlogDump:      // binlog network stream, ERR_Packet or EOF_Packet
      case MySQLEventType::kTableDump:       // a table dump or ERR_Packet
      case MySQLEventType::kStatistics:      // string.EOF
        // Rely on recovery to re-sync responses based on timestamps.
        s = error::Internal("Unimplemented command $0.", command);
        break;

      default:
        s = error::Internal("Unknown command $0.", command);
    }

    if (!s.ok()) {
      LOG(ERROR) << absl::Substitute("MySQL packet processing error: msg=$0", s.msg());
    } else {
      ParseState result = s.ValueOrDie();
      DCHECK(result == ParseState::kSuccess || result == ParseState::kNeedsMoreData);

      if (result == ParseState::kNeedsMoreData) {
        bool is_last_req = req_packets->size() == 1;
        bool resp_looks_healthy = resp_packets_view.size() == resp_packets->size();
        if (is_last_req && resp_looks_healthy) {
          VLOG(3) << "Appears to be an incomplete message. Waiting for more data";
          // More response data will probably be captured in next iteration, so stop.
          // Also, undo the entry we started.
          entries.pop_back();
          break;
        }
        LOG(ERROR)
            << "Didn't have enough response packets, but doesn't appear to be partial either.";
        // Continue on, since waiting for more packets likely won't help.
      }
    }

    req_packets->pop_front();
    resp_packets->erase(resp_packets->begin(), resp_packets->begin() + resp_packets_view.size());
  }
  return entries;
}

// Process a COM_STMT_PREPARE request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-stmt-prepare.html
StatusOr<ParseState> ProcessStmtPrepare(const Packet& req_packet, DequeView<Packet> resp_packets,
                                        mysql::State* state, Record* entry) {
  //----------------
  // Request
  //----------------

  HandleStringRequest(req_packet, entry);

  //----------------
  // Response
  //----------------

  if (resp_packets.empty()) {
    entry->resp.status = MySQLRespStatus::kUnknown;
    return ParseState::kNeedsMoreData;
  }

  const Packet& first_resp_packet = resp_packets.front();

  if (IsErrPacket(first_resp_packet)) {
    HandleErrMessage(resp_packets, entry);

    if (resp_packets.size() > 1) {
      LOG(ERROR) << absl::Substitute(
          "Did not expect packets after error packet [cmd=$0]. Remaining $1 response packets will "
          "be ignored.",
          static_cast<uint8_t>(req_packet.msg[0]), resp_packets.size() - 1);
    }

    return ParseState::kSuccess;
  }

  return HandleStmtPrepareOKResponse(resp_packets, state, entry);
}

// Process a COM_STMT_SEND_LONG_DATA request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-stmt-send-long-data.html
StatusOr<ParseState> ProcessStmtSendLongData(const Packet& req_packet,
                                             DequeView<Packet> resp_packets,
                                             mysql::State* /* state */, Record* entry) {
  //----------------
  // Request
  //----------------

  HandleNonStringRequest(req_packet, entry);

  //----------------
  // Response
  //----------------

  // COM_STMT_CLOSE doesn't use any response packets.
  if (!resp_packets.empty()) {
    LOG(ERROR) << absl::Substitute(
        "Did not expect any response packets [cmd=$0]. All response packets will be ignored.",
        static_cast<uint8_t>(req_packet.msg[0]));
  }

  return ParseState::kSuccess;
}

// Process a COM_STMT_EXECUTE request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-stmt-execute.html
StatusOr<ParseState> ProcessStmtExecute(const Packet& req_packet, DequeView<Packet> resp_packets,
                                        mysql::State* state, Record* entry) {
  //----------------
  // Request
  //----------------

  HandleStmtExecuteRequest(req_packet, &state->prepared_statements, entry);

  //----------------
  // Response
  //----------------

  if (resp_packets.empty()) {
    entry->resp.status = MySQLRespStatus::kUnknown;
    return ParseState::kNeedsMoreData;
  }

  const Packet& first_resp_packet = resp_packets.front();

  if (IsErrPacket(first_resp_packet)) {
    HandleErrMessage(resp_packets, entry);

    if (resp_packets.size() > 1) {
      LOG(ERROR) << absl::Substitute(
          "Did not expect packets after error packet [cmd=$0]. Remaining $1 response packets will "
          "be ignored.",
          static_cast<uint8_t>(req_packet.msg[0]), resp_packets.size() - 1);
    }

    return ParseState::kSuccess;
  }

  if (IsOKPacket(first_resp_packet)) {
    HandleOKMessage(resp_packets, entry);

    if (resp_packets.size() > 1) {
      LOG(ERROR) << absl::Substitute(
          "Did not expect more than one response packet [cmd=$0]. Remaining $1 response packets "
          "will be ignored.",
          static_cast<uint8_t>(req_packet.msg[0]), resp_packets.size() - 1);
    }

    return ParseState::kSuccess;
  }

  return HandleResultsetResponse(resp_packets, entry);
}

// Process a COM_STMT_CLOSE request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-stmt-close.html
StatusOr<ParseState> ProcessStmtClose(const Packet& req_packet, DequeView<Packet> resp_packets,
                                      mysql::State* state, Record* entry) {
  //----------------
  // Request
  //----------------

  HandleStmtCloseRequest(req_packet, &state->prepared_statements, entry);

  //----------------
  // Response
  //----------------

  // COM_STMT_CLOSE doesn't use any response packets.
  if (!resp_packets.empty()) {
    LOG(ERROR) << absl::Substitute(
        "Did not expect any response packets [cmd=$0]. Remaining $1 response packets will be "
        "ignored.",
        static_cast<uint8_t>(req_packet.msg[0]), resp_packets.size());
  }

  entry->resp.status = MySQLRespStatus::kOK;
  // Use request as the timestamp because a close has no response. Latency is 0.
  entry->resp.timestamp_ns = req_packet.timestamp_ns;
  return ParseState::kSuccess;
}

// Process a COM_STMT_FETCH request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-stmt-fetch.html
StatusOr<ParseState> ProcessStmtFetch(const Packet& req_packet,
                                      DequeView<Packet> /* resp_packets */,
                                      mysql::State* /* state */, Record* entry) {
  //----------------
  // Request
  //----------------

  HandleNonStringRequest(req_packet, entry);

  //----------------
  // Response
  //----------------

  entry->resp.status = MySQLRespStatus::kUnknown;
  return error::Unimplemented("COM_STMT_FETCH response is unhandled.");
}

// Process a COM_STMT_RESET request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-stmt-reset.html
StatusOr<ParseState> ProcessStmtReset(const Packet& req_packet, DequeView<Packet> resp_packets,
                                      mysql::State* state, Record* entry) {
  PL_UNUSED(state);

  // Defer to basic response for now.
  return ProcessRequestWithBasicResponse(req_packet, /* string_req */ false, resp_packets, entry);
}

// Process a COM_QUERY request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-query.html
StatusOr<ParseState> ProcessQuery(const Packet& req_packet, DequeView<Packet> resp_packets,
                                  Record* entry) {
  //----------------
  // Request
  //----------------

  HandleStringRequest(req_packet, entry);

  //----------------
  // Response
  //----------------

  if (resp_packets.empty()) {
    entry->resp.status = MySQLRespStatus::kUnknown;
    return ParseState::kNeedsMoreData;
  }

  const Packet& first_resp_packet = resp_packets.front();

  if (IsErrPacket(first_resp_packet)) {
    HandleErrMessage(resp_packets, entry);

    if (resp_packets.size() > 1) {
      LOG(ERROR) << absl::Substitute(
          "Did not expect packets after error packet [cmd=$0]. Remaining $1 response packets will "
          "be ignored.",
          static_cast<uint8_t>(req_packet.msg[0]), resp_packets.size() - 1);
    }

    return ParseState::kSuccess;
  }

  if (IsOKPacket(first_resp_packet)) {
    HandleOKMessage(resp_packets, entry);

    if (resp_packets.size() > 1) {
      LOG(ERROR) << absl::Substitute(
          "Did not expect more than one response packet [cmd=$0]. Remaining $1 response packets "
          "will be ignored.",
          static_cast<uint8_t>(req_packet.msg[0]), resp_packets.size() - 1);
    }

    return ParseState::kSuccess;
  }

  return HandleResultsetResponse(resp_packets, entry);
}

// Process a COM_FIELD_LIST request and response, and populate details into a record entry.
// MySQL documentation: https://dev.mysql.com/doc/internals/en/com-field-list.html
StatusOr<ParseState> ProcessFieldList(const Packet& req_packet,
                                      DequeView<Packet> /* resp_packets */, Record* entry) {
  //----------------
  // Request
  //----------------

  HandleStringRequest(req_packet, entry);

  //----------------
  // Response
  //----------------

  entry->resp.status = MySQLRespStatus::kUnknown;
  return error::Unimplemented("COM_FIELD_LIST response is unhandled.");
}

// Process a simple request and response pair, and populate details into a record entry.
// This is for MySQL commands that have only a single OK, ERR or EOF response.
// TODO(oazizi): Currently any of OK, ERR or EOF are accepted, but could specialize
// to expect a subset, since some responses are invalid for certain commands.
// For example, a COM_INIT_DB command should never receive an EOF response.
// All we would do is print a warning, though, so this is low priority.
StatusOr<ParseState> ProcessRequestWithBasicResponse(const Packet& req_packet, bool string_req,
                                                     DequeView<Packet> resp_packets,
                                                     Record* entry) {
  //----------------
  // Request
  //----------------

  if (string_req) {
    HandleStringRequest(req_packet, entry);
  } else {
    HandleNonStringRequest(req_packet, entry);
  }

  //----------------
  // Response
  //----------------

  if (resp_packets.empty()) {
    entry->resp.status = MySQLRespStatus::kUnknown;
    return ParseState::kNeedsMoreData;
  }

  if (resp_packets.size() > 1) {
    LOG(ERROR) << absl::Substitute(
        "Did not expect more than one response packet [cmd=$0]. Remaining $1 response packets will "
        "be ignored.",
        static_cast<uint8_t>(req_packet.msg[0]), resp_packets.size() - 1);
  }

  const Packet& resp_packet = resp_packets.front();

  if (IsOKPacket(resp_packet) || IsEOFPacket(resp_packet)) {
    entry->resp.status = MySQLRespStatus::kOK;
    entry->resp.timestamp_ns = resp_packet.timestamp_ns;
    return ParseState::kSuccess;
  }

  if (IsErrPacket(resp_packet)) {
    HandleErrMessage(resp_packets, entry);
    return ParseState::kSuccess;
  }

  entry->resp.status = MySQLRespStatus::kUnknown;
  return error::Internal("Unexpected packet");
}

}  // namespace mysql
}  // namespace stirling
}  // namespace pl
