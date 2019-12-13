#include <string>
#include <utility>
#include <vector>

#include <absl/strings/str_join.h>
#include <absl/strings/substitute.h>
#include "src/carnot/exec/filter_node.h"
#include "src/carnot/planpb/plan.pb.h"
#include "src/shared/types/arrow_adapter.h"

namespace pl {
namespace carnot {
namespace exec {

using table_store::schema::RowBatch;
using table_store::schema::RowDescriptor;

std::string FilterNode::DebugStringImpl() {
  return absl::Substitute("Exec::FilterNode<$0>", evaluator_->DebugString());
}

Status FilterNode::InitImpl(const plan::Operator& plan_node, const RowDescriptor& output_descriptor,
                            const std::vector<RowDescriptor>& input_descriptors) {
  CHECK(plan_node.op_type() == planpb::OperatorType::FILTER_OPERATOR);
  const auto* filter_plan_node = static_cast<const plan::FilterOperator*>(&plan_node);
  // copy the plan node to local object;
  plan_node_ = std::make_unique<plan::FilterOperator>(*filter_plan_node);
  output_descriptor_ = std::make_unique<RowDescriptor>(output_descriptor);
  PL_UNUSED(input_descriptors);
  return Status::OK();
}
Status FilterNode::PrepareImpl(ExecState* exec_state) {
  function_ctx_ = exec_state->CreateFunctionContext();
  evaluator_ = std::make_unique<VectorNativeScalarExpressionEvaluator>(
      plan::ConstScalarExpressionVector{plan_node_->expression()}, function_ctx_.get());
  return Status::OK();
}

Status FilterNode::OpenImpl(ExecState* exec_state) {
  PL_RETURN_IF_ERROR(evaluator_->Open(exec_state));
  return Status::OK();
}

Status FilterNode::CloseImpl(ExecState* exec_state) {
  PL_RETURN_IF_ERROR(evaluator_->Close(exec_state));
  return Status::OK();
}

template <types::DataType T>
Status PredicateCopyValues(const types::BoolValueColumnWrapper& pred, const arrow::Array* input_col,
                           RowBatch* output_rb) {
  DCHECK_EQ(pred.Size(), static_cast<size_t>(input_col->length()));
  size_t num_output_records = output_rb->num_rows();
  size_t num_input_records = input_col->length();
  auto output_col_builder_generic = MakeArrowBuilder(T, arrow::default_memory_pool());
  auto* output_col_builder = static_cast<typename types::DataTypeTraits<T>::arrow_builder_type*>(
      output_col_builder_generic.get());
  PL_RETURN_IF_ERROR(output_col_builder->Reserve(num_output_records));
  for (size_t idx = 0; idx < num_input_records; ++idx) {
    if (udf::UnWrap(pred[idx])) {
      output_col_builder->UnsafeAppend(types::GetValueFromArrowArray<T>(input_col, idx));
    }
  }
  std::shared_ptr<arrow::Array> output_array;
  PL_RETURN_IF_ERROR(output_col_builder->Finish(&output_array));
  PL_RETURN_IF_ERROR(output_rb->AddColumn(output_array));
  return Status::OK();
}

template <>
Status PredicateCopyValues<types::STRING>(const types::BoolValueColumnWrapper& pred,
                                          const arrow::Array* input_col, RowBatch* output_rb) {
  DCHECK_EQ(pred.Size(), static_cast<size_t>(input_col->length()));
  size_t num_output_records = output_rb->num_rows();
  size_t num_input_records = input_col->length();
  size_t reserved =
      100;  // This can be an arbritrary number, since we do exponential doubling below.
  size_t total_size = 0;

  auto output_col_builder_generic = MakeArrowBuilder(types::STRING, arrow::default_memory_pool());
  auto* output_col_builder = static_cast<types::DataTypeTraits<types::STRING>::arrow_builder_type*>(
      output_col_builder_generic.get());

  PL_RETURN_IF_ERROR(output_col_builder->Reserve(num_output_records));
  PL_RETURN_IF_ERROR(output_col_builder->ReserveData(reserved));
  for (size_t idx = 0; idx < num_input_records; ++idx) {
    if (udf::UnWrap(pred[idx])) {
      auto res = types::GetValueFromArrowArray<types::STRING>(input_col, idx);
      total_size += res.size();
      while (total_size >= reserved) {
        reserved *= 2;
      }
      PL_RETURN_IF_ERROR(output_col_builder->ReserveData(reserved));
      output_col_builder->UnsafeAppend(std::move(res));
    }
  }
  std::shared_ptr<arrow::Array> output_array;
  PL_RETURN_IF_ERROR(output_col_builder->Finish(&output_array));
  PL_RETURN_IF_ERROR(output_rb->AddColumn(output_array));
  return Status::OK();
}

Status FilterNode::ConsumeNextImpl(ExecState* exec_state, const RowBatch& rb, size_t) {
  // TODO(zasgar/michelle): Current implementation does not merge across row batches, which is
  // something we should consider in case the filter has really low selectivity.

  PL_ASSIGN_OR_RETURN(auto pred_col, evaluator_->EvaluateSingleExpression(
                                         exec_state, rb, *plan_node_->expression()));

  // Verify that the type of the column is boolean.
  DCHECK_EQ(pred_col->data_type(), types::BOOLEAN) << "Predicate expression must be a boolean";

  const types::BoolValueColumnWrapper& pred_col_wrapper =
      *static_cast<types::BoolValueColumnWrapper*>(pred_col.get());
  size_t num_pred = pred_col_wrapper.Size();

  DCHECK_EQ(static_cast<size_t>(rb.num_rows()), num_pred);

  // Find out how many of them returned true;
  size_t num_output_records = 0;
  for (size_t i = 0; i < num_pred; ++i) {
    if (pred_col_wrapper[i].val) {
      ++num_output_records;
    }
  }

  RowBatch output_rb(*output_descriptor_, num_output_records);
  size_t num_columns = rb.num_columns();
  // Filter expects to copy all input columns to the output.
  DCHECK_EQ(output_descriptor_->size(), num_columns);

  for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
    auto input_col = rb.ColumnAt(col_idx);
    auto col_type = output_descriptor_->type(col_idx);
#define TYPE_CASE(_dt_) \
  PL_RETURN_IF_ERROR(PredicateCopyValues<_dt_>(pred_col_wrapper, input_col.get(), &output_rb));
    PL_SWITCH_FOREACH_DATATYPE(col_type, TYPE_CASE);
#undef TYPE_CASE
  }

  output_rb.set_eow(rb.eow());
  output_rb.set_eos(rb.eos());
  PL_RETURN_IF_ERROR(SendRowBatchToChildren(exec_state, output_rb));
  return Status::OK();
}

}  // namespace exec
}  // namespace carnot
}  // namespace pl
