#pragma once

#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>

#include <memory>
#include <string>
#include <vector>

#include <absl/strings/substitute.h>
#include "src/carnot/compiler/distributedpb/distributed_plan.pb.h"
#include "src/carnot/plan/dag.h"
#include "src/carnot/plan/plan_fragment.h"
#include "src/carnot/plan/plan_graph.h"
#include "src/common/base/base.h"

namespace pl {
namespace carnot {
namespace compiler {
namespace logical_planner {
namespace testutils {

/**
 * This files provides canonical test protos that
 * other parts of the project can use to provide "fakes" for the
 * plan.
 *
 * Protos in this file are always valid as they are not expected to be used for
 * error case testing.
 */

constexpr char kSchema[] = R"proto(
relation_map {
  key: "table1"
  value {
    columns {
      column_name: "time_"
      column_type: TIME64NS
    }
    columns {
      column_name: "cpu_cycles"
      column_type: INT64
    }
    columns {
      column_name: "upid"
      column_type: UINT128
    }
  }
}

)proto";

constexpr char kHttpEventsSchema[] = R"proto(
relation_map {
  key: "http_events"
  value {
    columns {
      column_name: "time_"
      column_type: TIME64NS
    }
    columns {
      column_name: "upid"
      column_type: UINT128
    }
    columns {
      column_name: "remote_addr"
      column_type: STRING
    }
    columns {
      column_name: "remote_port"
      column_type: INT64
    }
    columns {
      column_name: "http_major_version"
      column_type: INT64
    }
    columns {
      column_name: "http_minor_version"
      column_type: INT64
    }
    columns {
      column_name: "http_content_type"
      column_type: INT64
    }
    columns {
      column_name: "http_req_headers"
      column_type: STRING
    }
    columns {
      column_name: "http_req_method"
      column_type: STRING
    }
    columns {
      column_name: "http_req_path"
      column_type: STRING
    }
    columns {
      column_name: "http_req_body"
      column_type: STRING
    }
    columns {
      column_name: "http_resp_headers"
      column_type: STRING
    }
    columns {
      column_name: "http_resp_status"
      column_type: INT64
    }
    columns {
      column_name: "http_resp_message"
      column_type: STRING
    }
    columns {
      column_name: "http_resp_body"
      column_type: STRING
    }
    columns {
      column_name: "http_resp_latency_ns"
      column_type: INT64
    }
  }
}
)proto";

constexpr char kAgentCarnotInfoTpl[] = R"proto(
query_broker_address: "$0"
has_grpc_server: false
has_data_store: true
processes_data: true
accepts_remote_sources: false
asid: $1
$2
)proto";

constexpr char kKelvinCarnotInfoTpl[] = R"proto(
query_broker_address: "$0"
grpc_address: "$1"
has_grpc_server: true
has_data_store: false
processes_data: true
accepts_remote_sources: true
asid: $2
)proto";

constexpr char kTableInfoTpl[] = R"proto(
table_info{
  table: "$0"
  tabletization_key: "$1"
  $2
}
)proto";

constexpr char kTabletValueTpl[] = R"proto(
tablets: "$0"
)proto";

constexpr char kQueryForTwoAgents[] = "df = px.DataFrame(table = 'table1')\npx.display(df, 'out')";

constexpr char kHttpRequestStats[] = R"pxl(
t1 = px.DataFrame(table='http_events', start_time='-30s')

t1['service'] = t1.ctx['service']
t1['http_resp_latency_ms'] = t1['http_resp_latency_ns'] / 1.0E6
t1['failure'] = t1['http_resp_status'] >= 400
t1['range_group'] = t1['time_'] - px.modulo(t1['time_'], 1000000000)

quantiles_agg = t1.groupby('service').agg(
  latency_quantiles=('http_resp_latency_ms', px.quantiles),
  errors=('failure', px.mean),
  throughput_total=('http_resp_status', px.count),
)

