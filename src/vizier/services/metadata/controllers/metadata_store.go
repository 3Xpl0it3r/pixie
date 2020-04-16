package controllers

import (
	"errors"
	"fmt"
	"net"
	"path"
	"strconv"
	"strings"
	"time"

	"github.com/gogo/protobuf/proto"
	uuid "github.com/satori/go.uuid"
	log "github.com/sirupsen/logrus"

	"pixielabs.ai/pixielabs/src/shared/k8s"
	metadatapb "pixielabs.ai/pixielabs/src/shared/k8s/metadatapb"
	"pixielabs.ai/pixielabs/src/shared/types"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/controllers/kvstore"
	agentpb "pixielabs.ai/pixielabs/src/vizier/services/shared/agentpb"
)

// KVMetadataStore stores metadata using keys and values.
type KVMetadataStore struct {
	cache          *kvstore.Cache
	expiryDuration time.Duration
	clusterInfo    ClusterInfo
}

// ClusterInfo contains static information about the cluster, stored in memory.
type ClusterInfo struct {
	ClusterCIDR *net.IPNet // This is the cluster (pod) CIDR block.
	ServiceCIDR *net.IPNet // This is the service CIDR block; it is inferred from all observed service IPs.
}

// NewKVMetadataStore creates a new key-value metadata store.
func NewKVMetadataStore(cache *kvstore.Cache) (*KVMetadataStore, error) {
	return NewKVMetadataStoreWithExpiryTime(cache, 24*time.Hour)
}

// NewKVMetadataStoreWithExpiryTime creates a new metadata store with the given expiry time. A KVMetadataStore
// stores metadata using keys and values.
func NewKVMetadataStoreWithExpiryTime(cache *kvstore.Cache, expiryDuration time.Duration) (*KVMetadataStore, error) {
	mds := &KVMetadataStore{
		cache:          cache,
		expiryDuration: expiryDuration,
		clusterInfo:    ClusterInfo{ClusterCIDR: nil, ServiceCIDR: nil},
	}

	return mds, nil
}

// HostnameIPPair is a unique identifies for a K8s node.
type HostnameIPPair struct {
	Hostname string
	IP       string
}

// GetClusterCIDR returns the CIDR for the current cluster.
func (mds *KVMetadataStore) GetClusterCIDR() string {
	if mds.clusterInfo.ClusterCIDR == nil {
		return ""
	}
	return mds.clusterInfo.ClusterCIDR.String()
}

// GetServiceCIDR returns the service CIDR for the current cluster.
func (mds *KVMetadataStore) GetServiceCIDR() string {
	if mds.clusterInfo.ServiceCIDR == nil {
		return ""
	}
	return mds.clusterInfo.ServiceCIDR.String()
}

// SetClusterCIDR sets the static pod CIDR for the current cluster.
func (mds *KVMetadataStore) SetClusterCIDR(cidr string) {
	_, ipNet, err := net.ParseCIDR(cidr)

	if err != nil {
		log.WithError(err).Error("Could not parse cluster (pod) CIDR.")
		return
	}

	mds.clusterInfo.ClusterCIDR = ipNet
}

/* ================= Keys =================*/

func getNamespaceFromMetadata(md *metadatapb.ObjectMetadata) string {
	return getNamespaceFromString(md.Namespace)
}

func getNamespaceFromString(ns string) string {
	if ns == "" {
		ns = "default"
	}
	return ns
}

func getAsidKey() string {
	return "/asid"
}

func getAgentKeyPrefix() string {
	return "/agent/"
}

// GetAgentKey gets the key for the agent.
func getAgentKey(agentID uuid.UUID) string {
	return path.Join("/", "agent", agentID.String())
}

// GetHostnamePairAgentKey gets the key for the hostname pair's agent.
func GetHostnamePairAgentKey(pair *HostnameIPPair) string {
	return path.Join("/", "hostnameIP", fmt.Sprintf("%s-%s", pair.Hostname, pair.IP), "agent")
}

// GetKelvinAgentKey gets the key for a kelvin node.
func getKelvinAgentKeyPrefix() string {
	return "/kelvin/"
}

// GetKelvinAgentKey gets the key for a kelvin node.
func getKelvinAgentKey(agentID uuid.UUID) string {
	return path.Join("/", "kelvin", agentID.String())
}

// GetAgentSchemasKey gets all schemas belonging to an agent.
func getAgentSchemasKey(agentID uuid.UUID) string {
	return path.Join("/", "agents", agentID.String(), "schema")
}

