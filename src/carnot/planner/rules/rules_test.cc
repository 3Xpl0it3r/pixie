#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>
#include <pypa/parser/parser.hh>

#include "src/carnot/planner/compiler/test_utils.h"
#include "src/carnot/planner/distributedpb/distributed_plan.pb.h"
#include "src/carnot/planner/ir/ir_nodes.h"
#include "src/carnot/planner/rules/rule_mock.h"
#include "src/carnot/planner/rules/rules.h"
#include "src/carnot/udf_exporter/udf_exporter.h"

namespace pl {
namespace carnot {
namespace planner {

using table_store::schema::Relation;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Not;
using ::testing::Return;
using ::testing::UnorderedElementsAreArray;

class RulesTest : public OperatorTests {
 protected:
  void SetUpRegistryInfo() { info_ = udfexporter::ExportUDFInfo().ConsumeValueOrDie(); }
  void SetUpImpl() override {
    SetUpRegistryInfo();

    auto rel_map = std::make_unique<RelationMap>();
    cpu_relation = table_store::schema::Relation(
        std::vector<types::DataType>({types::DataType::INT64, types::DataType::FLOAT64,
                                      types::DataType::FLOAT64, types::DataType::FLOAT64}),
        std::vector<std::string>({"count", "cpu0", "cpu1", "cpu2"}));
    rel_map->emplace("cpu", cpu_relation);

    compiler_state_ = std::make_unique<CompilerState>(std::move(rel_map), info_.get(), time_now);
    md_handler = MetadataHandler::Create();
  }
  FilterIR* MakeFilter(OperatorIR* parent) {
    auto constant1 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
    auto column = MakeColumn("column", 0);

    auto filter_func = graph
                           ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::eq, "==", "equals"},
                                                std::vector<ExpressionIR*>{constant1, column})
                           .ValueOrDie();
    filter_func->SetOutputDataType(types::DataType::BOOLEAN);

    return graph->CreateNode<FilterIR>(ast, parent, filter_func).ValueOrDie();
  }
  FilterIR* MakeFilter(OperatorIR* parent, ColumnIR* filter_value) {
    auto constant1 = graph->CreateNode<StringIR>(ast, "value").ValueOrDie();
    FuncIR* filter_func =
        graph
            ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::eq, "==", "equals"},
                                 std::vector<ExpressionIR*>{constant1, filter_value})
            .ValueOrDie();
    return graph->CreateNode<FilterIR>(ast, parent, filter_func).ValueOrDie();
  }
  FilterIR* MakeFilter(OperatorIR* parent, FuncIR* filter_expr) {
    return graph->CreateNode<FilterIR>(ast, parent, filter_expr).ValueOrDie();
  }
  using OperatorTests::MakeBlockingAgg;
  BlockingAggIR* MakeBlockingAgg(OperatorIR* parent, ColumnIR* by_column, ColumnIR* fn_column) {
    auto agg_func = graph
                        ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::non_op, "", "mean"},
                                             std::vector<ExpressionIR*>{fn_column})
                        .ValueOrDie();
    return graph
        ->CreateNode<BlockingAggIR>(ast, parent, std::vector<ColumnIR*>{by_column},
                                    ColExpressionVector{{"agg_fn", agg_func}})
        .ValueOrDie();
  }

  void TearDown() override {
    if (skip_check_stray_nodes_) {
      return;
    }
    CleanUpStrayIRNodesRule cleanup;
    auto before = graph->DebugString();
    auto result = cleanup.Execute(graph.get());
    ASSERT_OK(result);
    ASSERT_FALSE(result.ConsumeValueOrDie())
        << "Rule left stray non-Operator IRNodes in graph: " << before;
  }

  // skip_check_stray_nodes_ should only be set to 'true' for tests of rules when they return an
  // error.
  bool skip_check_stray_nodes_ = false;
  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<RegistryInfo> info_;
  int64_t time_now = 1552607213931245000;
  table_store::schema::Relation cpu_relation;
  std::unique_ptr<MetadataHandler> md_handler;
};

class DataTypeRuleTest : public RulesTest {
 protected:
  void SetUp() override {
    RulesTest::SetUp();
    mem_src =
        graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{}).ValueOrDie();
    PL_CHECK_OK(mem_src->SetRelation(cpu_relation));
  }
  MemorySourceIR* mem_src;
};

// Simple map function.
TEST_F(DataTypeRuleTest, map_function) {
  auto constant = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  auto col = MakeColumn("count", /* parent_op_idx */ 0);
  auto func = graph
                  ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "+", "add"},
                                       std::vector<ExpressionIR*>({constant, col}))
                  .ValueOrDie();
  EXPECT_OK(graph->CreateNode<MapIR>(ast, mem_src, ColExpressionVector{{"func", func}},
                                     /* keep_input_columns */ false));
  // No rule has been run, don't expect any of these to be evaluated.
  EXPECT_FALSE(func->IsDataTypeEvaluated());
  EXPECT_FALSE(col->IsDataTypeEvaluated());

  // Expect the data_rule to change something.
  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // Function shouldn't be updated, it had unresolved dependencies.
  EXPECT_FALSE(func->IsDataTypeEvaluated());
  // Column should be updated, it had unresolved dependencies.
  EXPECT_TRUE(col->IsDataTypeEvaluated());

  // Expect the data_rule to change something.
  result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // The function should now be evaluated, the column should stay evaluated.
  EXPECT_TRUE(func->IsDataTypeEvaluated());
  EXPECT_TRUE(col->IsDataTypeEvaluated());

  // Both should be integers.
  EXPECT_EQ(col->EvaluatedDataType(), types::DataType::INT64);
  EXPECT_EQ(func->EvaluatedDataType(), types::DataType::INT64);

  // Expect the data_rule to do nothing, no more work left.
  result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());

  // Both should stay evaluated.
  EXPECT_TRUE(func->IsDataTypeEvaluated());
  EXPECT_TRUE(col->IsDataTypeEvaluated());
}

// The DataType shouldn't be resolved for compiler functions. They should be handled with a
// different rule.
TEST_F(DataTypeRuleTest, compiler_function_no_match) {
  // Compiler function should not get resolved.
  auto constant1 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  auto constant2 = graph->CreateNode<IntIR>(ast, 12).ValueOrDie();
  auto constant3 = graph->CreateNode<IntIR>(ast, 24).ValueOrDie();
  auto func2 = graph
                   ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "+", "add"},
                                        std::vector<ExpressionIR*>{constant1, constant2})
                   .ValueOrDie();

  EXPECT_OK(graph->CreateNode<MapIR>(
      ast, mem_src, ColExpressionVector{{"func", func2}, {"const", constant3}}, false));

  // No rule has been run, don't expect any of these to be evaluated.
  EXPECT_FALSE(func2->IsDataTypeEvaluated());
  // Expect the data_rule to do nothing, compiler function shouldn't be matched.
  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
  // No rule has been run, don't expect any of these to be evaluated.
  EXPECT_FALSE(func2->IsDataTypeEvaluated());
}

// The DataType shouldn't be resolved for a function without a name.
TEST_F(DataTypeRuleTest, missing_udf_name) {
  auto constant = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  auto col = MakeColumn("count", /* parent_op_idx */ 0);
  auto func = graph
                  ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "+", "gobeldy"},
                                       std::vector<ExpressionIR*>({constant, col}))
                  .ValueOrDie();
  EXPECT_OK(graph->CreateNode<MapIR>(ast, mem_src, ColExpressionVector{{"func", func}},
                                     /* keep_input_columns */ false));
  // Expect the data_rule to successfully change columnir.
  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // Expect the data_rule to change something.
  result = data_rule.Execute(graph.get());
  EXPECT_NOT_OK(result);

  // The function should not be evaluated, the function was not matched.
  EXPECT_FALSE(func->IsDataTypeEvaluated());
  auto result_or_s = data_rule.Execute(graph.get());
  ASSERT_NOT_OK(result_or_s);
  EXPECT_THAT(result_or_s.status(), HasCompilerError("Could not find function 'gobeldy'."));
}

// Checks to make sure that agg functions work properly.
TEST_F(DataTypeRuleTest, function_in_agg) {
  auto col = MakeColumn("count", /* parent_op_idx */ 0);
  auto func = graph
                  ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::non_op, "", "mean"},
                                       std::vector<ExpressionIR*>{col})
                  .ValueOrDie();
  EXPECT_OK(graph->CreateNode<BlockingAggIR>(ast, mem_src, std::vector<ColumnIR*>{},
                                             ColExpressionVector{{"func", func}}));

  // Expect the data_rule to successfully evaluate the column.
  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  EXPECT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_TRUE(col->IsDataTypeEvaluated());
  EXPECT_FALSE(func->IsDataTypeEvaluated());

  // Expect the data_rule to change the function.
  result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // The function should be evaluated.
  EXPECT_TRUE(func->IsDataTypeEvaluated());
  EXPECT_EQ(func->EvaluatedDataType(), types::DataType::FLOAT64);
}

// Checks to make sure that nested functions are evaluated as expected.
TEST_F(DataTypeRuleTest, nested_functions) {
  auto constant = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  auto constant2 = graph->CreateNode<IntIR>(ast, 12).ValueOrDie();
  auto col = MakeColumn("count", /* parent_op_idx */ 0);
  auto func = graph
                  ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "+", "add"},
                                       std::vector<ExpressionIR*>({constant, col}))
                  .ValueOrDie();
  auto func2 = graph
                   ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "-", "subtract"},
                                        std::vector<ExpressionIR*>({constant2, func}))
                   .ValueOrDie();
  EXPECT_OK(graph->CreateNode<MapIR>(ast, mem_src, ColExpressionVector{{"col_name", func2}},
                                     /* keep_input_columns */ false));
  // No rule has been run, don't expect any of these to be evaluated.
  EXPECT_FALSE(func->IsDataTypeEvaluated());
  EXPECT_FALSE(func2->IsDataTypeEvaluated());
  EXPECT_FALSE(col->IsDataTypeEvaluated());

  // Expect the data_rule to change something.
  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // Functions shouldn't be updated, they have unresolved dependencies.
  EXPECT_FALSE(func->IsDataTypeEvaluated());
  EXPECT_FALSE(func2->IsDataTypeEvaluated());
  // Column should be updated, it had no dependencies.
  EXPECT_TRUE(col->IsDataTypeEvaluated());

  // Expect the data_rule to change something.
  result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // func1 should now be evaluated, the column should stay evaluated, func2 is not evaluated.
  EXPECT_TRUE(func->IsDataTypeEvaluated());
  EXPECT_FALSE(func2->IsDataTypeEvaluated());
  EXPECT_TRUE(col->IsDataTypeEvaluated());

  // Everything should be evaluated, func2 changes.
  result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // All should be evaluated.
  EXPECT_TRUE(func->IsDataTypeEvaluated());
  EXPECT_TRUE(func2->IsDataTypeEvaluated());
  EXPECT_TRUE(col->IsDataTypeEvaluated());

  // All should be integers.
  EXPECT_EQ(col->EvaluatedDataType(), types::DataType::INT64);
  EXPECT_EQ(func->EvaluatedDataType(), types::DataType::INT64);
  EXPECT_EQ(func2->EvaluatedDataType(), types::DataType::INT64);

  // Expect the data_rule to do nothing, no more work left.
  result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
}

TEST_F(DataTypeRuleTest, metadata_column) {
  std::string metadata_name = "pod_name";
  MetadataProperty* property = md_handler->GetProperty(metadata_name).ValueOrDie();

  MetadataIR* metadata_ir = MakeMetadataIR(metadata_name, /* parent_op_idx */ 0);
  metadata_ir->set_property(property);
  MakeFilter(MakeMemSource(), metadata_ir);
  EXPECT_FALSE(metadata_ir->IsDataTypeEvaluated());

  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  EXPECT_TRUE(metadata_ir->IsDataTypeEvaluated());
  EXPECT_EQ(metadata_ir->EvaluatedDataType(), types::DataType::STRING);
}

class SourceRelationTest : public RulesTest {
 protected:
  void SetUp() override { RulesTest::SetUp(); }
};

// Simple check with select all.
TEST_F(SourceRelationTest, set_source_select_all) {
  MemorySourceIR* mem_src =
      graph->CreateNode<MemorySourceIR>(ast, "cpu", std::vector<std::string>{}).ValueOrDie();
  EXPECT_FALSE(mem_src->IsRelationInit());

  SourceRelationRule source_relation_rule(compiler_state_.get());
  auto result = source_relation_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  EXPECT_TRUE(mem_src->IsRelationInit());
  // Make sure the relations are the same after processing.
  table_store::schema::Relation relation = mem_src->relation();
  EXPECT_TRUE(relation.col_types() == cpu_relation.col_types());
  EXPECT_TRUE(relation.col_names() == cpu_relation.col_names());
}

TEST_F(SourceRelationTest, set_source_variable_columns) {
  std::vector<std::string> str_columns = {"cpu1", "cpu2"};
  MemorySourceIR* mem_src = graph->CreateNode<MemorySourceIR>(ast, "cpu", str_columns).ValueOrDie();

  EXPECT_FALSE(mem_src->IsRelationInit());

  SourceRelationRule source_relation_rule(compiler_state_.get());
  auto result = source_relation_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  EXPECT_TRUE(mem_src->IsRelationInit());
  // Make sure the relations are the same after processing.
  table_store::schema::Relation relation = mem_src->relation();
}

TEST_F(SourceRelationTest, missing_table_name) {
  auto table_name = "tablename_22";
  MemorySourceIR* mem_src =
      graph->CreateNode<MemorySourceIR>(ast, table_name, std::vector<std::string>{}).ValueOrDie();
  EXPECT_FALSE(mem_src->IsRelationInit());

  EXPECT_FALSE(mem_src->IsRelationInit());

  SourceRelationRule source_relation_rule(compiler_state_.get());
  auto result = source_relation_rule.Execute(graph.get());
  EXPECT_NOT_OK(result);
  EXPECT_THAT(result.status(), HasCompilerError("Table '$0' not found.", table_name));
}

