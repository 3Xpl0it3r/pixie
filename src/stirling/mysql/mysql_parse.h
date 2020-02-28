#pragma once

#include <deque>
#include <string>
#include <vector>
#include "src/stirling/common/event_parser.h"
#include "src/stirling/mysql/types.h"

namespace pl {
namespace stirling {

/**
 * Parses a single MySQL packet from the input string.
 */
template <>
ParseState ParseFrame(MessageType type, std::string_view* buf, mysql::Packet* frame);

template <>
size_t FindFrameBoundary<mysql::Packet>(MessageType type, std::string_view buf, size_t start_pos);

}  // namespace stirling
}  // namespace pl
