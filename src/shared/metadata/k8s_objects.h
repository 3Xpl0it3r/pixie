#pragma once

#include <memory>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include "src/common/base/base.h"
#include "src/shared/k8s/metadatapb/metadata.pb.h"
#include "src/shared/metadata/base_types.h"

namespace pl {
namespace md {

/**
 * Enum with all the different metadata types.
 */
enum class K8sObjectType { kUnknown, kPod, kService };

/**
 * Base class for all K8s metadata objects.
 */
class K8sMetadataObject {
 public:
  K8sMetadataObject() = delete;
  virtual ~K8sMetadataObject() = default;

  K8sMetadataObject(K8sObjectType type, UID uid, std::string_view ns, std::string_view name,
                    int64_t start_time_ns = 0, int64_t stop_time_ns = 0)
      : type_(type),
        uid_(std::move(uid)),
        ns_(ns),
        name_(name),
        start_time_ns_(start_time_ns),
        stop_time_ns_(stop_time_ns) {}

  K8sObjectType type() { return type_; }

  const UID& uid() const { return uid_; }

  const std::string& name() const { return name_; }
  const std::string& ns() const { return ns_; }

  int64_t start_time_ns() const { return start_time_ns_; }
  void set_start_time_ns(int64_t start_time_ns) { start_time_ns_ = start_time_ns; }

  int64_t stop_time_ns() const { return stop_time_ns_; }
  void set_stop_time_ns(int64_t stop_time_ns) { stop_time_ns_ = stop_time_ns; }

  virtual std::unique_ptr<K8sMetadataObject> Clone() const = 0;
  virtual std::string DebugString(int indent = 0) const = 0;

 protected:
  K8sMetadataObject(const K8sMetadataObject& other) = default;
  K8sMetadataObject& operator=(const K8sMetadataObject& other) = delete;

 private:
  /**
   * The type of this object.
   */
  const K8sObjectType type_ = K8sObjectType::kUnknown;

  /**
   * The ID assigned by K8s that is unique in both space and time.
   */
  const UID uid_ = 0;

  /**
   * The namespace for this object.
   */

  std::string ns_;

  /**
   * The name which is unique in space but not time.
   */
  std::string name_;

  /**
   * Start time of this K8s object.
   */
  int64_t start_time_ns_ = 0;

  /**
   * Stop time of this K8s object.
   * A value of 0 implies that the object is still active.
   */
  int64_t stop_time_ns_ = 0;
};

enum class PodQOSClass : uint8_t { kUnknown = 0, kGuaranteed, kBestEffort, kBurstable };

inline PodQOSClass ConvertToPodQOsClass(pl::shared::k8s::metadatapb::PodQOSClass pb_enum) {
  using qos_pb = pl::shared::k8s::metadatapb::PodQOSClass;
  switch (pb_enum) {
    case qos_pb::QOS_CLASS_BURSTABLE:
      return PodQOSClass::kBurstable;
    case qos_pb::QOS_CLASS_BEST_EFFORT:
      return PodQOSClass::kBestEffort;
    case qos_pb::QOS_CLASS_GUARANTEED:
      return PodQOSClass::kGuaranteed;
    default:
      return PodQOSClass::kUnknown;
  }
}

enum class PodPhase : uint8_t { kUnknown = 0, kPending, kRunning, kSucceeded, kFailed };

inline PodPhase ConvertToPodPhase(pl::shared::k8s::metadatapb::PodPhase pb_enum) {
  using phase_pb = pl::shared::k8s::metadatapb::PodPhase;
  switch (pb_enum) {
    case phase_pb::PENDING:
      return PodPhase::kPending;
    case phase_pb::RUNNING:
      return PodPhase::kRunning;
    case phase_pb::SUCCEEDED:
      return PodPhase::kSucceeded;
    case phase_pb::FAILED:
      return PodPhase::kFailed;
    default:
      return PodPhase::kUnknown;
  }
}

/**
 * PodInfo contains information about K8s pods.
 */
class PodInfo : public K8sMetadataObject {
 public:
  PodInfo(UID uid, std::string_view ns, std::string_view name, PodQOSClass qos_class,
          PodPhase phase, std::string_view node_name, std::string_view hostname,
          std::string_view pod_ip, int64_t start_timestamp_ns = 0, int64_t stop_timestamp_ns = 0)
      : K8sMetadataObject(K8sObjectType::kPod, uid, ns, name, start_timestamp_ns,
                          stop_timestamp_ns),
        qos_class_(qos_class),
        phase_(phase),
        node_name_(node_name),
        hostname_(hostname),
        pod_ip_(pod_ip) {}

  explicit PodInfo(const pl::shared::k8s::metadatapb::PodUpdate& pod_update_info)
      : PodInfo(pod_update_info.uid(), pod_update_info.namespace_(), pod_update_info.name(),
                ConvertToPodQOsClass(pod_update_info.qos_class()),
                ConvertToPodPhase(pod_update_info.phase()), pod_update_info.node_name(),
                pod_update_info.hostname(), pod_update_info.pod_ip(),
                pod_update_info.start_timestamp_ns(), pod_update_info.stop_timestamp_ns()) {}

  virtual ~PodInfo() = default;

  void AddContainer(CIDView cid) { containers_.emplace(cid); }
  void RmContainer(CIDView cid) { containers_.erase(cid); }

  void AddService(UIDView uid) { services_.emplace(uid); }
  void RmService(UIDView uid) { services_.erase(uid); }
  PodQOSClass qos_class() const { return qos_class_; }
  PodPhase phase() const { return phase_; }

