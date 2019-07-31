#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "src/carnot/compiler/ast_visitor.h"
#include "src/carnot/compiler/rules.h"
namespace pl {
namespace carnot {
namespace compiler {

StatusOr<bool> Rule::Execute(IR* ir_graph) const {
  std::vector<int64_t> topo_graph = ir_graph->dag().TopologicalSort();
  bool any_changed = false;
  for (int64_t node_i : topo_graph) {
    PL_ASSIGN_OR_RETURN(bool node_is_changed, Apply(ir_graph->Get(node_i)));
    any_changed = any_changed || node_is_changed;
  }
  return any_changed;
}

StatusOr<bool> DataTypeRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, UnresolvedRTFuncMatchAllArgs(ResolvedExpression()))) {
    // Match any function that has all args resolved.
    return EvaluateFunc(static_cast<FuncIR*>(ir_node));
  } else if (Match(ir_node, UnresolvedFuncType())) {
    // Matches any function that has some unresolved args.
    VLOG(1) << absl::Substitute("$1(id=$0) has unresolved args.", ir_node->id(),
                                ir_node->type_string());
  } else if (Match(ir_node, UnresolvedColumnType())) {
    // Evaluate any unresolved columns.
    return EvaluateColumn(static_cast<ColumnIR*>(ir_node));
  } else if (Match(ir_node, UnresolvedMetadataType())) {
    // Evaluate any unresolved columns.
    return EvaluateColumn(static_cast<ColumnIR*>(ir_node));
  }
  return false;
}

StatusOr<bool> DataTypeRule::EvaluateFunc(FuncIR* func) const {
  // Get the types of the children of this function.
  std::vector<types::DataType> children_data_types;
  for (const auto& arg : func->args()) {
    types::DataType t = arg->EvaluatedDataType();
    DCHECK(t != types::DataType::DATA_TYPE_UNKNOWN);
    children_data_types.push_back(t);
  }
  PL_ASSIGN_OR_RETURN(IRNode * containing_op, func->ContainingOp());
  IRNodeType containing_op_type = containing_op->type();
  if (containing_op_type != IRNodeType::kBlockingAgg) {
    // Attempt to resolve UDF function for non-Aggregate nodes.
    PL_ASSIGN_OR_RETURN(types::DataType data_type, compiler_state_->registry_info()->GetUDF(
                                                       func->func_name(), children_data_types));
    func->set_func_id(
        compiler_state_->GetUDFID(RegistryKey(func->func_name(), children_data_types)));
    func->SetOutputDataType(data_type);
  } else {
    // Attempt to resolve UDA function for Aggregate nodes.
    PL_ASSIGN_OR_RETURN(types::DataType data_type, compiler_state_->registry_info()->GetUDA(
                                                       func->func_name(), children_data_types));
    func->set_func_id(
        compiler_state_->GetUDAID(RegistryKey(func->func_name(), children_data_types)));
    func->SetOutputDataType(data_type);
  }
  func->SetArgsTypes(children_data_types);
  return true;
}

StatusOr<bool> DataTypeRule::EvaluateColumn(ColumnIR* column) const {
  PL_ASSIGN_OR_RETURN(OperatorIR * container_op, column->ContainingOp());
  if (!container_op->HasParent()) {
    return column->CreateIRNodeError("No parent for operator $1(id=$2). Can't resolve column '$0'.",
                                     column->col_name(), container_op->type_string(),
                                     container_op->id());
  }

  OperatorIR* parent_op = container_op->parent();
  if (!parent_op->IsRelationInit()) {
    // Missing a relation in parent op is not a failure, it means the parent op still has to
    // propogate results.
    VLOG(1) << absl::Substitute("Have yet to evaluate relation for operator $1(id=$0)",
                                parent_op->id(), parent_op->type_string());
    return false;
  }

  // Get the parent relation and find the column in it.
  Relation relation = parent_op->relation();
  if (!relation.HasColumn(column->col_name())) {
    return column->CreateIRNodeError("Column '$0' not found in relation of $1(id=$2)",
                                     column->col_name(), parent_op->type_string(), parent_op->id());
  }
  types::DataType col_type = relation.GetColumnType(column->col_name());
  int64_t col_idx = relation.GetColumnIndex(column->col_name());
  column->ResolveColumn(col_idx, col_type, parent_op);

  return true;
}

