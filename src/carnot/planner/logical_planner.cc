#include "src/carnot/planner/logical_planner.h"
#include "src/carnot/planner/distributed/distributed_analyzer.h"

#include <utility>

#include "src/carnot/udf_exporter/udf_exporter.h"
#include "src/shared/scriptspb/scripts.pb.h"

namespace pl {
namespace carnot {
namespace planner {

using table_store::schemapb::Schema;

StatusOr<std::unique_ptr<RelationMap>> LogicalPlanner::MakeRelationMap(const Schema& schema_pb) {
  auto rel_map = std::make_unique<RelationMap>();
  for (auto& relation_pair : schema_pb.relation_map()) {
    pl::table_store::schema::Relation rel;
    PL_RETURN_IF_ERROR(rel.FromProto(&relation_pair.second));
    rel_map->emplace(relation_pair.first, rel);
  }

  return rel_map;
}

StatusOr<std::unique_ptr<CompilerState>> LogicalPlanner::CreateCompilerState(
    const Schema& schema, RegistryInfo* registry_info, int64_t max_output_rows_per_table) {
  PL_ASSIGN_OR_RETURN(std::unique_ptr<RelationMap> rel_map, MakeRelationMap(schema));

  // Create a CompilerState obj using the relation map and grabbing the current time.

  return std::make_unique<planner::CompilerState>(std::move(rel_map), registry_info,
                                                  pl::CurrentTimeNS(), max_output_rows_per_table);
}

StatusOr<std::unique_ptr<LogicalPlanner>> LogicalPlanner::Create(const udfspb::UDFInfo& udf_info) {
  auto planner = std::unique_ptr<LogicalPlanner>(new LogicalPlanner());
  PL_RETURN_IF_ERROR(planner->Init(udf_info));
  return planner;
}

Status LogicalPlanner::Init(const udfspb::UDFInfo& udf_info) {
  compiler_ = compiler::Compiler();
  registry_info_ = std::make_unique<planner::RegistryInfo>();
  PL_RETURN_IF_ERROR(registry_info_->Init(udf_info));

  PL_ASSIGN_OR_RETURN(distributed_planner_, distributed::DistributedPlanner::Create());
  return Status::OK();
}

StatusOr<std::unique_ptr<distributed::DistributedPlan>> LogicalPlanner::Plan(
    const distributedpb::LogicalPlannerState& logical_state,
    const plannerpb::QueryRequest& query_request) {
  PL_ASSIGN_OR_RETURN(std::unique_ptr<RegistryInfo> registry_info, udfexporter::ExportUDFInfo());
  // Compile into the IR.
  auto ms = logical_state.plan_options().max_output_rows_per_table();
  LOG(ERROR) << "Max output rows: " << ms;
  PL_ASSIGN_OR_RETURN(std::unique_ptr<CompilerState> compiler_state,
                      CreateCompilerState(logical_state.schema(), registry_info.get(), ms));

  std::vector<plannerpb::QueryRequest::FlagValue> flag_values;
  for (const auto& flag_value : query_request.flag_values()) {
    flag_values.push_back(flag_value);
  }

  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<IR> single_node_plan,
      compiler_.CompileToIR(query_request.query_str(), compiler_state.get(), flag_values));
  // Create the distributed plan.
  return distributed_planner_->Plan(logical_state.distributed_state(), compiler_state.get(),
                                    single_node_plan.get());
}

StatusOr<plannerpb::QueryFlagsSpec> LogicalPlanner::GetAvailableFlags(
    const plannerpb::QueryRequest& query_request) {
  PL_ASSIGN_OR_RETURN(std::unique_ptr<RegistryInfo> registry_info, udfexporter::ExportUDFInfo());
  PL_ASSIGN_OR_RETURN(std::unique_ptr<CompilerState> compiler_state,
                      CreateCompilerState({}, registry_info.get(), 0));

  return compiler_.GetAvailableFlags(query_request.query_str(), compiler_state.get());
}

StatusOr<pl::shared::scriptspb::VizFuncsInfo> LogicalPlanner::GetVizFuncsInfo(
    const std::string& script_str) {
  PL_ASSIGN_OR_RETURN(std::unique_ptr<RegistryInfo> registry_info, udfexporter::ExportUDFInfo());
  PL_ASSIGN_OR_RETURN(std::unique_ptr<CompilerState> compiler_state,
                      CreateCompilerState({}, registry_info.get(), 0));

  return compiler_.GetVizFuncsInfo(script_str, compiler_state.get());
}

}  // namespace planner
}  // namespace carnot
}  // namespace pl