func getAgentSchemaKey(agentID uuid.UUID, schemaName string) string {
	return path.Join(getAgentSchemasKey(agentID), schemaName)
}

func getComputedSchemasKey() string {
	return path.Join("/", "computedSchema")
}

func getComputedSchemaKey(schemaName string) string {
	return path.Join(getComputedSchemasKey(), schemaName)
}

func getProcessKey(upid string) string {
	return path.Join("/", "processes", upid)
}

func getPodsKey() string {
	return path.Join("/", "pod") + "/"
}

func getPodKey(e *metadatapb.Pod) string {
	return getPodKeyFromStrings(e.Metadata.UID, getNamespaceFromMetadata(e.Metadata))
}

func getPodToHostnamePairKey(podName string, namespace string) string {
	return path.Join("/podHostnamePair/", fmt.Sprintf("%s-%s", namespace, podName))
}

func getPodKeyFromStrings(uid string, namespace string) string {
	return path.Join("/", "pod", namespace, uid)
}

func getContainersKey() string {
	return path.Join("/", "containers") + "/"
}

func getContainerKey(c *metadatapb.ContainerInfo) string {
	return getContainerKeyFromStrings(c.UID)
}

func getContainerKeyFromStrings(containerID string) string {
	return path.Join("/", "containers", containerID, "info")
}

func getEndpointsKey() string {
	return path.Join("/", "endpoints") + "/"
}

func getEndpointKey(e *metadatapb.Endpoints) string {
	return path.Join(getEndpointsKey(), getNamespaceFromMetadata(e.Metadata), e.Metadata.UID)
}

func getServicePodMapKey(e *metadatapb.Endpoints) string {
	return path.Join("/", "services", getNamespaceFromMetadata(e.Metadata), e.Metadata.Name, "pods")
}

func getServicesKey() string {
	return path.Join("/", "service") + "/"
}

func getServiceKey(e *metadatapb.Service) string {
	return path.Join("/", "service", getNamespaceFromMetadata(e.Metadata), e.Metadata.UID)
}

func getNamespacesKey() string {
	return path.Join("/", "namespace") + "/"
}

func getNamespaceKey(e *metadatapb.Namespace) string {
	return path.Join("/", "namespace", e.Metadata.UID)
}

func getResourceVersionMapKey(rv string) string {
	return path.Join("/", "resourceVersionUpdate", rv)
}

func getSubscriberResourceVersionKey(sub string) string {
	return path.Join("/", "subscriber", "resourceVersion", sub)
}

/* =============== Agent Operations ============== */

// GetAgent gets the agent info for the agent with the given id.
func (mds *KVMetadataStore) GetAgent(agentID uuid.UUID) (*agentpb.Agent, error) {
	resp, err := mds.cache.Get(getAgentKey(agentID))
	if err != nil {
		return nil, err
	}
	if resp == nil {
		return nil, nil
	}
	aPb := &agentpb.Agent{}
	err = proto.Unmarshal(resp, aPb)
	if err != nil {
		return nil, err
	}
	return aPb, nil
}

// GetKelvinIDs gets the IDs of the current active kelvins.
func (mds *KVMetadataStore) GetKelvinIDs() ([]string, error) {
	var ids []string

	// Get all kelvins.
	_, vals, err := mds.cache.GetWithPrefix(getKelvinAgentKeyPrefix())
	if err != nil {
		return nil, err
	}
	for _, v := range vals {
		ids = append(ids, string(v))
	}

	return ids, nil
}

// GetAgentIDForHostnamePair gets the agent for the given hostnamePair, if it exists.
func (mds *KVMetadataStore) GetAgentIDForHostnamePair(hnPair *HostnameIPPair) (string, error) {
	id, err := mds.cache.Get(GetHostnamePairAgentKey(hnPair))
	if err != nil {
		return "", err
	}
	if id == nil {
		return "", nil
	}

	return string(id), err
}

// GetAgentsForHostnamePairs gets the agents running on the given hostnames.
func (mds *KVMetadataStore) GetAgentsForHostnamePairs(hostnames *[]*HostnameIPPair) ([]string, error) {
	if len(*hostnames) == 0 {
		return nil, nil
	}

	agents := []string{}
	for _, hn := range *hostnames {
		resp, err := mds.cache.Get(GetHostnamePairAgentKey(hn))
		if err != nil {
			continue
		}
		if resp == nil {
			continue
		}
		agents = append(agents, string(resp))
	}

	return agents, nil
}

