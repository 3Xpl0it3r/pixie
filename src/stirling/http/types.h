#pragma once

#include <chrono>
#include <string>

#include "src/common/base/utils.h"
#include "src/stirling/bcc_bpf_interface/common.h"
#include "src/stirling/common/event_parser.h"  // For FrameBase
#include "src/stirling/common/protocol_traits.h"

namespace pl {
namespace stirling {
namespace http {

//-----------------------------------------------------------------------------
// HTTP Message
//-----------------------------------------------------------------------------

// HTTP1.x headers can have multiple values for the same name, and field names are case-insensitive:
// https://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.2
using HeadersMap = std::multimap<std::string, std::string, CaseInsensitiveLess>;

inline constexpr char kContentEncoding[] = "Content-Encoding";
inline constexpr char kContentLength[] = "Content-Length";
inline constexpr char kContentType[] = "Content-Type";
inline constexpr char kTransferEncoding[] = "Transfer-Encoding";
inline constexpr char kUpgrade[] = "Upgrade";

struct Message : public stirling::FrameBase {
  MessageType type = MessageType::kUnknown;

  int minor_version = -1;
  HeadersMap headers = {};

  std::string req_method = "-";
  std::string req_path = "-";

  int resp_status = -1;
  std::string resp_message = "-";

  std::string body = "-";

  // The number of bytes in the HTTP header, used in ByteSize(),
  // as an approximation of the size of the non-body fields.
  size_t headers_byte_size = 0;

  size_t ByteSize() const override {
    return sizeof(Message) + headers_byte_size + body.size() + resp_message.size();
  }
};

//-----------------------------------------------------------------------------
// Table Store Entry Level Structs
//-----------------------------------------------------------------------------

/**
 *  Record is the primary output of the http stitcher.
 */
struct Record {
  Message req;
  Message resp;

  // Debug information that we want to pass up this record.
  // Used to record info/warnings.
  // Only pushed to table store on debug builds.
  std::string px_info = "";
};

struct ProtocolTraits {
  using frame_type = Message;
  using record_type = Record;
  using state_type = NoState;
};

};  // namespace http
}  // namespace stirling
}  // namespace pl