StatusOr<bool> SourceRelationRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, UnresolvedSource())) {
    return GetSourceRelation(static_cast<OperatorIR*>(ir_node));
  }
  return false;
}

StatusOr<bool> SourceRelationRule::GetSourceRelation(OperatorIR* source_op) const {
  if (source_op->type() != IRNodeType::kMemorySource) {
    return source_op->CreateIRNodeError(
        "Object $0(id=$1) not treated as a Source Op. No relation could be mapped.",
        source_op->type_string(), source_op->id());
  }
  MemorySourceIR* mem_node = static_cast<MemorySourceIR*>(source_op);
  ListIR* select = mem_node->select();
  std::string table_str = mem_node->table_name();
  // get the table_str from the relation map
  auto relation_map_it = compiler_state_->relation_map()->find(table_str);
  if (relation_map_it == compiler_state_->relation_map()->end()) {
    return mem_node->CreateIRNodeError("Table '$0' not found.", table_str);
  }
  Relation table_relation = relation_map_it->second;
  // get the children.
  std::vector<std::string> columns;
  Relation select_relation;
  if (!mem_node->select_all()) {
    PL_ASSIGN_OR_RETURN(columns, GetColumnNames(select->children()));
    PL_ASSIGN_OR_RETURN(select_relation, GetSelectRelation(select, table_relation, columns));
  } else {
    columns = table_relation.col_names();
    select_relation = table_relation;
  }
  PL_ASSIGN_OR_RETURN(std::vector<ColumnIR*> cols,
                      GetColumnsFromRelation(mem_node, columns, table_relation));
  mem_node->SetColumns(cols);
  PL_RETURN_IF_ERROR(mem_node->SetRelation(select_relation));
  return true;
}

StatusOr<Relation> SourceRelationRule::GetSelectRelation(
    IRNode* node, const Relation& relation, const std::vector<std::string>& columns) const {
  Relation new_relation;
  std::vector<std::string> missing_columns;
  for (auto& c : columns) {
    if (!relation.HasColumn(c)) {
      missing_columns.push_back(c);
      continue;
    }
    auto col_type = relation.GetColumnType(c);
    new_relation.AddColumn(col_type, c);
  }
  if (missing_columns.size() > 0) {
    return node->CreateIRNodeError("Columns {$0} are missing in table.",
                                   absl::StrJoin(missing_columns, ","));
  }
  return new_relation;
}

StatusOr<std::vector<ColumnIR*>> SourceRelationRule::GetColumnsFromRelation(
    OperatorIR* node, std::vector<std::string> col_names, const Relation& relation) const {
  auto graph = node->graph_ptr();
  auto result = std::vector<ColumnIR*>();
  // iterates through the columns, finds their relation position,
  // then create columns with index and type.
  for (const auto& col_name : col_names) {
    int64_t i = relation.GetColumnIndex(col_name);
    PL_ASSIGN_OR_RETURN(auto col_node, graph->MakeNode<ColumnIR>());
    PL_RETURN_IF_ERROR(col_node->Init(col_name, node->ast_node()));
    col_node->ResolveColumn(i, relation.GetColumnType(i), node);
    result.push_back(col_node);
  }
  return result;
}

StatusOr<std::vector<std::string>> SourceRelationRule::GetColumnNames(
    std::vector<ExpressionIR*> select_children) const {
  std::vector<std::string> columns;
  for (size_t idx = 0; idx < select_children.size(); idx++) {
    IRNode* col_string_node = select_children[idx];
    if (col_string_node->type() != IRNodeType::kString) {
      return col_string_node->CreateIRNodeError(
          "The elements of the select list must be of type `str`. Found a '$0' for idx $1.",
          col_string_node->type_string(), idx);
    }
    columns.push_back(static_cast<StringIR*>(col_string_node)->str());
  }
  return columns;
}

