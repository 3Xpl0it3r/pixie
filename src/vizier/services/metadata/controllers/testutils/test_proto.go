package testutils

// This file contains the protobufs used in vizier/services/metadata/controllers tests.

// AgentInfo

// ClockNowNS is the now time to use for the test clocks.
const ClockNowNS = 1e9 * 70 // 70s in NS. This is slightly greater than the expiration time for the unhealthy agent.
// HealthyAgentLastHeartbeatNS is the last heartbeat of the healthy agent.
const HealthyAgentLastHeartbeatNS = 1e9 * 65 // 65 seconds in NS. This is slightly less than the current time.

// NewAgentUUID is the UUID of the agent that doesn't yet exist.
var NewAgentUUID = "6ba7b810-9dad-11d1-80b4-00c04fd430c8"

// ExistingAgentUUID is the UUID of an agent that already exists and is healthy.
var ExistingAgentUUID = "7ba7b810-9dad-11d1-80b4-00c04fd430c8"

// UnhealthyAgentUUID is the UUID of an agent that exists but is unhealthy.
var UnhealthyAgentUUID = "8ba7b810-9dad-11d1-80b4-00c04fd430c8"

// KelvinAgentUUID is the UUID of a Kelvin agent.
var KelvinAgentUUID = "9ba7b810-9dad-11d1-80b4-00c04fd430c8"

// UnhealthyKelvinAgentUUID is the UUID of an unhealthy Kelvin agent.
var UnhealthyKelvinAgentUUID = "5ba7b810-9dad-11d1-80b4-00c04fd430c8"

// ExistingAgentInfo is the agent info for the healthy agent that already exists.
var ExistingAgentInfo = `
info {
  agent_id {
    data: "7ba7b8109dad11d180b400c04fd430c8"
  }
  host_info {
    hostname: "testhost"
  }
  capabilities {
    collects_data: true
  }
}
create_time_ns: 0
last_heartbeat_ns: 65000000000
asid: 123
`

// UnhealthyAgentInfo is the agent info for the unhealthy agent.
var UnhealthyAgentInfo = `
info {
  agent_id {
    data: "8ba7b8109dad11d180b400c04fd430c8"
  }
  host_info {
    hostname: "anotherhost"
  }
  capabilities {
    collects_data: true
  }
}
create_time_ns: 0
last_heartbeat_ns: 0
asid: 456
`

// UnhealthyKelvinAgentInfo is the agent info for the unhealthy kelvin.
var UnhealthyKelvinAgentInfo = `
info {
  agent_id {
    data: "5ba7b8109dad11d180b400c04fd430c8"
  }
  host_info {
    hostname: "abcd"
  }
  capabilities {
    collects_data: false
  }
}
create_time_ns: 0
last_heartbeat_ns: 0
asid: 789
`

// AgentStatus

// Agent1StatusPB is a protobuf for an agent status.
const Agent1StatusPB = `
agent {
  info {
    agent_id {
      data: "11285cdd-1de9-4ab1-ae6a-0ba08c8c676c"
    }
    host_info {
      hostname: "test_host"
    }
  }
  last_heartbeat_ns: 10
  create_time_ns: 5
  asid: 123
}
status {
  ns_since_last_heartbeat: 60
  state: 1
}
`

// Agent2StatusPB is the protobuf for another agent status.
const Agent2StatusPB = `
agent {
  info {
    agent_id {
      data: "21285cdd-1de9-4ab1-ae6a-0ba08c8c676c"
    }
    host_info {
      hostname: "another_host"
    }
  }
  last_heartbeat_ns: 20
  create_time_ns: 0
  asid: 456
}
status {
  ns_since_last_heartbeat: 50
  state: 1
}
`

// Processes

// ProcessCreated1PB is the protobuf for a created process.
var ProcessCreated1PB = `
upid {
  low: 89101
  high: 528280977975
}
pid: 123
start_timestamp_ns: 4
cmdline: "./bin/bash"
cid: "container_1"
`

// ProcessInfo1PB is the process info for the first created process.
var ProcessInfo1PB = `
upid {
  low: 89101
  high: 528280977975
}
pid: 123
start_timestamp_ns: 4
process_args: "./bin/bash"
cid: "container_1"
`

