#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "src/common/event/timer.h"

namespace pl {
namespace event {

// Alias to make it easier to reference.
using SystemTimePoint = std::chrono::time_point<std::chrono::system_clock>;
using MonotonicTimePoint = std::chrono::time_point<std::chrono::steady_clock>;

// Scheduler needs to be forward declared to prevent a circular dependency.
class Scheduler;
using SchedulerUPtr = std::unique_ptr<Scheduler>;

/**
 * Captures a system-time source, capable of computing both monotonically increasing
 * and real time.
 */
class TimeSource {
 public:
  virtual ~TimeSource() = default;

  /**
   * @return the current system time; not guaranteed to be monotonically increasing.
   */
  virtual SystemTimePoint SystemTime() const = 0;

  /**
   * @return the current monotonic time.
   */
  virtual MonotonicTimePoint MonotonicTime() const = 0;
};

class TimeSystem : public TimeSource {
 public:
  ~TimeSystem() override = default;

  using Duration = MonotonicTimePoint::duration;

  /**
   * Creates a timer factory. This indirection enables thread-local timer-queue management,
   * so servers can have a separate timer-factory in each thread.
   */
  virtual SchedulerUPtr CreateScheduler(Scheduler* base_scheduler) = 0;
};

}  // namespace event
}  // namespace pl