TEST_F(SourceRelationTest, missing_columns) {
  std::string missing_column = "blah_column";
  std::vector<std::string> str_columns = {"cpu1", "cpu2", missing_column};
  MemorySourceIR* mem_src = graph->CreateNode<MemorySourceIR>(ast, "cpu", str_columns).ValueOrDie();

  EXPECT_FALSE(mem_src->IsRelationInit());

  SourceRelationRule source_relation_rule(compiler_state_.get());
  auto result = source_relation_rule.Execute(graph.get());
  EXPECT_NOT_OK(result);
  VLOG(1) << result.status().ToString();

  EXPECT_THAT(result.status(),
              HasCompilerError("Columns \\{$0\\} are missing in table.", missing_column));
}

TEST_F(SourceRelationTest, UDTFDoesNothing) {
  udfspb::UDTFSourceSpec udtf_spec;
  Relation relation{{types::INT64, types::STRING}, {"fd", "name"}};
  ASSERT_OK(relation.ToProto(udtf_spec.mutable_relation()));

  auto udtf =
      graph
          ->CreateNode<UDTFSourceIR>(ast, "GetOpenNetworkConnections",
                                     absl::flat_hash_map<std::string, ExpressionIR*>{}, udtf_spec)
          .ConsumeValueOrDie();

  EXPECT_TRUE(udtf->IsRelationInit());

  SourceRelationRule source_relation_rule(compiler_state_.get());
  auto did_change_or_s = source_relation_rule.Execute(graph.get());
  ASSERT_OK(did_change_or_s);
  // Should not change.
  EXPECT_FALSE(did_change_or_s.ConsumeValueOrDie());
}

class BlockingAggRuleTest : public RulesTest {
 protected:
  void SetUp() override { RulesTest::SetUp(); }
  void SetUpGraph(bool resolve_agg_func, bool resolve_agg_group) {
    mem_src =
        graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{}).ValueOrDie();
    PL_CHECK_OK(mem_src->SetRelation(cpu_relation));
    auto constant = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();

    auto agg_func = graph
                        ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::non_op, "", "mean"},
                                             std::vector<ExpressionIR*>{constant})
                        .ValueOrDie();
    if (resolve_agg_func) {
      agg_func->SetOutputDataType(func_data_type);
    }

    auto group = MakeColumn(group_name, /* parent_op_idx */ 0);
    // Code to resolve column.
    if (resolve_agg_group) {
      group->ResolveColumnType(group_data_type);
    }

    agg = graph
              ->CreateNode<BlockingAggIR>(ast, mem_src, std::vector<ColumnIR*>{group},
                                          ColExpressionVector{{agg_func_col, agg_func}})
              .ValueOrDie();
  }
  MemorySourceIR* mem_src;
  BlockingAggIR* agg;
  types::DataType func_data_type = types::DataType::FLOAT64;
  types::DataType group_data_type = types::DataType::INT64;
  std::string group_name = "group";
  std::string agg_func_col = "meaned";
};

// Relation should resolve, all expressions in operator are resolved.
TEST_F(BlockingAggRuleTest, successful_resolve) {
  SetUpGraph(true /* resolve_agg_func */, true /* resolve_agg_group */);
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  EXPECT_FALSE(agg->IsRelationInit());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  ASSERT_TRUE(agg->IsRelationInit());

  auto result_relation = agg->relation();
  table_store::schema::Relation expected_relation(
      {types::DataType::INT64, types::DataType::FLOAT64}, {group_name, agg_func_col});
  EXPECT_EQ(result_relation, expected_relation);
}
// Rule shouldn't work because column is not resolved.
TEST_F(BlockingAggRuleTest, failed_resolve_column) {
  SetUpGraph(true /* resolve_agg_func */, false /* resolve_agg_group */);
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  EXPECT_FALSE(agg->IsRelationInit());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
  EXPECT_FALSE(agg->IsRelationInit());
}

// Rule shouldn't work because function is not resolved.
TEST_F(BlockingAggRuleTest, failed_resolve_function) {
  SetUpGraph(false /* resolve_agg_func */, true /* resolve_agg_group */);
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  EXPECT_FALSE(agg->IsRelationInit());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
  EXPECT_FALSE(agg->IsRelationInit());
}
class MapRuleTest : public RulesTest {
 protected:
  void SetUp() override { RulesTest::SetUp(); }
  void SetUpGraph(bool resolve_map_func, bool keep_input_columns) {
    mem_src =
        graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{}).ValueOrDie();
    PL_CHECK_OK(mem_src->SetRelation(cpu_relation));
    auto constant1 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
    auto constant2 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();

    auto func_1 = graph
                      ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "+", "add"},
                                           std::vector<ExpressionIR*>{constant1, constant2})
                      .ValueOrDie();
    auto func_2 = graph
                      ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "*", "multiply"},
                                           std::vector<ExpressionIR*>{constant1, constant2})
                      .ValueOrDie();
    if (resolve_map_func) {
      func_1->SetOutputDataType(func_data_type);
      func_2->SetOutputDataType(func_data_type);
    }

    map = graph
              ->CreateNode<MapIR>(
                  ast, mem_src, ColExpressionVector{{new_col_name, func_1}, {old_col_name, func_2}},
                  keep_input_columns)
              .ValueOrDie();
  }
  MemorySourceIR* mem_src;
  MapIR* map;
  types::DataType func_data_type = types::DataType::INT64;
  std::string new_col_name = "sum";
  std::string old_col_name = "cpu0";
};

// Relation should resolve, all expressions in operator are resolved.
TEST_F(MapRuleTest, successful_resolve) {
  SetUpGraph(true /* resolve_map_func */, false /* keep_input_columns */);
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  EXPECT_FALSE(map->IsRelationInit());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  ASSERT_TRUE(map->IsRelationInit());

  auto result_relation = map->relation();
  table_store::schema::Relation expected_relation({types::DataType::INT64, types::DataType::INT64},
                                                  {new_col_name, old_col_name});
  EXPECT_EQ(result_relation, expected_relation);
}

// Relation should resolve, all expressions in operator are resolved, and add the previous columns.
TEST_F(MapRuleTest, successful_resolve_keep_input_columns) {
  SetUpGraph(true /* resolve_map_func */, true /* keep_input_columns */);
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  EXPECT_FALSE(map->IsRelationInit());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  ASSERT_TRUE(map->IsRelationInit());

  auto result_relation = map->relation();
  table_store::schema::Relation expected_relation(
      {types::DataType::INT64, types::DataType::FLOAT64, types::DataType::FLOAT64,
       types::DataType::INT64, types::DataType::INT64},
      {"count", "cpu1", "cpu2", new_col_name, old_col_name});
  EXPECT_EQ(result_relation, expected_relation);
}

// Rule shouldn't work because function is not resolved.
TEST_F(MapRuleTest, failed_resolve_function) {
  SetUpGraph(false /* resolve_map_func */, false /* keep_input_columns */);
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  EXPECT_FALSE(map->IsRelationInit());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
  EXPECT_FALSE(map->IsRelationInit());
}

using UnionRelationTest = RulesTest;
TEST_F(UnionRelationTest, union_relation_setup) {
  auto mem_src1 = MakeMemSource(MakeRelation());
  auto mem_src2 = MakeMemSource(MakeRelation());
  auto union_op = MakeUnion({mem_src1, mem_src2});
  EXPECT_FALSE(union_op->IsRelationInit());

  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  ASSERT_TRUE(union_op->IsRelationInit());

  auto result_relation = union_op->relation();
  table_store::schema::Relation expected_relation = MakeRelation();
  EXPECT_THAT(result_relation.col_types(), ElementsAreArray(expected_relation.col_types()));
  EXPECT_THAT(result_relation.col_names(), ElementsAreArray(expected_relation.col_names()));

  EXPECT_EQ(union_op->column_mappings().size(), 2);
  std::vector<std::string> expected_names{"count", "cpu0", "cpu1", "cpu2"};
  auto actual_mappings_1 = union_op->column_mappings()[0];
  auto actual_mappings_2 = union_op->column_mappings()[1];

  EXPECT_EQ(actual_mappings_1.size(), expected_names.size());
  EXPECT_EQ(actual_mappings_2.size(), expected_names.size());

  for (const auto& [i, col] : Enumerate(actual_mappings_1)) {
    EXPECT_EQ(expected_names[i], col->col_name());
    EXPECT_EQ(0, col->container_op_parent_idx());
    EXPECT_EQ(mem_src1, col->ReferencedOperator().ConsumeValueOrDie());
  }
  for (const auto& [i, col] : Enumerate(actual_mappings_2)) {
    EXPECT_EQ(expected_names[i], col->col_name());
    EXPECT_EQ(1, col->container_op_parent_idx());
    EXPECT_EQ(mem_src2, col->ReferencedOperator().ConsumeValueOrDie());
  }
}

// Test whether the union disagreement fails with expected message.
TEST_F(UnionRelationTest, union_relations_disagree) {
  Relation relation1 = MakeRelation();
  Relation relation2({types::DataType::INT64, types::DataType::FLOAT64}, {"count", "cpu0"});
  auto mem_src1 = MakeMemSource(relation1);
  auto mem_src2 = MakeMemSource(relation2);
  auto union_op = MakeUnion({mem_src1, mem_src2});
  EXPECT_FALSE(union_op->IsRelationInit());
  EXPECT_TRUE(mem_src1->IsRelationInit());
  EXPECT_TRUE(mem_src2->IsRelationInit());

  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_NOT_OK(result);
  auto memory_src_regex = "MemorySource\\([0-9A-z,=\\s]*\\)";
  EXPECT_THAT(result.status(),
              HasCompilerError("Table schema disagreement between parent ops $0 and "
                               "$0 of Union\\(id=[0-9]*\\). $0: \\[count:INT64, "
                               "cpu0:FLOAT64, cpu1:FLOAT64, "
                               "cpu2:FLOAT64\\] vs $0: \\[count:INT64, "
                               "cpu0:FLOAT64\\]. Column count wrong.",
                               memory_src_regex));

  skip_check_stray_nodes_ = true;
}

TEST_F(UnionRelationTest, union_relation_different_order) {
  Relation relation1({types::DataType::TIME64NS, types::DataType::STRING, types::DataType::INT64},
                     {"time_", "strCol", "count"});
  Relation relation2({types::DataType::INT64, types::DataType::TIME64NS, types::DataType::STRING},
                     {"count", "time_", "strCol"});
  auto mem_src1 = MakeMemSource(relation1);
  auto mem_src2 = MakeMemSource(relation2);
  auto union_op = MakeUnion({mem_src1, mem_src2});
  EXPECT_FALSE(union_op->IsRelationInit());
  EXPECT_TRUE(mem_src1->IsRelationInit());
  EXPECT_TRUE(mem_src2->IsRelationInit());

  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);

  ASSERT_TRUE(union_op->IsRelationInit());
  Relation result_relation = union_op->relation();
  Relation expected_relation = relation1;
  EXPECT_THAT(result_relation.col_types(), ElementsAreArray(expected_relation.col_types()));
  EXPECT_THAT(result_relation.col_names(), ElementsAreArray(expected_relation.col_names()));

  EXPECT_EQ(union_op->column_mappings().size(), 2);

  std::vector<std::string> expected_names{"time_", "strCol", "count"};
  auto actual_mappings_1 = union_op->column_mappings()[0];
  auto actual_mappings_2 = union_op->column_mappings()[1];

  EXPECT_EQ(actual_mappings_1.size(), expected_names.size());
  EXPECT_EQ(actual_mappings_2.size(), expected_names.size());

  for (const auto& [i, col] : Enumerate(actual_mappings_1)) {
    EXPECT_EQ(expected_names[i], col->col_name());
    EXPECT_EQ(0, col->container_op_parent_idx());
  }
  for (const auto& [i, col] : Enumerate(actual_mappings_2)) {
    EXPECT_EQ(expected_names[i], col->col_name());
    EXPECT_EQ(1, col->container_op_parent_idx());
  }
}

class OperatorRelationTest : public RulesTest {
 protected:
  void SetUp() override {
    RulesTest::SetUp();
    mem_src =
        graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{}).ValueOrDie();
    PL_CHECK_OK(mem_src->SetRelation(cpu_relation));
  }
  LimitIR* MakeLimit(OperatorIR* parent) {
    return graph->CreateNode<LimitIR>(ast, parent, 10).ValueOrDie();
  }
  MemorySourceIR* mem_src;
};

// Make sure that relations are copied from node to node and the rule will execute consecutively.
TEST_F(OperatorRelationTest, propogate_test) {
  FilterIR* filter = MakeFilter(mem_src);
  LimitIR* limit = MakeLimit(filter);
  EXPECT_FALSE(filter->IsRelationInit());
  EXPECT_FALSE(limit->IsRelationInit());
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // Because limit comes after filter, it can actually evaluate in a single run.
  EXPECT_TRUE(filter->IsRelationInit());
  EXPECT_TRUE(limit->IsRelationInit());

  // Should not have any work left.
  result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
}

