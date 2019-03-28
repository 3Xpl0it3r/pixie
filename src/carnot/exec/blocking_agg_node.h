#pragma once
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libcuckoo/cuckoohash_map.hh>

#include "src/carnot/exec/exec_node.h"
#include "src/carnot/exec/exec_state.h"
#include "src/carnot/exec/expression_evaluator.h"
#include "src/carnot/exec/row_tuple.h"
#include "src/carnot/plan/operators.h"
#include "src/common/memory/memory.h"
#include "src/shared/types/hash_utils.h"

namespace pl {
namespace carnot {
namespace exec {

struct UDAInfo {
  UDAInfo(std::unique_ptr<udf::UDA> uda_inst, udf::UDADefinition *def_ptr)
      : uda(std::move(uda_inst)), def(def_ptr) {}
  std::unique_ptr<udf::UDA> uda;
  // unowned pointer to the definition;
  udf::UDADefinition *def = nullptr;
};

struct AggHashValue {
  std::vector<UDAInfo> udas;
  std::vector<types::SharedColumnWrapper> agg_cols;
};

struct GroupArgs {
  explicit GroupArgs(RowTuple *rt) : rt(rt), av(nullptr) {}
  RowTuple *rt;
  AggHashValue *av;
};

class BlockingAggNode : public ProcessingNode {
  using AggHashMap = cuckoohash_map<RowTuple *, AggHashValue *, RowTuplePtrHasher, RowTuplePtrEq>;

 public:
  BlockingAggNode() = default;

 protected:
  Status AggregateGroupByNone(ExecState *exec_state, const table_store::schema::RowBatch &rb);
  Status AggregateGroupByClause(ExecState *exec_state, const table_store::schema::RowBatch &rb);

  std::string DebugStringImpl() override;
  Status InitImpl(
      const plan::Operator &plan_node, const table_store::schema::RowDescriptor &output_descriptor,
      const std::vector<table_store::schema::RowDescriptor> &input_descriptors) override;
  Status PrepareImpl(ExecState *exec_state) override;
  Status OpenImpl(ExecState *exec_state) override;
  Status CloseImpl(ExecState *exec_state) override;
  Status ConsumeNextImpl(ExecState *exec_state, const table_store::schema::RowBatch &rb) override;

 private:
  AggHashMap agg_hash_map_;
  bool HasNoGroups() const { return plan_node_->groups().empty(); }

  Status EvaluateSingleExpressionNoGroups(ExecState *exec_state, const UDAInfo &uda_info,
                                          plan::AggregateExpression *expr,
                                          const table_store::schema::RowBatch &rb);
  Status EvaluateAggHashValue(ExecState *exec_state, AggHashValue *val);
  StatusOr<types::DataType> GetTypeOfDep(const plan::ScalarExpression &expr) const;

  // Store information about aggregate node from the query planner.
  std::unique_ptr<plan::BlockingAggregateOperator> plan_node_;
  std::unique_ptr<table_store::schema::RowDescriptor> output_descriptor_;
  std::unique_ptr<table_store::schema::RowDescriptor> input_descriptor_;

  // Variables specific to GroupByNone Agg.
  std::vector<UDAInfo> udas_no_groups_;
  // END: Variables specific to GroupByNone Agg.

  // Variables specific to GroupBy Agg.

  // As the row batches come in we insert the correct values into the hash map based
  // on the group by key. To do this we need to keep track of which input columns we need
  // to eventually run the agg funcs.
  // 1. We need to keep a mapping from plan columns to stored columns.
  std::map<int64_t, int64_t> plan_cols_to_stored_map_;
  // 2. The reverse mapping from stored_cols to plan cols. This can be a regular vector,
  // since the stored column indices are contiguous.
  std::vector<int64_t> stored_cols_to_plan_idx_;
  // 3. The data type of the stored colums, by the index they are stored at.
  std::vector<types::DataType> stored_cols_data_types_;

  ObjectPool group_args_pool_;
  ObjectPool udas_pool_;

  std::vector<types::DataType> group_data_types_;
  std::vector<types::DataType> value_data_types_;

  // We construct row-tuples in a batch, chunked by each column.
  // This vector holds pointers to the row_tuples which are managed by the group_args_pool_.

  std::vector<GroupArgs> group_args_chunk_;
  // END: Variables specific to GroupBy Agg.

  // Creates a mapping between plan cols and stored cols (see above comment).
  Status CreateColumnMapping();

  Status ExtractRowTupleForBatch(const table_store::schema::RowBatch &rb);
  Status HashRowBatch(ExecState *exec_state, const table_store::schema::RowBatch &rb);
  Status EvaluatePartialAggregates(ExecState *exec_state, size_t num_records);
  Status ResetGroupArgs();
  Status ConvertAggHashMapToRowBatch(ExecState *exec_state,
                                     table_store::schema::RowBatch *output_rb);

  AggHashValue *CreateAggHashValue(ExecState *exec_state);
  RowTuple *CreateGroupArgsRowTuple() {
    return group_args_pool_.Add(new RowTuple(&group_data_types_));
  }

  Status CreateUDAInfoValues(std::vector<UDAInfo> *val, ExecState *exec_state);
};

}  // namespace exec
}  // namespace carnot
}  // namespace pl