quantiles_agg['latency_p50'] = px.pluck(quantiles_agg['latency_quantiles'], 'p50')
quantiles_agg['latency_p90'] = px.pluck(quantiles_agg['latency_quantiles'], 'p90')
quantiles_agg['latency_p99'] = px.pluck(quantiles_agg['latency_quantiles'], 'p99')
quantiles_table = quantiles_agg[['service', 'latency_p50', 'latency_p90', 'latency_p99', 'errors', 'throughput_total']]

# The Range aggregate to calcualte the requests per second.
requests_agg = t1.groupby(['service', 'range_group']).agg(
  requests_per_window=('http_resp_status', px.count),
)

rps_table = requests_agg.groupby('service').agg(rps=('requests_per_window',px.mean))

joined_table = quantiles_table.merge(rps_table,
                                     how='inner',
                                     left_on=['service'],
                                     right_on=['service'],
                                     suffixes=['', '_x'])

joined_table['latency(p50)'] = joined_table['latency_p50']
joined_table['latency(p90)'] = joined_table['latency_p90']
joined_table['latency(p99)'] = joined_table['latency_p99']
joined_table['throughput (rps)'] = joined_table['rps']
joined_table['throughput total'] = joined_table['throughput_total']

joined_table = joined_table[[
  'service',
  'latency(p50)',
  'latency(p90)',
  'latency(p99)',
  'errors',
  'throughput (rps)',
  'throughput total']]
df = joined_table[joined_table['service'] != '']
px.display(df)
)pxl";

distributedpb::DistributedState LoadDistributedStatePb(const std::string& distributed_state_str) {
  distributedpb::DistributedState distributed_state_pb;
  CHECK(
      google::protobuf::TextFormat::MergeFromString(distributed_state_str, &distributed_state_pb));
  return distributed_state_pb;
}

table_store::schemapb::Schema LoadSchemaPb(std::string_view schema_str) {
  table_store::schemapb::Schema schema_pb;
  CHECK(google::protobuf::TextFormat::MergeFromString(schema_str.data(), &schema_pb));
  return schema_pb;
}

distributedpb::LogicalPlannerState LoadLogicalPlannerStatePB(
    const std::string& distributed_state_str, table_store::schemapb::Schema schema) {
  distributedpb::LogicalPlannerState logical_planner_state_pb;
  *(logical_planner_state_pb.mutable_distributed_state()) =
      LoadDistributedStatePb(distributed_state_str);
  *(logical_planner_state_pb.mutable_schema()) = schema;
  return logical_planner_state_pb;
}

distributedpb::LogicalPlannerState LoadLogicalPlannerStatePB(
    const std::string& distributed_state_str, std::string_view schema_str) {
  return LoadLogicalPlannerStatePB(distributed_state_str, LoadSchemaPb(schema_str));
}

std::string MakeTableInfoStr(const std::string& table_name, const std::string& tabletization_key,
                             const std::vector<std::string>& tablets) {
  std::vector<std::string> formatted_tablets;
  for (const auto& t : tablets) {
    formatted_tablets.push_back(absl::Substitute(kTabletValueTpl, t));
  }
  return absl::Substitute(kTableInfoTpl, table_name, tabletization_key,
                          absl::StrJoin(formatted_tablets, "\n"));
}

std::string MakeAgentCarnotInfo(const std::string& agent_name, uint32_t asid,
                                const std::vector<std::string>& table_info) {
  return absl::Substitute(kAgentCarnotInfoTpl, agent_name, asid, absl::StrJoin(table_info, "\n"));
}

std::string MakeKelvinCarnotInfo(const std::string& kelvin_name, const std::string& grpc_address,
                                 uint32_t asid) {
  return absl::Substitute(kKelvinCarnotInfoTpl, kelvin_name, grpc_address, asid);
}

std::string MakeDistributedState(const std::vector<std::string>& carnot_info_strs) {
  std::vector<std::string> carnot_info_proto_strs;
  for (const auto& carnot_info : carnot_info_strs) {
    std::string proto_tpl = R"proto(carnot_info{
      $0
    })proto";
    carnot_info_proto_strs.push_back(absl::Substitute(proto_tpl, carnot_info));
  }
  return absl::StrJoin(carnot_info_proto_strs, "\n");
}