TEST_F(OperatorRelationTest, mem_sink_with_columns_test) {
  auto src_relation = MakeRelation();
  MemorySourceIR* src = MakeMemSource(src_relation);
  MemorySinkIR* sink = MakeMemSink(src, "foo", {"cpu0"});

  OperatorRelationRule rule(compiler_state_.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(Relation({types::DataType::FLOAT64}, {"cpu0"}), sink->relation());
}

TEST_F(OperatorRelationTest, mem_sink_all_columns_test) {
  auto src_relation = MakeRelation();
  MemorySourceIR* src = MakeMemSource(src_relation);
  MemorySinkIR* sink = MakeMemSink(src, "foo", {});

  OperatorRelationRule rule(compiler_state_.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(src_relation, sink->relation());
}

TEST_F(OperatorRelationTest, JoinCreateOutputColumns) {
  std::string join_key = "key";
  Relation rel1({types::INT64, types::FLOAT64, types::STRING}, {join_key, "latency", "data"});
  Relation rel2({types::INT64, types::FLOAT64}, {join_key, "cpu_usage"});
  auto mem_src1 = MakeMemSource(rel1);
  auto mem_src2 = MakeMemSource(rel2);

  std::string left_suffix = "_x";
  std::string right_suffix = "_y";

  JoinIR* join = graph
                     ->CreateNode<JoinIR>(ast, std::vector<OperatorIR*>{mem_src1, mem_src2},
                                          "inner", std::vector<ColumnIR*>{MakeColumn(join_key, 0)},
                                          std::vector<ColumnIR*>{MakeColumn(join_key, 1)},
                                          std::vector<std::string>{left_suffix, right_suffix})
                     .ConsumeValueOrDie();

  EXPECT_TRUE(mem_src1->IsRelationInit());
  EXPECT_TRUE(mem_src2->IsRelationInit());
  EXPECT_FALSE(join->IsRelationInit());

  EXPECT_EQ(join->output_columns().size(), 0);

  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // Check that output columns are named what we expect.
  EXPECT_EQ(join->output_columns().size(), 5);
  EXPECT_TRUE(Match(join->output_columns()[0], ColumnNode(join_key, /* parent_idx */ 0)))
      << join->output_columns()[0]->DebugString();
  EXPECT_MATCH(join->output_columns()[1], ColumnNode("latency", /* parent_idx */ 0));
  EXPECT_MATCH(join->output_columns()[2], ColumnNode("data", /* parent_idx */ 0));
  EXPECT_MATCH(join->output_columns()[3], ColumnNode(join_key, /* parent_idx */ 1));
  EXPECT_MATCH(join->output_columns()[4], ColumnNode("cpu_usage", /* parent_idx */ 1));

  // Match expected data types.
  EXPECT_MATCH(join->output_columns()[0], Expression(types::INT64));
  EXPECT_MATCH(join->output_columns()[1], Expression(types::FLOAT64));
  EXPECT_MATCH(join->output_columns()[2], Expression(types::STRING));
  EXPECT_MATCH(join->output_columns()[3], Expression(types::INT64));
  EXPECT_MATCH(join->output_columns()[4], Expression(types::FLOAT64));

  // Join relation should be set.
  EXPECT_TRUE(join->IsRelationInit());
  EXPECT_EQ(join->relation(),
            Relation({types::INT64, types::FLOAT64, types::STRING, types::INT64, types::FLOAT64},
                     {"key_x", "latency", "data", "key_y", "cpu_usage"}));
}

TEST_F(OperatorRelationTest, JoinCreateOutputColumnsFailsDuplicateResultColumns) {
  std::string join_key = "key";
  std::string left_suffix = "_x";
  std::string right_suffix = "_y";
  std::string dup_key = absl::Substitute("$0$1", join_key, left_suffix);
  Relation rel1({types::INT64, types::FLOAT64, types::STRING}, {join_key, dup_key, "data"});
  Relation rel2({types::INT64, types::FLOAT64}, {join_key, "cpu_usage"});
  auto mem_src1 = MakeMemSource(rel1);
  auto mem_src2 = MakeMemSource(rel2);

  JoinIR* join = graph
                     ->CreateNode<JoinIR>(ast, std::vector<OperatorIR*>{mem_src1, mem_src2},
                                          "inner", std::vector<ColumnIR*>{MakeColumn(join_key, 0)},
                                          std::vector<ColumnIR*>{MakeColumn(join_key, 1)},
                                          std::vector<std::string>{left_suffix, right_suffix})
                     .ConsumeValueOrDie();

  EXPECT_TRUE(mem_src1->IsRelationInit());
  EXPECT_TRUE(mem_src2->IsRelationInit());
  EXPECT_FALSE(join->IsRelationInit());

  EXPECT_EQ(join->output_columns().size(), 0);

  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_NOT_OK(result);
  EXPECT_THAT(result.status(), HasCompilerError("duplicate column '$0' after merge. Change the "
                                                "specified suffixes .*'$1','$2'.* to fix this",
                                                dup_key, left_suffix, right_suffix));
}

TEST_F(OperatorRelationTest, JoinCreateOutputColumnsFailsDuplicateNoSuffixes) {
  std::string join_key = "key";
  std::string left_suffix = "";
  std::string right_suffix = "";
  Relation rel1({types::INT64, types::FLOAT64, types::STRING}, {join_key, "latency_ns", "data"});
  Relation rel2({types::INT64, types::FLOAT64}, {join_key, "cpu_usage"});
  auto mem_src1 = MakeMemSource(rel1);
  auto mem_src2 = MakeMemSource(rel2);

  JoinIR* join = graph
                     ->CreateNode<JoinIR>(ast, std::vector<OperatorIR*>{mem_src1, mem_src2},
                                          "inner", std::vector<ColumnIR*>{MakeColumn(join_key, 0)},
                                          std::vector<ColumnIR*>{MakeColumn(join_key, 1)},
                                          std::vector<std::string>{left_suffix, right_suffix})
                     .ConsumeValueOrDie();

  EXPECT_TRUE(mem_src1->IsRelationInit());
  EXPECT_TRUE(mem_src2->IsRelationInit());
  EXPECT_FALSE(join->IsRelationInit());

  EXPECT_EQ(join->output_columns().size(), 0);

  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_NOT_OK(result);
  EXPECT_THAT(result.status(),
              HasCompilerError("duplicate column '$0' after merge. Change the specified suffixes.*",
                               join_key));
}

// The right join is a weird special case for output columns - we need the order of the output
// columns to be the same -> this ensures that.
TEST_F(OperatorRelationTest, JoinCreateOutputColumnsAfterRightJoin) {
  std::string join_key = "key";
  Relation rel1({types::INT64, types::FLOAT64, types::STRING}, {join_key, "latency", "data"});
  Relation rel2({types::INT64, types::FLOAT64}, {join_key, "cpu_usage"});
  auto mem_src1 = MakeMemSource(rel1);
  auto mem_src2 = MakeMemSource(rel2);

  std::string left_suffix = "_x";
  std::string right_suffix = "_y";

  JoinIR* join = graph
                     ->CreateNode<JoinIR>(ast, std::vector<OperatorIR*>{mem_src1, mem_src2},
                                          "right", std::vector<ColumnIR*>{MakeColumn(join_key, 0)},
                                          std::vector<ColumnIR*>{MakeColumn(join_key, 1)},
                                          std::vector<std::string>{left_suffix, right_suffix})
                     .ConsumeValueOrDie();

  EXPECT_TRUE(mem_src1->IsRelationInit());
  EXPECT_TRUE(mem_src2->IsRelationInit());
  EXPECT_FALSE(join->IsRelationInit());

  EXPECT_EQ(join->output_columns().size(), 0);

  // Join should be a right join.
  EXPECT_TRUE(join->specified_as_right());
  EXPECT_TRUE(join->join_type() == JoinIR::JoinType::kRight);

  // Converts right join to left join.
  SetupJoinTypeRule rule;
  auto result_or_s = rule.Execute(graph.get());
  ASSERT_OK(result_or_s);
  EXPECT_TRUE(result_or_s.ValueOrDie());

  // Join should still be specified as a  right join.
  EXPECT_TRUE(join->specified_as_right());
  // But this switches over as internally Left is a simple column reshuffling of a Right join.
  EXPECT_TRUE(join->join_type() == JoinIR::JoinType::kLeft);

  OperatorRelationRule op_rel_rule(compiler_state_.get());
  auto result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  // Check that output columns are named what we expect.
  EXPECT_EQ(join->output_columns().size(), 5);
  EXPECT_TRUE(Match(join->output_columns()[0], ColumnNode(join_key, /* parent_idx */ 1)))
      << join->output_columns()[0]->DebugString();
  EXPECT_MATCH(join->output_columns()[1], ColumnNode("latency", /* parent_idx */ 1));
  EXPECT_MATCH(join->output_columns()[2], ColumnNode("data", /* parent_idx */ 1));
  EXPECT_MATCH(join->output_columns()[3], ColumnNode(join_key, /* parent_idx */ 0));
  EXPECT_MATCH(join->output_columns()[4], ColumnNode("cpu_usage", /* parent_idx */ 0));

  // Match expected data types.
  EXPECT_MATCH(join->output_columns()[0], Expression(types::INT64));
  EXPECT_MATCH(join->output_columns()[1], Expression(types::FLOAT64));
  EXPECT_MATCH(join->output_columns()[2], Expression(types::STRING));
  EXPECT_MATCH(join->output_columns()[3], Expression(types::INT64));
  EXPECT_MATCH(join->output_columns()[4], Expression(types::FLOAT64));

  // Join relation should be set.
  EXPECT_TRUE(join->IsRelationInit());
  EXPECT_EQ(join->relation(),
            Relation({types::INT64, types::FLOAT64, types::STRING, types::INT64, types::FLOAT64},
                     {"key_x", "latency", "data", "key_y", "cpu_usage"}));
}

class CompileTimeExpressionTest : public RulesTest {
 protected:
  void SetUp() override {
    RulesTest::SetUp();
    mem_src =
        graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{}).ValueOrDie();
    PL_CHECK_OK(mem_src->SetRelation(cpu_relation));
  }
  FuncIR* MakeConstantAddition(int64_t l, int64_t r) {
    auto constant1 = graph->CreateNode<IntIR>(ast, l).ValueOrDie();
    auto constant2 = graph->CreateNode<IntIR>(ast, r).ValueOrDie();

    return graph
        ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::add, "+", "add"},
                             std::vector<ExpressionIR*>{constant1, constant2})
        .ValueOrDie();
  }

  MemorySourceIR* mem_src;
};

TEST_F(CompileTimeExpressionTest, mem_src_one_argument_string) {
  int64_t num_minutes_ago = 2;
  std::chrono::nanoseconds exp_time = std::chrono::minutes(num_minutes_ago);
  int64_t expected_time = time_now - exp_time.count();
  std::string stop_str_repr = absl::Substitute("-$0m", num_minutes_ago);

  auto stop = graph->CreateNode<StringIR>(ast, stop_str_repr).ValueOrDie();
  auto start = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();

  EXPECT_OK(mem_src->SetTimeExpressions(start, stop));
  ConvertStringTimesRule compiler_expr_rule(compiler_state_.get());

  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  auto start_res = mem_src->start_time_expr();
  auto end_res = mem_src->end_time_expr();
  EXPECT_MATCH(start_res, Int());
  EXPECT_MATCH(end_res, Int());
  EXPECT_EQ(static_cast<IntIR*>(start_res)->val(), 10);
  EXPECT_EQ(static_cast<IntIR*>(end_res)->val(), expected_time);
}

TEST_F(CompileTimeExpressionTest, mem_src_two_argument_string) {
  int64_t start_num_minutes_ago = 2;
  int64_t stop_num_minutes_ago = 1;
  std::chrono::nanoseconds exp_stop_time = std::chrono::minutes(stop_num_minutes_ago);
  int64_t expected_stop_time = time_now - exp_stop_time.count();
  std::string stop_str_repr = absl::Substitute("-$0m", stop_num_minutes_ago);

  std::chrono::nanoseconds exp_start_time = std::chrono::minutes(start_num_minutes_ago);
  int64_t expected_start_time = time_now - exp_start_time.count();
  std::string start_str_repr = absl::Substitute("-$0m", start_num_minutes_ago);

  auto start = graph->CreateNode<StringIR>(ast, start_str_repr).ValueOrDie();
  auto stop = graph->CreateNode<StringIR>(ast, stop_str_repr).ValueOrDie();

  EXPECT_OK(mem_src->SetTimeExpressions(start, stop));
  ConvertStringTimesRule compiler_expr_rule(compiler_state_.get());

  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  auto start_res = mem_src->start_time_expr();
  auto end_res = mem_src->end_time_expr();
  EXPECT_MATCH(start_res, Int());
  EXPECT_MATCH(end_res, Int());
  EXPECT_EQ(static_cast<IntIR*>(start_res)->val(), expected_start_time);
  EXPECT_EQ(static_cast<IntIR*>(end_res)->val(), expected_stop_time);
}

TEST_F(CompileTimeExpressionTest, rolling_time_string) {
  int64_t window_size_minutes = 1;
  int64_t expected_window_size =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::minutes(1)).count();
  std::string window_size_str = absl::Substitute("$0m", window_size_minutes);
  auto window_size = graph->CreateNode<StringIR>(ast, window_size_str).ValueOrDie();

  auto window_col = graph->CreateNode<ColumnIR>(ast, "time_", /* parent_op_idx */ 0).ValueOrDie();
  auto rolling = graph->CreateNode<RollingIR>(ast, mem_src, window_col, window_size).ValueOrDie();

  ConvertStringTimesRule compiler_expr_rule(compiler_state_.get());
  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  auto new_window_size = rolling->window_size();
  EXPECT_MATCH(new_window_size, Int());
  EXPECT_EQ(static_cast<IntIR*>(new_window_size)->val(), expected_window_size);
}

TEST_F(CompileTimeExpressionTest, mem_src_set_times) {
  auto start = graph->CreateNode<IntIR>(ast, 19).ValueOrDie();
  auto stop = graph->CreateNode<IntIR>(ast, 20).ValueOrDie();

  EXPECT_OK(mem_src->SetTimeExpressions(start, stop));
  SetMemSourceNsTimesRule times_rule;

  auto result = times_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(mem_src->time_start_ns(), 19);
  EXPECT_EQ(mem_src->time_stop_ns(), 20);
}

