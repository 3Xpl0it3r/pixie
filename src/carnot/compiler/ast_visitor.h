#pragma once
#include <algorithm>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pypa/ast/ast.hh>
#include <pypa/ast/tree_walker.hh>

#include "src/carnot/compiler/ast/ast_visitor.h"
#include "src/carnot/compiler/compiler_state/compiler_state.h"
#include "src/carnot/compiler/ir/ast_utils.h"
#include "src/carnot/compiler/ir/ir_nodes.h"
#include "src/carnot/compiler/objects/dataframe.h"
#include "src/carnot/compiler/objects/pixie_module.h"
#include "src/carnot/compiler/var_table.h"

namespace pl {
namespace carnot {
namespace compiler {

#define PYPA_PTR_CAST(TYPE, VAL) \
  std::static_pointer_cast<typename pypa::AstTypeByID<pypa::AstType::TYPE>::Type>(VAL)

#define PYPA_CAST(TYPE, VAL) static_cast<AstTypeByID<AstType::TYPE>::Type&>(VAL)

struct OperatorContext {
  const std::vector<OperatorIR*> parent_ops;
  std::string operator_name;
  // A list of the names of dataframes that can be accessed in this operator.
  const std::vector<std::string> referenceable_dataframes;
  OperatorContext(const std::vector<OperatorIR*>& parents, std::string op_name)
      : OperatorContext(parents, op_name, {}) {}
  OperatorContext(const std::vector<OperatorIR*>& parents, std::string op_name,
                  const std::vector<std::string>& dfs)
      : parent_ops(parents), operator_name(op_name), referenceable_dataframes(dfs) {}
  OperatorContext(const std::vector<OperatorIR*>& parents, OperatorIR* op)
      : parent_ops(parents), operator_name(op->type_string()) {}
};

class ASTVisitorImpl : public ASTVisitor {
 public:
  /**
   * @brief Creates a top-level AST Visitor with the given graph and compiler state.
   * Variable table and the pixie module are created with this call.
   *
   * @param graph
   * @param compiler_state
   * @param flag_values
   * @return StatusOr<std::shared_ptr<ASTVisitorImpl>>
   */
  static StatusOr<std::shared_ptr<ASTVisitorImpl>> Create(IR* graph, CompilerState* compiler_state,
                                                          const FlagValues& flag_values);

  /**
   * @brief The entry point into traversal as the root AST is a module.
   *
   * @param node: the ptr to the ast node.
   * @return Status
   */
  Status ProcessModuleNode(const pypa::AstModulePtr& m) override;

  /**
   * @brief Processes a single expression into an IRNode. If the parameter has more than one
   * line or contains a module we can't process, then it will error out.
   *
   * @param m the poitner to the parsed ast module.
   * @param op_context the operator context to operate with.
   * @return StatusOr<QLObjectPtr> the QL object representation of the expression, or an error if
   * the operator fails.
   */
  StatusOr<QLObjectPtr> ProcessSingleExpressionModule(const pypa::AstModulePtr& m) override;

  /**
   * @brief Parses and processes out a single expression in the form of an IRNode.
   *
   * @param str the input string
   * @return StatusOr<QLObjectPtr> the QL object of the expression or an error if something fails
   * during processing.
   */
  StatusOr<QLObjectPtr> ParseAndProcessSingleExpression(std::string_view str) override;

  StatusOr<plannerpb::QueryFlagsSpec> GetAvailableFlags(const pypa::AstModulePtr&) override;

  IR* ir_graph() const { return ir_graph_; }
  std::shared_ptr<VarTable> var_table() const { return var_table_; }

  // Reserved keywords
  inline static constexpr char kTimeConstantColumnName[] = "time_";
  inline static constexpr char kStringTypeName[] = "str";
  inline static constexpr char kIntTypeName[] = "int";
  inline static constexpr char kNoneName[] = "None";

 private:
  /**
   * @brief Construct a new ASTWalker object.
   * This constructor will be used at the top level.
   *
   * @param ir_graph
   */
  ASTVisitorImpl(IR* ir_graph, CompilerState* compiler_state, std::shared_ptr<VarTable> var_table)
      : ir_graph_(ir_graph), compiler_state_(compiler_state), var_table_(var_table) {}

