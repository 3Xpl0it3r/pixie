#include "src/vizier/services/agent/manager/manager.h"

#include <limits.h>

#include <memory>
#include <string>
#include <utility>

#include "src/common/base/base.h"
#include "src/common/perf/perf.h"
#include "src/vizier/funcs/context/vizier_context.h"
#include "src/vizier/funcs/funcs.h"
#include "src/vizier/services/agent/manager/exec.h"
#include "src/vizier/services/agent/manager/heartbeat.h"
#include "src/vizier/services/agent/manager/ssl.h"

namespace {

pl::StatusOr<std::string> GetHostname() {
  char hostname[HOST_NAME_MAX];
  int err = gethostname(hostname, sizeof(hostname));
  if (err != 0) {
    return pl::error::Unknown("Failed to get hostname");
  }
  return std::string(hostname);
}

}  // namespace

namespace pl {
namespace vizier {
namespace agent {
using ::pl::event::Dispatcher;

Manager::Manager(sole::uuid agent_id, int grpc_server_port,
                 services::shared::agent::AgentCapabilities capabilities, std::string_view nats_url,
                 std::string_view mds_url)
    : Manager(agent_id, grpc_server_port, std::move(capabilities), mds_url,
              Manager::CreateDefaultNATSConnector(agent_id, nats_url)) {}

Manager::Manager(sole::uuid agent_id, int grpc_server_port,
                 services::shared::agent::AgentCapabilities capabilities, std::string_view mds_url,
                 std::unique_ptr<VizierNATSConnector> nats_connector)
    : grpc_channel_creds_(SSL::DefaultGRPCClientCreds()),
      time_system_(std::make_unique<pl::event::RealTimeSystem>()),
      api_(std::make_unique<pl::event::APIImpl>(time_system_.get())),
      dispatcher_(api_->AllocateDispatcher("manager")),
      nats_connector_(std::move(nats_connector)),
      // TODO(zasgar): Not constructing the MDS by checking the url being empty is a bit janky. Fix
      // this.
      func_context_(
          this, mds_url.size() == 0 ? nullptr : CreateDefaultMDSStub(mds_url, grpc_channel_creds_)),
      table_store_(std::make_shared<table_store::TableStore>()) {
  // Register Vizier specific and carnot builtin functions.

  auto func_registry = std::make_unique<pl::carnot::udf::Registry>("vizier_func_registry");
  ::pl::vizier::funcs::RegisterFuncsOrDie(func_context_, func_registry.get());

  // TODO(zasgar/nserrino): abstract away the stub generator.
  carnot_ = pl::carnot::Carnot::Create(
                std::move(func_registry), table_store_,
                [&](const std::string& remote_addr)
                    -> std::unique_ptr<pl::carnotpb::KelvinService::StubInterface> {
                  grpc::ChannelArguments args;
                  args.SetSslTargetNameOverride("kelvin.pl.svc");

                  auto chan = grpc::CreateCustomChannel(remote_addr, grpc_channel_creds_, args);
                  return pl::carnotpb::KelvinService::NewStub(chan);
                },
                grpc_server_port, SSL::DefaultGRPCServerCreds())
                .ConsumeValueOrDie();

  info_.agent_id = agent_id;
  info_.capabilities = std::move(capabilities);
}

Status Manager::RegisterAgent() {
  // Send the registration request.
  messages::VizierMessage req;
  auto agent_info = req.mutable_register_agent_request()->mutable_info();
  ToProto(info_.agent_id, agent_info->mutable_agent_id());
  agent_info->set_ip_address(info_.address);
  auto host_info = agent_info->mutable_host_info();
  host_info->set_hostname(info_.hostname);
  *agent_info->mutable_capabilities() = info_.capabilities;
  PL_RETURN_IF_ERROR(nats_connector_->Publish(req));
  return Status::OK();
}

Status Manager::Init() {
  auto hostname_or_s = GetHostname();
  if (!hostname_or_s.ok()) {
    return hostname_or_s.status();
  }

  info_.hostname = hostname_or_s.ConsumeValueOrDie();

  LOG(INFO) << "Hostname: " << info_.hostname;

  // The first step is to connect to stats and register the agent.
  // Downstream dependencies like stirling/carnot depend on knowing
  // ASID and metadata state, which is only available after registration is
  // complete.
  if (nats_connector_ == nullptr) {
    LOG(WARNING) << "NATS is not configured, skip connecting. Stirling and Carnot might not behave "
                    "as expected because of this.";
  } else {
    PL_RETURN_IF_ERROR(nats_connector_->Connect(dispatcher_.get()));
    // Attach the message handler for nats:
    nats_connector_->RegisterMessageHandler(
        std::bind(&Manager::NATSMessageHandler, this, std::placeholders::_1));
    registration_timeout_ = dispatcher_->CreateTimer([this] {
      if (agent_registered_) {
        registration_timeout_.release();
        return;
      }
      LOG(FATAL) << "Timeout waiting for registration ack";
    });
    // Send the agent info.
    PL_RETURN_IF_ERROR(RegisterAgent());
    registration_timeout_->EnableTimer(kRegistrationPeriod);
  }

  return InitImpl();
}

Status Manager::Run() {
  running_ = true;
  dispatcher_->Run(pl::event::Dispatcher::RunType::Block);
  running_ = false;
  return Status::OK();
}

Status Manager::Stop(std::chrono::milliseconds timeout) {
  // Already stopping, protect against multiple calls.
  if (stop_called_) {
    return Status::OK();
  }
  stop_called_ = true;

  dispatcher_->Stop();
  auto s = StopImpl(timeout);

  // Wait for a limited amount of time for main thread to stop processing.
  std::chrono::time_point expiration_time = time_system_->MonotonicTime() + timeout;
  while (running_ && time_system_->MonotonicTime() < expiration_time) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }

