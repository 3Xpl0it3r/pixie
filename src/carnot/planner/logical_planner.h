#pragma once
#include <memory>
#include <string>
#include <vector>

#include "src/carnot/planner/compiler/compiler.h"
#include "src/carnot/planner/compiler_state/compiler_state.h"
#include "src/carnot/planner/distributed/distributed_plan.h"
#include "src/carnot/planner/distributed/distributed_planner.h"
#include "src/carnot/planner/distributed/tablet_rules.h"
#include "src/carnot/planner/plannerpb/func_args.pb.h"
#include "src/shared/scriptspb/scripts.pb.h"

namespace pl {
namespace carnot {
namespace planner {

/**
 * @brief The logical planner takes in queries and a Logical Planner State and
 *
 */
class LogicalPlanner : public NotCopyable {
 public:
  /**
   * @brief The Creation function for the planner.
   *
   * @return StatusOr<std::unique_ptr<DistributedPlanner>>: the distributed planner object or an
   * error.
   */
  static StatusOr<std::unique_ptr<LogicalPlanner>> Create(const udfspb::UDFInfo& udf_info);

  /**
   * @brief Takes in a logical plan and outputs the distributed plan.
   *
   * @param logical_state: the distributed layout of the vizier instance.
   * @param query: QueryRequest
   * @return std::unique_ptr<DistributedPlan> or error if one occurs during compilation.
   */
  StatusOr<std::unique_ptr<distributed::DistributedPlan>> Plan(
      const distributedpb::LogicalPlannerState& logical_state,
      const plannerpb::QueryRequest& query);

  /**
   * @brief Takes in a query request and outputs the flag spec for that request.
   *
   * @param query_request: the query request from the UI
   * @return QueryFlagsSpec or an error if one occurs during compilation.
   *
   * Currently, not implemented and just returns an empty QueryFlagsSpec
   */
  StatusOr<plannerpb::QueryFlagsSpec> GetAvailableFlags(
      const plannerpb::QueryRequest& query_request);

  /**
   * @brief Get the Main Func Args Spec for a query. Must have a main function in the query or the
   * method will return an error.
   *
   * @param query_request
   * @return StatusOr<shared::scriptspb::FuncArgsSpec>
   */
  StatusOr<shared::scriptspb::FuncArgsSpec> GetMainFuncArgsSpec(
      const plannerpb::QueryRequest& query_request);

  /**
   * @brief Takes in a script string and outputs information about viz funcs for that script.
   *
   * @param script: the string of the script.
   * @return VizFuncsInfo or error if one occurs during compilation.
   */
  StatusOr<pl::shared::scriptspb::VizFuncsInfo> GetVizFuncsInfo(const std::string& script_str);

  Status Init(std::unique_ptr<planner::RegistryInfo> registry_info);
  Status Init(const udfspb::UDFInfo& udf_info);

 protected:
  LogicalPlanner() {}

 private:
  StatusOr<std::unique_ptr<RelationMap>> MakeRelationMap(
      const table_store::schemapb::Schema& schema_pb);

  StatusOr<std::unique_ptr<CompilerState>> CreateCompilerState(
      const table_store::schemapb::Schema& schema, RegistryInfo* registry_info,
      int64_t max_output_rows_per_table);

  compiler::Compiler compiler_;
  std::unique_ptr<distributed::Planner> distributed_planner_;
  std::unique_ptr<planner::RegistryInfo> registry_info_;
};

}  // namespace planner
}  // namespace carnot
}  // namespace pl
