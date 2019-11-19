#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "src/carnot/exec/grpc_sink_node.h"
#include "src/carnot/planpb/plan.pb.h"
#include "src/common/uuid/uuid_utils.h"
#include "src/table_store/table_store.h"

namespace pl {
namespace carnot {
namespace exec {

using table_store::schema::RowBatch;
using table_store::schema::RowDescriptor;

std::string GRPCSinkNode::DebugStringImpl() {
  return absl::Substitute("Exec::GRPCSinkNode: {address: $0, destination_id: $1, output: $2}",
                          plan_node_->address(), plan_node_->destination_id(),
                          input_descriptor_->DebugString());
}

Status GRPCSinkNode::InitImpl(const plan::Operator& plan_node, const RowDescriptor&,
                              const std::vector<RowDescriptor>& input_descriptors) {
  CHECK(plan_node.op_type() == planpb::OperatorType::GRPC_SINK_OPERATOR);
  if (input_descriptors.size() != 1) {
    return error::InvalidArgument("GRPCSink operator expects a single input relation, got $0",
                                  input_descriptors.size());
  }
  input_descriptor_ = std::make_unique<RowDescriptor>(input_descriptors[0]);
  const auto* sink_plan_node = static_cast<const plan::GRPCSinkOperator*>(&plan_node);
  plan_node_ = std::make_unique<plan::GRPCSinkOperator>(*sink_plan_node);
  return Status::OK();
}

Status GRPCSinkNode::PrepareImpl(ExecState*) { return Status::OK(); }

Status GRPCSinkNode::OpenImpl(ExecState* exec_state) {
  stub_ = exec_state->KelvinServiceStub(plan_node_->address());
  return Status::OK();
}

Status GRPCSinkNode::CloseWriter() {
  if (writer_ == nullptr) {
    return Status::OK();
  }
  writer_->WritesDone();
  auto s = writer_->Finish();
  if (!s.ok()) {
    return error::Internal(absl::Substitute(
        "GRPCSink node: Error calling Finish on stream, message: $0", s.error_message()));
  }
  return Status::OK();
}

Status GRPCSinkNode::CloseImpl(ExecState*) {
  if (sent_eos_) {
    return Status::OK();
  }

  if (writer_ != nullptr) {
    PL_RETURN_IF_ERROR(CloseWriter());
    return error::Internal("Closing GRPCSinkNode without receiving EOS.");
  }

  return Status::OK();
}

Status GRPCSinkNode::ConsumeNextImpl(ExecState* exec_state, const RowBatch& rb, size_t) {
  if (writer_ == nullptr) {
    writer_ = stub_->TransferRowBatch(&context_, &response_);
  }

  carnotpb::RowBatchRequest req;
  // Set the metadata for the RowBatch (where it should go).
  req.set_address(plan_node_->address());
  req.set_destination_id(plan_node_->destination_id());
  ToProto(exec_state->query_id(), req.mutable_query_id());
  // Serialize the RowBatch.
  PL_RETURN_IF_ERROR(rb.ToProto(req.mutable_row_batch()));

  writer_->Write(req);
  if (!rb.eos()) {
    return Status::OK();
  }

  sent_eos_ = true;
  PL_RETURN_IF_ERROR(CloseWriter());

  return response_.success()
             ? Status::OK()
             : error::Internal(absl::Substitute(
                   "GRPCSinkNode: error sending stream to address $0, error message: $1",
                   plan_node_->address(), response_.message()));
}

}  // namespace exec
}  // namespace carnot
}  // namespace pl