  return s;
}

Status Manager::RegisterBackgroundHelpers() {
  metadata_update_timer_ = dispatcher_->CreateTimer([this]() {
    VLOG(1) << "State Update";
    ECHECK_OK(mds_manager_->PerformMetadataStateUpdate());
    if (metadata_update_timer_) {
      metadata_update_timer_->EnableTimer(std::chrono::seconds(5));
    }
  });
  metadata_update_timer_->EnableTimer(std::chrono::seconds(5));

  // Add Heartbeat and execute query handlers.
  auto heartbeat_handler = std::make_shared<HeartbeatMessageHandler>(
      dispatcher_.get(), mds_manager_.get(), relation_info_manager_.get(), &info_,
      nats_connector_.get());
  PL_CHECK_OK(
      RegisterMessageHandler(messages::VizierMessage::MsgCase::kHeartbeatAck, heartbeat_handler));
  PL_CHECK_OK(
      RegisterMessageHandler(messages::VizierMessage::MsgCase::kHeartbeatNack, heartbeat_handler));

  return Status::OK();
}

Status Manager::RegisterMessageHandler(Manager::MsgCase c, std::shared_ptr<MessageHandler> handler,
                                       bool override) {
  if (message_handlers_.contains(c) && !override) {
    return error::AlreadyExists("message handler already exists for case: $0", c);
  }
  message_handlers_[c] = handler;
  return Status::OK();
}

void Manager::NATSMessageHandler(Manager::VizierNATSConnector::MsgType msg) {
  // NATS returns data to us in an arbritrary thread. We need to handle it in the event
  // loop thread so we post to the event loop.

  // This funny pointer stuff is required because we generate an std::function,
  // that requires a copy of the lambda. The release allows us to recapture the value
  // into another unique pointer.
  messages::VizierMessage* m = msg.release();
  dispatcher_->Post(
      [m, this]() mutable { HandleMessage(std::unique_ptr<messages::VizierMessage>(m)); });
}

void Manager::HandleMessage(std::unique_ptr<messages::VizierMessage> msg) {
  VLOG(1) << "Manager::Run::GotMessage " << msg->DebugString();

  if (msg->msg_case() == messages::VizierMessage::MsgCase::kRegisterAgentResponse) {
    HandleRegisterAgentResponse(std::move(msg));
    return;
  }
  auto c = msg->msg_case();
  auto it = message_handlers_.find(c);
  if (it != message_handlers_.end()) {
    ECHECK_OK(it->second->HandleMessage(std::move(msg))) << "message handler failed... ignoring";
    // Handler found.
  } else {
    LOG(ERROR) << "Unhandled message type: " << c << " Message: " << msg->DebugString();
  }
}

void Manager::HandleRegisterAgentResponse(std::unique_ptr<messages::VizierMessage> msg) {
  LOG_IF(FATAL, !msg->has_register_agent_response())
      << "Did not get register agent response. Got: " << msg->DebugString();
  CHECK(!agent_registered_) << "Agent already registered, but got another registration response.";
  info_.asid = msg->register_agent_response().asid();

  // Save typing
  const auto& cluster_cidr_str = msg->register_agent_response().cluster_cidr();
  CIDRBlock cidr;
  Status status = ParseCIDRBlock(cluster_cidr_str, &cidr);
  LOG_IF(ERROR, !status.ok()) << "Could not parse CIDR block string, status: " << status.ToString();
  absl::optional<CIDRBlock> cluster_cidr_opt;
  if (status.ok()) {
    LOG(INFO) << "cluster_cidr is set to: " << cluster_cidr_str;
    cluster_cidr_opt = cidr;
  } else {
    LOG(ERROR) << absl::Substitute("Cloud not obtain cluster_cidr, cidr string: '$0'",
                                   cluster_cidr_str);
  }

  mds_manager_ = std::make_unique<pl::md::AgentMetadataStateManager>(
      info_.hostname, info_.asid, info_.agent_id, info_.capabilities.collects_data(),
      cluster_cidr_opt, pl::system::Config::GetInstance());
  relation_info_manager_ = std::make_unique<RelationInfoManager>();

  PL_CHECK_OK(PostRegisterHook());

  // Register the Carnot callback for metadata.
  carnot_->RegisterAgentMetadataCallback(
      std::bind(&pl::md::AgentMetadataStateManager::CurrentAgentMetadataState, mds_manager_.get()));

  PL_CHECK_OK(RegisterBackgroundHelpers());
  agent_registered_ = true;
}

std::unique_ptr<Manager::VizierNATSConnector> Manager::CreateDefaultNATSConnector(
    const sole::uuid& agent_id, std::string_view nats_url) {
  if (nats_url.empty()) {
    LOG(WARNING) << "--nats_url is empty, skip connecting to NATS.";
    return nullptr;
  }

  auto tls_config = SSL::DefaultNATSCreds();
  std::string agent_sub_topic = absl::StrFormat("/agent/%s", agent_id.str());

  return std::make_unique<Manager::VizierNATSConnector>(nats_url, "update_agent" /*pub_topic*/,
                                                        agent_sub_topic, std::move(tls_config));
}

Manager::MDSServiceSPtr Manager::CreateDefaultMDSStub(
    std::string_view mds_addr, std::shared_ptr<grpc::ChannelCredentials> channel_creds) {
  // We need to move the channel here since gRPC mocking is done by the stub.
  auto chan = grpc::CreateChannel(std::string(mds_addr), channel_creds);
  return std::make_shared<Manager::MDSService::Stub>(chan);
}

Manager::MessageHandler::MessageHandler(Dispatcher* dispatcher, Info* agent_info,
                                        Manager::VizierNATSConnector* nats_conn)
    : agent_info_(agent_info), nats_conn_(nats_conn), dispatcher_(dispatcher) {}

}  // namespace agent
}  // namespace vizier
}  // namespace pl