TEST_F(CompileTimeExpressionTest, map_nested) {
  auto top_level = MakeConstantAddition(4, 6);
  auto nested =
      MakeFunc("non_compile", std::vector<ExpressionIR*>{MakeConstantAddition(5, 6), MakeInt(2)});
  auto int_node = MakeInt(2);

  ColExpressionVector exprs{{"top", top_level}, {"nested", nested}, {"int", int_node}};
  auto map =
      graph->CreateNode<MapIR>(ast, mem_src, exprs, /* keep_input_columns */ false).ValueOrDie();

  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());
  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  auto col_exprs = map->col_exprs();
  EXPECT_EQ(3, col_exprs.size());
  EXPECT_EQ(IRNodeType::kInt, col_exprs[0].node->type());
  EXPECT_EQ(IRNodeType::kFunc, col_exprs[1].node->type());
  EXPECT_EQ(IRNodeType::kInt, col_exprs[2].node->type());

  // arg 0
  auto top_level_res = static_cast<IntIR*>(col_exprs[0].node);
  EXPECT_EQ(10, top_level_res->val());

  // arg 1
  auto nested_res = static_cast<FuncIR*>(col_exprs[1].node);
  EXPECT_EQ(2, nested_res->args().size());
  EXPECT_EQ(IRNodeType::kInt, nested_res->args()[0]->type());
  EXPECT_EQ(IRNodeType::kInt, nested_res->args()[1]->type());
  EXPECT_EQ(11, static_cast<IntIR*>(nested_res->args()[0])->val());
  EXPECT_EQ(2, static_cast<IntIR*>(nested_res->args()[1])->val());

  // arg 2
  EXPECT_EQ(2, static_cast<IntIR*>(col_exprs[2].node)->val());
}

TEST_F(CompileTimeExpressionTest, filter_eval) {
  auto col = MakeColumn("cpu0", /* parent_op_idx */ 0);
  auto expr = MakeConstantAddition(5, 6);
  auto filter_func = MakeEqualsFunc(col, expr);
  auto filter = graph->CreateNode<FilterIR>(ast, mem_src, filter_func).ValueOrDie();

  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());
  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(IRNodeType::kFunc, filter->filter_expr()->type());
}

TEST_F(CompileTimeExpressionTest, filter_no_eval) {
  auto col = MakeColumn("cpu0", /* parent_op_idx */ 0);
  auto expr = MakeInt(5);
  auto filter_func = graph
                         ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::eq, "==", "equals"},
                                              std::vector<ExpressionIR*>({col, expr}))
                         .ValueOrDie();
  ASSERT_OK(graph->CreateNode<FilterIR>(ast, mem_src, filter_func));
  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());
  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
}

TEST_F(CompileTimeExpressionTest, mem_src_one_argument_function) {
  auto start = MakeConstantAddition(4, 6);
  auto stop = graph->CreateNode<IntIR>(ast, 13).ValueOrDie();
  EXPECT_OK(mem_src->SetTimeExpressions(start, stop));

  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());

  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ConsumeValueOrDie());

  // Make sure that we don't manipulate the start value.
  EXPECT_EQ(static_cast<IntIR*>(mem_src->start_time_expr())->val(), 10);
  EXPECT_EQ(static_cast<IntIR*>(mem_src->end_time_expr())->val(), 13);
}

TEST_F(CompileTimeExpressionTest, mem_src_two_argument_function) {
  auto start = MakeConstantAddition(4, 6);
  auto stop = MakeConstantAddition(123, 321);
  EXPECT_OK(mem_src->SetTimeExpressions(start, stop));
  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());

  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(static_cast<IntIR*>(mem_src->start_time_expr())->val(), 10);
  EXPECT_EQ(static_cast<IntIR*>(mem_src->end_time_expr())->val(), 444);
}

TEST_F(CompileTimeExpressionTest, subtraction_handling) {
  IntIR* constant1 = graph->CreateNode<IntIR>(ast, 111).ValueOrDie();
  IntIR* constant2 = graph->CreateNode<IntIR>(ast, 11).ValueOrDie();
  IntIR* start = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  FuncIR* stop = graph
                     ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::sub, "-", "subtract"},
                                          std::vector<ExpressionIR*>{constant1, constant2})
                     .ValueOrDie();

  EXPECT_OK(mem_src->SetTimeExpressions(start, stop));
  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());

  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(static_cast<IntIR*>(mem_src->start_time_expr())->val(), 10);
  EXPECT_EQ(static_cast<IntIR*>(mem_src->end_time_expr())->val(), 100);
}

TEST_F(CompileTimeExpressionTest, multiplication_handling) {
  IntIR* constant1 = graph->CreateNode<IntIR>(ast, 3).ValueOrDie();
  IntIR* constant2 = graph->CreateNode<IntIR>(ast, 8).ValueOrDie();
  IntIR* start = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  FuncIR* stop = graph
                     ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::mult, "*", "multiply"},
                                          std::vector<ExpressionIR*>{constant1, constant2})
                     .ValueOrDie();

  EXPECT_OK(mem_src->SetTimeExpressions(start, stop));
  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());

  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(static_cast<IntIR*>(mem_src->start_time_expr())->val(), 10);
  EXPECT_EQ(static_cast<IntIR*>(mem_src->end_time_expr())->val(), 24);
}

TEST_F(CompileTimeExpressionTest, already_completed) {
  IntIR* constant1 = graph->CreateNode<IntIR>(ast, 24).ValueOrDie();
  IntIR* constant2 = graph->CreateNode<IntIR>(ast, 8).ValueOrDie();

  EXPECT_OK(mem_src->SetTimeExpressions(constant1, constant2));
  // The rule does this.
  mem_src->SetTimeValuesNS(24, 8);
  OperatorCompileTimeExpressionRule compiler_expr_rule(compiler_state_.get());

  auto result = compiler_expr_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());

  EXPECT_EQ(mem_src->time_start_ns(), 24);
  EXPECT_EQ(mem_src->time_stop_ns(), 8);
}
class VerifyFilterExpressionTest : public RulesTest {
 protected:
  void SetUp() override {
    RulesTest::SetUp();
    mem_src =
        graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{}).ValueOrDie();
    PL_CHECK_OK(mem_src->SetRelation(cpu_relation));
  }
  FuncIR* MakeFilter() {
    auto constant1 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
    auto constant2 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();

    filter_func = graph
                      ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::eq, "==", "equals"},
                                           std::vector<ExpressionIR*>{constant1, constant2})
                      .ValueOrDie();
    PL_CHECK_OK(graph->CreateNode<FilterIR>(ast, mem_src, filter_func));
    return filter_func;
  }
  MemorySourceIR* mem_src;
  FuncIR* filter_func;
};

TEST_F(VerifyFilterExpressionTest, basic_test) {
  FuncIR* filter_func = MakeFilter();
  filter_func->SetOutputDataType(types::DataType::BOOLEAN);
  VerifyFilterExpressionRule rule(compiler_state_.get());
  auto status_or = rule.Execute(graph.get());
  EXPECT_OK(status_or);
  EXPECT_FALSE(status_or.ValueOrDie());
}

TEST_F(VerifyFilterExpressionTest, wrong_filter_func_type) {
  FuncIR* filter_func = MakeFilter();
  filter_func->SetOutputDataType(types::DataType::INT64);
  VerifyFilterExpressionRule rule(compiler_state_.get());
  auto status_or = rule.Execute(graph.get());
  EXPECT_NOT_OK(status_or);
}

TEST_F(VerifyFilterExpressionTest, filter_func_not_set) {
  FuncIR* filter_func = MakeFilter();
  EXPECT_EQ(filter_func->EvaluatedDataType(), types::DataType::DATA_TYPE_UNKNOWN);
  VerifyFilterExpressionRule rule(compiler_state_.get());
  auto status_or = rule.Execute(graph.get());
  EXPECT_NOT_OK(status_or);
}

class CheckRelationRule : public RulesTest {
 protected:
  void SetUp() override {
    RulesTest::SetUp();
    mem_src =
        graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{}).ValueOrDie();
    PL_CHECK_OK(mem_src->SetRelation(cpu_relation));
  }

  MapIR* MakeMap(OperatorIR* parent, std::string column_name) {
    auto map_func = MakeAddFunc(MakeInt(10), MakeInt(12));
    MapIR* map = graph
                     ->CreateNode<MapIR>(ast, parent, ColExpressionVector{{column_name, map_func}},
                                         /* keep_input_columns */ false)
                     .ValueOrDie();
    return map;
  }

  MapIR* MakeMap(OperatorIR* parent) { return MakeMap(parent, "map_fn"); }

  table_store::schema::Relation ViolatingRelation() {
    auto relation = mem_src->relation();
    relation.AddColumn(types::DataType::STRING,
                       absl::Substitute("$0_pod_name", MetadataProperty::kMetadataColumnPrefix));
    return relation;
  }
  table_store::schema::Relation PassingRelation() { return mem_src->relation(); }

  MemorySourceIR* mem_src;
};

TEST_F(RulesTest, drop_to_map) {
  MemorySourceIR* mem_src =
      graph->CreateNode<MemorySourceIR>(ast, "source", std::vector<std::string>{})
          .ConsumeValueOrDie();
  DropIR* drop = graph->CreateNode<DropIR>(ast, mem_src, std::vector<std::string>{"cpu0", "cpu1"})
                     .ConsumeValueOrDie();
  MemorySinkIR* sink = MakeMemSink(drop, "sink");

  EXPECT_OK(mem_src->SetRelation(cpu_relation));
  EXPECT_THAT(graph->dag().TopologicalSort(), ElementsAre(0, 1, 2));

  auto drop_id = drop->id();

  // Apply the rule.
  DropToMapOperatorRule rule(compiler_state_.get());
  auto status = rule.Execute(graph.get());
  ASSERT_OK(status);
  EXPECT_TRUE(status.ValueOrDie());

  EXPECT_FALSE(graph->dag().HasNode(drop_id));

  ASSERT_EQ(mem_src->Children().size(), 1);
  EXPECT_MATCH(mem_src->Children()[0], Map());
  auto op = static_cast<MapIR*>(mem_src->Children()[0]);
  EXPECT_EQ(op->col_exprs().size(), 2);
  EXPECT_EQ(op->col_exprs()[0].name, "count");
  EXPECT_EQ(op->col_exprs()[1].name, "cpu2");

  EXPECT_TRUE(Match(op->col_exprs()[0].node, ColumnNode("count")))
      << op->col_exprs()[0].node->DebugString();
  EXPECT_TRUE(Match(op->col_exprs()[1].node, ColumnNode("cpu2")))
      << op->col_exprs()[1].node->DebugString();

  EXPECT_EQ(op->relation(), Relation({types::INT64, types::FLOAT64}, {"count", "cpu2"}));

  EXPECT_EQ(op->Children().size(), 1);
  EXPECT_EQ(op->Children()[0], sink);
}

TEST_F(RulesTest, drop_middle_columns) {
  MemorySourceIR* mem_src =
      MakeMemSource(Relation({types::STRING, types::TIME64NS, types::STRING, types::FLOAT64,
                              types::FLOAT64, types::TIME64NS},
                             {"service", "window", "quantiles", "p50", "p99", "time_"}));
  DropIR* drop =
      graph->CreateNode<DropIR>(ast, mem_src, std::vector<std::string>{"window", "quantiles"})
          .ConsumeValueOrDie();
  auto drop_id = drop->id();
  MemorySinkIR* sink = MakeMemSink(drop, "sink");

  EXPECT_THAT(graph->dag().TopologicalSort(), ElementsAre(0, 1, 2));

  // Apply the rule.
  DropToMapOperatorRule rule(compiler_state_.get());
  auto status = rule.Execute(graph.get());
  ASSERT_OK(status);
  EXPECT_TRUE(status.ValueOrDie());

  EXPECT_FALSE(graph->dag().HasNode(drop_id));

  ASSERT_EQ(mem_src->Children().size(), 1);
  EXPECT_MATCH(mem_src->Children()[0], Map());
  auto op = static_cast<MapIR*>(mem_src->Children()[0]);
  EXPECT_EQ(op->col_exprs().size(), 4);
  EXPECT_EQ(op->col_exprs()[0].name, "service");
  EXPECT_EQ(op->col_exprs()[1].name, "p50");
  EXPECT_EQ(op->col_exprs()[2].name, "p99");
  EXPECT_EQ(op->col_exprs()[3].name, "time_");

  EXPECT_TRUE(Match(op->col_exprs()[0].node, ColumnNode("service")))
      << op->col_exprs()[0].node->DebugString();
  EXPECT_TRUE(Match(op->col_exprs()[1].node, ColumnNode("p50")))
      << op->col_exprs()[1].node->DebugString();
  EXPECT_TRUE(Match(op->col_exprs()[2].node, ColumnNode("p99")))
      << op->col_exprs()[2].node->DebugString();
  EXPECT_TRUE(Match(op->col_exprs()[3].node, ColumnNode("time_")))
      << op->col_exprs()[3].node->DebugString();

  EXPECT_EQ(op->relation(),
            Relation({types::STRING, types::FLOAT64, types::FLOAT64, types::TIME64NS},
                     {"service", "p50", "p99", "time_"}));
  EXPECT_EQ(op->Children().size(), 1);
  EXPECT_EQ(op->Children()[0], sink);
}

TEST_F(RulesTest, setup_join_type_rule) {
  Relation relation0({types::DataType::INT64, types::DataType::INT64, types::DataType::INT64,
                      types::DataType::INT64},
                     {"left_only", "col1", "col2", "col3"});
  auto mem_src1 = MakeMemSource(relation0);

  Relation relation1({types::DataType::INT64, types::DataType::INT64, types::DataType::INT64,
                      types::DataType::INT64, types::DataType::INT64},
                     {"right_only", "col1", "col2", "col3", "col4"});
  auto mem_src2 = MakeMemSource(relation1);

  auto join_op =
      MakeJoin({mem_src1, mem_src2}, "right", relation0, relation1,
               std::vector<std::string>{"col1", "col3"}, std::vector<std::string>{"col2", "col4"});

  SetupJoinTypeRule rule;
  auto result = rule.Execute(graph.get());
  EXPECT_OK(result);
  EXPECT_TRUE(result.ConsumeValueOrDie());

  EXPECT_EQ(join_op->parents()[0], mem_src2);
  EXPECT_EQ(join_op->parents()[1], mem_src1);
}