StatusOr<bool> OperatorRelationRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, UnresolvedReadyBlockingAgg())) {
    return SetBlockingAgg(static_cast<BlockingAggIR*>(ir_node));
  } else if (Match(ir_node, UnresolvedReadyMap())) {
    return SetMap(static_cast<MapIR*>(ir_node));
  } else if (Match(ir_node, UnresolvedReadyMetadataResolver())) {
    return SetMetadataResolver(static_cast<MetadataResolverIR*>(ir_node));
  } else if (Match(ir_node, UnresolvedReadyOp())) {
    return SetOther(static_cast<OperatorIR*>(ir_node));
  }
  return false;
}
bool UpdateColumn(ColumnIR* col_expr, Relation* relation_ptr) {
  if (!col_expr->IsDataTypeEvaluated()) {
    return false;
  }
  relation_ptr->AddColumn(col_expr->EvaluatedDataType(), col_expr->col_name());
  return true;
}

StatusOr<bool> OperatorRelationRule::SetBlockingAgg(BlockingAggIR* agg_ir) const {
  Relation agg_rel;
  for (ColumnIR* group : agg_ir->groups()) {
    if (!UpdateColumn(group, &agg_rel)) {
      return false;
    }
  }
  ColExpressionVector col_exprs = agg_ir->aggregate_expressions();
  for (auto& entry : col_exprs) {
    std::string col_name = entry.name;
    if (!entry.node->IsDataTypeEvaluated()) {
      return false;
    }
    agg_rel.AddColumn(entry.node->EvaluatedDataType(), col_name);
  }

  PL_RETURN_IF_ERROR(agg_ir->SetRelation(agg_rel));
  return true;
}
StatusOr<bool> OperatorRelationRule::SetMap(MapIR* map_ir) const {
  Relation map_rel;
  // Make a new relation with each of the expression key, type pairs.
  for (auto& entry : map_ir->col_exprs()) {
    std::string col_name = entry.name;
    if (!entry.node->IsDataTypeEvaluated()) {
      return false;
    }
    map_rel.AddColumn(entry.node->EvaluatedDataType(), col_name);
  }
  PL_RETURN_IF_ERROR(map_ir->SetRelation(map_rel));
  return true;
}

StatusOr<bool> OperatorRelationRule::SetMetadataResolver(MetadataResolverIR* md_ir) const {
  Relation md_rel = md_ir->parent()->relation();
  // Iterate through the columns and add them in.
  for (const auto& col_entry : md_ir->metadata_columns()) {
    std::string column_name = col_entry.second->GetColumnRepr();
    if (md_rel.HasColumn(column_name)) {
      VLOG(2) << absl::Substitute(
          "Skipping '$0' in setting metadata resolver relation, already in relation", column_name);
      continue;
    }
    md_rel.AddColumn(col_entry.second->column_type(), column_name);
  }
  PL_RETURN_IF_ERROR(md_ir->SetRelation(md_rel));
  return true;
}

StatusOr<bool> OperatorRelationRule::SetOther(OperatorIR* operator_ir) const {
  PL_RETURN_IF_ERROR(operator_ir->SetRelation(operator_ir->parent()->relation()));
  return true;
}

StatusOr<bool> RangeArgExpressionRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, Range(Int(), Int()))) {
    // If Range matches this format, don't do any work.
    return false;
  } else if (Match(ir_node, Range())) {
    RangeIR* range = static_cast<RangeIR*>(ir_node);
    IRNode* start = range->start_repr();
    IRNode* stop = range->stop_repr();
    PL_ASSIGN_OR_RETURN(start, EvalExpression(start));
    PL_ASSIGN_OR_RETURN(stop, EvalExpression(stop));
    PL_RETURN_IF_ERROR(range->SetStartStop(start, stop));
    return true;
  }
  return false;
}

