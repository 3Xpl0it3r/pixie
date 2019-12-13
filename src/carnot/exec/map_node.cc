#include <string>
#include <utility>
#include <vector>

#include <absl/strings/str_join.h>
#include <absl/strings/substitute.h>
#include "src/carnot/exec/map_node.h"
#include "src/carnot/planpb/plan.pb.h"

namespace pl {
namespace carnot {
namespace exec {

using table_store::schema::RowBatch;
using table_store::schema::RowDescriptor;

std::string MapNode::DebugStringImpl() {
  return absl::Substitute("Exec::MapNode<$0>", evaluator_->DebugString());
}

Status MapNode::InitImpl(const plan::Operator& plan_node, const RowDescriptor& output_descriptor,
                         const std::vector<RowDescriptor>& input_descriptors) {
  CHECK(plan_node.op_type() == planpb::OperatorType::MAP_OPERATOR);
  const auto* map_plan_node = static_cast<const plan::MapOperator*>(&plan_node);
  // copy the plan node to local object;
  plan_node_ = std::make_unique<plan::MapOperator>(*map_plan_node);
  output_descriptor_ = std::make_unique<RowDescriptor>(output_descriptor);
  PL_UNUSED(input_descriptors);
  return Status::OK();
}
Status MapNode::PrepareImpl(ExecState* exec_state) {
  function_ctx_ = exec_state->CreateFunctionContext();
  evaluator_ = ScalarExpressionEvaluator::Create(
      plan_node_->expressions(), ScalarExpressionEvaluatorType::kArrowNative, function_ctx_.get());
  return Status::OK();
}

Status MapNode::OpenImpl(ExecState* exec_state) {
  PL_RETURN_IF_ERROR(evaluator_->Open(exec_state));
  return Status::OK();
}

Status MapNode::CloseImpl(ExecState* exec_state) {
  PL_RETURN_IF_ERROR(evaluator_->Close(exec_state));
  return Status::OK();
}
Status MapNode::ConsumeNextImpl(ExecState* exec_state, const RowBatch& rb, size_t) {
  RowBatch output_rb(*output_descriptor_, rb.num_rows());
  PL_RETURN_IF_ERROR(evaluator_->Evaluate(exec_state, rb, &output_rb));
  output_rb.set_eow(rb.eow());
  output_rb.set_eos(rb.eos());
  PL_RETURN_IF_ERROR(SendRowBatchToChildren(exec_state, output_rb));
  return Status::OK();
}

}  // namespace exec
}  // namespace carnot
}  // namespace pl
