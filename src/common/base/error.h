#pragma once

#include <string>

#include <absl/strings/substitute.h>
#include <magic_enum.hpp>

#include "src/common/base/proto/status.pb.h"
#include "src/common/base/status.h"

namespace pl {
namespace error {

// Declare convenience functions:
// error::InvalidArgument(...)
// error::IsInvalidArgument(stat)
#define DECLARE_ERROR(FUNC, CONST)                                           \
  template <typename... Args>                                                \
  Status FUNC(std::string_view format, Args... args) {                       \
    return Status(::pl::statuspb::CONST, absl::Substitute(format, args...)); \
  }                                                                          \
  inline bool Is##FUNC(const Status& status) { return status.code() == ::pl::statuspb::CONST; }

DECLARE_ERROR(Cancelled, CANCELLED)
DECLARE_ERROR(Unknown, UNKNOWN)
DECLARE_ERROR(InvalidArgument, INVALID_ARGUMENT)
DECLARE_ERROR(DeadlineExceeded, DEADLINE_EXCEEDED)
DECLARE_ERROR(NotFound, NOT_FOUND)
DECLARE_ERROR(AlreadyExists, ALREADY_EXISTS)
DECLARE_ERROR(PermissionDenied, PERMISSION_DENIED)
DECLARE_ERROR(Unauthenticated, UNAUTHENTICATED)
DECLARE_ERROR(Internal, INTERNAL)
DECLARE_ERROR(Unimplemented, UNIMPLEMENTED)
DECLARE_ERROR(ResourceUnavailable, RESOURCE_UNAVAILABLE)
DECLARE_ERROR(System, SYSTEM)

#undef DECLARE_ERROR

inline std::string CodeToString(pl::statuspb::Code code) {
  std::string_view code_str_view = magic_enum::enum_name(code);
  if (code_str_view.empty()) {
    return "Unknown error_code";
  }

  std::string code_str(code_str_view);
  // Example transformation: INVALID_ARGUMENT -> Invalid Argument
  int last = ' ';
  std::for_each(code_str.begin(), code_str.end(), [&last](char& c) {
    if (c == '_') {
      c = ' ';
    } else {
      c = (last == ' ') ? std::toupper(c) : std::tolower(c);
    }
    last = c;
  });

  return code_str;
}

}  // namespace error
}  // namespace pl