StatusOr<IntIR*> RangeArgExpressionRule::EvalExpression(IRNode* node) const {
  if (Match(node, Int())) {
    return static_cast<IntIR*>(node);
  } else if (Match(node, CompileTimeFunc())) {
    auto func_node = static_cast<FuncIR*>(node);
    std::vector<IntIR*> evaled_args;
    for (const auto ag : func_node->args()) {
      PL_ASSIGN_OR_RETURN(auto eval_result, EvalExpression(ag));
      evaled_args.push_back(eval_result);
    }
    PL_ASSIGN_OR_RETURN(auto node_result, EvalFunc(func_node->func_name(), evaled_args, func_node));
    return node_result;
  } else if (Match(node, String())) {
    // Do the string processing
    auto str_node = static_cast<StringIR*>(node);
    // TODO(philkuz) (PL-708) make StringToTimeInt also take time_now as an argument.
    PL_ASSIGN_OR_RETURN(int64_t int_val, StringToTimeInt(str_node->str()));
    int64_t time_repr = compiler_state_->time_now().val + int_val;
    PL_ASSIGN_OR_RETURN(auto out_node, node->graph_ptr()->MakeNode<IntIR>());
    PL_RETURN_IF_ERROR(out_node->Init(time_repr, node->ast_node()));
    return out_node;
  }
  return node->CreateIRNodeError(
      "Expected integer, time expression, or a string representation of time, not $0",
      node->type_string());
}
StatusOr<IntIR*> RangeArgExpressionRule::EvalFunc(std::string name, std::vector<IntIR*> evaled_args,
                                                  FuncIR* func) const {
  if (evaled_args.size() != 2) {
    return func->CreateIRNodeError("Expected 2 argument to $0 call, got $1.", name,
                                   evaled_args.size());
  }
  int64_t result = 0;
  // TODO(philkuz) (PL-709) Make a UDCF (C := CompileTime) to combine these together.
  if (name == "plc.multiply") {
    result = 1;
    for (auto a : evaled_args) {
      result *= a->val();
    }
  } else if (name == "plc.add") {
    for (auto a : evaled_args) {
      result += a->val();
    }
  } else if (name == "plc.subtract") {
    result = evaled_args[0]->val() - evaled_args[1]->val();
  } else {
    return func->CreateIRNodeError("Only allowing [multiply, add, subtract], not $0", name);
  }
  PL_ASSIGN_OR_RETURN(IntIR * ir_result, func->graph_ptr()->MakeNode<IntIR>());
  PL_RETURN_IF_ERROR(ir_result->Init(result, func->ast_node()));
  return ir_result;
}

StatusOr<bool> VerifyFilterExpressionRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, Filter())) {
    // Match any function that has all args resolved.
    FilterIR* filter = static_cast<FilterIR*>(ir_node);
    ExpressionIR* expr = filter->filter_expr();
    types::DataType expr_type = expr->EvaluatedDataType();
    if (expr_type != types::DataType::BOOLEAN) {
      return ir_node->CreateIRNodeError("Expected Boolean for Filter expression, got $0",
                                        types::DataType_Name(expr_type));
    }
  }
  return false;
}

StatusOr<bool> ResolveMetadataRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, UnresolvedMetadataIR())) {
    // Match any function that has all args resolved.
    return HandleMetadata(static_cast<MetadataIR*>(ir_node));
  }
  return false;
}

StatusOr<MetadataResolverIR*> ResolveMetadataRule::InsertMetadataResolver(
    OperatorIR* container_op, OperatorIR* parent_op) const {
  DCHECK_EQ(container_op->parent()->id(), parent_op->id())
      << "Parent arg should be the actual parent of the container_op.";
  IR* graph = container_op->graph_ptr();
  PL_ASSIGN_OR_RETURN(auto md_resolver, graph->MakeNode<MetadataResolverIR>());
  PL_RETURN_IF_ERROR(md_resolver->Init(parent_op, {{}}, container_op->ast_node()));
  PL_RETURN_IF_ERROR(container_op->RemoveParent(parent_op));
  PL_RETURN_IF_ERROR(container_op->SetParent(md_resolver));
  return md_resolver;
}

