/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "src/stirling/source_connectors/socket_tracer/protocols/http/parse.h"

#include <picohttpparser.h>

#include <algorithm>
#include <string>
#include <utility>

namespace px {
namespace stirling {
namespace protocols {
namespace http {

namespace pico_wrapper {

constexpr size_t kMaxNumHeaders = 50;

// Fields populated by picohttpparser phr_parse_request().
struct HTTPRequest {
  const char* method = nullptr;
  size_t method_len = 0;
  const char* path = nullptr;
  size_t path_len = 0;
  int minor_version = 0;
  struct phr_header headers[kMaxNumHeaders];
  // Set header number to maximum we can accept.
  // Pico will change it to the number of headers parsed for us.
  size_t num_headers = kMaxNumHeaders;
};

int ParseRequest(std::string_view buf, HTTPRequest* result) {
  return phr_parse_request(buf.data(), buf.size(), &result->method, &result->method_len,
                           &result->path, &result->path_len, &result->minor_version,
                           result->headers, &result->num_headers, /*last_len*/ 0);
}

// Fields populated by picohttpparser phr_parse_response().
struct HTTPResponse {
  const char* msg = nullptr;
  size_t msg_len = 0;
  int status = 0;
  int minor_version = 0;
  struct phr_header headers[kMaxNumHeaders];
  // Set header number to maximum we can accept.
  // Pico will change it to the number of headers parsed for us.
  size_t num_headers = kMaxNumHeaders;
};

int ParseResponse(std::string_view buf, HTTPResponse* result) {
  return phr_parse_response(buf.data(), buf.size(), &result->minor_version, &result->status,
                            &result->msg, &result->msg_len, result->headers, &result->num_headers,
                            /*last_len*/ 0);
}

HeadersMap GetHTTPHeadersMap(const phr_header* headers, size_t num_headers) {
  HeadersMap result;
  for (size_t i = 0; i < num_headers; i++) {
    std::string name(headers[i].name, headers[i].name_len);
    std::string value(headers[i].value, headers[i].value_len);
    result.emplace(std::move(name), std::move(value));
  }
  return result;
}

}  // namespace pico_wrapper

// TODO(oazizi): ParseChunk makes a copy of the data. Consider finding a way
//               to mutate the input buffer such that we can avoid this copy.
//               phr_decode_chunked() already mutates the input buffer, but
//               this needs to be done in a way that doesn't mess up the rest of
//               the parsing, since there will be "unused" bytes at the end of the
//               chunk, but before the rest of the data in the DataStreamBuffer.
//               Note that the copy is not overhead when a complete message is found,
//               since the data is std::moved to the result.
ParseState ParseChunk(std::string_view* data, Message* result) {
  phr_chunked_decoder chunk_decoder = {};
  std::string data_copy(*data);
  char* buf = data_copy.data();
  size_t buf_size = data_copy.size();
  ssize_t retval = phr_decode_chunked(&chunk_decoder, buf, &buf_size);
  if (retval == -1) {
    // Parse failed.
    return ParseState::kInvalid;
  } else if (retval >= 0) {
    // Complete message.
    data_copy.resize(buf_size);
    result->body = std::move(data_copy);
    // phr_decode_chunked rewrites the buffer in place, removing chunked-encoding headers.
    // So we cannot simply remove the prefix, but rather have to shorten the buffer too.
    // This is done via retval, which specifies how many unprocessed bytes are left.
    data->remove_prefix(data->size() - retval);
    // Pico claims that the last \r\n are unparsed, manually remove them.
    while (!data->empty() && (data->front() == '\r' || data->front() == '\n')) {
      data->remove_prefix(1);
    }
    return ParseState::kSuccess;
  } else if (retval == -2) {
    // Incomplete message.
    return ParseState::kNeedsMoreData;
  }
  LOG(DFATAL) << "Unexpected retval from phr_decode_chunked()";
  return ParseState::kUnknown;
}

ParseState ParseBody(std::string_view* buf, Message* result) {
  // Try to find boundary of message by looking at Content-Length and Transfer-Encoding.

  // From https://tools.ietf.org/html/rfc7230:
  //  A sender MUST NOT send a Content-Length header field in any message
  //  that contains a Transfer-Encoding header field.
  //
  //  A user agent SHOULD send a Content-Length in a request message when
  //  no Transfer-Encoding is sent and the request method defines a meaning
  //  for an enclosed payload body.  For example, a Content-Length header
  //  field is normally sent in a POST request even when the value is 0
  //  (indicating an empty payload body).  A user agent SHOULD NOT send a
  //  Content-Length header field when the request message does not contain
  //  a payload body and the method semantics do not anticipate such a
  //  body.

  // Case 0: Check for a HEAD response with no body.
  // Responses to HEAD requests are special, because they may include Content-Length
  // or Transfer-Encodings, but the body will still be empty.
  // Reference: https://developer.mozilla.org/en-US/docs/Web/HTTP/Methods/HEAD
  // TODO(rcheng): Pass in state to the parser so we know when to expect HEAD responses.
  if (result->type == message_type_t::kResponse) {
    // We typically expect a body at this point, but for responses to HEAD requests,
    // there won't be a body. To detect such HEAD responses, we check to see if the next bytes
    // are actually the beginning of the next response by attempting to parse it.
    pico_wrapper::HTTPResponse r;
    bool adjacent_resp =
        absl::StartsWith(*buf, "HTTP") && (pico_wrapper::ParseResponse(*buf, &r) > 0);

    // TODO(rcheng): Use actual conn_closed information once it's piped in.
    bool conn_closed = false;

    if (adjacent_resp || (buf->empty() && conn_closed)) {
      result->body = "";
      return ParseState::kSuccess;
    }
  }

  // Case 1: Content-Length
  const auto content_length_iter = result->headers.find(kContentLength);
  if (content_length_iter != result->headers.end()) {
    size_t len;
    if (!absl::SimpleAtoi(content_length_iter->second, &len)) {
      LOG(ERROR) << absl::Substitute("Unable to parse Content-Length: $0",
                                     content_length_iter->second);
      return ParseState::kInvalid;
    }

    if (buf->size() < len) {
      return ParseState::kNeedsMoreData;
    }

    result->body = buf->substr(0, len);
    buf->remove_prefix(std::min(len, buf->size()));
    return ParseState::kSuccess;
  }

  // Case 2: Chunked transfer.
  const auto transfer_encoding_iter = result->headers.find(kTransferEncoding);
  if (transfer_encoding_iter != result->headers.end() &&
      transfer_encoding_iter->second == "chunked") {
    return ParseChunk(buf, result);
  }

  // Case 3: Message has content, but no Content-Length or Transfer-Encoding.

  // Case 3A: Requests where we can assume no body.
  // An HTTP request with no Content-Length and no Transfer-Encoding should not have a body when
  // no Content-Length or Transfer-Encoding is set:
  // "A user agent SHOULD NOT send a Content-Length header field when the request message does
  // not contain a payload body and the method semantics do not anticipate such a body."
  //
  // We apply this to all methods, since we have no better strategy in other cases.
  if (result->type == message_type_t::kRequest) {
    result->body = "";
    return ParseState::kSuccess;
  }

  // Case 3B: Responses where we can assume no body.
  // The status codes below MUST not have a body, according to the spec,
  // so if no Content-Length or Transfer-Encoding are present,
  // assume they don't have a body.
  // See: https://tools.ietf.org/html/rfc2616#section-4.4
  if ((result->resp_status >= 100 && result->resp_status < 200) || result->resp_status == 204 ||
      result->resp_status == 304) {
    result->body = "";

    // Status 101 is an even more special case.
    if (result->resp_status == 101) {
      const auto upgrade_iter = result->headers.find(kUpgrade);
      if (upgrade_iter == result->headers.end()) {
        LOG(WARNING) << "Expected an Upgrade header with HTTP status 101";
        return ParseState::kEOS;
      }

      // Header 'Upgrade: h2c' indicates protocol switch is to HTTP/2.
      // See: https://http2.github.io/http2-spec/#discover-http
      if (upgrade_iter->second == "h2c") {
        LOG(WARNING) << "HTTP upgrades to HTTP2 are not yet supported";
      }

      return ParseState::kEOS;
    }

    return ParseState::kSuccess;
  }

  // Case 3C: Response where we can't assume no body, but where no Content-Length or
  // Transfer-Encoding is provided. In these cases we should wait for close().
  // According to HTTP/1.1 standard:
  // https://www.w3.org/Protocols/HTTP/1.0/draft-ietf-http-spec.html#BodyLength
  // such messages are terminated by the close of the connection.
  // TODO(yzhao): For now we just accumulate messages, let probe_close() submit a message to
  // perf buffer, so that we can terminate such messages.
  if (!buf->empty()) {
    // Currently, we output the parsed message with a potentially partial body.
    // Only the body that is present at the time is emitted, since we don't
    // know if the data is actually complete or not without a length.

    result->body = *buf;
    buf->remove_prefix(buf->size());
    LOG_FIRST_N(WARNING, 10)
        << "HTTP message with no Content-Length or Transfer-Encoding may produce "
           "incomplete message bodies.";
    return ParseState::kSuccess;
  }

  LOG_FIRST_N(WARNING, 10) << "Could not figure out how to extract body" << std::endl;
  return ParseState::kInvalid;
}

ParseState ParseRequest(std::string_view* buf, Message* result) {
  pico_wrapper::HTTPRequest req;
  int retval = pico_wrapper::ParseRequest(*buf, &req);

  if (retval >= 0) {
    buf->remove_prefix(retval);

    result->type = message_type_t::kRequest;
    result->minor_version = req.minor_version;
    result->headers = pico_wrapper::GetHTTPHeadersMap(req.headers, req.num_headers);
    result->req_method = std::string(req.method, req.method_len);
    result->req_path = std::string(req.path, req.path_len);
    result->headers_byte_size = retval;

    return ParseBody(buf, result);
  }
  if (retval == -2) {
    return ParseState::kNeedsMoreData;
  }
  return ParseState::kInvalid;
}

ParseState ParseResponse(std::string_view* buf, Message* result) {
  pico_wrapper::HTTPResponse resp;
  int retval = pico_wrapper::ParseResponse(*buf, &resp);

  if (retval >= 0) {
    buf->remove_prefix(retval);

    result->type = message_type_t::kResponse;
    result->minor_version = resp.minor_version;
    result->headers = pico_wrapper::GetHTTPHeadersMap(resp.headers, resp.num_headers);
    result->resp_status = resp.status;
    result->resp_message = std::string(resp.msg, resp.msg_len);
    result->headers_byte_size = retval;

    return ParseBody(buf, result);
  }
  if (retval == -2) {
    return ParseState::kNeedsMoreData;
  }
  return ParseState::kInvalid;
}

/**
 * Parses a raw input buffer for HTTP messages.
 * HTTP headers are parsed by pico. Body is extracted separately.
 *
 * @param type: request or response
 * @param buf: The source buffer to parse. The prefix of this buffer will be consumed to indicate
 * the point until which the parse has progressed.
 * @param result: A parsed HTTP message, if parse was successful (must consider return value).
 * @return parse state indicating how the parse progressed.
 */
ParseState ParseFrame(message_type_t type, std::string_view* buf, Message* result) {
  switch (type) {
    case message_type_t::kRequest:
      return ParseRequest(buf, result);
    case message_type_t::kResponse:
      return ParseResponse(buf, result);
    default:
      return ParseState::kInvalid;
  }
}

// TODO(oazizi/yzhao): This function should use is_http_{response,request} inside
// bcc_bpf/socket_trace.c to check if a sequence of bytes are aligned on HTTP message boundary.
// ATM, they actually do not share the same logic. As a result, BPF events detected as HTTP traffic,
// can actually fail to find any valid boundary by this function. Unfortunately, BPF has many
// restrictions that likely make this a difficult or impossible goal.
size_t FindFrameBoundary(message_type_t type, std::string_view buf, size_t start_pos) {
  // List of all HTTP request methods. All HTTP requests start with one of these.
  // https://developer.mozilla.org/en-US/docs/Web/HTTP/Methods
  static constexpr std::string_view kHTTPReqStartPatternArray[] = {
      "GET ", "HEAD ", "POST ", "PUT ", "DELETE ", "CONNECT ", "OPTIONS ", "TRACE ", "PATCH ",
  };

  // List of supported HTTP protocol versions. HTTP responses typically start with one of these.
  // https://developer.mozilla.org/en-US/docs/Web/HTTP/Messages
  static constexpr std::string_view kHTTPRespStartPatternArray[] = {"HTTP/1.1 ", "HTTP/1.0 "};

  static constexpr ArrayView<std::string_view> kHTTPReqStartPatterns =
      ArrayView<std::string_view>(kHTTPReqStartPatternArray);
  static constexpr ArrayView<std::string_view> kHTTPRespStartPatterns =
      ArrayView<std::string_view>(kHTTPRespStartPatternArray);

  static constexpr std::string_view kBoundaryMarker = "\r\n\r\n";

  // Choose the right set of patterns for request vs response.
  const ArrayView<std::string_view>* start_patterns = nullptr;
  switch (type) {
    case message_type_t::kRequest:
      start_patterns = &kHTTPReqStartPatterns;
      break;
    case message_type_t::kResponse:
      start_patterns = &kHTTPRespStartPatterns;
      break;
    case message_type_t::kUnknown:
      return std::string::npos;
  }

  // Search for a boundary marker, preceded with a message start.
  // Example, using HTTP Response:
  //   leftover body (from previous message)
  //   HTTP/1.1 ...
  //   headers
  //   \r\n\r\n
  //   body
  // We first search forwards for \r\n\r\n, then we search backwards from there for HTTP/1.1.
  //
  // Note that we don't search forwards for HTTP/1.1 directly, because it could result in matches
  // inside the request/response body.
  while (true) {
    size_t marker_pos = buf.find(kBoundaryMarker, start_pos);

    if (marker_pos == std::string::npos) {
      return std::string::npos;
    }

    std::string_view buf_substr = buf.substr(start_pos, marker_pos - start_pos);

    size_t substr_pos = std::string::npos;
    for (auto& start_pattern : *start_patterns) {
      size_t current_substr_pos = buf_substr.rfind(start_pattern);
      if (current_substr_pos != std::string::npos) {
        // Found a match. Check if it is closer to the marker than our previous match.
        // We want to return the match that is closest to the marker, so we aren't
        // matching to something in a previous message's body.
        substr_pos = (substr_pos == std::string::npos) ? current_substr_pos
                                                       : std::max(substr_pos, current_substr_pos);
      }
    }

    if (substr_pos != std::string::npos) {
      return start_pos + substr_pos;
    }

    // Couldn't find a start position. Move to the marker, and search for another marker.
    start_pos = marker_pos + kBoundaryMarker.size();
  }
}

}  // namespace http

template <>
ParseState ParseFrame(message_type_t type, std::string_view* buf, http::Message* result,
                      NoState* /*state*/) {
  return http::ParseFrame(type, buf, result);
}

template <>
size_t FindFrameBoundary<http::Message>(message_type_t type, std::string_view buf, size_t start_pos,
                                        NoState* /*state*/) {
  return http::FindFrameBoundary(type, buf, start_pos);
}

}  // namespace protocols
}  // namespace stirling
}  // namespace px