// DeleteAgent deletes the agent with the given ID.
func (mds *KVMetadataStore) DeleteAgent(agentID uuid.UUID) error {
	resp, err := mds.cache.Get(getAgentKey(agentID))
	if err != nil {
		return err
	}

	// Agent does not exist, no need to delete.
	if resp == nil {
		log.Info("Tried to delete an agent that was already deleted")
		return nil
	}

	aPb := &agentpb.Agent{}
	err = proto.Unmarshal(resp, aPb)
	if err != nil {
		return err
	}

	hnPair := &HostnameIPPair{
		Hostname: aPb.Info.HostInfo.Hostname,
		IP:       aPb.Info.HostInfo.HostIP,
	}
	delKeys := []string{getAgentKey(agentID), GetHostnamePairAgentKey(hnPair)}

	// Info.Capabiltiies should never be nil with our new PEMs/Kelvin. If it is nil,
	// this means that the protobuf we retrieved from etcd belongs to an older agent.
	collectsData := aPb.Info.Capabilities == nil || aPb.Info.Capabilities.CollectsData
	if !collectsData {
		delKeys = append(delKeys, getKelvinAgentKey(agentID))
	}

	mds.cache.DeleteAll(delKeys)

	return mds.cache.DeleteWithPrefix(getAgentSchemasKey(agentID))
}

// CreateAgent creates a new agent.
func (mds *KVMetadataStore) CreateAgent(agentID uuid.UUID, a *agentpb.Agent) error {
	i, err := a.Marshal()
	if err != nil {
		return errors.New("Unable to marshal agent protobuf: " + err.Error())
	}
	hnPair := &HostnameIPPair{
		Hostname: a.Info.HostInfo.Hostname,
		IP:       a.Info.HostInfo.HostIP,
	}
	mds.cache.Set(GetHostnamePairAgentKey(hnPair), agentID.String())
	mds.cache.Set(getAgentKey(agentID), string(i))

	collectsData := a.Info.Capabilities == nil || a.Info.Capabilities.CollectsData
	if !collectsData {
		mds.cache.Set(getKelvinAgentKey(agentID), agentID.String())
	}

	log.WithField("hostname", hnPair.Hostname).WithField("HostIP", hnPair.IP).Info("Registering agent")
	return nil
}

// UpdateAgent updates the agent info for the agent with the given ID.
func (mds *KVMetadataStore) UpdateAgent(agentID uuid.UUID, a *agentpb.Agent) error {
	i, err := a.Marshal()
	if err != nil {
		return errors.New("Unable to marshal agent protobuf: " + err.Error())
	}

	mds.cache.Set(getAgentKey(agentID), string(i))
	return nil
}

// GetAgents gets all of the current active agents.
func (mds *KVMetadataStore) GetAgents() ([]*agentpb.Agent, error) {
	var agents []*agentpb.Agent

	keys, vals, err := mds.cache.GetWithPrefix(getAgentKeyPrefix())
	if err != nil {
		return nil, err
	}

	for i, key := range keys {
		// Filter out keys that aren't of the form /agent/<uuid>.
		splitKey := strings.Split(string(key), "/")
		if len(splitKey) != 3 {
			continue
		}

		pb := &agentpb.Agent{}
		proto.Unmarshal(vals[i], pb)
		if pb.Info != nil && len(pb.Info.AgentID.Data) > 0 {
			agents = append(agents, pb)
		}
	}

	return agents, nil
}

// GetASID gets the next assignable ASID.
func (mds *KVMetadataStore) GetASID() (uint32, error) {
	asid := "1" // Starting ASID.

	resp, err := mds.cache.Get(getAsidKey())
	if err != nil {
		return 0, err
	}
	if resp != nil {
		asid = string(resp)
	}

	// Convert ASID from etcd into uint32.
	asidInt, err := strconv.ParseUint(asid, 10, 32)
	if err != nil {
		return 0, err
	}

	// Increment ASID in datastore.
	updatedAsid := asidInt + 1
	mds.cache.Set(getAsidKey(), fmt.Sprint(updatedAsid))

	return uint32(asidInt), nil
}

/* =============== Schema Operations ============== */