StatusOr<bool> ResolveMetadataRule::HandleMetadata(MetadataIR* metadata) const {
  // Get containing operator.
  PL_ASSIGN_OR_RETURN(OperatorIR * container_op, metadata->ContainingOp());
  if (!container_op->HasParent()) {
    return metadata->CreateIRNodeError(
        "No parent for operator $1(id=$2). Can't resolve column '$0'.", metadata->col_name(),
        container_op->type_string(), container_op->id());
  }

  OperatorIR* parent_op = container_op->parent();
  if (parent_op->type() != IRNodeType::kMetadataResolver) {
    // If the parent is not a metadata resolver, add a parent metadata resolver node.
    PL_ASSIGN_OR_RETURN(parent_op, InsertMetadataResolver(container_op, parent_op));
  }
  auto md_resolver_op = static_cast<MetadataResolverIR*>(parent_op);

  // Check to see whether metadata is valid.
  if (!md_handler_->HasProperty(metadata->name())) {
    return metadata->CreateIRNodeError("Specified metadata value '$0' is not properly handled.",
                                       metadata->name());
  }
  PL_ASSIGN_OR_RETURN(MetadataProperty * md_property, md_handler_->GetProperty(metadata->name()));
  PL_RETURN_IF_ERROR(metadata->ResolveMetadataColumn(md_resolver_op, md_property));
  PL_RETURN_IF_ERROR(md_resolver_op->AddMetadata(md_property));

  return true;
}

StatusOr<bool> MetadataFunctionFormatRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, Equals(Metadata(), MetadataLiteral()))) {
    // If the literal already matches, then no need to do any work.
    return false;
  } else if (Match(ir_node, Equals(Metadata(), String()))) {
    FuncIR* func = static_cast<FuncIR*>(ir_node);
    StringIR* out_expr;
    MetadataIR* md_expr;
    int64_t update_idx;
    DCHECK_EQ(func->args().size(), 2UL);
    if (Match(func->args()[1], Metadata())) {
      update_idx = 0;
      out_expr = static_cast<StringIR*>(func->args()[0]);
      md_expr = static_cast<MetadataIR*>(func->args()[1]);
    } else {
      update_idx = 1;
      out_expr = static_cast<StringIR*>(func->args()[1]);
      md_expr = static_cast<MetadataIR*>(func->args()[0]);
    }

    DCHECK(md_expr->type() == IRNodeType::kMetadata) << absl::Substitute(
        "Expected Metadata at idx $1, found '$0.'", md_expr->type_string(), update_idx);

    PL_ASSIGN_OR_RETURN(MetadataLiteralIR * metadata_literal,
                        WrapLiteral(out_expr, md_expr->property()));
    PL_RETURN_IF_ERROR(func->UpdateArg(update_idx, metadata_literal));
    return true;
  } else if (Match(ir_node, FuncAnyArg(Metadata()))) {
    FuncIR* func = static_cast<FuncIR*>(ir_node);
    std::vector<std::string> other_args;
    for (ExpressionIR* arg : func->args()) {
      if (Match(arg, Metadata())) {
        continue;
      }
      other_args.push_back(arg->type_string());
    }
    return func->CreateIRNodeError(
        "Function '$0' with metadata arg in conjunction with '[$1]' is not supported.",
        func->func_name(), absl::StrJoin(other_args, ""));
  }
  return false;
}

StatusOr<MetadataLiteralIR*> MetadataFunctionFormatRule::WrapLiteral(
    DataIR* data, MetadataProperty* md_property) const {
  if (!md_property->ExprFitsFormat(data)) {
    return data->CreateIRNodeError("$0 not formatted properly for metadata operation. Expected $1",
                                   data->type_string(), md_property->ExplainFormat());
  }
  PL_ASSIGN_OR_RETURN(MetadataLiteralIR * literal,
                      data->graph_ptr()->MakeNode<MetadataLiteralIR>());
  PL_RETURN_IF_ERROR(literal->Init(data, data->ast_node()));

  return literal;
}