  /**
   * @brief Creates a child AST Visitor from the top-level AST Visitor, sharing the graph,
   * compiler_state, and creating a child var table.
   *
   * @return std::shared_ptr<ASTVisitorImpl>
   */
  std::shared_ptr<ASTVisitorImpl> CreateChild();

  Status InitGlobals(const FlagValues& flag_values);

  /**
   * @brief Process statements list passed in as an argument.
   *
   * @param body the body ast object to parse.
   * @param is_function_definition_body whether the body is in a function definition. There are some
   * extra steps that occur when is_function_definition_body is true, ie processing Return
   * statements.
   * @return Status any errors that come up.
   */
  StatusOr<QLObjectPtr> ProcessASTSuite(const pypa::AstSuitePtr& body,
                                        bool is_function_definition_body);

  /**
   * @brief Processes any expression node into a QLObjectPtr.
   *
   * @param node
   * @return StatusOr<QLObjectPtr>
   */
  StatusOr<QLObjectPtr> Process(const pypa::AstExpr& node, const OperatorContext& op_context);

  /**
   * @brief ProcessArgs traverses an arg_ast tree, converts the expressions into IR and then returns
   * a data structure of positional and keyword arguments representing the arguments in IR.
   *
   * @param call_ast the ast node that calls the arguments.
   * @param op_context the op context used to process args.
   * @return StatusOr<ArgMap> a mapping of arguments (positional and kwargs) to the resulting
   * IRNode.
   */
  StatusOr<ArgMap> ProcessArgs(const pypa::AstCallPtr& call_ast, const OperatorContext& op_context);

  /**
   * @brief ProcessExprStmtNode handles full lines that are expression statements.
   * ie in the following lines
   *  1: a = pl.DataFrame(...)
   *  2: a.drop(...)
   * Line 1 will be wrapped in an AstAssignNode
   * Line 2 will be wrapped in an AstExpressionStatementNode.
   *
   * The entirety of line 2 is the expression statement and will be handled by this function.
   *
   * @param node
   * @return Status: whether the expression could be parsed or not.
   */
  Status ProcessExprStmtNode(const pypa::AstExpressionStatementPtr& node);

  /**
   * @brief ProcessSubscriptAssignment (& Attribute) handles lines with statements like
   *  1: a['foo'] = a['bar'] * 2 + a.abc
   *  1: a.foo = a.bar * 2 + a['col with space']
   *   *
   * @param assign_node the target of the assignment
   * @param expr_node the expression of the assignment
   * @return Status whether the assignment worked or not.
   */
  Status ProcessSubscriptAssignment(const pypa::AstSubscriptPtr& assign_node,
                                    const pypa::AstPtr& expr_node);
  Status ProcessAttributeAssignment(const pypa::AstAttributePtr& assign_node,
                                    const pypa::AstPtr& expr_node);
  Status ProcessMapAssignment(const pypa::AstNamePtr& assign_name, IRNode* processed_column,
                              const pypa::AstPtr& expr_node);
  /**
   * @brief ProcessAssignNode handles lines where an expression is assigned to a value.
   * ie in the following lines
   *  1: a = pl.DataFrame(...)
   *  2: a.drop(...)
   * Line 1 will be wrapped in an AstAssignNode
   * Line 2 will be wrapped in an AstExpressionStatementNode.
   *
   * The entirety of line 1 is the assign statement and will be handled by this function.
   *
   * @param node
   * @return Status whether the assignment worked or not.
   */
  Status ProcessAssignNode(const pypa::AstAssignPtr& node);

  /**
   * @brief ProcessFunctionDefNode handles function definitions in the query language.
   * ```
   * def func(blah):
   *     return blah
   * ```
   *
   * @param node
   * @return Status of whether creation the function definition worked or not.
   */
  Status ProcessFunctionDefNode(const pypa::AstFunctionDefPtr& node);

  /**
   * @brief Gets the function name out of the call node into a string.
   *
   * @param call ptr ast node.
   * @return StatusOr<std::string> the string name of the function.
   */
  StatusOr<std::string> GetFuncName(const pypa::AstCallPtr& node);