// UpdateSchemas updates the given schemas in the metadata store.
func (mds *KVMetadataStore) UpdateSchemas(agentID uuid.UUID, schemas []*metadatapb.SchemaInfo) error {
	computedSchemaPb := metadatapb.ComputedSchema{
		Tables: schemas,
	}
	computedSchema, err := computedSchemaPb.Marshal()
	if err != nil {
		log.WithError(err).Error("Could not computed schema update message.")
		return err
	}

	for _, schemaPb := range schemas {
		schema, err := schemaPb.Marshal()
		if err != nil {
			log.WithError(err).Error("Could not marshall schema update message.")
			continue
		}
		mds.cache.Set(getAgentSchemaKey(agentID, schemaPb.Name), string(schema))
	}
	// TODO(michelle): PL-695 This currently assumes that if a schema is available on one agent,
	// then it is available on all agents. This should be updated so that we handle situations where that is not the case.
	mds.cache.Set(getComputedSchemasKey(), string(computedSchema))

	return nil
}

// GetComputedSchemas gets all computed schemas in the metadata store.
func (mds *KVMetadataStore) GetComputedSchemas() ([]*metadatapb.SchemaInfo, error) {
	cSchemas, err := mds.cache.Get(getComputedSchemasKey())
	if err != nil {
		return nil, err
	}
	if cSchemas == nil {
		return nil, fmt.Errorf("Could not find any computed schemas")
	}

	computedSchemaPb := &metadatapb.ComputedSchema{}
	err = proto.Unmarshal(cSchemas, computedSchemaPb)
	if err != nil {
		return nil, err
	}

	return computedSchemaPb.Tables, nil
}

/* =============== Process Operations ============== */

// UpdateProcesses updates the given processes in the metadata store.
func (mds *KVMetadataStore) UpdateProcesses(processes []*metadatapb.ProcessInfo) error {
	for _, processPb := range processes {
		process, err := processPb.Marshal()
		if err != nil {
			log.WithError(err).Error("Could not marshall processInfo.")
			continue
		}
		upid := types.UInt128FromProto(processPb.UPID)
		processKey := getProcessKey(k8s.StringFromUPID(upid))

		if processPb.StopTimestampNS > 0 {
			mds.cache.SetWithTTL(processKey, string(process), mds.expiryDuration)
		} else {
			mds.cache.Set(processKey, string(process))
		}
	}
	return nil
}

// GetProcesses gets the process infos for the given process upids.
func (mds *KVMetadataStore) GetProcesses(upids []*types.UInt128) ([]*metadatapb.ProcessInfo, error) {
	processes := make([]*metadatapb.ProcessInfo, len(upids))

	for i, upid := range upids {
		process, err := mds.cache.Get(getProcessKey(k8s.StringFromUPID(upid)))
		if err != nil {
			return nil, err
		}
		if process == nil {
			processes[i] = nil
		} else {
			processPb := metadatapb.ProcessInfo{}
			if err := proto.Unmarshal(process, &processPb); err != nil {
				log.WithError(err).Error("Could not unmarshal process pb.")
				processes[i] = nil
				continue
			}
			processes[i] = &processPb
		}

	}
	return processes, nil
}

/* =============== Pod Operations ============== */

// GetNodePods gets all pods belonging to a node in the metadata store.
func (mds *KVMetadataStore) GetNodePods(hnPair *HostnameIPPair) ([]*metadatapb.Pod, error) {
	_, vals, err := mds.cache.GetWithPrefix(getPodsKey())
	if err != nil {
		return nil, err
	}

	var pods []*metadatapb.Pod
	for _, val := range vals {
		pb := &metadatapb.Pod{}
		proto.Unmarshal(val, pb)
		if pb.Status.HostIP == "" {
			log.WithField("pod_name", pb.Metadata.Name).Info("Pod has no hostIP")
		}
		if (hnPair == nil || pb.Status.HostIP == hnPair.IP) && pb.Metadata.DeletionTimestampNS == 0 {
			pods = append(pods, pb)
		}
	}
	return pods, nil
}

// GetPods gets all pods in the metadata store.
func (mds *KVMetadataStore) GetPods() ([]*metadatapb.Pod, error) {
	_, vals, err := mds.cache.GetWithPrefix(getPodsKey())
	if err != nil {
		return nil, err
	}

	pods := make([]*metadatapb.Pod, len(vals))
	for i, val := range vals {
		pb := &metadatapb.Pod{}
		proto.Unmarshal(val, pb)
		pods[i] = pb
	}
	return pods, nil
}