// ProcessCreated2PB is the protobuf for another created process.
var ProcessCreated2PB = `
upid {
  low: 468
  high: 528280977975
}
pid: 456
start_timestamp_ns: 4
cmdline: "test"
cid: "container_2"
`

// ProcessInfo2PB is the process info for the second created process.
var ProcessInfo2PB = `
upid {
  low: 468
  high: 528280977975
}
pid: 456
start_timestamp_ns: 4
process_args: "test"
cid: "container_2"
`

// ProcessTerminated1PB is the protobuf for a terminated process.
var ProcessTerminated1PB = `
upid {
  low: 89101
  high: 528280977975
}
stop_timestamp_ns: 6
`

// ProcessTerminated2PB is a protobuf for another terminated process.
var ProcessTerminated2PB = `
upid {
  low:  468
  high: 528280977975
}
stop_timestamp_ns: 10
`

// Process1PB is the protobuf for a process.
var Process1PB = `
name: 'p1'
upid {
  low: 89101
  high: 528280977975
}
cid: "container_1"
`

// Process2PB is the protobuf for another process.
var Process2PB = `
name: 'p2'
upid {
  low: 246
  high: 528280977975
}
cid: "container_2"
`

// Containers

// ContainerInfoPB is the protobuf for a container info.
var ContainerInfoPB = `
name: "container_1"
uid: "container1"
pod_uid: "ijkl"
namespace: "ns"
`

// Schema

// SchemaInfoPB is the protobuf for a schema info.
var SchemaInfoPB = `
name: "a_table"
start_timestamp_ns: 2
columns {
  name: "column_1"
  data_type: 2
}
columns {
  name: "column_2"
  data_type: 4
}
`

// RegisterAgentRequest

// RegisterAgentRequestPB is the protobuf for a register agent request.
var RegisterAgentRequestPB = `
register_agent_request {
  info {
    agent_id {
      data: "11285cdd1de94ab1ae6a0ba08c8c676c"
    }
    host_info {
      hostname: "test-host"
    }
  }
}
`

// RegisterKelvinRequestPB is the protobuf for a register agent request.
var RegisterKelvinRequestPB = `
register_agent_request {
  info {
    agent_id {
      data: "11285cdd1de94ab1ae6a0ba08c8c676c"
    }
    host_info {
      hostname: "test-host"
    }
    capabilities {
      collects_data: false
    }
  }
}
`

// InvalidRegisterAgentRequestPB is an invalid protobuf of a register agent request.
var InvalidRegisterAgentRequestPB = `
register_agent_request {
  info {
    agent_id {
      data: "11285cdd1de94ab1ae6a0ba08c8c676c11285cdd1de94ab1ae6a0ba08c8c676c"
    }
    host_info {
      hostname: "test-host"
    }
  }
}
`

// UpdateAgentRequest

// UpdateAgentRequestPB is the protobuf for an update agent request.
var UpdateAgentRequestPB = `
update_agent_request {
  info {
    agent_id {
      data: "11285cdd1de94ab1ae6a0ba08c8c676c"
    }
    host_info {
      hostname: "test-host"
    }
  }
}
`

// InvalidUpdateAgentRequestPB is an invalid protobuf for an update agent request.
var InvalidUpdateAgentRequestPB = `
update_agent_request {
  info {
    agent_id {
      data: "11285cdd1de94ab1ae6a0ba08c8c676c11285cdd1de94ab1ae6a0ba08c8c676c"
    }
    host_info {
      hostname: "test-host"
    }
  }
}
`

// HeartbeatAck

// HeartbeatAckPB is a protobuf for a heartbeat ack.
var HeartbeatAckPB = `
heartbeat_ack {
  time: 10
  update_info {
    updates {
      pod_update {
        uid:  "podUid"
        name: "podName"
      }
    }
    updates {
      pod_update {
        uid:  "podUid2"
        name: "podName2"
      }
    }
  }
}
`

// Heartbeat

// HeartbeatPB is the protobuf for a heartbeat.
var HeartbeatPB = `
heartbeat {
  time: 1,
  agent_id: {
    data: "11285cdd1de94ab1ae6a0ba08c8c676c"
  }
  update_info {
    process_created {
      pid: 1
    }
  }
}
`