  /**
   * @brief ProcessOpCallNode handles call nodes which are created for any function call
   * ie
   *  drop(...)
   *
   * Meant to handle operators and only extracts the name of the function, then passes to
   * ProcessFunc.
   *
   *
   * @param node
   * @return the op contained by the call ast.
   */
  StatusOr<QLObjectPtr> ProcessCallNode(const pypa::AstCallPtr& node,
                                        const OperatorContext& op_context);

  /**
   * @brief Processes a subscript call.
   *
   * @param node
   * @return the operator contained by the subscript.
   */
  StatusOr<QLObjectPtr> ProcessSubscriptCall(const pypa::AstSubscriptPtr& node,
                                             const OperatorContext& op_context);

  /**
   * @brief Validates whether the value you subscript on is valid. Only does this if on the
   * MapContext.
   *
   * @param node
   * @param op_context
   * @return StatusOr<QLObjectPtr>
   */
  Status ValidateSubscriptValue(const pypa::AstExpr& node, const OperatorContext& op_context);

  /**
   * @brief Processes an Attribute and returns the QLObject that the attribute references.
   *
   * @param node
   * @return StatusOr<QLObjectPtr>
   */
  StatusOr<QLObjectPtr> ProcessAttribute(const pypa::AstAttributePtr& node,
                                         const OperatorContext& op_context);

  /**
   * @brief Gets the string name of the attribute in an attribute struvct.
   * (AstAttribute := <value>.<attribute>; `abc.blah()` -> `blah` is the attribute of `abc.blah`)
   *
   * @param attr the attribute struct to grab the attribute field from as a string.
   * @return the string representation of the attribute in the attribute structure.
   */
  StatusOr<std::string> GetAttributeStr(const pypa::AstAttributePtr& attr);

  /**
   * @brief Helper function for processing lists and tuples children into IR nodes.
   *
   * @param elements elements of the input collection
   * @param op_context: The context of the operator which this is contained within.
   * @return StatusOr<std::vector<IRNode*>> the IR representation of the list.
   */
  StatusOr<std::vector<IRNode*>> ProcessCollectionChildren(const pypa::AstExprList& elements,
                                                           const OperatorContext& op_context);

  /**
   * @brief Processes a list ptr into an IR node.
   *
   * @param ast
   * @param op_context: The context of the operator which this is contained within.
   * @return StatusOr<IRNode*> the IR representation of the list.
   */
  StatusOr<QLObjectPtr> ProcessList(const pypa::AstListPtr& ast, const OperatorContext& op_context);

  /**
   * @brief Processes a tuple ptr into an IR node.
   *
   * @param ast
   * @param op_context: The context of the operator which this is contained within.
   * @return StatusOr<IRNode*> the IR representation of the liset.
   */
  StatusOr<QLObjectPtr> ProcessTuple(const pypa::AstTuplePtr& ast,
                                     const OperatorContext& op_context);

  /**
   * @brief Processes a number into an IR Node.
   *
   * @param node
   * @return StatusOr<ExpressionIR*> the IR representation of the number.
   */
  StatusOr<QLObjectPtr> ProcessNumber(const pypa::AstNumberPtr& node);

  /**
   * @brief Processes a str ast ptr into an IR node.
   *
   * @param ast
   * @return StatusOr<ExpressionIR*> the ir representation of the string.
   */
  StatusOr<QLObjectPtr> ProcessStr(const pypa::AstStrPtr& ast);

  /**
   * @brief ProcessData takes in what are typically function arguments and returns the
   * approriate data representation.
   *
   * Ie it might take in an AST tree that represents a list of strings, and convert that into a
   * ListIR node.
   *
   * @param ast
   * @param op_context: The context of the operator which this is contained within.
   * @return StatusOr<IRNode*>
   */
  StatusOr<IRNode*> ProcessData(const pypa::AstPtr& ast, const OperatorContext& op_context);

  /**
   * @brief Gets the name string contained within the Name ast node and returns the IRNode
   * referenced by that name, or errors out with an undefined variable.
   *
   * @param name
   * @return StatusOr<OperatorIR*> - The operator referenced by the name, or an error if not
   * found.
   */
  StatusOr<OperatorIR*> LookupName(const pypa::AstNamePtr& name);

