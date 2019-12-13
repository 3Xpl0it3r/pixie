#pragma once
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include "src/carnot/compiler/distributedpb/distributed_plan.pb.h"
#include "src/carnot/compiler/ir/ir_nodes.h"
#include "src/carnot/compiler/ir/pattern_match.h"
#include "src/carnot/compiler/rules.h"

namespace pl {
namespace carnot {
namespace compiler {
namespace distributed {

/**
 * @brief Expands memory sources to use tablets if they have tabletization keys
 *
 */
class TabletSourceConversionRule : public Rule {
 public:
  explicit TabletSourceConversionRule(const distributedpb::CarnotInfo& carnot_info)
      : Rule(nullptr), carnot_info_(carnot_info) {}

 private:
  StatusOr<bool> Apply(IRNode* ir_node) override;
  StatusOr<bool> ReplaceMemorySourceWithTabletSourceGroup(MemorySourceIR* mem_source_ir);
  const distributedpb::TableInfo* GetTableInfo(const std::string& table_name);

  const distributedpb::CarnotInfo& carnot_info_;
};

/**
 * @brief Converts TabletSourceGroups into MemorySources with unions.
 */
class MemorySourceTabletRule : public Rule {
 public:
  MemorySourceTabletRule() : Rule(nullptr) {}

 private:
  StatusOr<bool> Apply(IRNode* ir_node) override;
  StatusOr<bool> ReplaceTabletSourceGroup(TabletSourceGroupIR* tablet_source_group);
  StatusOr<bool> ReplaceTabletSourceGroupAndFilter(
      TabletSourceGroupIR* tablet_source_group, FilterIR* filter_op,
      const absl::flat_hash_set<types::TabletID>& match_tablets);
  StatusOr<MemorySourceIR*> CreateMemorySource(const MemorySourceIR* original_memory_source,
                                               const types::TabletID& tablet_value);

  StatusOr<bool> ReplaceTabletSourceGroupWithFilterChild(TabletSourceGroupIR* tablet_source_group);
  void DeleteNodeAndNonOperatorChildren(OperatorIR* op);
  StatusOr<OperatorIR*> MakeNewSources(const std::vector<types::TabletID>& tablets,
                                       TabletSourceGroupIR* tablet_source_group);
  /**
   * @brief Get the tablet keys that match an equality condition.
   *
   * @param func: the ir for a function that contains an equality condition where one argument is a
   * tablet value.
   * @return absl::flat_hash_set<types::TabletID>: the set of tablet values that appear in this
   * function.
   */
  absl::flat_hash_set<types::TabletID> GetEqualityTabletValues(FuncIR* func);

  /**
   * @brief Get the tablet values that match a series of equality conditions combined with AND
   * TODO(philkuz) this should be OR not And.
   *
   * @param func: the
   * @return absl::flat_hash_set<TabletKeyType>: the set of tablet values that appear in this
   * function.
   */
  absl::flat_hash_set<types::TabletID> GetAndTabletValues(FuncIR* func);
};

class Tabletizer {
 public:
  static StatusOr<bool> Execute(const distributedpb::CarnotInfo& carnot_info, IR* ir_plan);
};

}  // namespace distributed
}  // namespace compiler
}  // namespace carnot
}  // namespace pl