// InvalidHeartbeatPB is an invalid protobuf for a heartbeat.
var InvalidHeartbeatPB = `
heartbeat {
  time: 1,
  agent_id: {
    data: "11285cdd1de94ab1ae6a0ba08c8c676c"
  }
}
`

// Endpoint

// EndpointsPb is the protobuf for an endpoints object.
const EndpointsPb = `
subsets {
  addresses {
    ip: "127.0.0.1"
    hostname: "host"
    node_name: "this-is-a-node"
    target_ref {
      kind: "Pod"
      namespace: "pl"
      uid: "abcd"
    }
  }
  addresses {
    ip: "127.0.0.2"
    hostname: "host-2"
    node_name: "node-a"
    target_ref {
      kind: "Pod"
      namespace: "pl"
      uid: "efgh"
    }
  }
  not_ready_addresses {
    ip: "127.0.0.3"
    hostname: "host-3"
    node_name: "node-b"
  }
  ports {
    name: "endpt"
    port: 10,
    protocol: 1
  }
  ports {
    name: "abcd"
    port: 500,
    protocol: 1
  }
}
metadata {
  name: "object_md"
  namespace: "a_namespace"
  uid: "ijkl"
  resource_version: "1"
  creation_timestamp_ns: 4
  deletion_timestamp_ns: 6
  owner_references {
    kind: "pod"
    name: "test"
    uid: "abcd"
  }
}
`

// Service

// ServicePb is the protobuf for a service object.
const ServicePb = `
metadata {
  name: "object_md"
  namespace: "a_namespace"
  uid: "ijkl"
  resource_version: "1",
  cluster_name: "a_cluster",
  owner_references {
    kind: "pod"
    name: "test"
    uid: "abcd"
  }
  creation_timestamp_ns: 4
  deletion_timestamp_ns: 6
}
spec {
  cluster_ip: "127.0.0.1"
  external_ips: "127.0.0.2"
  external_ips: "127.0.0.3"
  load_balancer_ip: "127.0.0.4"
  external_name: "hello"
  external_traffic_policy: 1
  ports {
    name: "endpt"
    port: 10
    protocol: 1
    node_port: 20
  }
  ports {
    name: "another_port"
    port: 50
    protocol: 1
    node_port: 60
  }
  type: 1
}
`

// Pod

// PodPb is the protobuf for a pod object.
const PodPb = `
metadata {
  name: "object_md"
  uid: "ijkl"
  resource_version: "1",
  cluster_name: "a_cluster",
  owner_references {
    kind: "pod"
    name: "test"
    uid: "abcd"
  }
  creation_timestamp_ns: 4
  deletion_timestamp_ns: 6
}
status {
  message: "this is message"
  phase: 2
  conditions: 2
}
spec {
  node_name: "test"
  hostname: "hostname"
  dns_policy: 2
}
`

// PodPbWithContainers is a protobuf for a pod object that has containers.
const PodPbWithContainers = `
metadata {
  name: "object_md"
  uid: "ijkl"
  resource_version: "1",
  cluster_name: "a_cluster",
  owner_references {
    kind: "pod"
    name: "test"
    uid: "abcd"
  }
  creation_timestamp_ns: 4
  deletion_timestamp_ns: 6
}
status {
  message: "this is message"
  phase: 2
  conditions: 2
  container_statuses {
    name: "container1"
    container_state: 3
    container_id: "docker://test"
  }
  qos_class: QOS_CLASS_BURSTABLE
}
spec {
  node_name: "test"
  hostname: "hostname"
  dns_policy: 2
}
`

// PendingPodPb is a protobuf for a pending pod.
const PendingPodPb = `
metadata {
  name: "object_md"
  uid: "ijkl"
  resource_version: "1",
  cluster_name: "a_cluster",
  owner_references {
    kind: "pod"
    name: "test"
    uid: "abcd"
  }
  creation_timestamp_ns: 4
  deletion_timestamp_ns: 6
}
status {
  message: "this is message"
  phase: 1
  conditions: 2
  container_statuses {
    name: "container1"
    container_state: 0
    container_id: ""
  }
  container_statuses {
    name: "container2"
    container_state: 0
    container_id: ""
  }
  qos_class: QOS_CLASS_BURSTABLE
}
spec {
  node_name: "test"
  hostname: "hostname"
  dns_policy: 2
}
`