  /**
   * @brief Returns the FuncIR::Op struct that corresponds to a python_op
   * representation. This includes the operator code and the full carnot-udf name.
   *
   * @param python_op: the string representation as found in the AST to query on.
   * @param node: the pointer to ast.
   * @return StatusOr<FuncIR::op>: the struct that corresponds to a python op representation.
   */
  StatusOr<FuncIR::Op> GetOp(const std::string& python_op, pypa::AstPtr node);

  /**
   * @brief Returns the FuncIR::Op struct that corresponds to a python_op representation for a unary
   * operator.
   *
   * @param python_op: the string representation as found in the AST.
   * @param node: the pointer to ast.
   * @return StatusOr<FuncIR::Op>: the struct that corresponds to a python op representation.
   */
  StatusOr<FuncIR::Op> GetUnaryOp(const std::string& python_op, const pypa::AstPtr node);
  /**
   * @brief Handler for Binary operations.
   *
   * @param ast
   * @param op_context: The context of the operator which this is contained within.
   * @return StatusOr<ExpressionIR*>
   */
  StatusOr<QLObjectPtr> ProcessDataBinOp(const pypa::AstBinOpPtr& node,
                                         const OperatorContext& op_context);

  /**
   * @brief Handler for Bool operations.
   *
   * @param ast
   * @param op_context: The context of the operator which this is contained within.
   * @return StatusOr<ExpressionIR*>
   */
  StatusOr<QLObjectPtr> ProcessDataBoolOp(const pypa::AstBoolOpPtr& node,
                                          const OperatorContext& op_context);

  /**
   * @brief Processes compare nodes
   *
   * @param node
   * @param op_context
   * @return the evaluated data compare object.
   */
  StatusOr<QLObjectPtr> ProcessDataCompare(const pypa::AstComparePtr& node,
                                           const OperatorContext& op_context);
  /**
   * @brief Processes unary operator nodes
   *
   * @param node
   * @param op_context
   * @return the evaluated unary operator object
   */
  StatusOr<QLObjectPtr> ProcessDataUnaryOp(const pypa::AstUnaryOpPtr& node,
                                           const OperatorContext& op_context);
  /**
   * @brief  Returns the variable specified by the name pointer.
   * @param node the ast node to run this on.
   * @param name the string to lookup.
   * @return StatusOr<QLObjectPtr>  The pyobject referenced by name, or an error if not found.
   */
  StatusOr<QLObjectPtr> LookupVariable(const pypa::AstPtr& node, const std::string& name);
  StatusOr<QLObjectPtr> LookupVariable(const pypa::AstNamePtr& name) {
    return LookupVariable(name, name->id);
  }

  /**
   * @brief This is the FuncObject caller logic for functions defined inside of the query
   *
   * @param arg_names The name of the arguments that are passed in.
   * @param body the body of the defined function
   * @param ast the ast node of the function call
   * @param args the parsed arguments passed into the function during a call.
   * @return StatusOr<QLObjectPtr>
   */
  StatusOr<QLObjectPtr> FuncDefHandler(
      const std::vector<std::string>& arg_names,
      const absl::flat_hash_map<std::string, pypa::AstExpr>& arg_annotations,
      const pypa::AstSuitePtr& body, const pypa::AstPtr& ast, const ParsedArgs& args);

  /**
   * @brief Returns an error if the arg does not match the annotation.
   *
   * @param arg
   * @param annotation
   * @return Status
   */
  Status DoesArgMatchAnnotation(QLObjectPtr arg, const pypa::AstExpr& annotation);

  /**
   * @brief Handles the return statements of function definitions.
   *
   * @param ret the return ast node
   * @return StatusOr<QLObjectPtr> the ql object ptr meant by the return statement.
   */
  StatusOr<QLObjectPtr> ProcessFuncDefReturn(const pypa::AstReturnPtr& ret);

  // Calls a FuncObject. TODO(nserrino): Remove when PL-1431 is done.
  StatusOr<QLObjectPtr> CallFunc(const pypa::AstPtr& ast, QLObjectPtr ql_object);

  IR* ir_graph_;
  CompilerState* compiler_state_;
  std::shared_ptr<VarTable> var_table_;
};

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
