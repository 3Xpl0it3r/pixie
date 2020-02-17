#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "src/carnot/planner/distributed/distributed_stitcher_rules.h"

namespace pl {
namespace carnot {
namespace planner {
namespace distributed {

StatusOr<bool> SetSourceGroupGRPCAddressRule::Apply(IRNode* ir_node) {
  if (Match(ir_node, GRPCSourceGroup())) {
    static_cast<GRPCSourceGroupIR*>(ir_node)->SetGRPCAddress(grpc_address_);
    return true;
  }
  return false;
}

StatusOr<bool> AssociateDistributedPlanEdgesRule::Apply(CarnotInstance* from_carnot_instance) {
  bool did_connect_any_graph = false;
  for (int64_t to_carnot_instance_id :
       from_carnot_instance->distributed_plan()->dag().DependenciesOf(from_carnot_instance->id())) {
    CarnotInstance* to_carnot_instance =
        from_carnot_instance->distributed_plan()->Get(to_carnot_instance_id);
    PL_ASSIGN_OR_RETURN(bool did_connect_this_graph,
                        ConnectGraphs(from_carnot_instance->plan(), to_carnot_instance->plan()));
    did_connect_any_graph |= did_connect_this_graph;
  }
  // Make sure we can connect to self.
  PL_ASSIGN_OR_RETURN(bool did_connect_graph_to_self,
                      ConnectGraphs(from_carnot_instance->plan(), from_carnot_instance->plan()));
  did_connect_any_graph |= did_connect_graph_to_self;
  return did_connect_any_graph;
}

StatusOr<bool> AssociateDistributedPlanEdgesRule::ConnectGraphs(IR* from_graph, IR* to_graph) {
  // In this, we find the bridge ids that overlap between the from_graph and to_graph.
  // 1. We get a mapping of bridge id to grpc sink in the from_graph
  // 2. We iterate through the GRPCSourceGroups, if any are connected to existing sinks, we add them
  // to the bridge list with the sink.
  // 3. We connect the bridges we find between the two of them.
  absl::flat_hash_map<int64_t, GRPCSinkIR*> bridge_id_to_grpc_sink;
  std::vector<std::pair<GRPCSourceGroupIR*, GRPCSinkIR*>> grpc_bridges;
  for (IRNode* ir_node : from_graph->FindNodesOfType(IRNodeType::kGRPCSink)) {
    DCHECK(Match(ir_node, GRPCSink()));
    auto sink = static_cast<GRPCSinkIR*>(ir_node);
    bridge_id_to_grpc_sink[sink->destination_id()] = sink;
  }

  // Make a map of from's source_ids to : GRPC source group ids.
  for (IRNode* ir_node : to_graph->FindNodesOfType(IRNodeType::kGRPCSourceGroup)) {
    DCHECK(Match(ir_node, GRPCSourceGroup()));
    auto source = static_cast<GRPCSourceGroupIR*>(ir_node);
    // Only add sources that have a matching grpc sink, otherwise the bridge is for another plan.
    if (!bridge_id_to_grpc_sink.contains(source->source_id())) {
      continue;
    }
    grpc_bridges.push_back({source, bridge_id_to_grpc_sink[source->source_id()]});
  }

  bool did_connect_graph = false;
  for (const auto& [source_group, sink] : grpc_bridges) {
    PL_RETURN_IF_ERROR(source_group->AddGRPCSink(sink));
    did_connect_graph = true;
  }

  return did_connect_graph;
}

}  // namespace distributed
}  // namespace planner
}  // namespace carnot
}  // namespace pl