distributedpb::LogicalPlannerState CreateTwoAgentsPlannerState(
    table_store::schemapb::Schema schema) {
  distributedpb::LogicalPlannerState plan;
  std::string table_name = "table1";
  std::string tabletization_key = "upid";
  std::string table_info1 = MakeTableInfoStr(table_name, tabletization_key, {"1", "2"});
  std::string table_info2 = MakeTableInfoStr(table_name, tabletization_key, {"3", "4"});
  std::string distributed_state_proto =
      MakeDistributedState({MakeAgentCarnotInfo("agent1", 123, {table_info1}),
                            MakeAgentCarnotInfo("agent2", 456, {table_info2})});

  return LoadLogicalPlannerStatePB(distributed_state_proto, schema);
}

distributedpb::LogicalPlannerState CreateTwoAgentsPlannerState(std::string_view schema) {
  return CreateTwoAgentsPlannerState(LoadSchemaPb(schema));
}

distributedpb::LogicalPlannerState CreateTwoAgentsPlannerState() {
  return CreateTwoAgentsPlannerState(kSchema);
}

distributedpb::LogicalPlannerState CreateOneAgentOneKelvinPlannerState(
    table_store::schemapb::Schema schema) {
  distributedpb::LogicalPlannerState plan;
  std::string table_info1 = MakeTableInfoStr("table1", "upid", {"1", "2"});
  std::string distributed_state_proto =
      MakeDistributedState({MakeAgentCarnotInfo("agent", 123, {table_info1}),
                            MakeKelvinCarnotInfo("agent", "1111", 456)});

  return LoadLogicalPlannerStatePB(distributed_state_proto, schema);
}

distributedpb::LogicalPlannerState CreateOneAgentOneKelvinPlannerState(std::string_view schema) {
  return CreateOneAgentOneKelvinPlannerState(LoadSchemaPb(schema));
}

distributedpb::LogicalPlannerState CreateOneAgentOneKelvinPlannerState() {
  return CreateOneAgentOneKelvinPlannerState(kSchema);
}

std::string TwoAgentsOneKelvinDistributedState() {
  std::string table_name = "table1";
  std::string tabletization_key = "upid";
  std::string table_info1 = MakeTableInfoStr(table_name, tabletization_key, {"1", "2"});
  std::string table_info2 = MakeTableInfoStr(table_name, tabletization_key, {"3", "4"});
  return MakeDistributedState({MakeAgentCarnotInfo("agent1", 123, {table_info1}),
                               MakeAgentCarnotInfo("agent2", 456, {table_info2}),
                               MakeKelvinCarnotInfo("kelvin", "1111", 789)});
}

distributedpb::LogicalPlannerState CreateTwoAgentsOneKelvinPlannerState(const std::string& schema) {
  std::string distributed_state_proto = TwoAgentsOneKelvinDistributedState();
  return LoadLogicalPlannerStatePB(distributed_state_proto, schema);
}

distributedpb::LogicalPlannerState CreateTwoAgentsOneKelvinPlannerState(
    table_store::schemapb::Schema schema) {
  auto distributed_state_proto = TwoAgentsOneKelvinDistributedState();
  return LoadLogicalPlannerStatePB(distributed_state_proto, schema);
}

distributedpb::LogicalPlannerState CreateTwoAgentsOneKelvinPlannerState() {
  return CreateTwoAgentsOneKelvinPlannerState(kSchema);
}

