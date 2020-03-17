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

#include "src/carnot/planner/ir/ir_nodes.h"
#include "src/carnot/planner/plannerpb/func_args.pb.h"
#include "src/shared/scriptspb/scripts.pb.h"

namespace pl {
namespace carnot {
namespace planner {
namespace compiler {

// Forward declare;
class QLObject;
using QLObjectPtr = std::shared_ptr<QLObject>;

class ASTVisitor {
 public:
  // Interface boilerplate.
  virtual ~ASTVisitor() = default;

  /**
   * @brief The entry point into traversal as the root AST is a module.
   *
   * @param m the ptr to the ast node
   * @return Status
   */
  virtual Status ProcessModuleNode(const pypa::AstModulePtr& m) = 0;

  /**
   * @brief Processes a single expression into an IRNode. If the parameter has more than one
   * line or contains a module we can't process, then it will error out.
   *
   * @param m the ptr to the ast node
   * @param op_context the operator context to operate with.
   * @return StatusOr<QLObjectPtr> the QL object representation of the expression, or an error if
   * the operator fails.
   */
  virtual StatusOr<QLObjectPtr> ProcessSingleExpressionModule(const pypa::AstModulePtr& m) = 0;

  /**
   * @brief Parses and processes out a single expression in the form of an IRNode.
   *
   * @param str the input string
   * @param import_px whether or not to import the pixie module first.
   * @return StatusOr<QLObjectPtr> the QL object representation of the expression or an error if
   * something fails during processing.
   */
  virtual StatusOr<QLObjectPtr> ParseAndProcessSingleExpression(std::string_view str,
                                                                bool import_px) = 0;

  /**
   * @brief Parses the AST for the available flags (default, description, etc).
   *
   * @param m the ptr to the ast node
   * @return StatusOr<plannerpb::QueryFlagsSpec> the available flags
   *
   */
  virtual StatusOr<plannerpb::QueryFlagsSpec> GetAvailableFlags(const pypa::AstModulePtr& m) = 0;

  /**
   * @brief Get the Viz Funcs Info spec from functions decorated with the viz decorator.
   *
   * @return StatusOr<pl::shared::scriptspb::VizFuncsInfo> the
   */
  virtual StatusOr<pl::shared::scriptspb::VizFuncsInfo> GetVizFuncsInfo() const = 0;

  /**
   * @brief Get the Main Func Args Spec.
   *
   * @return StatusOr<shared::scriptspb::FuncArgsSpec> spec or error.
   */
  virtual StatusOr<shared::scriptspb::FuncArgsSpec> GetMainFuncArgsSpec() const = 0;
};

}  // namespace compiler
}  // namespace planner
}  // namespace carnot
}  // namespace pl