StatusOr<bool> CheckMetadataColumnNamingRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, MetadataResolver())) {
    // If the MetadataResolver, then don't do anything.
    return false;
  } else if (Match(ir_node, Map())) {
    return CheckMapColumns(static_cast<MapIR*>(ir_node));
  } else if (Match(ir_node, BlockingAgg())) {
    return CheckAggColumns(static_cast<BlockingAggIR*>(ir_node));
  }
  return false;
}  // namespace compiler

StatusOr<bool> CheckMetadataColumnNamingRule::CheckMapColumns(MapIR* op) const {
  for (const auto& col_expr : op->col_exprs()) {
    if (absl::StartsWith(col_expr.name, IdMetadataProperty::kMetadataColumnPrefix)) {
      return op->CreateIRNodeError(
          "Column name '$1' violates naming rules. The '$0' prefix is reserved for internal "
          "use.",
          MetadataProperty::kMetadataColumnPrefix, col_expr.name);
    }
  }
  return false;
}
StatusOr<bool> CheckMetadataColumnNamingRule::CheckAggColumns(BlockingAggIR* op) const {
  for (const auto& col_expr : op->aggregate_expressions()) {
    if (absl::StartsWith(col_expr.name, IdMetadataProperty::kMetadataColumnPrefix)) {
      return op->CreateIRNodeError(
          "Column name '$1' violates naming rules. The '$0' prefix is reserved for internal "
          "use.",
          MetadataProperty::kMetadataColumnPrefix, col_expr.name);
    }
  }
  return false;
}

StatusOr<bool> MetadataResolverConversionRule::Apply(IRNode* ir_node) const {
  if (Match(ir_node, MetadataResolver())) {
    return ReplaceMetadataResolver(static_cast<MetadataResolverIR*>(ir_node));
  }
  return false;
}

Status MetadataResolverConversionRule::RemoveMetadataResolver(
    MetadataResolverIR* md_resolver) const {
  IR* graph = md_resolver->graph_ptr();
  // Get the parent of the metadsata resolver
  OperatorIR* parent = md_resolver->parent();
  PL_RETURN_IF_ERROR(md_resolver->RemoveParent(parent));

  // Get the child of the metadata operator.
  std::vector<int64_t> dependent_nodes = graph->dag().DependenciesOf(md_resolver->id());
  DCHECK_EQ(dependent_nodes.size(), 1UL);
  int64_t child_op = dependent_nodes[0];
  PL_RETURN_IF_ERROR(graph->DeleteEdge(md_resolver->id(), child_op));
  IRNode* node = graph->Get(child_op);
  DCHECK(node->IsOp()) << "Expected node to be operator.";
  OperatorIR* op = static_cast<OperatorIR*>(node);

  // Set the parent of the child_op to the md_resolver parent.
  DCHECK_EQ(op->parent(), md_resolver);
  PL_RETURN_IF_ERROR(op->SetParent(parent));

  // delete metadata_resolver
  PL_RETURN_IF_ERROR(graph->DeleteNode(md_resolver->id()));
  return Status::OK();
}

Status MetadataResolverConversionRule::RemoveMap(MapIR* map) const {
  IR* graph = map->graph_ptr();
  PL_RETURN_IF_ERROR(map->RemoveParent(map->parent()));

  // Get the child of the metadata operator.
  std::vector<int64_t> map_dependent_nodes = graph->dag().DependenciesOf(map->id());
  CHECK_EQ(map_dependent_nodes.size(), map->col_exprs().size());
  for (const int64_t& child_node_idx : map_dependent_nodes) {
    PL_RETURN_IF_ERROR(graph->DeleteEdge(map->id(), child_node_idx));
    IRNode* node = graph->Get(child_node_idx);
    DCHECK(node->type() == IRNodeType::kColumn)
        << "Got: " << node->type_string() << "Expected: Column.";
    PL_RETURN_IF_ERROR(graph->DeleteNode(node->id()));
  }

  // Delete map.
  PL_RETURN_IF_ERROR(graph->DeleteNode(map->id()));
  return Status::OK();
}