constexpr char kExpectedPlanTwoAgents[] = R"proto(
qb_address_to_plan {
  key: "agent1"
  value {
    nodes {
      id: 1
      dag {
        nodes {
          id: 10
          sorted_children: 11
        }
        nodes {
          id: 9
          sorted_children: 11
        }
        nodes {
          id: 11
          sorted_children: 7
          sorted_parents: 9
          sorted_parents: 10
        }
        nodes {
          id: 7
          sorted_parents: 11
        }
      }
      nodes {
        id: 10
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "2"
          }
        }
      }
      nodes {
        id: 9
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "1"
          }
        }
      }
      nodes {
        id: 11
        op {
          op_type: UNION_OPERATOR
          union_op {
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
          }
        }
      }
      nodes {
        id: 7
        op {
          op_type: MEMORY_SINK_OPERATOR
          mem_sink_op {
            name: "out"
          }
        }
      }
    }
  }
}
qb_address_to_plan {
  key: "agent2"
  value {
    nodes {
      id: 1
      dag {
        nodes {
          id: 10
          sorted_children: 11
        }
        nodes {
          id: 9
          sorted_children: 11
        }
        nodes {
          id: 11
          sorted_children: 7
          sorted_parents: 9
          sorted_parents: 10
        }
        nodes {
          id: 7
          sorted_parents: 11
        }
      }
      nodes {
        id: 10
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "4"
          }
        }
      }
      nodes {
        id: 9
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "3"
          }
        }
      }
      nodes {
        id: 11
        op {
          op_type: UNION_OPERATOR
          union_op {
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
          }
        }
      }
      nodes {
        id: 7
        op {
          op_type: MEMORY_SINK_OPERATOR
          mem_sink_op {
            name: "out"
          }
        }
      }
    }
  }
}
qb_address_to_dag_id {
  key: "agent1"
  value: 0
}
qb_address_to_dag_id {
  key: "agent2"
  value: 1
}
dag {
  nodes {
    id: 1
  }
  nodes {
  }
}
)proto";

constexpr char kExpectedPlanTwoAgentOneKelvin[] = R"proto(
  qb_address_to_plan {
  key: "agent1"
  value {
    nodes {
      id: 1
      dag {
        nodes {
          id: 12
          sorted_children: 13
        }
        nodes {
          id: 11
          sorted_children: 13
        }
        nodes {
          id: 13
          sorted_children: 9
          sorted_parents: 11
          sorted_parents: 12
        }
        nodes {
          id: 9
          sorted_parents: 13
        }
      }
      nodes {
        id: 12
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "2"
          }
        }
      }
      nodes {
        id: 11
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "1"
          }
        }
      }
      nodes {
        id: 13
        op {
          op_type: UNION_OPERATOR
          union_op {
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
          }
        }
      }
      nodes {
        id: 9
        op {
          op_type: GRPC_SINK_OPERATOR
          grpc_sink_op {
            address: "1111"
            destination_id: 10
          }
        }
      }
    }
  }
}
qb_address_to_plan {
  key: "agent2"
  value {
    nodes {
      id: 1
      dag {
        nodes {
          id: 12
          sorted_children: 13
        }
        nodes {
          id: 11
          sorted_children: 13
        }
        nodes {
          id: 13
          sorted_children: 9
          sorted_parents: 11
          sorted_parents: 12
        }
        nodes {
          id: 9
          sorted_parents: 13
        }
      }
      nodes {
        id: 12
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "4"
          }
        }
      }
      nodes {
        id: 11
        op {
          op_type: MEMORY_SOURCE_OPERATOR
          mem_source_op {
            name: "table1"
            tablet: "3"
          }
        }
      }
      nodes {
        id: 13
        op {
          op_type: UNION_OPERATOR
          union_op {
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
          }
        }
      }
      nodes {
        id: 9
        op {
          op_type: GRPC_SINK_OPERATOR
          grpc_sink_op {
            address: "1111"
            destination_id: 9
          }
        }
      }
    }
  }
}
qb_address_to_plan {
  key: "kelvin"
  value {
    dag {
      nodes {
        id: 1
      }
    }
    nodes {
      id: 1
      dag {
        nodes {
          id: 10
          sorted_children: 11
        }
        nodes {
          id: 9
          sorted_children: 11
        }
        nodes {
          id: 11
          sorted_children: 7
          sorted_parents: 9
          sorted_parents: 10
        }
        nodes {
          id: 7
          sorted_parents: 11
        }
      }
      nodes {
        id: 10
        op {
          op_type: GRPC_SOURCE_OPERATOR
          grpc_source_op {
            column_types: TIME64NS
            column_types: INT64
            column_types: UINT128
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
          }
        }
      }
      nodes {
        id: 9
        op {
          op_type: GRPC_SOURCE_OPERATOR
          grpc_source_op {
            column_types: TIME64NS
            column_types: INT64
            column_types: UINT128
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
          }
        }
      }
      nodes {
        id: 11
        op {
          op_type: UNION_OPERATOR
          union_op {
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
            column_mappings {
              column_indexes: 0
              column_indexes: 1
              column_indexes: 2
            }
          }
        }
      }
      nodes {
        id: 7
        op {
          op_type: MEMORY_SINK_OPERATOR
          mem_sink_op {
            name: "out"
            column_types: TIME64NS
            column_types: INT64
            column_types: UINT128
            column_names: "time_"
            column_names: "cpu_cycles"
            column_names: "upid"
          }
        }
      }
    }
  }
}
qb_address_to_dag_id {
  key: "agent1"
  value: 1
}
qb_address_to_dag_id {
  key: "agent2"
  value: 2
}
qb_address_to_dag_id {
  key: "kelvin"
  value: 0
}
dag {
  nodes {
    id: 2
    sorted_children: 0
  }
  nodes {
    id: 1
    sorted_children: 0
  }
  nodes {
    sorted_parents: 1
    sorted_parents: 2
  }
}
)proto";