// UpdatePod adds or updates the given pod in the metadata store.
func (mds *KVMetadataStore) UpdatePod(p *metadatapb.Pod, deleted bool) error {
	if deleted && p.Metadata.DeletionTimestampNS == 0 {
		p.Metadata.DeletionTimestampNS = time.Now().UnixNano()
	}

	val, err := p.Marshal()
	if err != nil {
		return errors.New("Unable to marshal endpoints pb")
	}

	key := getPodKey(p)

	if p.Metadata.DeletionTimestampNS > 0 {
		mds.cache.SetWithTTL(key, string(val), mds.expiryDuration)
	} else {
		mds.cache.Set(key, string(val))
	}

	mds.cache.Set(getPodToHostnamePairKey(p.Metadata.Name, p.Metadata.Namespace), fmt.Sprintf("%s:%s", p.Spec.NodeName, p.Status.HostIP))

	// Add mapping from resource version -> pod.
	rvUpdate := &metadatapb.MetadataObject{
		Object: &metadatapb.MetadataObject_Pod{
			Pod: p,
		},
	}
	val, err = rvUpdate.Marshal()
	if err != nil {
		return errors.New("Unable to marshal rv pb")
	}
	mds.cache.Set(getResourceVersionMapKey(p.Metadata.ResourceVersion), string(val))

	return nil
}

// GetHostnameIPPairFromPodName gets the hostname IP pair from a given pod name.
func (mds *KVMetadataStore) GetHostnameIPPairFromPodName(podName string, namespace string) (*HostnameIPPair, error) {
	resp, err := mds.cache.Get(getPodToHostnamePairKey(podName, namespace))
	if err != nil {
		return nil, err
	}
	if resp == nil {
		return nil, nil
	}
	splitVal := strings.Split(string(resp), ":")
	if len(splitVal) != 2 {
		return nil, errors.New("malformed hostname IP pair")
	}
	return &HostnameIPPair{
		splitVal[0], splitVal[1]}, nil
}

/* =============== Container Operations ============== */

// GetContainers gets all containers in the metadata store.
func (mds *KVMetadataStore) GetContainers() ([]*metadatapb.ContainerInfo, error) {
	_, vals, err := mds.cache.GetWithPrefix(getContainersKey())
	if err != nil {
		return nil, err
	}

	containers := make([]*metadatapb.ContainerInfo, len(vals))
	for i, val := range vals {
		pb := &metadatapb.ContainerInfo{}
		proto.Unmarshal(val, pb)
		containers[i] = pb
	}
	return containers, nil
}

// UpdateContainer adds or updates the given container in the metadata store.
func (mds *KVMetadataStore) UpdateContainer(c *metadatapb.ContainerInfo) error {
	val, err := c.Marshal()
	if err != nil {
		return errors.New("Unable to marshal containerInfo pb")
	}

	key := getContainerKey(c)

	if c.StopTimestampNS > 0 {
		mds.cache.SetWithTTL(key, string(val), mds.expiryDuration)
	} else {
		mds.cache.Set(key, string(val))
	}
	return nil
}

// UpdateContainersFromPod updates the containers from the given pod in the metadata store.
func (mds *KVMetadataStore) UpdateContainersFromPod(pod *metadatapb.Pod, deleted bool) error {
	containers := make([]*metadatapb.ContainerStatus, 0)
	for _, status := range pod.Status.ContainerStatuses {
		if status.ContainerID != "" {
			containers = append(containers, status)
		}
	}

	for _, container := range containers {
		cid := formatContainerID(container.ContainerID)
		key := getContainerKeyFromStrings(cid)

		stopTime := container.StopTimestampNS
		if deleted && stopTime == 0 {
			stopTime = time.Now().UnixNano()
		}

		cInfo := metadatapb.ContainerInfo{
			Name:             container.Name,
			UID:              cid,
			StartTimestampNS: container.StartTimestampNS,
			StopTimestampNS:  stopTime,
			PodUID:           pod.Metadata.UID,
			Namespace:        getNamespaceFromMetadata(pod.Metadata),
		}
		val, err := cInfo.Marshal()
		if err != nil {
			return errors.New("Unable to marshal containerInfo pb")
		}

		if container.StopTimestampNS > 0 {
			mds.cache.SetWithTTL(key, string(val), mds.expiryDuration)
		} else {
			mds.cache.Set(key, string(val))
		}
	}

	return nil
}

