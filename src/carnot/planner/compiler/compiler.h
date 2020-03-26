#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/carnot/planner/compiler/ast_visitor.h"
#include "src/carnot/planner/compiler_state/compiler_state.h"
#include "src/carnot/planner/ir/ir_nodes.h"
#include "src/carnot/planpb/plan.pb.h"

namespace pl {
namespace carnot {
namespace planner {
namespace compiler {

/**
 * The compiler takes a query in the form of a string and compiles it into a logical plan.
 */
class Compiler {
 public:
  /**
   * Compile the query into a logical plan.
   * @param query the query to compile
   * @param compiler_state compiler state
   * @param flag_values values for flags passed in with the query
   * @return the logical plan in the form of a plan protobuf message.
   */
  StatusOr<planpb::Plan> Compile(const std::string& query, CompilerState* compiler_state,
                                 const ArgValues& arg_values);
  StatusOr<std::shared_ptr<IR>> CompileToIR(const std::string& query, CompilerState* compiler_state,
                                            const ArgValues& arg_values);
  StatusOr<shared::scriptspb::FuncArgsSpec> GetMainFuncArgsSpec(const std::string& query,
                                                                CompilerState* compiler_state);
  StatusOr<pl::shared::scriptspb::VisFuncsInfo> GetVisFuncsInfo(const std::string& query,
                                                                CompilerState* compiler_state);

 private:
  StatusOr<std::shared_ptr<IR>> QueryToIR(const std::string& query, CompilerState* compiler_state,
                                          const ArgValues& arg_values);

  Status Analyze(IR* ir, CompilerState* compiler_state);
  Status VerifyGraphHasMemorySink(IR* ir);
};

}  // namespace compiler
}  // namespace planner
}  // namespace carnot
}  // namespace pl
