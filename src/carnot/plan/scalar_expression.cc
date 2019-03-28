#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "src/carnot/plan/plan_state.h"
#include "src/carnot/plan/scalar_expression.h"
#include "src/carnot/plan/utils.h"
#include "src/carnot/udf/registry.h"
#include "src/carnot/udf/udf.h"
#include "src/common/base/base.h"

namespace pl {
namespace carnot {
namespace plan {

pl::Status ScalarValue::Init(const pl::carnot::carnotpb::ScalarValue &pb) {
  DCHECK(!is_initialized_) << "Already initialized";
  CHECK(pb.data_type() != types::DATA_TYPE_UNKNOWN);
  CHECK(types::DataType_IsValid(pb.data_type()));
  // TODO(zasgar): We should probably add a check to make sure that when a given
  // DataType is set, the wrong field value is not set.

  // Copy the proto.
  pb_ = pb;
  is_initialized_ = true;
  return Status::OK();
}

// PL_CARNOT_UPDATE_FOR_NEW_TYPES

int64_t ScalarValue::Int64Value() const {
  DCHECK(is_initialized_) << "Not initialized";
  VLOG_IF(1, pb_.value_case() != carnotpb::ScalarValue::kInt64Value)

      << "Calling accessor on null/invalid value";
  return pb_.int64_value();
}

double ScalarValue::Float64Value() const {
  DCHECK(is_initialized_) << "Not initialized";
  VLOG_IF(1, pb_.value_case() != carnotpb::ScalarValue::kFloat64Value)
      << "Calling accessor on null/invalid value";
  return pb_.float64_value();
}

std::string ScalarValue::StringValue() const {
  DCHECK(is_initialized_) << "Not initialized";
  VLOG_IF(1, pb_.value_case() != carnotpb::ScalarValue::kStringValue)
      << "Calling accessor on null/invalid value";
  return pb_.string_value();
}

bool ScalarValue::BoolValue() const {
  DCHECK(is_initialized_) << "Not initialized";
  VLOG_IF(1, pb_.value_case() != carnotpb::ScalarValue::kBoolValue)
      << "Calling accessor on null/invalid value";
  return pb_.bool_value();
}

int64_t ScalarValue::Time64NSValue() const {
  DCHECK(is_initialized_) << "Not initialized";
  VLOG_IF(1, pb_.value_case() != carnotpb::ScalarValue::kTime64NsValue)

      << "Calling accessor on null/invalid value";
  return pb_.time64_ns_value();
}

bool ScalarValue::IsNull() const {
  DCHECK(is_initialized_) << "Not initialized";
  return pb_.value_case() == carnotpb::ScalarValue::VALUE_NOT_SET;
}

// PL_CARNOT_UPDATE_FOR_NEW_TYPES
std::string ScalarValue::DebugString() const {
  DCHECK(is_initialized_) << "Not initialized";
  if (IsNull()) {
    return "<null>";
  }
  switch (DataType()) {
    case types::BOOLEAN:
      return BoolValue() ? "true" : "false";
    case types::INT64:
      return absl::StrFormat("%d", Int64Value());
    case types::FLOAT64:
      return absl::StrFormat("%ff", Float64Value());
    case types::STRING:
      return absl::StrFormat("\"%s\"", StringValue());
    case types::TIME64NS:
      return absl::StrFormat("%d", Time64NSValue());
    default:
      return "<Unknown>";
  }
}

StatusOr<types::DataType> ScalarValue::OutputDataType(const PlanState &,
                                                      const table_store::schema::Schema &) const {
  DCHECK(is_initialized_) << "Not initialized";
  return DataType();
}

std::vector<const Column *> ScalarValue::ColumnDeps() {
  DCHECK(is_initialized_) << "Not initialized";
  return {};
}

std::vector<ScalarExpression *> ScalarValue::Deps() const {
  DCHECK(is_initialized_) << "Not initialized";
  return {};
}

Expression ScalarValue::ExpressionType() const { return Expression::kConstant; }

Status Column::Init(const carnotpb::Column &pb) {
  DCHECK(!is_initialized_) << "Already initialized";
  pb_ = pb;
  is_initialized_ = true;
  return Status::OK();
}
int64_t Column::Index() const {
  DCHECK(is_initialized_) << "Not initialized";
  return pb_.index();
}
int64_t Column::NodeID() const {
  DCHECK(is_initialized_) << "Not initialized";
  return pb_.node();
}
std::string Column::DebugString() const {
  DCHECK(is_initialized_) << "Not initialized";
  return absl::StrFormat("node<%d>::col[%d]", NodeID(), Index());
}

StatusOr<types::DataType> Column::OutputDataType(
    const PlanState &, const table_store::schema::Schema &input_schema) const {
  DCHECK(is_initialized_) << "Not initialized";
  StatusOr<const table_store::schema::Relation> s = input_schema.GetRelation(NodeID());

  PL_RETURN_IF_ERROR(s);
  const auto &relation = s.ValueOrDie();
  types::DataType dt = relation.GetColumnType(Index());
  return dt;
}

std::vector<const Column *> Column::ColumnDeps() {
  DCHECK(is_initialized_) << "Not initialized";
  return {this};
}

Expression Column::ExpressionType() const { return Expression::kColumn; }

std::vector<ScalarExpression *> Column::Deps() const {
  DCHECK(is_initialized_) << "Not initialized";
  return {};
}

template <typename T, typename TProto>
StatusOr<std::unique_ptr<ScalarExpression>> MakeExprHelper(const TProto &pb) {
  auto expr = std::make_unique<T>();
  auto s = expr->Init(pb);
  PL_RETURN_IF_ERROR(s);
  return std::unique_ptr<ScalarExpression>(std::move(expr));
}

StatusOr<std::unique_ptr<ScalarExpression>> ScalarExpression::FromProto(
    const carnotpb::ScalarExpression &pb) {
  switch (pb.value_case()) {
    case carnotpb::ScalarExpression::kColumn:
      return MakeExprHelper<Column>(pb.column());
    case carnotpb::ScalarExpression::kConstant:
      return MakeExprHelper<ScalarValue>(pb.constant());
    case carnotpb::ScalarExpression::kFunc:
      return MakeExprHelper<ScalarFunc>(pb.func());
    default:
      return error::Unimplemented("Expression type: %d", pb.value_case());
  }
}

Status ScalarFunc::Init(const carnotpb::ScalarFunc &pb) {
  name_ = pb.name();
  udf_id_ = pb.id();
  for (const auto arg : pb.args()) {
    auto s = ScalarExpression::FromProto(arg);
    if (!s.ok()) {
      return s.status();
    }
    arg_deps_.emplace_back(s.ConsumeValueOrDie());
  }
  return Status::OK();
}

std::vector<ScalarExpression *> ScalarFunc::Deps() const {
  std::vector<ScalarExpression *> deps;
  for (const auto &arg : arg_deps_) {
    // No ownership transfer.
    deps.emplace_back(arg.get());
  }
  return deps;
}

Expression ScalarFunc::ExpressionType() const { return Expression::kFunc; }

std::vector<const Column *> ScalarFunc::ColumnDeps() {
  std::vector<const Column *> cols;
  ExpressionWalker<int>()
      .OnColumn([&](const auto &col, const auto &) {
        cols.push_back(&col);
        return 0;
      })
      .Walk(*this);
  return cols;
}

StatusOr<types::DataType> ScalarFunc::OutputDataType(
    const PlanState &state, const table_store::schema::Schema &input_schema) const {
  // The output data type of a function is based on the computed types of the children
  // followed by the looking up the function in the registry and getting the output
  // data type of the function.
  auto res = ExpressionWalker<StatusOr<types::DataType>>()
                 .OnScalarValue([&](auto &val, auto &) -> StatusOr<types::DataType> {
                   return val.OutputDataType(state, input_schema);
                 })
                 .OnColumn([&](auto &col, auto &) -> StatusOr<types::DataType> {
                   return col.OutputDataType(state, input_schema);
                 })
                 .OnScalarFunc([&](auto &func, auto &child_results) -> StatusOr<types::DataType> {
                   std::vector<types::DataType> child_args;
                   child_args.reserve(child_results.size());
                   for (const auto &child_result : child_results) {
                     PL_RETURN_IF_ERROR(child_result);
                     child_args.push_back(child_result.ValueOrDie());
                   }
                   auto s = state.udf_registry()->GetDefinition(func.name(), child_args);
                   PL_RETURN_IF_ERROR(s);
                   return s.ValueOrDie()->exec_return_type();
                 })
                 .Walk(*this);

  // TODO(zasgar): Why is this necessary? For some reason the proper constructor is
  // not getting invoked.
  PL_RETURN_IF_ERROR(res);
  return res.ValueOrDie();
}

std::string ScalarFunc::DebugString() const {
  std::string debug_string;
  std::vector<std::string> arg_strings;
  for (const auto &arg : arg_deps_) {
    arg_strings.push_back(arg->DebugString());
  }
  debug_string += absl::StrFormat("fn:%s(%s)", name_, absl::StrJoin(arg_strings, ","));
  return debug_string;
}

Status AggregateExpression::Init(const carnotpb::AggregateExpression &pb) {
  name_ = pb.name();
  uda_id_ = pb.id();
  for (const auto arg : pb.args()) {
    // arg is of message type AggregateExpression.Arg. Needs to be casted to a ScalarExpression.
    carnotpb::ScalarExpression se;

    se.ParseFromString(arg.SerializeAsString());
    auto s = ScalarExpression::FromProto(se);
    if (!s.ok()) {
      return s.status();
    }
    arg_deps_.emplace_back(s.ConsumeValueOrDie());
  }
  return Status::OK();
}

Expression AggregateExpression::ExpressionType() const { return Expression::kAgg; }

std::vector<ScalarExpression *> AggregateExpression::Deps() const {
  std::vector<ScalarExpression *> deps;
  for (const auto &arg : arg_deps_) {
    // No ownership transfer.
    deps.emplace_back(arg.get());
  }
  return deps;
}

std::vector<const Column *> AggregateExpression::ColumnDeps() {
  std::vector<const Column *> cols;
  for (const auto &arg : arg_deps_) {
    auto dep = arg.get();
    if (dep->ExpressionType() == Expression::kColumn) {
      const auto *col = static_cast<const Column *>(dep);
      cols.push_back(col);
    }
  }
  return cols;
}

StatusOr<types::DataType> AggregateExpression::OutputDataType(
    const PlanState &state, const table_store::schema::Schema &input_schema) const {
  // The output data type of a function is based on the computed types of the args
  // followed by the looking up the function in the registry and getting the output
  // data type of the function.
  std::vector<types::DataType> child_args;
  child_args.reserve(arg_deps_.size());
  for (const auto &arg : arg_deps_) {
    child_args.push_back(arg->OutputDataType(state, input_schema).ValueOrDie());
  }
  PL_ASSIGN_OR_RETURN(auto s, state.uda_registry()->GetDefinition(name_, child_args));
  return s->finalize_return_type();
}

std::string AggregateExpression::DebugString() const {
  std::string debug_string;
  std::vector<std::string> arg_strings;
  for (const auto &arg : arg_deps_) {
    arg_strings.push_back(arg->DebugString());
  }
  debug_string +=
      absl::StrFormat("aggregate expression:%s(%s)", name_, absl::StrJoin(arg_strings, ","));
  return debug_string;
}

}  // namespace plan
}  // namespace carnot
}  // namespace pl
