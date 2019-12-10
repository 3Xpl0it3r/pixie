#pragma once

#include <memory>
#include <string>
#include <utility>

#include "src/vizier/services/agent/manager/manager.h"

namespace pl {
namespace vizier {
namespace agent {

class KelvinManager : public Manager {
 public:
  template <typename... Args>
  static StatusOr<std::unique_ptr<Manager>> Create(Args&&... args) {
    auto m = std::unique_ptr<KelvinManager>(new KelvinManager(std::forward<Args>(args)...));
    PL_RETURN_IF_ERROR(m->Init());
    return std::unique_ptr<Manager>(std::move(m));
  }

  ~KelvinManager() override = default;

 protected:
  KelvinManager() = delete;
  KelvinManager(sole::uuid agent_id, std::string_view addr, int grpc_server_port,
                std::string_view nats_url, std::string_view qb_url)
      : Manager(agent_id, grpc_server_port, KelvinManager::Capabilities(), nats_url, qb_url) {
    info()->address = std::string(addr);
  }

  Status InitImpl() override;
  Status PostRegisterHook() override;
  Status StopImpl(std::chrono::milliseconds) override;

 private:
  static services::shared::agent::AgentCapabilities Capabilities() {
    services::shared::agent::AgentCapabilities capabilities;
    capabilities.set_collects_data(false);
    return capabilities;
  }
};

}  // namespace agent
}  // namespace vizier
}  // namespace pl
