#pragma once

#include <regex>
#include <string>
#include <utility>

#include <absl/strings/str_format.h>
#include "src/common/base/error.h"
#include "src/common/base/statusor.h"

namespace pl {

inline StatusOr<std::pair<int64_t, int64_t>> StringToTimeRange(const std::string& str_time) {
  std::regex rgx("([0-9]+),([0-9]+)");
  std::smatch matches;
  if (std::regex_search(str_time, matches, rgx) && matches.size() == 3) {
    return std::make_pair(static_cast<int64_t>(std::stoi(matches[1])),
                          static_cast<int64_t>(std::stoi(matches[2])));
  }
  return error::InvalidArgument("String provided for Range is in incorrect format.");
}

inline StatusOr<int64_t> StringToTimeInt(const std::string& str_time) {
  std::regex rgx("([-]?[0-9]+)(ms|m|s|h|d)");
  std::smatch matches;
  bool matched = std::regex_search(str_time, matches, rgx);
  if (!matched) {
    return error::InvalidArgument("Time string is in wrong format.");
  }
  if (matches.size() != 3) {
    return error::InvalidArgument("Time string is in wrong format.");
  }
  auto amount = std::stoi(matches[1]);
  auto unit = matches[2];

  if (unit == "h") {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::hours(amount)).count();
  }
  if (unit == "m") {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::minutes(amount))
        .count();
  }
  if (unit == "ms") {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(amount))
        .count();
  }
  if (unit == "s") {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(amount))
        .count();
  }
  if (unit == "d") {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::hours(amount * 24))
        .count();
  }
  return error::InvalidArgument("Unsupported unit.");
}

/**
 * Print a duration with a suffix of us, ms, s.
 *
 * This makes times smaller than 0.5ms print as us. Then print with ms till 1 second.
 * @param duration_ns
 * @return
 */
inline std::string PrettyDuration(double duration_ns) {
  if (duration_ns < 500000) {
    return absl::StrFormat("%.2f \u03BCs", duration_ns / 1E3);
  }
  if (duration_ns < 1E9) {
    return absl::StrFormat("%.2f ms", duration_ns / 1E6);
  }
  return absl::StrFormat("%.2f s", duration_ns / 1E9);
}

/**
 * Helper to get the current time in nanoseconds.
 * @return the current time in ns.
 */
inline int64_t CurrentTimeNS() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace pl
