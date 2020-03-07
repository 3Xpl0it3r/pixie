#include "src/carnot/planner/objects/dataframe.h"
#include "src/carnot/planner/ir/ast_utils.h"
#include "src/carnot/planner/objects/collection_object.h"
#include "src/carnot/planner/objects/expr_object.h"
#include "src/carnot/planner/objects/funcobject.h"
#include "src/carnot/planner/objects/metadata_object.h"
#include "src/carnot/planner/objects/none_object.h"
#include "src/carnot/planner/objects/pixie_module.h"

namespace pl {
namespace carnot {
namespace planner {
namespace compiler {

StatusOr<std::shared_ptr<Dataframe>> Dataframe::Create(OperatorIR* op, ASTVisitor* visitor) {
  std::shared_ptr<Dataframe> df(new Dataframe(op, op->graph_ptr(), visitor));
  PL_RETURN_IF_ERROR(df->Init());
  return df;
}

StatusOr<std::shared_ptr<Dataframe>> Dataframe::Create(IR* graph, ASTVisitor* visitor) {
  std::shared_ptr<Dataframe> df(new Dataframe(nullptr, graph, visitor));
  PL_RETURN_IF_ERROR(df->Init());
  return df;
}

// Parses elements of type TIRNodeType, either a single one or a collection, as a vector of
// TIRNodeType. Used for dataframe methods that take either a list or single item, like drop.
// drop('foo') and drop(['foo', 'bar']) for example.
template <typename TIRNodeType>
StatusOr<std::vector<TIRNodeType*>> ParseAsListOf(QLObjectPtr obj, std::string_view arg_name) {
  std::vector<TIRNodeType*> result;
  std::vector<QLObjectPtr> items;
  bool with_index = CollectionObject::IsCollection(obj);
  if (CollectionObject::IsCollection(obj)) {
    items = std::static_pointer_cast<CollectionObject>(obj)->items();
  } else {
    items.push_back(obj);
  }
  for (const auto& [idx, item] : Enumerate(items)) {
    std::string name = std::string(arg_name);
    if (with_index) {
      name = absl::Substitute("$0 (index $1)", arg_name, idx);
    }
    PL_ASSIGN_OR_RETURN(auto casted, GetArgAs<TIRNodeType>(item, name));
    result.push_back(casted);
  }
  return result;
}

StatusOr<std::vector<std::string>> ParseAsListOfStrings(QLObjectPtr obj,
                                                        std::string_view arg_name) {
  PL_ASSIGN_OR_RETURN(auto string_irs, ParseAsListOf<StringIR>(obj, arg_name));
  std::vector<std::string> strs;
  strs.reserve(string_irs.size());
  for (StringIR* str : string_irs) {
    strs.push_back(str->str());
  }
  return strs;
}

Status Dataframe::Init() {
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> constructor_fn,
      FuncObject::Create(name(), {"table", "select", "start_time", "end_time"},
                         {{"select", "[]"},
                          {"start_time", "0"},
                          {"end_time", absl::Substitute("$0.$1()", PixieModule::kPixieModuleObjName,
                                                        PixieModule::kNowOpId)}},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&DataFrameHandler::Eval, graph(), std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  AddCallMethod(constructor_fn);
  // If the op is null, then don't init the other funcs.
  if (op() == nullptr) {
    return Status::OK();
  }

  /**
   * # Equivalent to the python method method syntax:
   * def merge(self, right, how, left_on, right_on, suffixes=['_x', '_y']):
   *     ...
   */
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> mergefn,
      FuncObject::Create(kMergeOpId, {"right", "how", "left_on", "right_on", "suffixes"},
                         {{"suffixes", "['_x', '_y']"}},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&JoinHandler::Eval, graph(), op(), std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  AddMethod(kMergeOpId, mergefn);

  /**
   * # Equivalent to the python method method syntax:
   * def agg(self, **kwargs):
   *     ...
   */
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> aggfn,
      FuncObject::Create(kBlockingAggOpId, {}, {},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ true,
                         std::bind(&AggHandler::Eval, graph(), op(), std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  AddMethod(kBlockingAggOpId, aggfn);

  /**
   * # Equivalent to the python method method syntax:
   * def drop(self, fn):
   *     ...
   */
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> dropfn,
      FuncObject::Create(kDropOpId, {"columns"}, {}, /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&DropHandler::Eval, graph(), op(), std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  AddMethod(kDropOpId, dropfn);

  /**
   * # Equivalent to the python method method syntax:
   * def head(self, n=5):
   *     ...
   */
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> limitfn,
      FuncObject::Create(kLimitOpId, {"n"}, {{"n", "5"}},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&LimitHandler::Eval, graph(), op(), std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  AddMethod(kLimitOpId, limitfn);

  /**
   *
   * # Equivalent to the python method method syntax:
   * def __getitem__(self, key):
   *     ...
   *
   * # It's important to note that this is added as a subscript method instead.
   */
  std::shared_ptr<FuncObject> subscript_fn(
      new FuncObject(kSubscriptMethodName, {"key"}, {},
                     /* has_variable_len_args */ false,
                     /* has_variable_len_kwargs */ false,
                     std::bind(&SubscriptHandler::Eval, graph(), op(), std::placeholders::_1,
                               std::placeholders::_2, std::placeholders::_3),
                     ast_visitor()));
  AddSubscriptMethod(subscript_fn);

  std::shared_ptr<FuncObject> group_by_fn(
      new FuncObject(kGroupByOpId, {"by"}, {},
                     /* has_variable_len_args */ false,
                     /* has_variable_len_kwargs */ false,
                     std::bind(&GroupByHandler::Eval, graph(), op(), std::placeholders::_1,
                               std::placeholders::_2, std::placeholders::_3),
                     ast_visitor()));
  AddMethod(kGroupByOpId, group_by_fn);

  /**
   * # Equivalent to the python method method syntax:
   * def append(self, fn):
   *     ...
   */
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> union_fn,
      FuncObject::Create(kUnionOpId, {"objs"}, {}, /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&UnionHandler::Eval, graph(), op(), std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  AddMethod(kUnionOpId, union_fn);

  /**
   * # Equivalent to the python method syntax:
   * def rolling(self, window, on="time_"):
   *     ...
   */
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> rolling_fn,
      FuncObject::Create(kRollingOpId, {"window", "on"}, {{"on", "'time_'"}},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&RollingHandler::Eval, graph(), op(), std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  AddMethod(kRollingOpId, rolling_fn);

  PL_ASSIGN_OR_RETURN(auto md, MetadataObject::Create(op(), ast_visitor()));
  return AssignAttribute(kMetadataAttrName, md);
}

StatusOr<QLObjectPtr> Dataframe::GetAttributeImpl(const pypa::AstPtr& ast,
                                                  std::string_view name) const {
  // If this gets to this point, should fail here.
  DCHECK(HasNonMethodAttribute(name));

  if (QLObject::HasNonMethodAttribute(name)) {
    return QLObject::GetAttributeImpl(ast, name);
  }
  // We evaluate schemas in the analyzer, so at this point assume 'name' is a valid column.
  PL_ASSIGN_OR_RETURN(ColumnIR * column,
                      graph()->CreateNode<ColumnIR>(ast, std::string(name), /* parent_op_idx */ 0));
  return ExprObject::Create(column, ast_visitor());
}

StatusOr<std::shared_ptr<Dataframe>> Dataframe::FromColumnAssignment(const pypa::AstPtr& expr_node,
                                                                     ColumnIR* column,
                                                                     ExpressionIR* expr) {
  auto col_name = column->col_name();
  ColExpressionVector map_exprs{{col_name, expr}};
  PL_ASSIGN_OR_RETURN(MapIR * ir_node, graph_->CreateNode<MapIR>(expr_node, op(), map_exprs,
                                                                 /*keep_input_cols*/ true));
  return Dataframe::Create(ir_node, ast_visitor());
}

StatusOr<QLObjectPtr> JoinHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                        const ParsedArgs& args, ASTVisitor* visitor) {
  // GetArg returns non-nullptr or errors out in Debug mode. No need
  // to check again.
  PL_ASSIGN_OR_RETURN(OperatorIR * right, GetArgAs<OperatorIR>(args, "right"));
  PL_ASSIGN_OR_RETURN(StringIR * how, GetArgAs<StringIR>(args, "how"));
  QLObjectPtr suffixes_node = args.GetArg("suffixes");
  std::string how_type = how->str();

  PL_ASSIGN_OR_RETURN(std::vector<ColumnIR*> left_on_cols,
                      ProcessCols(graph, ast, args.GetArg("left_on"), "left_on", 0));
  PL_ASSIGN_OR_RETURN(std::vector<ColumnIR*> right_on_cols,
                      ProcessCols(graph, ast, args.GetArg("right_on"), "right_on", 1));

  // TODO(philkuz) consider using a struct instead of a vector because it's a fixed size.
  if (!CollectionObject::IsCollection(suffixes_node)) {
    return suffixes_node->CreateError(
        "'suffixes' must be a list with 2 strings for the left and right suffixes. Received $0",
        suffixes_node->name());
  }

  PL_ASSIGN_OR_RETURN(std::vector<std::string> suffix_strs,
                      ParseAsListOfStrings(suffixes_node, "suffixes"));
  if (suffix_strs.size() != 2) {
    return suffixes_node->CreateError("'suffixes' must be a list with 2 elements. Received $0",
                                      suffix_strs.size());
  }

  PL_ASSIGN_OR_RETURN(JoinIR * join_op,
                      graph->CreateNode<JoinIR>(ast, std::vector<OperatorIR*>{op, right}, how_type,
                                                left_on_cols, right_on_cols, suffix_strs));
  return Dataframe::Create(join_op, visitor);
}

StatusOr<std::vector<ColumnIR*>> JoinHandler::ProcessCols(IR* graph, const pypa::AstPtr& ast,
                                                          QLObjectPtr obj, std::string arg_name,
                                                          int64_t parent_index) {
  DCHECK(obj != nullptr);
  PL_ASSIGN_OR_RETURN(std::vector<std::string> column_names, ParseAsListOfStrings(obj, arg_name));
  std::vector<ColumnIR*> columns;
  columns.reserve(column_names.size());

  for (const auto& col_name : column_names) {
    PL_ASSIGN_OR_RETURN(ColumnIR * col, graph->CreateNode<ColumnIR>(ast, col_name, parent_index));
    columns.push_back(col);
  }
  return columns;
}

StatusOr<QLObjectPtr> AggHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                       const ParsedArgs& args, ASTVisitor* visitor) {
  // converts the mapping of args.kwargs into ColExpressionvector
  ColExpressionVector aggregate_expressions;
  for (const auto& [name, expr_obj] : args.kwargs()) {
    if (expr_obj->type() != QLObjectType::kTuple) {
      return expr_obj->CreateError("Expected tuple for value at kwarg $0 but received $1", name,
                                   expr_obj->name());
    }
    PL_ASSIGN_OR_RETURN(
        FuncIR * parsed_expr,
        ParseNameTuple(graph, ast, std::static_pointer_cast<TupleObject>(expr_obj)));
    aggregate_expressions.push_back({name, parsed_expr});
  }

  PL_ASSIGN_OR_RETURN(
      BlockingAggIR * agg_op,
      graph->CreateNode<BlockingAggIR>(ast, op, std::vector<ColumnIR*>{}, aggregate_expressions));
  return Dataframe::Create(agg_op, visitor);
}

StatusOr<FuncIR*> AggHandler::ParseNameTuple(IR* ir, const pypa::AstPtr& ast,
                                             std::shared_ptr<TupleObject> tuple) {
  DCHECK_EQ(tuple->items().size(), 2UL);
  PL_ASSIGN_OR_RETURN(StringIR * name,
                      GetArgAs<StringIR>(tuple->items()[0], "first tuple argument"));

  auto func = tuple->items()[1];
  if (func->type() != QLObjectType::kFunction) {
    return func->CreateError("Expected second tuple argument to be type Func, received $0",
                             func->name());
  }
  PL_ASSIGN_OR_RETURN(auto called, std::static_pointer_cast<FuncObject>(func)->Call({}, ast));

  PL_ASSIGN_OR_RETURN(FuncIR * func_ir, GetArgAs<FuncIR>(called, "second tuple argument"));

  // The function should be specified as a single function by itself.
  // This could change in the future.
  if (func_ir->args().size() != 0) {
    return func_ir->CreateIRNodeError("Unexpected aggregate function");
  }

  // parent_op_idx is 0 because we only have one parent for an aggregate.
  PL_ASSIGN_OR_RETURN(ColumnIR * argcol, ir->CreateNode<ColumnIR>(name->ast_node(), name->str(),
                                                                  /* parent_op_idx */ 0));
  PL_RETURN_IF_ERROR(func_ir->AddArg(argcol));
  return func_ir;
}

StatusOr<QLObjectPtr> DropHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                        const ParsedArgs& args, ASTVisitor* visitor) {
  QLObjectPtr columns_arg = args.GetArg("columns");
  PL_ASSIGN_OR_RETURN(std::vector<std::string> columns,
                      ParseAsListOfStrings(args.GetArg("columns"), "columns"));
  PL_ASSIGN_OR_RETURN(DropIR * drop_op, graph->CreateNode<DropIR>(ast, op, columns));
  return Dataframe::Create(drop_op, visitor);
}

StatusOr<QLObjectPtr> LimitHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                         const ParsedArgs& args, ASTVisitor* visitor) {
  // TODO(philkuz) (PL-1161) Add support for compile time evaluation of Limit argument.
  PL_ASSIGN_OR_RETURN(IntIR * rows_node, GetArgAs<IntIR>(args, "n"));
  int64_t limit_value = rows_node->val();

  PL_ASSIGN_OR_RETURN(LimitIR * limit_op, graph->CreateNode<LimitIR>(ast, op, limit_value));
  // Delete the integer node.
  PL_RETURN_IF_ERROR(graph->DeleteNode(rows_node->id()));
  return Dataframe::Create(limit_op, visitor);
}

StatusOr<QLObjectPtr> SubscriptHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                             const ParsedArgs& args, ASTVisitor* visitor) {
  QLObjectPtr key = args.GetArg("key");
  if (key->HasNode() && Match(key->node(), String())) {
    return EvalColumn(graph, op, ast, static_cast<StringIR*>(key->node()), visitor);
  }
  if (CollectionObject::IsCollection(key)) {
    return EvalKeep(graph, op, ast, std::static_pointer_cast<CollectionObject>(key), visitor);
  }
  if (key->HasNode() && key->node()->IsExpression()) {
    return EvalFilter(graph, op, ast, static_cast<ExpressionIR*>(key->node()), visitor);
  }
  return key->CreateError(
      "subscript argument must have a list of strings or expression. '$0' not allowed",
      key->name());
}

StatusOr<QLObjectPtr> SubscriptHandler::EvalFilter(IR* graph, OperatorIR* op,
                                                   const pypa::AstPtr& ast, ExpressionIR* expr,
                                                   ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(FilterIR * filter_op, graph->CreateNode<FilterIR>(ast, op, expr));
  return Dataframe::Create(filter_op, visitor);
}

StatusOr<QLObjectPtr> SubscriptHandler::EvalColumn(IR* graph, OperatorIR*, const pypa::AstPtr&,
                                                   StringIR* expr, ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(ColumnIR * column, graph->CreateNode<ColumnIR>(expr->ast_node(), expr->str(),
                                                                     /* parent_op_idx */ 0));
  return ExprObject::Create(column, visitor);
}

StatusOr<QLObjectPtr> SubscriptHandler::EvalKeep(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                                 std::shared_ptr<CollectionObject> key,
                                                 ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(std::vector<std::string> keep_column_names, ParseAsListOfStrings(key, "key"));

  ColExpressionVector keep_exprs;
  for (const auto& col_name : keep_column_names) {
    // parent_op_idx is 0 because we only have one parent for a map.
    PL_ASSIGN_OR_RETURN(ColumnIR * keep_col,
                        graph->CreateNode<ColumnIR>(ast, col_name, /* parent_op_idx */ 0));
    keep_exprs.emplace_back(col_name, keep_col);
  }

  PL_ASSIGN_OR_RETURN(MapIR * map_op, graph->CreateNode<MapIR>(ast, op, keep_exprs,
                                                               /* keep_input_columns */ false));
  return Dataframe::Create(map_op, visitor);
}

StatusOr<QLObjectPtr> GroupByHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                           const ParsedArgs& args, ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(std::vector<std::string> group_names,
                      ParseAsListOfStrings(args.GetArg("by"), "by"));
  std::vector<ColumnIR*> groups;
  groups.reserve(group_names.size());
  for (const auto& group : group_names) {
    PL_ASSIGN_OR_RETURN(ColumnIR * col,
                        graph->CreateNode<ColumnIR>(ast, group, /* parent_idx */ 0));
    groups.push_back(col);
  }

  PL_ASSIGN_OR_RETURN(GroupByIR * group_by_op, graph->CreateNode<GroupByIR>(ast, op, groups));
  return Dataframe::Create(group_by_op, visitor);
}

StatusOr<QLObjectPtr> UnionHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                         const ParsedArgs& args, ASTVisitor* visitor) {
  std::vector<OperatorIR*> parents{op};
  PL_ASSIGN_OR_RETURN(std::vector<OperatorIR*> list_of_ops,
                      ParseAsListOf<OperatorIR>(args.GetArg("objs"), "objs"));
  parents.insert(parents.end(), list_of_ops.begin(), list_of_ops.end());
  PL_ASSIGN_OR_RETURN(UnionIR * union_op, graph->CreateNode<UnionIR>(ast, parents));
  return Dataframe::Create(union_op, visitor);
}

StatusOr<QLObjectPtr> RollingHandler::Eval(IR* graph, OperatorIR* op, const pypa::AstPtr& ast,
                                           const ParsedArgs& args, ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(StringIR * window_col_name, GetArgAs<StringIR>(args, "on"));
  PL_ASSIGN_OR_RETURN(ExpressionIR * window_size, GetArgAs<ExpressionIR>(args, "window"));

  if (window_col_name->str() != "time_") {
    return window_col_name->CreateIRNodeError(
        "Windowing is only supported on time_ at the moment, not $0", window_col_name->str());
  }

  PL_ASSIGN_OR_RETURN(ColumnIR * window_col,
                      graph->CreateNode<ColumnIR>(ast, window_col_name->str(), /* parent_idx */ 0));

  PL_ASSIGN_OR_RETURN(RollingIR * rolling_op,
                      graph->CreateNode<RollingIR>(ast, op, window_col, window_size));
  return Dataframe::Create(rolling_op, visitor);
}

StatusOr<QLObjectPtr> DataFrameHandler::Eval(IR* graph, const pypa::AstPtr& ast,
                                             const ParsedArgs& args, ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(StringIR * table, GetArgAs<StringIR>(args, "table"));
  PL_ASSIGN_OR_RETURN(std::vector<std::string> columns,
                      ParseAsListOfStrings(args.GetArg("select"), "select"));
  PL_ASSIGN_OR_RETURN(ExpressionIR * start_time, GetArgAs<ExpressionIR>(args, "start_time"));
  PL_ASSIGN_OR_RETURN(ExpressionIR * end_time, GetArgAs<ExpressionIR>(args, "end_time"));

  std::string table_name = table->str();
  PL_ASSIGN_OR_RETURN(MemorySourceIR * mem_source_op,
                      graph->CreateNode<MemorySourceIR>(ast, table_name, columns));
  // If both start_time and end_time are default arguments, then we don't substitute them.
  if (!(args.default_subbed_args().contains("start_time") &&
        args.default_subbed_args().contains("end_time"))) {
    PL_RETURN_IF_ERROR(mem_source_op->SetTimeExpressions(start_time, end_time));
  }
  return Dataframe::Create(mem_source_op, visitor);
}

}  // namespace compiler
}  // namespace planner
}  // namespace carnot
}  // namespace pl