/* =============== Endpoints Operations ============== */

// GetNodeEndpoints gets all endpoints in the metadata store that belong to a particular hostname.
func (mds *KVMetadataStore) GetNodeEndpoints(hnPair *HostnameIPPair) ([]*metadatapb.Endpoints, error) {
	_, vals, err := mds.cache.GetWithPrefix(getEndpointsKey())
	if err != nil {
		return nil, err
	}

	var endpoints []*metadatapb.Endpoints
	for _, val := range vals {
		pb := &metadatapb.Endpoints{}
		proto.Unmarshal(val, pb)
		for _, subset := range pb.Subsets {
			for _, address := range subset.Addresses {
				if address.TargetRef.Kind != "Pod" {
					continue
				}
				podPair, err := mds.GetHostnameIPPairFromPodName(address.TargetRef.Name, address.TargetRef.Namespace)
				if err != nil || podPair == nil {
					continue
				}
				if (hnPair == nil || (podPair.Hostname == hnPair.Hostname && podPair.IP == hnPair.IP)) && pb.Metadata.DeletionTimestampNS == 0 {
					endpoints = append(endpoints, pb)
				}
			}
		}
	}
	return endpoints, nil
}

// GetEndpoints gets all endpoints in the metadata store.
func (mds *KVMetadataStore) GetEndpoints() ([]*metadatapb.Endpoints, error) {
	_, vals, err := mds.cache.GetWithPrefix(getEndpointsKey())
	if err != nil {
		return nil, err
	}

	endpoints := make([]*metadatapb.Endpoints, len(vals))
	for i, val := range vals {
		pb := &metadatapb.Endpoints{}
		proto.Unmarshal(val, pb)
		endpoints[i] = pb
	}
	return endpoints, nil
}

// UpdateEndpoints adds or updates the given endpoint in the metadata store.
func (mds *KVMetadataStore) UpdateEndpoints(e *metadatapb.Endpoints, deleted bool) error {
	if deleted && e.Metadata.DeletionTimestampNS == 0 {
		e.Metadata.DeletionTimestampNS = time.Now().UnixNano()
	}

	val, err := e.Marshal()
	if err != nil {
		return errors.New("Unable to marshal endpoints pb")
	}

	key := getEndpointKey(e)

	// Update endpoints object.
	if e.Metadata.DeletionTimestampNS > 0 {
		mds.cache.SetWithTTL(key, string(val), mds.expiryDuration)
	} else {
		mds.cache.Set(key, string(val))
	}

	// Update service -> pod map.
	mapKey := getServicePodMapKey(e)
	var podIds []string
	for _, subset := range e.Subsets {
		for _, addr := range subset.Addresses {
			if addr.TargetRef != nil && addr.TargetRef.Kind == "Pod" {
				podIds = append(podIds, addr.TargetRef.UID)
			}
		}
	}
	mapVal := strings.Join(podIds, ",")

	if e.Metadata.DeletionTimestampNS > 0 {
		mds.cache.SetWithTTL(mapKey, mapVal, mds.expiryDuration)
	} else {
		mds.cache.Set(mapKey, mapVal)
	}

	// Add mapping from resource version -> endpoint.
	rvUpdate := &metadatapb.MetadataObject{
		Object: &metadatapb.MetadataObject_Endpoints{
			Endpoints: e,
		},
	}
	val, err = rvUpdate.Marshal()
	if err != nil {
		return errors.New("Unable to marshal rv pb")
	}
	mds.cache.Set(getResourceVersionMapKey(e.Metadata.ResourceVersion), string(val))

	return nil
}

/* =============== Service Operations ============== */

// GetServices gets all services in the metadata store.
func (mds *KVMetadataStore) GetServices() ([]*metadatapb.Service, error) {
	_, vals, err := mds.cache.GetWithPrefix(getServicesKey())
	if err != nil {
		return nil, err
	}

	services := make([]*metadatapb.Service, len(vals))
	for i, val := range vals {
		pb := &metadatapb.Service{}
		proto.Unmarshal(val, pb)
		services[i] = pb
	}
	return services, nil
}

