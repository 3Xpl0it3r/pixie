#pragma once
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include "src/carnot/planner/distributed/distributed_coordinator.h"
#include "src/carnot/planner/distributed/distributed_plan.h"
#include "src/carnot/planner/distributed/distributed_rules.h"
#include "src/carnot/planner/ir/ir_nodes.h"
#include "src/carnot/planner/ir/pattern_match.h"

namespace pl {
namespace carnot {
namespace planner {
namespace distributed {

using distributedpb::CarnotInfo;

/**
 * @brief Sets the GRPC addresses and query broker addresses in GRPCSourceGroups.
 */
class SetSourceGroupGRPCAddressRule : public Rule {
 public:
  explicit SetSourceGroupGRPCAddressRule(const std::string& grpc_address)
      : Rule(nullptr), grpc_address_(grpc_address) {}

 private:
  StatusOr<bool> Apply(IRNode* node) override;
  std::string grpc_address_;
};

/**
 * @brief Distributed wrapper of SetSourceGroupGRPCAddressRule to apply the rule using the info of
 * each carnot instance.
 */
class DistributedSetSourceGroupGRPCAddressRule : public DistributedRule {
 public:
  DistributedSetSourceGroupGRPCAddressRule() : DistributedRule(nullptr) {}

 protected:
  StatusOr<bool> Apply(CarnotInstance* carnot_instance) override {
    SetSourceGroupGRPCAddressRule rule(carnot_instance->carnot_info().grpc_address());
    return rule.Execute(carnot_instance->plan());
  }
};

/**
 * @brief Connects the GRPCSinks to GRPCSourceGroups.
 *
 */
class AssociateDistributedPlanEdgesRule : public DistributedRule {
 public:
  AssociateDistributedPlanEdgesRule() : DistributedRule(nullptr) {}

 protected:
  StatusOr<bool> Apply(CarnotInstance* from_carnot_instance) override;
  StatusOr<bool> ConnectGraphs(IR* from_graph, IR* to_graph);
};

}  // namespace distributed
}  // namespace planner
}  // namespace carnot
}  // namespace pl