TEST_F(RulesTest, eval_compile_time_test) {
  auto c1 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  auto c2 = graph->CreateNode<IntIR>(ast, 9).ValueOrDie();

  auto add_func = MakeAddFunc(c1, c2);
  auto mult_func = MakeMultFunc(c1, add_func);

  // hours(10*(10 + 9))
  auto hours_func = graph
                        ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::non_op, "", "hours"},
                                             std::vector<ExpressionIR*>{mult_func})
                        .ValueOrDie();

  // Add the func as part of a map so that CleanUpStrayIRNodesRule passes.
  MemorySourceIR* mem_src = MakeMemSource();
  auto map = MakeMap(mem_src, {ColumnExpression{"col", hours_func}}, true);

  EvaluateCompileTimeExpr evaluator(compiler_state_.get());
  auto evaluted = evaluator.Evaluate(hours_func).ValueOrDie();

  // Update so CleanUpStrayIRNodesRule passes.
  ASSERT_OK(map->UpdateColExpr("col", evaluted));

  EXPECT_EQ(IRNodeType::kInt, evaluted->type());
  auto casted_int = static_cast<IntIR*>(evaluted);
  std::chrono::nanoseconds time_output = 190 * std::chrono::hours(1);
  EXPECT_EQ(time_output.count(), casted_int->val());
}

TEST_F(RulesTest, eval_partial_compile_time_test) {
  auto c1 = graph->CreateNode<IntIR>(ast, 10).ValueOrDie();
  auto c2 = graph->CreateNode<IntIR>(ast, 9).ValueOrDie();

  auto add_func = MakeAddFunc(c1, c2);
  auto mult_func = MakeMultFunc(c1, add_func);

  // not_hours(10*(10 + 9))
  auto not_hours_func =
      graph
          ->CreateNode<FuncIR>(ast, FuncIR::Op{FuncIR::Opcode::non_op, "", "not_hours"},
                               std::vector<ExpressionIR*>{mult_func})
          .ValueOrDie();

  // Add the func as part of a map so that CleanUpStrayIRNodesRule passes.
  MemorySourceIR* mem_src = MakeMemSource();
  MakeMap(mem_src, {ColumnExpression{"col", not_hours_func}}, true);

  EvaluateCompileTimeExpr evaluator(compiler_state_.get());
  auto evaluted = evaluator.Evaluate(not_hours_func).ValueOrDie();
  EXPECT_EQ(IRNodeType::kFunc, evaluted->type());
  auto casted = static_cast<FuncIR*>(evaluted);
  EXPECT_EQ(1, casted->args().size());
  EXPECT_EQ(IRNodeType::kInt, casted->args()[0]->type());
  auto casted_int_arg = static_cast<IntIR*>(casted->args()[0]);
  EXPECT_EQ(190, casted_int_arg->val());
}

TEST_F(RulesTest, MergeGroupByAggRule) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  BlockingAggIR* agg =
      MakeBlockingAgg(group_by, {}, {{"outcount", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg, "");

  EXPECT_THAT(agg->parents(), ElementsAre(group_by));
  EXPECT_EQ(agg->groups().size(), 0);
  std::vector<int64_t> groupby_ids;
  for (ColumnIR* g : group_by->groups()) {
    groupby_ids.push_back(g->id());
  }

  // Do match and merge Groupby with agg
  // make sure agg parent changes from groupby to the parent of the groupby
  MergeGroupByIntoGroupAcceptorRule rule(IRNodeType::kBlockingAgg);
  auto result = rule.Execute(graph.get());
  EXPECT_OK(result);
  EXPECT_TRUE(result.ConsumeValueOrDie());

  EXPECT_THAT(agg->parents(), ElementsAre(mem_source));

  std::vector<std::string> actual_group_names;
  std::vector<int64_t> actual_group_ids;
  for (ColumnIR* g : agg->groups()) {
    actual_group_names.push_back(g->col_name());
    actual_group_ids.push_back(g->id());
  }

  EXPECT_THAT(actual_group_names, ElementsAre("col1", "col2"));
  EXPECT_NE(actual_group_ids, groupby_ids);
}

TEST_F(RulesTest, MergeGroupByRollingRule) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  RollingIR* rolling = MakeRolling(group_by, MakeColumn("time_", 0), MakeTime(0));
  MakeMemSink(rolling, "");

  EXPECT_THAT(rolling->parents(), ElementsAre(group_by));
  EXPECT_EQ(rolling->groups().size(), 0);
  std::vector<int64_t> groupby_ids;
  for (ColumnIR* g : group_by->groups()) {
    groupby_ids.push_back(g->id());
  }

  // Do match and merge Groupby with rolling
  // make sure rolling parent changes from groupby to the parent of the groupby
  MergeGroupByIntoGroupAcceptorRule rule(IRNodeType::kRolling);
  auto result = rule.Execute(graph.get());
  EXPECT_OK(result);
  EXPECT_TRUE(result.ConsumeValueOrDie());

  EXPECT_THAT(rolling->parents(), ElementsAre(mem_source));

  std::vector<std::string> actual_group_names;
  std::vector<int64_t> actual_group_ids;
  for (ColumnIR* g : rolling->groups()) {
    actual_group_names.push_back(g->col_name());
    actual_group_ids.push_back(g->id());
  }

  EXPECT_THAT(actual_group_names, ElementsAre("col1", "col2"));
  EXPECT_NE(actual_group_ids, groupby_ids);
}

