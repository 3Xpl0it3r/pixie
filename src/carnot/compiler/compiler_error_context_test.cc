#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include "absl/strings/str_format.h"
#include "src/carnot/compiler/compiler_error_context.h"
#include "src/carnot/compiler/compilerpb/compiler_status.pb.h"
#include "src/common/base/proto/status.pb.h"
#include "src/common/base/status.h"

namespace pl {
namespace carnot {
namespace compiler {

TEST(CompilerErrorContextStatus, Default) {
  int64_t line = 10;
  int64_t column = 12;
  int64_t num_errors = 4;
  std::string message = "There's an error here.";
  compilerpb::CompilerErrorGroup errorgroup_in, errorgroup_out;
  for (int64_t i = 0; i < num_errors; i++) {
    compilerpb::CompilerError* error_parent = errorgroup_in.add_errors();
    compilerpb::LineColError* error = error_parent->mutable_line_col_error();

    error->set_line(line + i);
    error->set_column(column + i);
    error->set_message(absl::StrFormat("msg: %s, idx: %d", message, i));
  }
  Status status(pl::statuspb::INVALID_ARGUMENT, "Issue",
                std::make_unique<compilerpb::CompilerErrorGroup>(errorgroup_in));

  pl::statuspb::Status status_pb = status.ToProto();
  ASSERT_TRUE(status_pb.context().Is<compilerpb::CompilerErrorGroup>());

  status_pb.context().UnpackTo(&errorgroup_out);
  EXPECT_EQ(errorgroup_in.DebugString(), errorgroup_out.DebugString());
  for (int64_t i = 0; i < errorgroup_in.errors_size(); i++) {
    auto error_parent_out = errorgroup_in.errors(i);
    auto error_out = error_parent_out.line_col_error();
    EXPECT_EQ(error_out.line(), line + i);
    EXPECT_EQ(error_out.column(), column + i);
    EXPECT_EQ(error_out.message(), absl::StrFormat("msg: %s, idx: %d", message, i));
  }
}

TEST(CompilerErrorBuilder, LineColErrorPb) {
  std::string error_msg1 = "Error ova here.";
  compilerpb::CompilerErrorGroup error1 = LineColErrorPb(1, 2, error_msg1);

  // Parallel construction to make sure child content is created properly.
  compilerpb::LineColError line_col_error;
  line_col_error.set_line(1);
  line_col_error.set_column(2);
  line_col_error.set_message(error_msg1);

  ASSERT_EQ(error1.errors_size(), 1);
  EXPECT_TRUE(error1.errors(0).has_line_col_error());
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(error1.errors(0).line_col_error(),
                                                                 line_col_error));
}

TEST(CompilerErrorBuilder, MergedGroups) {
  compilerpb::CompilerErrorGroup error1 = LineColErrorPb(1, 2, "Error ova here.");
  compilerpb::CompilerErrorGroup error2 = LineColErrorPb(20, 19, "Error ova there.");
  compilerpb::CompilerErrorGroup error3 = LineColErrorPb(20, 4, "Error right here.");
  std::vector<compilerpb::CompilerErrorGroup> all_errors = {error1, error2, error3};

  compilerpb::CompilerErrorGroup merged_errors = MergeGroups(all_errors);
  ASSERT_EQ(merged_errors.errors_size(), 3);
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
      merged_errors.errors(0).line_col_error(), error1.errors(0).line_col_error()));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
      merged_errors.errors(1).line_col_error(), error2.errors(0).line_col_error()));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
      merged_errors.errors(2).line_col_error(), error3.errors(0).line_col_error()));
}

TEST(CompilerErrorBuilder, MergedStatuses) {
  compilerpb::CompilerErrorGroup error1 = LineColErrorPb(1, 2, "Error ova here.");
  compilerpb::CompilerErrorGroup error2 = LineColErrorPb(20, 19, "Error ova there.");
  compilerpb::CompilerErrorGroup error3 = LineColErrorPb(20, 4, "Error right here.");
  std::vector<compilerpb::CompilerErrorGroup> all_errors = {error1, error2, error3};

  compilerpb::CompilerErrorGroup merged_errors = MergeGroups(all_errors);

  Status s1(statuspb::INVALID_ARGUMENT, "ContextError",
            std::make_unique<compilerpb::CompilerErrorGroup>(error1));
  Status s2(s1.code(), s1.msg(), std::make_unique<compilerpb::CompilerErrorGroup>(error2));
  Status s3(s1.code(), s1.msg(), std::make_unique<compilerpb::CompilerErrorGroup>(error3));
  Status merged_statuses = MergeStatuses({s1, s2, s3});
  EXPECT_EQ(Status(s1.code(), absl::StrJoin({s1.msg(), s2.msg(), s3.msg()}, "\n"),
                   std::make_unique<compilerpb::CompilerErrorGroup>(merged_errors)),
            merged_statuses);
}
}  // namespace compiler
}  // namespace carnot
}  // namespace pl