constexpr char kThreeAgentsOneKelvinDistributedState[] = R"proto(
carnot_info {
  query_broker_address: "agent1"
  has_grpc_server: false
  has_data_store: true
  processes_data: true
  accepts_remote_sources: false
  asid: 123
}
carnot_info {
  query_broker_address: "agent2"
  has_grpc_server: false
  has_data_store: true
  processes_data: true
  accepts_remote_sources: false
  asid: 789
}
carnot_info {
  query_broker_address: "agent3"
  has_grpc_server: false
  has_data_store: true
  processes_data: true
  accepts_remote_sources: false
  asid: 111
}
carnot_info {
  query_broker_address: "kelvin"
  grpc_address: "1111"
  has_grpc_server: true
  has_data_store: false
  processes_data: true
  accepts_remote_sources: true
  asid: 456
}
)proto";

constexpr char kOneAgentOneKelvinDistributedState[] = R"proto(
carnot_info {
  query_broker_address: "agent"
  has_grpc_server: false
  has_data_store: true
  processes_data: true
  accepts_remote_sources: false
  asid: 123
}
carnot_info {
  query_broker_address: "kelvin"
  grpc_address: "1111"
  has_grpc_server: true
  has_data_store: false
  processes_data: true
  accepts_remote_sources: true
  asid: 456
}
)proto";

constexpr char kOneAgentThreeKelvinsDistributedState[] = R"proto(
carnot_info {
  query_broker_address: "agent"
  has_grpc_server: false
  has_data_store: true
  processes_data: true
  accepts_remote_sources: false
  asid: 123
}
carnot_info {
  query_broker_address: "kelvin1"
  grpc_address: "1111"
  has_grpc_server: true
  has_data_store: false
  processes_data: true
  accepts_remote_sources: true
  asid: 456
}
carnot_info {
  query_broker_address: "kelvin2"
  grpc_address: "1112"
  has_grpc_server: true
  has_data_store: false
  processes_data: true
  accepts_remote_sources: true
  asid: 222
}
carnot_info {
  query_broker_address: "kelvin3"
  grpc_address: "1113"
  has_grpc_server: true
  has_data_store: false
  processes_data: true
  accepts_remote_sources: true
  asid: 333
}
)proto";

}  // namespace testutils
}  // namespace logical_planner
}  // namespace compiler
}  // namespace carnot
}  // namespace pl