TEST_F(RulesTest, MergeGroupByAggRule_MultipleAggsOneGroupBy) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  BlockingAggIR* agg1 =
      MakeBlockingAgg(group_by, {}, {{"outcount", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg1, "");
  BlockingAggIR* agg2 =
      MakeBlockingAgg(group_by, {}, {{"latency_mean", MakeMeanFunc(MakeColumn("latency", 0))}});
  MakeMemSink(agg2, "");

  EXPECT_THAT(agg1->parents(), ElementsAre(group_by));
  EXPECT_EQ(agg1->groups().size(), 0);
  EXPECT_THAT(agg2->parents(), ElementsAre(group_by));
  EXPECT_EQ(agg2->groups().size(), 0);

  MergeGroupByIntoGroupAcceptorRule rule(IRNodeType::kBlockingAgg);
  auto result = rule.Execute(graph.get());
  EXPECT_OK(result);
  EXPECT_TRUE(result.ConsumeValueOrDie());

  EXPECT_THAT(agg1->parents(), ElementsAre(mem_source));
  EXPECT_THAT(agg2->parents(), ElementsAre(mem_source));
  std::vector<std::string> group_names1;
  std::vector<std::string> group_names2;
  std::vector<int64_t> group_ids1;
  std::vector<int64_t> group_ids2;
  for (ColumnIR* g : agg1->groups()) {
    group_names1.push_back(g->col_name());
    group_ids1.push_back(g->id());
  }
  for (ColumnIR* g : agg2->groups()) {
    group_names2.push_back(g->col_name());
    group_ids2.push_back(g->id());
  }

  EXPECT_THAT(group_names1, ElementsAre("col1", "col2"));
  EXPECT_THAT(group_names2, ElementsAre("col1", "col2"));

  // Ids must be different -> must be a deep copy not a pointer copy.
  EXPECT_NE(group_ids1, group_ids2);
}

TEST_F(RulesTest, MergeGroupByAggRule_MissesSoleAgg) {
  MemorySourceIR* mem_source = MakeMemSource();
  BlockingAggIR* agg =
      MakeBlockingAgg(mem_source, {}, {{"outcount", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg, "");

  EXPECT_THAT(agg->parents(), ElementsAre(mem_source));
  EXPECT_EQ(agg->groups().size(), 0);

  // Don't match Agg by itself
  MergeGroupByIntoGroupAcceptorRule rule(IRNodeType::kBlockingAgg);
  auto result = rule.Execute(graph.get());
  EXPECT_OK(result);
  EXPECT_FALSE(result.ConsumeValueOrDie());

  // Agg parents don't change
  EXPECT_THAT(agg->parents(), ElementsAre(mem_source));
  // Agg groups should not change.
  EXPECT_EQ(agg->groups().size(), 0);
}

TEST_F(RulesTest, MergeGroupByAggRule_DoesNotTouchSoleGroupby) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  MakeMemSink(group_by, "");
  // Don't match groupby by itself
  MergeGroupByIntoGroupAcceptorRule rule(IRNodeType::kBlockingAgg);
  auto result = rule.Execute(graph.get());
  EXPECT_OK(result);
  // Should not do anything to the graph.
  EXPECT_FALSE(result.ConsumeValueOrDie());
}

TEST_F(RulesTest, RemoveGroupByRule) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  int64_t group_by_node_id = group_by->id();
  // Note that the parent is mem_source not group by.
  BlockingAggIR* agg = MakeBlockingAgg(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)},
                                       {{"outcount", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg, "");
  // Do match groupbys() that no longer have children
  RemoveGroupByRule rule;
  auto result = rule.Execute(graph.get());
  EXPECT_OK(result);
  EXPECT_TRUE(result.ConsumeValueOrDie());

  EXPECT_FALSE(graph->HasNode(group_by_node_id));
  // Make sure no groupby sticks around either
  for (int64_t i : graph->dag().TopologicalSort()) {
    EXPECT_FALSE(Match(graph->Get(i), GroupBy())) << absl::Substitute("Node $0 is a groupby()", i);
  }
}

TEST_F(RulesTest, RemoveGroupByRule_FailOnBadGroupBy) {
  // Error on groupbys() that have sinks or follow up nodes.
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  // Note that mem sink is conect to a groupby. Anything that has a group by as a parent should fail
  // at this point.
  MakeMemSink(group_by, "");
  RemoveGroupByRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_NOT_OK(result);
  EXPECT_THAT(result.status(), HasCompilerError("'groupby.*' should be followed by an 'agg.*'"));
}

TEST_F(RulesTest, MergeAndRemove) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  BlockingAggIR* agg =
      MakeBlockingAgg(group_by, {}, {{"outcount", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg, "");

  int64_t group_by_node_id = group_by->id();
  std::vector<int64_t> groupby_ids;
  for (ColumnIR* g : group_by->groups()) {
    groupby_ids.push_back(g->id());
  }

  // Do remove groupby after running both
  MergeGroupByIntoGroupAcceptorRule rule1(IRNodeType::kBlockingAgg);
  RemoveGroupByRule rule2;
  auto result1 = rule1.Execute(graph.get());
  ASSERT_OK(result1);
  ASSERT_TRUE(result1.ConsumeValueOrDie());
  auto result2 = rule2.Execute(graph.get());
  ASSERT_OK(result1);
  ASSERT_TRUE(result2.ConsumeValueOrDie());

  EXPECT_FALSE(graph->HasNode(group_by_node_id));
  // Make sure no groupby sticks around either
  for (int64_t i : graph->dag().TopologicalSort()) {
    EXPECT_FALSE(Match(graph->Get(i), GroupBy())) << absl::Substitute("Node $0 is a groupby()", i);
  }

  EXPECT_THAT(agg->parents(), ElementsAre(mem_source));

  std::vector<std::string> actual_group_names;
  std::vector<int64_t> actual_group_ids;
  for (ColumnIR* g : agg->groups()) {
    actual_group_names.push_back(g->col_name());
    actual_group_ids.push_back(g->id());
  }

  EXPECT_THAT(actual_group_names, ElementsAre("col1", "col2"));
  EXPECT_THAT(actual_group_ids, Not(UnorderedElementsAreArray(groupby_ids)));
}

TEST_F(RulesTest, MergeAndRemove_MultipleAggs) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by = MakeGroupBy(mem_source, {MakeColumn("col1", 0), MakeColumn("col2", 0)});
  BlockingAggIR* agg1 =
      MakeBlockingAgg(group_by, {}, {{"outcount", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg1, "");
  BlockingAggIR* agg2 =
      MakeBlockingAgg(group_by, {}, {{"latency_mean", MakeMeanFunc(MakeColumn("latency", 0))}});
  MakeMemSink(agg2, "");

  int64_t group_by_node_id = group_by->id();

  // Verification that everything is constructed correctly.
  EXPECT_THAT(agg1->parents(), ElementsAre(group_by));
  EXPECT_EQ(agg1->groups().size(), 0);
  EXPECT_THAT(agg2->parents(), ElementsAre(group_by));
  EXPECT_EQ(agg2->groups().size(), 0);

  // Do remove groupby after running both
  MergeGroupByIntoGroupAcceptorRule rule1(IRNodeType::kBlockingAgg);
  RemoveGroupByRule rule2;
  auto result1 = rule1.Execute(graph.get());
  ASSERT_OK(result1);
  ASSERT_TRUE(result1.ConsumeValueOrDie());
  auto result2 = rule2.Execute(graph.get());
  ASSERT_OK(result1);
  ASSERT_TRUE(result2.ConsumeValueOrDie());

  EXPECT_FALSE(graph->HasNode(group_by_node_id));
  // Make sure no groupby sticks around either
  for (int64_t i : graph->dag().TopologicalSort()) {
    EXPECT_FALSE(Match(graph->Get(i), GroupBy())) << absl::Substitute("Node $0 is a groupby()", i);
  }

  EXPECT_THAT(agg1->parents(), ElementsAre(mem_source));
  EXPECT_THAT(agg2->parents(), ElementsAre(mem_source));
  std::vector<std::string> group_names1;
  std::vector<std::string> group_names2;
  std::vector<int64_t> group_ids1;
  std::vector<int64_t> group_ids2;
  for (ColumnIR* g : agg1->groups()) {
    group_names1.push_back(g->col_name());
    group_ids1.push_back(g->id());
  }
  for (ColumnIR* g : agg2->groups()) {
    group_names2.push_back(g->col_name());
    group_ids2.push_back(g->id());
  }

  EXPECT_THAT(group_names1, ElementsAre("col1", "col2"));
  EXPECT_THAT(group_names2, ElementsAre("col1", "col2"));

  // Ids must be different -> must be a deep copy not a pointer copy.
  EXPECT_NE(group_ids1, group_ids2);
}

TEST_F(RulesTest, MergeAndRemove_GroupByOnMetadataColumns) {
  MemorySourceIR* mem_source = MakeMemSource();
  GroupByIR* group_by =
      MakeGroupBy(mem_source, {MakeMetadataIR("service", 0), MakeColumn("col2", 0)});
  BlockingAggIR* agg =
      MakeBlockingAgg(group_by, {}, {{"outcount", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg, "");

  int64_t group_by_node_id = group_by->id();
  std::vector<int64_t> groupby_ids;
  for (ColumnIR* g : group_by->groups()) {
    groupby_ids.push_back(g->id());
  }

  // Do remove groupby after running both
  MergeGroupByIntoGroupAcceptorRule rule1(IRNodeType::kBlockingAgg);
  RemoveGroupByRule rule2;
  auto result1 = rule1.Execute(graph.get());
  ASSERT_OK(result1);
  ASSERT_TRUE(result1.ConsumeValueOrDie());
  auto result2 = rule2.Execute(graph.get());
  ASSERT_OK(result1);
  ASSERT_TRUE(result2.ConsumeValueOrDie());

  EXPECT_FALSE(graph->HasNode(group_by_node_id));
  // Make sure no groupby sticks around either
  for (int64_t i : graph->dag().TopologicalSort()) {
    EXPECT_FALSE(Match(graph->Get(i), GroupBy())) << absl::Substitute("Node $0 is a groupby()", i);
  }

  EXPECT_THAT(agg->parents(), ElementsAre(mem_source));

  std::vector<std::string> actual_group_names;
  std::vector<int64_t> actual_group_ids;
  for (ColumnIR* g : agg->groups()) {
    actual_group_names.push_back(g->col_name());
    actual_group_ids.push_back(g->id());
  }
  EXPECT_THAT(actual_group_ids, Not(UnorderedElementsAreArray(groupby_ids)));

  EXPECT_MATCH(agg->groups()[0], Metadata());
  EXPECT_TRUE(!Match(agg->groups()[1], Metadata()));
  EXPECT_MATCH(agg->groups()[1], ColumnNode());
}

TEST_F(RulesTest, UniqueSinkNameRule) {
  MemorySourceIR* mem_src = MakeMemSource();
  MemorySinkIR* foo1 = MakeMemSink(mem_src, "foo");
  MemorySinkIR* foo2 = MakeMemSink(mem_src, "foo");
  MemorySinkIR* foo3 = MakeMemSink(mem_src, "foo");
  MemorySinkIR* bar1 = MakeMemSink(mem_src, "bar");
  MemorySinkIR* bar2 = MakeMemSink(mem_src, "bar");
  MemorySinkIR* abc = MakeMemSink(mem_src, "abc");

  UniqueSinkNameRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  std::vector<std::string> expected_sink_names{"foo", "foo_1", "foo_2", "bar", "bar_1", "abc"};
  std::vector<MemorySinkIR*> sinks{foo1, foo2, foo3, bar1, bar2, abc};
  for (const auto& [idx, sink] : Enumerate(sinks)) {
    EXPECT_EQ(sink->name(), expected_sink_names[idx]);
  }
}

TEST_F(RulesTest, CombineConsecutiveMapsRule_basic) {
  MemorySourceIR* mem_src = MakeMemSource();

  ColumnExpression parent_expr{"count_1", MakeColumn("count", 0)};
  ColumnExpression child_expr{"cpu0_1", MakeColumn("cpu0", 0)};

  auto map1 = MakeMap(mem_src, {parent_expr}, true);
  auto map2 = MakeMap(map1, {child_expr}, true);
  auto map2_id = map2->id();
  auto sink1 = MakeMemSink(map2, "abc");
  auto sink2 = MakeMemSink(map2, "def");

  CombineConsecutiveMapsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_TRUE(graph->HasNode(map1->id()));
  EXPECT_FALSE(graph->HasNode(map2_id));
  EXPECT_THAT(map1->Children(), ElementsAre(sink1, sink2));

  auto expected_map = MakeMap(mem_src, {parent_expr, child_expr}, true);
  CompareClone(expected_map, map1, "Map node");
}

TEST_F(RulesTest, CombineConsecutiveMapsRule_multiple_with_break) {
  MemorySourceIR* mem_src = MakeMemSource();

  ColumnExpression expr1{"count_1", MakeColumn("count", 0)};
  ColumnExpression expr2{"cpu0_1", MakeColumn("cpu0", 0)};
  ColumnExpression expr3{"cpu_sum", MakeAddFunc(MakeColumn("cpu1", 0), MakeColumn("cpu2", 0))};
  // Should break here because cpu_sum was used prior
  ColumnExpression expr4{"cpu_sum_1", MakeColumn("cpu_sum", 0)};

  auto map1 = MakeMap(mem_src, {expr1}, true);
  auto map2 = MakeMap(map1, {expr2}, true);
  auto map3 = MakeMap(map2, {expr3}, true);
  auto map4 = MakeMap(map3, {expr4}, true);
  auto map2_id = map2->id();
  auto map3_id = map3->id();

  auto sink1 = MakeMemSink(map4, "abc");
  auto sink2 = MakeMemSink(map4, "def");

  CombineConsecutiveMapsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_TRUE(graph->HasNode(map1->id()));
  EXPECT_FALSE(graph->HasNode(map2_id));
  EXPECT_FALSE(graph->HasNode(map3_id));
  EXPECT_TRUE(graph->HasNode(map4->id()));
  EXPECT_THAT(map1->Children(), ElementsAre(map4));
  EXPECT_THAT(map4->Children(), ElementsAre(sink1, sink2));

  auto expected_map = MakeMap(mem_src, {expr1, expr2, expr3}, true);
  CompareClone(expected_map, map1, "Map node");
}

TEST_F(RulesTest, CombineConsecutiveMapsRule_name_reassignment) {
  MemorySourceIR* mem_src = MakeMemSource();

  ColumnExpression parent_expr{"count_1", MakeColumn("count", 0)};
  ColumnExpression child_expr{"count_1", MakeColumn("count", 0)};

  auto map1 = MakeMap(mem_src, {parent_expr}, false);
  auto map2 = MakeMap(map1, {child_expr}, true);
  auto sink1 = MakeMemSink(map2, "abc");
  auto sink2 = MakeMemSink(map2, "def");
  auto map2_id = map2->id();

  CombineConsecutiveMapsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_TRUE(graph->HasNode(map1->id()));
  EXPECT_FALSE(graph->HasNode(map2_id));
  EXPECT_THAT(map1->Children(), ElementsAre(sink1, sink2));

  auto expected_map = MakeMap(mem_src, {child_expr}, true);
  CompareClone(expected_map, map1, "Map node");
}

TEST_F(RulesTest, CombineConsecutiveMapsRule_use_output_column) {
  MemorySourceIR* mem_src = MakeMemSource();

  ColumnExpression parent_expr{"count_1", MakeColumn("count", 0)};
  ColumnExpression child_expr{"sum", MakeAddFunc(MakeColumn("count", 0), MakeColumn("count_1", 0))};

  auto map1 = MakeMap(mem_src, {parent_expr}, false);
  auto map2 = MakeMap(map1, {child_expr}, true);
  MakeMemSink(map2, "abc");
  MakeMemSink(map2, "def");

  CombineConsecutiveMapsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_FALSE(result.ConsumeValueOrDie());
}

TEST_F(RulesTest, CombineConsecutiveMapsRule_dependencies) {
  MemorySourceIR* mem_src = MakeMemSource();

  ColumnExpression parent_expr{"count_1", MakeColumn("count", 0)};
  ColumnExpression child_expr{"cpu0_1", MakeColumn("cpu0", 0)};

  auto map1 = MakeMap(mem_src, {parent_expr}, true);
  MakeMap(map1, {child_expr}, true);
  MakeMemSink(map1, "abc");
  MakeMemSink(map1, "def");

  CombineConsecutiveMapsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_FALSE(result.ConsumeValueOrDie());
}

TEST_F(RulesTest, CombineConsecutiveMapsRule_parent_dont_keep_input_columns) {
  MemorySourceIR* mem_src = MakeMemSource();

  ColumnExpression parent_expr{"count_1", MakeColumn("count", 0)};
  ColumnExpression child_expr{"cpu0_1", MakeColumn("cpu0", 0)};

  auto map1 = MakeMap(mem_src, {parent_expr}, false);
  auto map2 = MakeMap(map1, {child_expr}, true);
  auto map2_id = map2->id();
  auto sink1 = MakeMemSink(map2, "abc");
  auto sink2 = MakeMemSink(map2, "def");

  CombineConsecutiveMapsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_TRUE(graph->HasNode(map1->id()));
  EXPECT_FALSE(graph->HasNode(map2_id));
  EXPECT_THAT(map1->Children(), ElementsAre(sink1, sink2));

  auto expected_map = MakeMap(mem_src, {parent_expr, child_expr}, true);
  CompareClone(expected_map, map1, "Map node");
}

TEST_F(RulesTest, CombineConsecutiveMapsRule_child_dont_keep_input_columns) {
  MemorySourceIR* mem_src = MakeMemSource();

  ColumnExpression parent_expr{"count_1", MakeColumn("count", 0)};
  ColumnExpression child_expr{"cpu0_1", MakeColumn("cpu0", 0)};

  auto map1 = MakeMap(mem_src, {parent_expr}, true);
  MakeMap(map1, {child_expr}, false);
  MakeMemSink(map1, "abc");
  MakeMemSink(map1, "def");

  CombineConsecutiveMapsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_FALSE(result.ConsumeValueOrDie());
}

TEST_F(RulesTest, PruneUnusedColumnsRule_basic) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());

  ColumnExpression expr1{"count_1", MakeColumn("count", 0)};
  ColumnExpression expr2{"cpu0_1", MakeColumn("cpu0", 0)};

  auto map = MakeMap(mem_src, {expr1, expr2}, false);
  Relation map_relation{{types::DataType::INT64, types::DataType::FLOAT64}, {"count_1", "cpu0_1"}};
  ASSERT_OK(map->SetRelation(map_relation));

  auto sink = MakeMemSink(map, "abc", {"cpu0_1"});
  Relation sink_relation{{types::DataType::FLOAT64}, {"cpu0_1"}};
  ASSERT_OK(sink->SetRelation(sink_relation));

  PruneUnusedColumnsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_EQ(mem_src->relation(), Relation({types::DataType::FLOAT64}, {"cpu0"}));
  EXPECT_THAT(mem_src->column_names(), ElementsAre("cpu0"));

  EXPECT_EQ(map->relation(), sink_relation);
  EXPECT_EQ(1, map->col_exprs().size());
  EXPECT_EQ(expr2.name, map->col_exprs()[0].name);
  EXPECT_EQ(expr2.node, map->col_exprs()[0].node);

  // Should be unchanged
  EXPECT_EQ(sink_relation, sink->relation());
}

TEST_F(RulesTest, PruneUnusedColumnsRule_filter) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());

  ColumnExpression expr1{"count_1", MakeColumn("count", 0)};
  ColumnExpression expr2{"cpu0_1", MakeColumn("cpu0", 0)};

  auto map = MakeMap(mem_src, {expr1, expr2}, false);
  Relation map_relation{{types::DataType::INT64, types::DataType::FLOAT64}, {"count_1", "cpu0_1"}};
  ASSERT_OK(map->SetRelation(map_relation));

  auto filter = MakeFilter(map, MakeEqualsFunc(MakeColumn("count_1", 0), MakeColumn("cpu0_1", 0)));
  ASSERT_OK(filter->SetRelation(map_relation));

  auto sink = MakeMemSink(filter, "abc", {"cpu0_1"});
  Relation sink_relation{{types::DataType::FLOAT64}, {"cpu0_1"}};
  ASSERT_OK(sink->SetRelation(sink_relation));

  PruneUnusedColumnsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_EQ(mem_src->relation(),
            Relation({types::DataType::INT64, types::DataType::FLOAT64}, {
                                                                             "count",
                                                                             "cpu0",
                                                                         }));
  EXPECT_THAT(mem_src->column_names(), ElementsAre("count", "cpu0"));

  EXPECT_EQ(map_relation, map->relation());
  EXPECT_EQ(2, map->col_exprs().size());
  EXPECT_EQ(expr1.name, map->col_exprs()[0].name);
  EXPECT_EQ(expr1.node, map->col_exprs()[0].node);
  EXPECT_EQ(expr2.name, map->col_exprs()[1].name);
  EXPECT_EQ(expr2.node, map->col_exprs()[1].node);

  // Should be unchanged
  EXPECT_EQ(sink_relation, sink->relation());
}

TEST_F(RulesTest, PruneUnusedColumnsRule_two_filters) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());

  auto filter1 = MakeFilter(mem_src, MakeEqualsFunc(MakeColumn("count", 0), MakeInt(10)));
  ASSERT_OK(filter1->SetRelation(MakeRelation()));
  auto filter2 = MakeFilter(filter1, MakeEqualsFunc(MakeColumn("cpu0", 0), MakeColumn("cpu1", 0)));
  ASSERT_OK(filter2->SetRelation(MakeRelation()));

  auto sink = MakeMemSink(filter2, "abc", {"cpu2"});
  Relation sink_relation{{types::DataType::FLOAT64}, {"cpu2"}};
  ASSERT_OK(sink->SetRelation(sink_relation));

  PruneUnusedColumnsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_THAT(mem_src->relation().col_names(), ElementsAre("count", "cpu0", "cpu1", "cpu2"));
  EXPECT_THAT(filter1->relation().col_names(), ElementsAre("cpu0", "cpu1", "cpu2"));
  EXPECT_THAT(filter2->relation().col_names(), ElementsAre("cpu2"));

  // Should be unchanged
  EXPECT_EQ(sink_relation, sink->relation());
}