bool MetadataResolverConversionRule::DoesMapOnlyCopy(MapIR* map) const {
  Relation rel = map->parent()->relation();
  if (rel.NumColumns() != map->col_exprs().size()) {
    return false;
  }
  int64_t idx = 0;
  for (auto const& col_expr : map->col_exprs()) {
    if (col_expr.node->type() != IRNodeType::kColumn && rel.GetColumnName(idx) != col_expr.name) {
      return false;
    }
    ++idx;
  }
  return true;
}
StatusOr<bool> MetadataResolverConversionRule::ReplaceMetadataResolver(
    MetadataResolverIR* md_resolver) const {
  PL_ASSIGN_OR_RETURN(MapIR * map, MakeMap(md_resolver));
  if (DoesMapOnlyCopy(map)) {
    PL_RETURN_IF_ERROR(RemoveMetadataResolver(md_resolver));
    PL_RETURN_IF_ERROR(RemoveMap(map));
    return true;
  }
  PL_RETURN_IF_ERROR(SwapInMap(md_resolver, map));
  return true;
}

Status MetadataResolverConversionRule::CopyParentColumns(IR* graph, OperatorIR* parent_op,
                                                         ColExpressionVector* col_exprs,
                                                         pypa::AstPtr ast_node) const {
  Relation parent_relation = parent_op->relation();
  for (size_t i = 0; i < parent_relation.NumColumns(); ++i) {
    // Make Column
    PL_ASSIGN_OR_RETURN(ColumnIR * column_ir, graph->MakeNode<ColumnIR>());
    std::string column_name = parent_relation.GetColumnName(i);
    PL_RETURN_IF_ERROR(column_ir->Init(column_name, ast_node));
    column_ir->ResolveColumn(i, parent_relation.GetColumnType(i), parent_op);
    col_exprs->emplace_back(column_name, column_ir);
  }
  return Status::OK();
}

Status MetadataResolverConversionRule::AddMetadataConversionFns(
    IR* graph, MetadataResolverIR* md_resolver, OperatorIR* parent_op,
    ColExpressionVector* col_exprs) const {
  Relation parent_relation = parent_op->relation();
  for (const auto& md_col_iter : md_resolver->metadata_columns()) {
    MetadataProperty* md_property = md_col_iter.second;
    // If parent relation has the column, we've already copied it, skip over.
    if (parent_relation.HasColumn(md_property->GetColumnRepr())) {
      continue;
    }
    PL_ASSIGN_OR_RETURN(FuncIR * conversion_func, graph->MakeNode<FuncIR>());
    PL_ASSIGN_OR_RETURN(std::string key_column,
                        FindKeyColumn(parent_relation, md_property, md_resolver));

    PL_ASSIGN_OR_RETURN(ColumnIR * column_ir, graph->MakeNode<ColumnIR>());
    PL_RETURN_IF_ERROR(column_ir->Init(key_column, md_resolver->ast_node()));
    int64_t parent_relation_idx = parent_relation.GetColumnIndex(key_column);
    PL_ASSIGN_OR_RETURN(std::string func_name, md_property->UDFName(key_column));
    column_ir->ResolveColumn(parent_relation_idx, md_property->column_type(), parent_op);

    std::vector<types::DataType> children_data_types = {
        parent_relation.GetColumnType(parent_relation_idx)};
    PL_RETURN_IF_ERROR(conversion_func->Init({FuncIR::Opcode::non_op, "", func_name},
                                             ASTWalker::kRunTimeFuncPrefix, {column_ir}, false,
                                             md_resolver->ast_node()));
    PL_ASSIGN_OR_RETURN(types::DataType out_type,
                        compiler_state_->registry_info()->GetUDF(conversion_func->func_name(),
                                                                 children_data_types));
    conversion_func->set_func_id(
        compiler_state_->GetUDFID(RegistryKey(conversion_func->func_name(), children_data_types)));

    // Conversion Func.
    DCHECK_EQ(out_type, md_property->column_type())
        << "Expected the parent_relation key column type and metadata property type to match.";

    conversion_func->SetOutputDataType(out_type);
    conversion_func->SetArgsTypes(children_data_types);
    col_exprs->emplace_back(md_property->GetColumnRepr(), conversion_func);
  }
  return Status::OK();
}

