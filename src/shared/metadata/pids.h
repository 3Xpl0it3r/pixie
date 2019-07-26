#pragma once

#include <memory>
#include <string>
#include <utility>

#include "src/common/base/base.h"
#include "src/shared/metadata/base_types.h"

namespace pl {
namespace md {

/**
 * Store information about PIDs.
 */
class PIDInfo {
 public:
  PIDInfo() = delete;
  PIDInfo(UPID upid, std::string_view cmdline, CID container_id)
      : upid_(upid), cmdline_(cmdline), container_id_(std::move(container_id)), stop_time_ns_(0) {}

  UPID upid() const { return upid_; }

  int64_t start_time_ns() const { return upid_.start_ts(); }

  int64_t stop_time_ns() const { return stop_time_ns_; }

  void set_stop_time_ns(int64_t ts) { stop_time_ns_ = ts; }

  const std::string& cmdline() const { return cmdline_; }

  // FIXME: change container_id to cid;
  CID cid() const { return container_id_; }

  std::unique_ptr<PIDInfo> Clone() {
    auto pid_info = std::make_unique<PIDInfo>(*this);
    return pid_info;
  }

  bool operator==(const PIDInfo& other) const {
    return (other.upid_ == upid_) && (other.cmdline_ == cmdline_) &&
           (other.container_id_ == container_id_) && (other.stop_time_ns_ == stop_time_ns_);
  }

  bool operator!=(const PIDInfo& other) const { return !(other == *this); }

  std::string DebugString() const;

 private:
  UPID upid_;

  /**
   * The command line used to start this PID.
   */
  std::string cmdline_;

  /**
   * The container running this PID.
   */
  CID container_id_;

  /**
   * The time that this PID stopped running. If 0 we can assume it's still running.
   */
  int64_t stop_time_ns_ = 0;
};

/*
 * Description of events used to transmit information about
 * PID creation and deletion.
 */

/**
 * The PID status event type.
 */
enum class PIDStatusEventType : uint8_t { kUnknown = 0, kStarted, kTerminated };

/**
 * Base class for PID status event.
 */
struct PIDStatusEvent {
  PIDStatusEvent() = delete;
  virtual ~PIDStatusEvent() = default;

  explicit PIDStatusEvent(PIDStatusEventType type) : type(type) {}
  PIDStatusEventType type = PIDStatusEventType::kUnknown;

  virtual std::string DebugString() const = 0;
};

/**
 * PIDStartEvent has information about new PIDs.
 * It contains a copy of the newly created PID Info.
 */
struct PIDStartedEvent : public PIDStatusEvent {
  explicit PIDStartedEvent(const PIDInfo& other)
      : PIDStatusEvent(PIDStatusEventType::kStarted), pid_info(other) {}

  std::string DebugString() const override;

  const PIDInfo pid_info;
};

/**
 * PIDTerminatedEvent has information about deleted PIDs.
 * It only contains the unique PID that was deleted and the termination time stamp.
 */
struct PIDTerminatedEvent : public PIDStatusEvent {
  explicit PIDTerminatedEvent(UPID stopped_pid, int64_t _stop_time_ns)
      : PIDStatusEvent(PIDStatusEventType::kTerminated),
        upid(stopped_pid),
        stop_time_ns(_stop_time_ns) {}

  std::string DebugString() const override;

  const UPID upid;
  const int64_t stop_time_ns;
};

/**
 * Print and compare functions.
 */
std::ostream& operator<<(std::ostream& os, const PIDInfo& info);
std::ostream& operator<<(std::ostream& os, const PIDStatusEvent& ev);

bool operator==(const PIDStartedEvent& lhs, const PIDStartedEvent& rhs);
bool operator==(const PIDTerminatedEvent& lhs, const PIDTerminatedEvent& rhs);

}  // namespace md
}  // namespace pl