  void set_node_name(std::string_view node_name) { node_name_ = node_name; }
  void set_hostname(std::string_view hostname) { hostname_ = hostname; }
  void set_pod_ip(std::string_view pod_ip) { pod_ip_ = pod_ip; }
  const std::string& node_name() const { return node_name_; }
  const std::string& hostname() const { return hostname_; }
  const std::string& pod_ip() const { return pod_ip_; }

  const absl::flat_hash_set<std::string>& containers() const { return containers_; }
  const absl::flat_hash_set<std::string>& services() const { return services_; }

  std::unique_ptr<K8sMetadataObject> Clone() const override {
    return std::unique_ptr<PodInfo>(new PodInfo(*this));
  }

  std::string DebugString(int indent = 0) const override;

 protected:
  PodInfo(const PodInfo& other) = default;
  PodInfo& operator=(const PodInfo& other) = delete;

 private:
  PodQOSClass qos_class_;
  PodPhase phase_;
  /**
   * Set of containers that are running on this pod.
   *
   * The ContainerInformation is located in containers in the K8s state.
   */
  absl::flat_hash_set<CID> containers_;
  /**
   * The set of services that associate with this pod. K8s allows
   * multiple services from exposing the same pod.
   *
   * Should point to ServiceInfo via the data structure containing this pod.
   */
  absl::flat_hash_set<UID> services_;

  std::string node_name_;
  std::string hostname_;
  std::string pod_ip_;
};

/**
 * Store information about containers.
 *
 * Though this is not strictly a K8s object, it's state is tracked by K8s
 * so we include it here.
 */
class ContainerInfo {
 public:
  ContainerInfo() = delete;
  ContainerInfo(CID cid, std::string_view name, int64_t start_time_ns, int64_t stop_time_ns = 0)
      : cid_(std::move(cid)),
        name_(std::string(name)),
        start_time_ns_(start_time_ns),
        stop_time_ns_(stop_time_ns) {}

  explicit ContainerInfo(const pl::shared::k8s::metadatapb::ContainerUpdate& container_update_info)
      : ContainerInfo(container_update_info.cid(), container_update_info.name(),
                      container_update_info.start_timestamp_ns(),
                      container_update_info.stop_timestamp_ns()) {}

  const CID& cid() const { return cid_; }
  const std::string& name() const { return name_; }

  void set_pod_id(std::string_view pod_id) { pod_id_ = pod_id; }
  const UID& pod_id() const { return pod_id_; }

  void AddUPID(UPID upid) { active_upids_.emplace(upid); }
  void DeactivateUPID(UPID upid) {
    auto it = active_upids_.find(upid);
    if (it != active_upids_.end()) {
      inactive_upids_.emplace(*it);
      active_upids_.erase(it);
    }
  }

  // This function can be used to mark the entire container as stopped.
  void DeactivateAllUPIDs() {
    auto it = active_upids_.begin();
    while (it != active_upids_.end()) {
      inactive_upids_.emplace(*it);
      ++it;
    }
    active_upids_.clear();
  }

  bool HasActiveUPID(UPID upid) const { return active_upids_.contains(upid); }
  bool HasInActiveUPID(UPID upid) const { return inactive_upids_.contains(upid); }
  bool HasUPID(UPID upid) const { return HasActiveUPID(upid) || HasInActiveUPID(upid); }

  const absl::flat_hash_set<UPID>& active_upids() const { return active_upids_; }
  const absl::flat_hash_set<UPID>& inactive_upids() const { return inactive_upids_; }

  int64_t start_time_ns() const { return start_time_ns_; }

  int64_t stop_time_ns() const { return stop_time_ns_; }
  void set_stop_time_ns(int64_t stop_time_ns) { stop_time_ns_ = stop_time_ns; }

  std::unique_ptr<ContainerInfo> Clone() const {
    return std::unique_ptr<ContainerInfo>(new ContainerInfo(*this));
  }

  std::string DebugString(int indent = 0) const;

 protected:
  ContainerInfo(const ContainerInfo& other) = default;
  ContainerInfo& operator=(const ContainerInfo& other) = delete;

 private:
  const CID cid_;
  const std::string name_;
  UID pod_id_ = "";

  /**
   * The set of UPIDs that are running on this container.
   */
  absl::flat_hash_set<UPID> active_upids_;

  /**
   * The set of UPIDs that used to run on this container but have since been killed.
   * We maintain them for a while so that they remain queryable.
   */
  absl::flat_hash_set<UPID> inactive_upids_;

  /**
   * Start time of this K8s object.
   */
  const int64_t start_time_ns_ = 0;

  /**
   * Stop time of this K8s object.
   * A value of 0 implies that the object is still active.
   */
  int64_t stop_time_ns_ = 0;
};

/**
 * ServiceInfo contains information about K8s services.
 */
class ServiceInfo : public K8sMetadataObject {
 public:
  ServiceInfo(UID uid, std::string_view ns, std::string_view name)
      : K8sMetadataObject(K8sObjectType::kService, std::move(uid), std::move(ns), std::move(name)) {
  }
  virtual ~ServiceInfo() = default;

  void AddPod(UIDView uid) { pods_.emplace(uid); }
  void RmPod(UIDView uid) { pods_.erase(uid); }

  const absl::flat_hash_set<std::string>& pods() const { return pods_; }

  std::unique_ptr<K8sMetadataObject> Clone() const override {
    return std::unique_ptr<ServiceInfo>(new ServiceInfo(*this));
  }

  std::string DebugString(int indent = 0) const override;

 protected:
  ServiceInfo(const ServiceInfo& other) = default;
  ServiceInfo& operator=(const ServiceInfo& other) = delete;

 private:
  /**
   * Set of pods that are running on this pod.
   *
   * The PodInfo is located in pods in the K8s state.
   */
  absl::flat_hash_set<UID> pods_;
};

}  // namespace md
}  // namespace pl