// UpdateServiceCIDR updates the current best guess of the service CIDR.
func (mds *KVMetadataStore) UpdateServiceCIDR(s *metadatapb.Service) {
	ip := net.ParseIP(s.Spec.ClusterIP).To16()

	// Some services don't have a ClusterIP.
	if ip == nil {
		return
	}

	if mds.clusterInfo.ServiceCIDR == nil {
		mds.clusterInfo.ServiceCIDR = &net.IPNet{IP: ip, Mask: net.CIDRMask(128, 128)}
	} else {
		for !mds.clusterInfo.ServiceCIDR.Contains(ip) {
			ones, bits := mds.clusterInfo.ServiceCIDR.Mask.Size()
			mds.clusterInfo.ServiceCIDR.Mask = net.CIDRMask(ones-1, bits)
			mds.clusterInfo.ServiceCIDR.IP = mds.clusterInfo.ServiceCIDR.IP.Mask(mds.clusterInfo.ServiceCIDR.Mask)
		}
	}
	log.Debug("Service IP: " + ip.String() + " -> Service CIDR updated to: " + mds.clusterInfo.ServiceCIDR.String())
}

// UpdateService adds or updates the given service in the metadata store.
func (mds *KVMetadataStore) UpdateService(s *metadatapb.Service, deleted bool) error {
	mds.UpdateServiceCIDR(s)

	if deleted && s.Metadata.DeletionTimestampNS == 0 {
		s.Metadata.DeletionTimestampNS = time.Now().UnixNano()
	}

	val, err := s.Marshal()
	if err != nil {
		return errors.New("Unable to marshal endpoints pb")
	}

	key := getServiceKey(s)

	if s.Metadata.DeletionTimestampNS > 0 {
		mds.cache.SetWithTTL(key, string(val), mds.expiryDuration)
	} else {
		mds.cache.Set(key, string(val))
	}

	// Add mapping from resource version -> service.
	rvUpdate := &metadatapb.MetadataObject{
		Object: &metadatapb.MetadataObject_Service{
			Service: s,
		},
	}
	val, err = rvUpdate.Marshal()
	if err != nil {
		return errors.New("Unable to marshal rv pb")
	}
	mds.cache.Set(getResourceVersionMapKey(s.Metadata.ResourceVersion), string(val))

	return nil
}

/* =============== Namespace Operations ============== */

// GetNamespaces gets all namespaces in the metadata store.
func (mds *KVMetadataStore) GetNamespaces() ([]*metadatapb.Namespace, error) {
	_, vals, err := mds.cache.GetWithPrefix(getNamespacesKey())
	if err != nil {
		return nil, err
	}

	namespaces := make([]*metadatapb.Namespace, len(vals))
	for i, val := range vals {
		pb := &metadatapb.Namespace{}
		proto.Unmarshal(val, pb)
		namespaces[i] = pb
	}
	return namespaces, nil
}

// UpdateNamespace adds or updates the given namespace in the metadata store.
func (mds *KVMetadataStore) UpdateNamespace(s *metadatapb.Namespace, deleted bool) error {
	if deleted && s.Metadata.DeletionTimestampNS == 0 {
		s.Metadata.DeletionTimestampNS = time.Now().UnixNano()
	}

	val, err := s.Marshal()
	if err != nil {
		return errors.New("Unable to marshal namespace pb")
	}

	key := getNamespaceKey(s)

	if s.Metadata.DeletionTimestampNS > 0 {
		mds.cache.SetWithTTL(key, string(val), mds.expiryDuration)
	} else {
		mds.cache.Set(key, string(val))
	}

	// Add mapping from resource version -> namespace.
	rvUpdate := &metadatapb.MetadataObject{
		Object: &metadatapb.MetadataObject_Namespace{
			Namespace: s,
		},
	}
	val, err = rvUpdate.Marshal()
	if err != nil {
		return errors.New("Unable to marshal rv pb")
	}
	mds.cache.Set(getResourceVersionMapKey(s.Metadata.ResourceVersion), string(val))

	return nil
}

/* =============== Cumulative Metadata Operations ============== */

// GetMetadataUpdates gets all metadata updates in the store.
func (mds *KVMetadataStore) GetMetadataUpdates(hnPair *HostnameIPPair) ([]*metadatapb.ResourceUpdate, error) {
	var updates []*metadatapb.ResourceUpdate

	namespaces, err := mds.GetNamespaces()
	if err != nil {
		return nil, err
	}

	pods, err := mds.GetNodePods(hnPair)
	if err != nil {
		return nil, err
	}

	endpoints, err := mds.GetNodeEndpoints(hnPair)
	if err != nil {
		return nil, err
	}

	for _, ns := range namespaces {
		nsUpdate := GetResourceUpdateFromNamespace(ns)
		updates = append(updates, nsUpdate)
	}

	for _, pod := range pods {
		containerUpdates := GetContainerResourceUpdatesFromPod(pod)
		updates = append(updates, containerUpdates...)

		podUpdate := GetResourceUpdateFromPod(pod)
		updates = append(updates, podUpdate)
	}

	for _, endpoint := range endpoints {
		epUpdate := GetNodeResourceUpdateFromEndpoints(endpoint, hnPair, mds)
		updates = append(updates, epUpdate)
	}

	return updates, nil
}