StatusOr<MapIR*> MetadataResolverConversionRule::MakeMap(MetadataResolverIR* md_resolver) const {
  IR* graph = md_resolver->graph_ptr();
  OperatorIR* parent_op = md_resolver->parent();
  ColExpressionVector col_exprs;
  PL_RETURN_IF_ERROR(CopyParentColumns(graph, parent_op, &col_exprs, md_resolver->ast_node()));

  PL_RETURN_IF_ERROR(AddMetadataConversionFns(graph, md_resolver, parent_op, &col_exprs));
  Relation relation = md_resolver->relation();
  std::unordered_set<std::string> col_names(std::make_move_iterator(relation.col_names().begin()),
                                            std::make_move_iterator(relation.col_names().end()));
  DCHECK_EQ(col_exprs.size(), md_resolver->relation().NumColumns());
  PL_ASSIGN_OR_RETURN(MapIR * map, graph->MakeNode<MapIR>());
  PL_ASSIGN_OR_RETURN(LambdaIR * lambda, graph->MakeNode<LambdaIR>());
  PL_RETURN_IF_ERROR(lambda->Init(col_names, col_exprs, md_resolver->ast_node()));
  PL_RETURN_IF_ERROR(map->Init(md_resolver->parent(), {{"fn", lambda}}, md_resolver->ast_node()));
  return map;
}

StatusOr<std::string> MetadataResolverConversionRule::FindKeyColumn(const Relation& parent_relation,
                                                                    MetadataProperty* property,
                                                                    IRNode* node_for_error) const {
  for (const std::string& key_col : property->GetKeyColumnReprs()) {
    if (parent_relation.HasColumn(key_col)) {
      return key_col;
    }
  }
  return node_for_error->CreateIRNodeError(
      "Can't resolve metadata because of lack of converting columns in the parent. Need one of "
      "[$0]. Parent relation has columns [$1] available.",
      absl::StrJoin(property->GetKeyColumnReprs(), ","),
      absl::StrJoin(parent_relation.col_names(), ","));
}
Status MetadataResolverConversionRule::SwapInMap(MetadataResolverIR* md_resolver,
                                                 MapIR* map) const {
  IR* graph = md_resolver->graph_ptr();
  PL_RETURN_IF_ERROR(md_resolver->RemoveParent(md_resolver->parent()));
  std::vector<int64_t> dependent_nodes = graph->dag().DependenciesOf(md_resolver->id());
  DCHECK_EQ(dependent_nodes.size(), 1UL);
  PL_RETURN_IF_ERROR(graph->DeleteEdge(md_resolver->id(), dependent_nodes[0]));

  IRNode* node = graph->Get(dependent_nodes[0]);
  DCHECK(node->IsOp()) << "Expected node to be operator.";
  OperatorIR* op = static_cast<OperatorIR*>(node);

  DCHECK_EQ(op->parent(), md_resolver);
  DCHECK(md_resolver->IsRelationInit());

  PL_RETURN_IF_ERROR(op->SetParent(map));
  PL_RETURN_IF_ERROR(map->SetRelation(md_resolver->relation()));
  // transfer ownership of columns.
  for (ColumnIR* col : md_resolver->referencing_columns()) {
    col->SetParentOperator(map);
  }

  // delete metadata_resolver
  PL_RETURN_IF_ERROR(graph->DeleteNode(md_resolver->id()));
  return Status::OK();
}

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