TEST_F(RulesTest, PruneUnusedColumnsRule_multiparent) {
  Relation relation0{{types::DataType::INT64, types::DataType::INT64, types::DataType::INT64,
                      types::DataType::INT64},
                     {"left_only", "col1", "col2", "col3"}};
  auto mem_src1 = MakeMemSource(relation0);

  Relation relation1{{types::DataType::INT64, types::DataType::INT64, types::DataType::INT64,
                      types::DataType::INT64, types::DataType::INT64},
                     {"right_only", "col1", "col2", "col3", "col4"}};
  auto mem_src2 = MakeMemSource(relation1);

  auto join_op = MakeJoin({mem_src1, mem_src2}, "inner", relation0, relation1,
                          std::vector<std::string>{"col1"}, std::vector<std::string>{"col2"});

  std::vector<std::string> join_out_cols{"right_only", "col2_right", "left_only", "col1_left"};
  ASSERT_OK(join_op->SetOutputColumns(join_out_cols,
                                      {MakeColumn("right_only", 1), MakeColumn("col2", 1),
                                       MakeColumn("left_only", 0), MakeColumn("col1", 0)}));
  Relation join_relation{{types::DataType::INT64, types::DataType::INT64, types::DataType::INT64,
                          types::DataType::INT64},
                         join_out_cols};
  ASSERT_OK(join_op->SetRelation(join_relation));

  std::vector<std::string> sink_out_cols{"right_only", "col1_left"};
  auto sink = MakeMemSink(join_op, "abc", sink_out_cols);
  Relation sink_relation{{types::DataType::INT64, types::DataType::INT64}, sink_out_cols};
  ASSERT_OK(sink->SetRelation(sink_relation));

  PruneUnusedColumnsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  // Check mem sources
  Relation mem_src1_relation{{types::DataType::INT64}, {"col1"}};
  EXPECT_EQ(mem_src1_relation, mem_src1->relation());
  EXPECT_THAT(mem_src1->column_names(), ElementsAre("col1"));

  Relation mem_src2_relation{{types::DataType::INT64, types::DataType::INT64},
                             {"right_only", "col2"}};
  EXPECT_EQ(mem_src2_relation, mem_src2->relation());
  EXPECT_THAT(mem_src2->column_names(), ElementsAre("right_only", "col2"));

  // Check join
  Relation new_join_relation{{types::DataType::INT64, types::DataType::INT64},
                             {"right_only", "col1_left"}};
  EXPECT_EQ(new_join_relation, join_op->relation());
  EXPECT_EQ(2, join_op->output_columns().size());
  EXPECT_EQ("right_only", join_op->output_columns()[0]->col_name());
  EXPECT_EQ(1, join_op->output_columns()[0]->container_op_parent_idx());
  EXPECT_EQ("col1", join_op->output_columns()[1]->col_name());
  EXPECT_EQ(0, join_op->output_columns()[1]->container_op_parent_idx());
  EXPECT_THAT(join_op->column_names(), ElementsAre("right_only", "col1_left"));

  // Check mem sink, should be unchanged
  EXPECT_EQ(sink_relation, sink->relation());
}

TEST_F(RulesTest, PruneUnusedColumnsRule_unchanged) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());

  ColumnExpression expr1{"count_1", MakeColumn("count", 0)};
  ColumnExpression expr2{"cpu0_1", MakeColumn("cpu0", 0)};
  ColumnExpression expr3{"cpu1_1", MakeColumn("cpu1", 0)};
  ColumnExpression expr4{"cpu2_1", MakeColumn("cpu2", 0)};

  auto map = MakeMap(mem_src, {expr1, expr2, expr3, expr4}, false);
  std::vector<std::string> out_cols{"count_1", "cpu0_1", "cpu1_1", "cpu2_1"};
  Relation relation{{types::DataType::INT64, types::DataType::FLOAT64, types::DataType::FLOAT64,
                     types::DataType::FLOAT64},
                    out_cols};
  ASSERT_OK(map->SetRelation(relation));

  auto sink = MakeMemSink(map, "abc", out_cols);
  ASSERT_OK(sink->SetRelation(relation));

  PruneUnusedColumnsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_FALSE(result.ConsumeValueOrDie());
}

TEST_F(RulesTest, CleanUpStrayIRNodesRule_basic) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());
  auto count_col = MakeColumn("count", 0);
  auto cpu1_col = MakeColumn("cpu1", 0);
  auto cpu2_col = MakeColumn("cpu2", 0);
  auto cpu_sum = MakeAddFunc(cpu1_col, cpu2_col);
  ColumnExpression expr1{"count_1", count_col};
  ColumnExpression expr2{"cpu_sum", cpu_sum};
  ColumnExpression expr3{"cpu1_1", cpu1_col};

  MakeMap(mem_src, {expr1, expr2}, false);
  MakeMap(mem_src, {expr1, expr3}, false);

  auto non_stray_nodes = graph->dag().TopologicalSort();

  auto not_in_op_col = MakeColumn("not_in_op", 0);
  auto not_in_op_int = MakeInt(10);
  auto not_in_op_func = MakeAddFunc(not_in_op_col, not_in_op_int);
  auto not_in_op_col_id = not_in_op_col->id();
  auto not_in_op_int_id = not_in_op_int->id();
  auto not_in_op_func_id = not_in_op_func->id();

  CleanUpStrayIRNodesRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_EQ(non_stray_nodes, graph->dag().TopologicalSort());
  EXPECT_FALSE(graph->HasNode(not_in_op_int_id));
  EXPECT_FALSE(graph->HasNode(not_in_op_col_id));
  EXPECT_FALSE(graph->HasNode(not_in_op_func_id));

  result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_FALSE(result.ConsumeValueOrDie());
}

TEST_F(RulesTest, CleanUpStrayIRNodesRule_mixed_parents) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());
  auto count_col = MakeColumn("count", 0);
  auto cpu1_col = MakeColumn("cpu1", 0);
  auto cpu2_col = MakeColumn("cpu2", 0);
  auto cpu_sum = MakeAddFunc(cpu1_col, cpu2_col);
  ColumnExpression expr1{"count_1", count_col};
  ColumnExpression expr2{"cpu_sum", cpu_sum};
  ColumnExpression expr3{"cpu1_1", cpu1_col};

  MakeMap(mem_src, {expr1, expr2}, false);
  MakeMap(mem_src, {expr1, expr3}, false);

  auto non_stray_nodes = graph->dag().TopologicalSort();

  auto not_in_op_col = MakeColumn("not_in_op", 0);
  auto not_in_op_func = MakeAddFunc(not_in_op_col, cpu1_col);
  auto not_in_op_nested_func = MakeAddFunc(not_in_op_col, cpu_sum);
  auto not_in_op_col_id = not_in_op_col->id();
  auto not_in_op_func_id = not_in_op_func->id();
  auto not_in_op_nested_func_id = not_in_op_nested_func->id();

  CleanUpStrayIRNodesRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_EQ(non_stray_nodes, graph->dag().TopologicalSort());
  EXPECT_FALSE(graph->HasNode(not_in_op_col_id));
  EXPECT_FALSE(graph->HasNode(not_in_op_func_id));
  EXPECT_FALSE(graph->HasNode(not_in_op_nested_func_id));
}

TEST_F(RulesTest, CleanUpStrayIRNodesRule_unchanged) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());
  auto count_col = MakeColumn("count", 0);
  auto cpu1_col = MakeColumn("cpu1", 0);
  auto cpu2_col = MakeColumn("cpu2", 0);
  auto cpu_sum = MakeAddFunc(cpu1_col, cpu2_col);
  ColumnExpression expr1{"count_1", count_col};
  ColumnExpression expr2{"cpu_sum", cpu_sum};
  ColumnExpression expr3{"cpu1_1", cpu1_col};

  MakeMap(mem_src, {expr1, expr2}, false);
  MakeMap(mem_src, {expr1, expr3}, false);

  auto nodes_before = graph->dag().TopologicalSort();

  CleanUpStrayIRNodesRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_FALSE(result.ConsumeValueOrDie());

  EXPECT_EQ(nodes_before, graph->dag().TopologicalSort());
}

TEST_F(RulesTest, PruneUnconnectedOperatorsRule_basic) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());

  ColumnExpression expr1{"count_1", MakeColumn("count", 0)};
  ColumnExpression expr2{"cpu0_1", MakeColumn("cpu0", 0)};

  auto map1 = MakeMap(mem_src, {expr1}, false);
  Relation map1_relation{{types::DataType::INT64}, {"count_1"}};
  ASSERT_OK(map1->SetRelation(map1_relation));
  auto map1_id = map1->id();

  auto map2 = MakeMap(mem_src, {expr2}, false);
  Relation map2_relation{{types::DataType::FLOAT64}, {"cpu0_1"}};
  ASSERT_OK(map2->SetRelation(map2_relation));
  auto map2_id = map2->id();

  auto sink = MakeMemSink(map2, "abc", {"cpu0_1"});
  Relation sink_relation{{types::DataType::FLOAT64}, {"cpu0_1"}};
  ASSERT_OK(sink->SetRelation(sink_relation));

  PruneUnconnectedOperatorsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ConsumeValueOrDie());

  EXPECT_TRUE(graph->HasNode(map2_id));
  EXPECT_FALSE(graph->HasNode(map1_id));

  // Should be unchanged
  EXPECT_EQ(sink_relation, sink->relation());
}

TEST_F(RulesTest, PruneUnconnectedOperatorsRule_unchanged) {
  MemorySourceIR* mem_src = MakeMemSource(MakeRelation());

  auto count_col = MakeColumn("count", 0);
  auto cpu1_col = MakeColumn("cpu1", 0);
  auto cpu2_col = MakeColumn("cpu2", 0);
  auto cpu_sum = MakeAddFunc(cpu1_col, cpu2_col);
  ColumnExpression expr1{"count_1", count_col};
  ColumnExpression expr2{"cpu_sum", cpu_sum};
  ColumnExpression expr3{"cpu1_1", cpu1_col};

  auto map1 = MakeMap(mem_src, {expr1, expr2}, false);
  auto map2 = MakeMap(mem_src, {expr1, expr3}, false);

  MakeMemSink(map1, "out1", {"count_1", "cpu_sum"});
  MakeMemSink(map2, "out2", {"count_1", "cpu1_1"});

  auto nodes_before = graph->dag().TopologicalSort();

  PruneUnconnectedOperatorsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_FALSE(result.ConsumeValueOrDie());

  EXPECT_EQ(nodes_before, graph->dag().TopologicalSort());
}