// GetMetadataUpdatesForHostname get the metadata updates that should be sent to the hostname in the given range.
func (mds *KVMetadataStore) GetMetadataUpdatesForHostname(hnPair *HostnameIPPair, from string, to string) ([]*metadatapb.ResourceUpdate, error) {
	// To/From can be of the format <resource_version>_<number> for pods/container updates. We want to parse the from into just <resource_version>.
	fromFormatted := strings.Split(from, "_")[0]

	// Get all updates within range.
	_, vals, err := mds.cache.GetWithRange(getResourceVersionMapKey(fromFormatted), getResourceVersionMapKey(to))
	if err != nil {
		return nil, err
	}

	updatePbs := make([]*metadatapb.ResourceUpdate, 0)
	for _, v := range vals {
		obj := &metadatapb.MetadataObject{}
		err = proto.Unmarshal(v, obj)
		if err != nil {
			log.WithError(err).Error("Could not unmarshal resource update")
			// We used to store the values in this map as a ResourceUpdate rather than a K8s object, so we probably fetched
			// some data in the old format.
			continue
		}

		switch m := obj.Object.(type) {
		case *metadatapb.MetadataObject_Pod:
			if hnPair != nil && m.Pod.Spec.NodeName != hnPair.Hostname && m.Pod.Status.HostIP != hnPair.IP {
				continue
			}
			updatePbs = append(updatePbs, GetContainerResourceUpdatesFromPod(m.Pod)...)
			updatePbs = append(updatePbs, GetResourceUpdateFromPod(m.Pod))
		case *metadatapb.MetadataObject_Endpoints:
			updatePbs = append(updatePbs, GetNodeResourceUpdateFromEndpoints(m.Endpoints, hnPair, mds))
		case *metadatapb.MetadataObject_Service:
			// We currently don't send updates for services. They are covered by endpoints.
			continue
		case *metadatapb.MetadataObject_Namespace:
			updatePbs = append(updatePbs, GetResourceUpdateFromNamespace(m.Namespace))
		default:
			log.Info("Got unknown K8s object")
		}
	}

	// Set prevRVs for updates.
	allUpdates := make([]*metadatapb.ResourceUpdate, 0)
	prevRV := ""
	for i, u := range updatePbs {
		// Since a single resourceUpdate entry in etcd can be equivalent to multiple updates
		// (for example, a resourcePodUpdate actually maps to a pod update + container updates), updatesPb
		// may contain updates that aren't within the requested range. We need to
		// filter those out here.
		if u.ResourceVersion >= from && u.ResourceVersion < to {
			upb := updatePbs[i]
			upb.PrevResourceVersion = prevRV
			prevRV = upb.ResourceVersion
			allUpdates = append(allUpdates, upb)
		}
	}

	return allUpdates, nil
}

/* =============== Resource Versions ============== */

// AddResourceVersion creates a mapping from a resourceVersion to the update for that resource.
func (mds *KVMetadataStore) AddResourceVersion(rv string, update *metadatapb.MetadataObject) error {
	val, err := update.Marshal()
	if err != nil {
		return err
	}

	mds.cache.Set(getResourceVersionMapKey(rv), string(val))
	return nil
}

// UpdateSubscriberResourceVersion updates the last resource version processed by a subscriber.
func (mds *KVMetadataStore) UpdateSubscriberResourceVersion(sub string, rv string) error {
	mds.cache.Set(getSubscriberResourceVersionKey(sub), rv)
	return nil
}

// GetSubscriberResourceVersion gets the last resource version processed by a subscriber.
func (mds *KVMetadataStore) GetSubscriberResourceVersion(sub string) (string, error) {
	resp, err := mds.cache.Get(getSubscriberResourceVersionKey(sub))
	if err != nil {
		return "", err
	}
	if resp == nil {
		return "", nil
	}

	return string(resp), nil
}
