#pragma once
#include "src/common/base/macros.h"  // NOLINT(build/include_order).
// This header file should be used for all logging.

// Include gflags before glog so flags get set correctly.
PL_SUPPRESS_WARNINGS_START()
#include <gflags/gflags.h>     // NOLINT(build/include_order).
#include <glog/logging.h>      // NOLINT(build/include_order).
#include <glog/stl_logging.h>  // NOLINT(build/include_order).
PL_SUPPRESS_WARNINGS_END()

#include <string>

// A new form of CHECK is defined below, to extend those in the glog implementation.
// The following table shows the behavioral differences.
//
// MODE               DEBUG           PRODUCTION
// -------------------------------------------------------
// DCHECK             FATAL           nothing
// CHECK              FATAL           FATAL
// ECHECK             FATAL           ERROR
//
// It is essentially a short-hand for LOG(DFATAL).

// NOTE: Macros copied from glog/logging.h, and simplified.
//       They may not be as optimal as those in glog.
// TODO(oazizi): Consider optimizations (compare to glog).

#define ECHECK(condition) \
  LOG_IF(DFATAL, GOOGLE_PREDICT_BRANCH_NOT_TAKEN(!(condition))) << "Check failed: " #condition " "

#define ECHECK_OK(status) \
  LOG_IF(DFATAL, GOOGLE_PREDICT_BRANCH_NOT_TAKEN(!(status.ok()))) << "Check failed: " #status " "

// In optimized mode, CheckOpString provides to hint to compiler that
// the if statement is unlikely.
#define ECHECK_OP_LOG(name, op, val1, val2)                                                        \
  if (google::CheckOpString _result =                                                              \
          google::Check##name##Impl(google::GetReferenceableValue(val1),                           \
                                    google::GetReferenceableValue(val2), #val1 " " #op " " #val2)) \
  LOG(DFATAL) << "Check failed: " << (*_result.str_) << " "

#define ECHECK_OP(name, op, val1, val2) ECHECK_OP_LOG(name, op, val1, val2)

#define ECHECK_EQ(val1, val2) ECHECK_OP(_EQ, ==, val1, val2)
#define ECHECK_NE(val1, val2) ECHECK_OP(_NE, !=, val1, val2)
#define ECHECK_LE(val1, val2) ECHECK_OP(_LE, <=, val1, val2)
#define ECHECK_LT(val1, val2) ECHECK_OP(_LT, <, val1, val2)
#define ECHECK_GE(val1, val2) ECHECK_OP(_GE, >=, val1, val2)
#define ECHECK_GT(val1, val2) ECHECK_OP(_GT, >, val1, val2)

namespace pl {

// Indent provides a consitent indent base on level.
inline std::string Indent(int level) { return std::string(level, '\t'); }

}  // namespace pl