TEST_F(RulesTest, AddLimitToMemorySinkRuleTest_basic) {
  MemorySourceIR* src = MakeMemSource(MakeRelation());
  MemorySinkIR* sink = MakeMemSink(src, "foo", {});

  auto compiler_state =
      std::make_unique<CompilerState>(std::make_unique<RelationMap>(), info_.get(), time_now, 1000);

  AddLimitToMemorySinkRule rule(compiler_state.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(3, graph->FindNodesThatMatch(Operator()).size());
  auto limit_nodes = graph->FindNodesOfType(IRNodeType::kLimit);
  EXPECT_EQ(1, limit_nodes.size());

  auto limit = static_cast<LimitIR*>(limit_nodes[0]);
  EXPECT_TRUE(limit->limit_value_set());
  EXPECT_EQ(1000, limit->limit_value());
  EXPECT_THAT(sink->parents(), ElementsAre(limit));
  EXPECT_THAT(limit->parents(), ElementsAre(src));
}

TEST_F(RulesTest, AddLimitToMemorySinkRuleTest_overwrite_higher) {
  MemorySourceIR* src = MakeMemSource(MakeRelation());
  auto limit = graph->CreateNode<LimitIR>(ast, src, 1001).ValueOrDie();
  MakeMemSink(limit, "foo", {});

  auto compiler_state =
      std::make_unique<CompilerState>(std::make_unique<RelationMap>(), info_.get(), time_now, 1000);

  AddLimitToMemorySinkRule rule(compiler_state.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(3, graph->FindNodesThatMatch(Operator()).size());
  auto limit_nodes = graph->FindNodesOfType(IRNodeType::kLimit);
  EXPECT_EQ(1, limit_nodes.size());
  EXPECT_EQ(1000, limit->limit_value());
}

TEST_F(RulesTest, AddLimitToMemorySinkRuleTest_dont_overwrite_lower) {
  MemorySourceIR* src = MakeMemSource(MakeRelation());
  auto limit = graph->CreateNode<LimitIR>(ast, src, 999).ValueOrDie();
  MakeMemSink(limit, "foo", {});

  auto compiler_state =
      std::make_unique<CompilerState>(std::make_unique<RelationMap>(), info_.get(), time_now, 1000);

  AddLimitToMemorySinkRule rule(compiler_state.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
}

TEST_F(RulesTest, AddLimitToMemorySinkRuleTest_skip_if_no_limit) {
  MemorySourceIR* src = MakeMemSource(MakeRelation());
  MakeMemSink(src, "foo", {});

  auto compiler_state =
      std::make_unique<CompilerState>(std::make_unique<RelationMap>(), info_.get(), time_now);

  AddLimitToMemorySinkRule rule(compiler_state.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
}

TEST_F(RulesTest, PropagateExpressionAnnotationsRule_noop) {
  Relation relation({types::DataType::INT64, types::DataType::INT64}, {"abc", "xyz"});
  MemorySourceIR* src = MakeMemSource(relation);
  MapIR* map1 = MakeMap(src, {{"def", MakeColumn("abc", 0)}}, false);
  MapIR* map2 = MakeMap(map1, {{"xyz", MakeInt(3)}, {"def", MakeColumn("def", 0)}}, false);
  FilterIR* filter = MakeFilter(map2, MakeEqualsFunc(MakeColumn("def", 0), MakeInt(2)));
  MakeMemSink(filter, "foo", {});

  PropagateExpressionAnnotationsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
}

TEST_F(RulesTest, PropagateExpressionAnnotationsRule_rename) {
  Relation relation({types::DataType::INT64, types::DataType::INT64}, {"abc", "xyz"});
  MemorySourceIR* src = MakeMemSource(relation);
  auto map1_col = MakeColumn("abc", 0);
  auto annotations = ExpressionIR::Annotations(MetadataType::POD_NAME);
  map1_col->set_annotations(annotations);

  MapIR* map1 = MakeMap(src, {{"def", map1_col}}, false);
  auto map2_col1 = MakeColumn("def", 0);
  auto map2_col2 = MakeColumn("def", 0);
  MapIR* map2 = MakeMap(map1, {{"xyz", map2_col1}, {"def", map2_col2}, {"ghi", MakeInt(2)}}, false);
  auto filter_col1 = MakeColumn("xyz", 0);
  auto filter_col2 = MakeColumn("ghi", 0);
  FilterIR* filter = MakeFilter(map2, MakeEqualsFunc(filter_col1, filter_col2));
  MakeMemSink(filter, "foo", {});

  auto default_annotations = ExpressionIR::Annotations();
  EXPECT_EQ(default_annotations, map2_col1->annotations());
  EXPECT_EQ(default_annotations, map2_col2->annotations());
  ASSERT_MATCH(filter->filter_expr(), Func());
  auto filter_func = static_cast<FuncIR*>(filter->filter_expr());
  EXPECT_EQ(default_annotations, filter_func->args()[0]->annotations());

  PropagateExpressionAnnotationsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(annotations, map1_col->annotations());
  EXPECT_EQ(annotations, map2_col1->annotations());
  EXPECT_EQ(annotations, map2_col2->annotations());
  EXPECT_EQ(annotations, filter_func->args()[0]->annotations());
  EXPECT_EQ(default_annotations, filter_func->args()[1]->annotations());
}

TEST_F(RulesTest, PropagateExpressionAnnotationsRule_join) {
  std::string join_key = "key";
  Relation rel1({types::FLOAT64, types::STRING}, {"latency", "data"});
  Relation rel2({types::STRING, types::FLOAT64}, {join_key, "cpu_usage"});

  auto mem_src1 = MakeMemSource(rel1);
  auto literal_with_annotations = MakeString("my_pod_name");
  auto annotations = ExpressionIR::Annotations(MetadataType::POD_NAME);
  literal_with_annotations->set_annotations(annotations);

  auto map = MakeMap(mem_src1, {
                                   {join_key, literal_with_annotations},
                                   {"latency", MakeColumn("latency", 0)},
                                   {"data", MakeColumn("data", 0)},
                               });

  auto mem_src2 = MakeMemSource(rel2);

  std::string left_suffix = "_x";
  std::string right_suffix = "_y";

  JoinIR* join = graph
                     ->CreateNode<JoinIR>(ast, std::vector<OperatorIR*>{map, mem_src2}, "inner",
                                          std::vector<ColumnIR*>{MakeColumn(join_key, 0)},
                                          std::vector<ColumnIR*>{MakeColumn(join_key, 1)},
                                          std::vector<std::string>{left_suffix, right_suffix})
                     .ConsumeValueOrDie();
  auto map_col1 = MakeColumn("key_x", 0);
  auto map_col2 = MakeColumn("latency", 0);
  auto last_node =
      MakeMap(join, {{"annotations_col", map_col1}, {"non_annotations_col", map_col2}});
  MakeMemSink(last_node, "foo", {});

  // use this to set data types, this rule will run before PropagateExpressionAnnotationsRule.
  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  // Use this to set output columns, this rule will run before PropagateExpressionAnnotationsRule.
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ValueOrDie());

  auto default_annotations = ExpressionIR::Annotations();

  EXPECT_EQ(join->relation(),
            Relation({types::STRING, types::FLOAT64, types::STRING, types::STRING, types::FLOAT64},
                     {"key_x", "latency", "data", "key_y", "cpu_usage"}));

  PropagateExpressionAnnotationsRule rule;
  result = rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ValueOrDie());

  EXPECT_EQ(annotations, join->output_columns()[0]->annotations());
  for (size_t i = 1; i < join->output_columns().size(); ++i) {
    EXPECT_EQ(default_annotations, join->output_columns()[i]->annotations());
  }
  EXPECT_EQ(annotations, map_col1->annotations());
  EXPECT_EQ(default_annotations, map_col2->annotations());
}

TEST_F(RulesTest, PropagateExpressionAnnotationsRule_agg) {
  Relation relation({types::DataType::INT64, types::DataType::INT64}, {"abc", "xyz"});
  MemorySourceIR* src = MakeMemSource(relation);
  // Set up the columns and their annotations.
  auto group_col = MakeColumn("abc", 0);
  auto agg_col = MakeColumn("xyz", 0);
  auto agg_func = MakeMeanFunc(agg_col);
  auto group_col_annotation = ExpressionIR::Annotations(MetadataType::POD_NAME);
  auto agg_col_annotation = ExpressionIR::Annotations(MetadataType::SERVICE_ID);
  auto agg_func_annotation = ExpressionIR::Annotations(MetadataType::POD_ID);
  group_col->set_annotations(group_col_annotation);
  agg_col->set_annotations(agg_col_annotation);
  agg_func->set_annotations(agg_func_annotation);

  BlockingAggIR* agg = MakeBlockingAgg(src, {group_col}, {{"out", agg_func}});
  auto filter_col = MakeColumn("out", 0);
  FilterIR* filter = MakeFilter(agg, MakeEqualsFunc(filter_col, MakeInt(2)));
  auto map_expr_col = MakeColumn("out", 0);
  auto map_group_col = MakeColumn("abc", 0);
  MapIR* map = MakeMap(filter, {{"agg_expr", map_expr_col}, {"agg_group", map_group_col}});
  MakeMemSink(map, "");

  PropagateExpressionAnnotationsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(agg_func_annotation, filter_col->annotations());
  EXPECT_EQ(agg_func_annotation, map_expr_col->annotations());
  EXPECT_EQ(group_col_annotation, map_group_col->annotations());
}

TEST_F(RulesTest, PropagateExpressionAnnotationsRule_union) {
  // Test to make sure that union columns that share annotations produce those annotations in the
  // union output, whereas annotations that are not shared are not produced in the output.
  Relation relation1({types::DataType::STRING, types::DataType::STRING}, {"pod_id", "pod_name"});
  Relation relation2({types::DataType::STRING, types::DataType::STRING},
                     {"pod_id", "random_string"});
  auto mem_src1 = MakeMemSource(relation1);
  auto mem_src2 = MakeMemSource(relation2);

  auto map1_col1 = MakeColumn("pod_id", 0);
  auto map1_col2 = MakeColumn("pod_name", 0);
  auto map2_col1 = MakeColumn("pod_id", 0);
  auto map2_col2 = MakeColumn("random_string", 0);

  auto map1 = MakeMap(mem_src1, {{"pod_id", map1_col1}, {"maybe_pod_name", map1_col2}});
  auto map2 = MakeMap(mem_src2, {{"pod_id", map2_col1}, {"maybe_pod_name", map2_col2}});

  auto union_op = MakeUnion({map1, map2});

  auto map3_col1 = MakeColumn("pod_id", 0);
  auto map3_col2 = MakeColumn("maybe_pod_name", 0);
  MakeMap(union_op, {{"pod_id", map3_col1}, {"maybe_pod_name", map3_col2}});

  // add metadata:
  auto pod_id_annotation = ExpressionIR::Annotations(MetadataType::POD_ID);
  auto pod_name_annotation = ExpressionIR::Annotations(MetadataType::POD_NAME);
  auto default_annotation = ExpressionIR::Annotations();
  map1_col1->set_annotations(pod_id_annotation);
  map2_col1->set_annotations(pod_id_annotation);
  map1_col2->set_annotations(pod_name_annotation);

  EXPECT_EQ(default_annotation, map3_col1->annotations());
  EXPECT_EQ(default_annotation, map3_col2->annotations());

  // use this to set data types, this rule will run before PropagateExpressionAnnotationsRule.
  DataTypeRule data_rule(compiler_state_.get());
  auto result = data_rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());
  // Use this to set output columns, this rule will run before PropagateExpressionAnnotationsRule.
  OperatorRelationRule op_rel_rule(compiler_state_.get());
  result = op_rel_rule.Execute(graph.get());
  ASSERT_OK(result);
  ASSERT_TRUE(result.ValueOrDie());

  PropagateExpressionAnnotationsRule rule;
  result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(pod_id_annotation, map3_col1->annotations());
  EXPECT_EQ(default_annotation, map3_col2->annotations());
}

TEST_F(RulesTest, PropagateExpressionAnnotationsRule_filter_limit) {
  Relation relation({types::DataType::INT64, types::DataType::INT64}, {"abc", "xyz"});
  MemorySourceIR* src = MakeMemSource(relation);

  auto map1_col = MakeColumn("abc", 0);
  auto annotations = ExpressionIR::Annotations(MetadataType::POD_NAME);
  auto default_annotation = ExpressionIR::Annotations();
  map1_col->set_annotations(annotations);

  auto map1 = MakeMap(src, {{"abc_1", map1_col}, {"xyz_1", MakeColumn("xyz", 0)}});
  auto limit1 = MakeLimit(map1, 100);
  auto filter1 = MakeFilter(limit1);
  auto limit2 = MakeLimit(filter1, 10);
  auto filter2 = MakeFilter(limit2);

  auto map1_col1 = MakeColumn("abc_1", 0);
  auto map1_col2 = MakeColumn("xyz_1", 0);
  MakeMap(filter2, {{"foo", map1_col2}, {"bar", map1_col1}});

  EXPECT_EQ(default_annotation, map1_col1->annotations());
  EXPECT_EQ(default_annotation, map1_col2->annotations());

  PropagateExpressionAnnotationsRule rule;
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(annotations, map1_col1->annotations());
  EXPECT_EQ(default_annotation, map1_col2->annotations());
}

TEST_F(RulesTest, ResolveMetadataPropertyRuleTest) {
  auto metadata_name = "pod_name";
  MetadataIR* metadata_ir = MakeMetadataIR(metadata_name, /* parent_op_idx */ 0);
  MakeMap(MakeMemSource(), {{"md", metadata_ir}});

  EXPECT_FALSE(metadata_ir->has_property());

  ResolveMetadataPropertyRule rule(compiler_state_.get(), md_handler.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_TRUE(metadata_ir->has_property());
  EXPECT_EQ(MetadataType::POD_NAME, metadata_ir->property()->metadata_type());
  EXPECT_EQ(types::DataType::STRING, metadata_ir->property()->column_type());
}

TEST_F(RulesTest, ResolveMetadataPropertyRuleTest_noop) {
  auto metadata_name = "pod_name";
  MetadataIR* metadata_ir = MakeMetadataIR(metadata_name, /* parent_op_idx */ 0);
  MakeMap(MakeMemSource(), {{"md", metadata_ir}});

  EXPECT_FALSE(metadata_ir->has_property());
  MetadataProperty* property = md_handler->GetProperty(metadata_name).ValueOrDie();
  metadata_ir->set_property(property);

  ResolveMetadataPropertyRule rule(compiler_state_.get(), md_handler.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_FALSE(result.ValueOrDie());
}

TEST_F(RulesTest, ConvertMetadataRuleTest_multichild) {
  auto relation = table_store::schema::Relation(cpu_relation);
  MetadataType conversion_column = MetadataType::UPID;
  std::string conversion_column_str = MetadataProperty::GetMetadataString(conversion_column);
  relation.AddColumn(types::DataType::UINT128, conversion_column_str);

  auto metadata_name = "pod_name";
  MetadataProperty* property = md_handler->GetProperty(metadata_name).ValueOrDie();
  MetadataIR* metadata_ir = MakeMetadataIR(metadata_name, /* parent_op_idx */ 0);
  metadata_ir->set_property(property);

  auto src = MakeMemSource(relation);
  auto map1 = MakeMap(src, {{"md", metadata_ir}});
  auto map2 = MakeMap(src, {{"other_col", MakeInt(2)}, {"md", metadata_ir}});
  auto filter = MakeFilter(src, MakeEqualsFunc(metadata_ir, MakeString("pl/foobar")));

  ConvertMetadataRule rule(compiler_state_.get());
  auto result = rule.Execute(graph.get());
  ASSERT_OK(result);
  EXPECT_TRUE(result.ValueOrDie());

  EXPECT_EQ(0, graph->FindNodesThatMatch(Metadata()).size());

  // Check the contents of the new func.
  EXPECT_MATCH(filter->filter_expr(), Equals(Func(), String()));
  auto converted_md = static_cast<FuncIR*>(filter->filter_expr())->args()[0];
  EXPECT_MATCH(converted_md, Func());
  auto converted_md_func = static_cast<FuncIR*>(converted_md);
  EXPECT_EQ(absl::Substitute("$0_to_$1", MetadataProperty::kUniquePIDColumn, metadata_name),
            converted_md_func->func_name());
  EXPECT_EQ(1, converted_md_func->args().size());
  auto input_col = converted_md_func->args()[0];
  EXPECT_MATCH(input_col, ColumnNode(MetadataProperty::kUniquePIDColumn));

  EXPECT_MATCH(converted_md, ResolvedExpression());
  EXPECT_MATCH(input_col, ResolvedExpression());
  EXPECT_EQ(types::DataType::STRING, converted_md->EvaluatedDataType());
  EXPECT_EQ(types::DataType::UINT128, input_col->EvaluatedDataType());
  EXPECT_EQ(ExpressionIR::Annotations(MetadataType::POD_NAME), converted_md->annotations());
  EXPECT_EQ(0, converted_md_func->func_id());

  // Check to make sure that all of the operators and expressions depending on the metadata
  // now have an updated reference to the func.
  EXPECT_EQ(converted_md, map1->col_exprs()[0].node);
  EXPECT_EQ(converted_md, map2->col_exprs()[1].node);
}

TEST_F(RulesTest, ConvertMetadataRuleTest_missing_conversion_column) {
  auto relation = table_store::schema::Relation(cpu_relation);

  auto metadata_name = "pod_name";
  NameMetadataProperty property(MetadataType::POD_NAME, {MetadataType::UPID});
  MetadataIR* metadata_ir = MakeMetadataIR(metadata_name, /* parent_op_idx */ 0);
  metadata_ir->set_property(&property);
  MakeMap(MakeMemSource(relation), {{"md", metadata_ir}});

  ConvertMetadataRule rule(compiler_state_.get());
  auto result = rule.Execute(graph.get());
  EXPECT_NOT_OK(result);
  VLOG(1) << result.ToString();
  EXPECT_THAT(result.status(),
              HasCompilerError(
                  "Can\'t resolve metadata because of lack of converting columns in the parent. "
                  "Need one of "
                  "\\[upid\\]. Parent relation has columns \\[count,cpu0,cpu1,cpu2\\] available."));

  skip_check_stray_nodes_ = true;
}

}  // namespace planner
}  // namespace carnot
}  // namespace pl
