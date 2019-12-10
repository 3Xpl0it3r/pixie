#pragma once

#include <arrow/memory_pool.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/carnot/compiler/compiler.h"
#include "src/carnot/exec/exec_state.h"
#include "src/carnot/queryresultspb/query_results.pb.h"
#include "src/common/base/base.h"
#include "src/shared/metadata/metadata_state.h"
#include "src/table_store/table_store.h"

namespace pl {
namespace carnot {

struct CarnotQueryResult {
  size_t NumTables() const { return output_tables_.size(); }
  table_store::Table* GetTable(int64_t i) const { return output_tables_[i]; }
  /**
   * Convert this query result to a proto that can be sent over the wire.
   * @param query_result The query result to fill in.
   */
  Status ToProto(queryresultspb::QueryResult* query_result) const;
  std::vector<table_store::Table*> output_tables_;
  int64_t rows_processed = 0;
  int64_t bytes_processed = 0;
  int64_t compile_time_ns = 0;
  int64_t exec_time_ns = 0;
};

class Carnot : public NotCopyable {
 public:
  static StatusOr<std::unique_ptr<Carnot>> Create(
      std::shared_ptr<table_store::TableStore> table_store,
      const exec::KelvinStubGenerator& stub_generator, int grpc_server_port = 0,
      std::shared_ptr<grpc::ServerCredentials> grpc_server_creds = nullptr);
  using AgentMetadataCallbackFunc = std::function<std::shared_ptr<const md::AgentMetadataState>()>;

  virtual ~Carnot() = default;

  /**
   * Executes the given query.
   *
   * @param query the query in the form of a string.
   * @param time_now the current time.
   * @return a Carnot Return with output_tables if successful. Error status otherwise.
   */
  virtual StatusOr<CarnotQueryResult> ExecuteQuery(const std::string& query,
                                                   const sole::uuid& query_id,
                                                   types::Time64NSValue time_now) = 0;
  /**
   * Executes the given logical plan.
   *
   * @param plan the plan protobuf describing what should be compiled.
   * @return a Carnot Return with output_tables if successful. Error status otherwise.
   */
  virtual StatusOr<CarnotQueryResult> ExecutePlan(const planpb::Plan& plan,
                                                  const sole::uuid& query_id) = 0;

  /**
   * Registers the callback for updating the agents metadata state.
   */
  virtual void RegisterAgentMetadataCallback(AgentMetadataCallbackFunc func) = 0;
};

}  // namespace carnot
}  // namespace pl
