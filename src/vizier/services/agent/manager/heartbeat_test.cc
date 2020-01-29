#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "src/common/event/api_impl.h"
#include "src/common/event/libuv.h"
#include "src/common/event/nats.h"
#include "src/common/system/config_mock.h"
#include "src/common/testing/event/simulated_time_system.h"
#include "src/vizier/messages/messagespb/messages.pb.h"
#include "src/vizier/services/agent/manager/heartbeat.h"
#include "src/vizier/services/agent/manager/manager.h"

#include "src/common/testing/testing.h"

namespace pl {
namespace vizier {
namespace agent {

using ::pl::table_store::schema::Relation;
using ::pl::testing::proto::EqualsProto;

const char* kAgentUpdateInfoSchemaNoTablets = R"proto(
does_update_schema: true
schema {
  name: "relation0"
  columns {
    name: "time_"
    data_type: TIME64NS
  }
  columns {
    name: "count"
    data_type: INT64
  }
}
schema {
  name: "relation1"
  columns {
    name: "time_"
    data_type: TIME64NS
  }
  columns {
    name: "gauge"
    data_type: FLOAT64
  }
})proto";

template <typename TMsg>
class FakeNATSConnector : public event::NATSConnector<TMsg> {
 public:
  FakeNATSConnector() : event::NATSConnector<TMsg>("", "", "", nullptr) {}
  ~FakeNATSConnector() override {}

  Status Connect(event::Dispatcher*) override { return Status::OK(); }

  Status Publish(const TMsg& msg) override {
    published_msgs_.push_back(msg);
    return Status::OK();
  }

  std::vector<TMsg> published_msgs_;
};

class HeartbeatMessageHandlerTest : public ::testing::Test {
 protected:
  void TearDown() override { dispatcher_->Exit(); }

  HeartbeatMessageHandlerTest() {
    start_monotonic_time_ = std::chrono::steady_clock::now();
    start_system_time_ = std::chrono::system_clock::now();
    time_system_ =
        std::make_unique<event::SimulatedTimeSystem>(start_monotonic_time_, start_system_time_);
    api_ = std::make_unique<pl::event::APIImpl>(time_system_.get());
    dispatcher_ = api_->AllocateDispatcher("manager");
    nats_conn_ = std::make_unique<FakeNATSConnector<pl::vizier::messages::VizierMessage>>();
    auto sys_config = system::MockConfig();
    EXPECT_CALL(sys_config, KernelTicksPerSecond()).WillRepeatedly(::testing::Return(10000000));

    mds_manager_ = std::make_unique<md::AgentMetadataStateManager>(
        "host", 1, true, absl::optional<CIDRBlock>{}, sys_config);

    // Relation info with no tabletization.
    Relation relation0({types::TIME64NS, types::INT64}, {"time_", "count"});
    RelationInfo relation_info0("relation0", /* id */ 0, relation0);
    // Relation info with no tabletization.
    Relation relation1({types::TIME64NS, types::FLOAT64}, {"time_", "gauge"});
    RelationInfo relation_info1("relation1", /* id */ 1, relation1);
    std::vector<RelationInfo> relation_info_vec({relation_info0, relation_info1});
    // Pass relation info to the manager.
    relation_info_manager_ = std::make_unique<RelationInfoManager>();
    EXPECT_OK(relation_info_manager_->UpdateRelationInfo(relation_info_vec));

    agent_info_ = agent::Info{};
    agent_info_.capabilities.set_collects_data(true);

    heartbeat_handler_ = std::make_unique<HeartbeatMessageHandler>(
        dispatcher_.get(), mds_manager_.get(), relation_info_manager_.get(), &agent_info_,
        nats_conn_.get());
  }

  event::MonotonicTimePoint start_monotonic_time_;
  event::SystemTimePoint start_system_time_;
  std::unique_ptr<event::SimulatedTimeSystem> time_system_;
  std::unique_ptr<event::APIImpl> api_;
  std::unique_ptr<event::Dispatcher> dispatcher_;
  std::unique_ptr<md::AgentMetadataStateManager> mds_manager_;
  std::unique_ptr<RelationInfoManager> relation_info_manager_;
  std::unique_ptr<HeartbeatMessageHandler> heartbeat_handler_;
  std::unique_ptr<FakeNATSConnector<pl::vizier::messages::VizierMessage>> nats_conn_;
  agent::Info agent_info_;
};

TEST_F(HeartbeatMessageHandlerTest, InitialHeartbeatTimeout) {
  dispatcher_->Run(event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ(1, nats_conn_->published_msgs_.size());
  auto hb = nats_conn_->published_msgs_[0].mutable_heartbeat();
  EXPECT_EQ(0, hb->sequence_number());

  time_system_->SetMonotonicTime(start_monotonic_time_ + std::chrono::seconds(6));
  dispatcher_->Run(event::Dispatcher::RunType::NonBlock);

  // If no ack received, hb manager should resend the same heartbeat.
  EXPECT_EQ(2, nats_conn_->published_msgs_.size());
  hb = nats_conn_->published_msgs_[1].mutable_heartbeat();
  EXPECT_EQ(0, hb->sequence_number());

  time_system_->SetMonotonicTime(start_monotonic_time_ + std::chrono::milliseconds(5 * 5000 + 1));
  ASSERT_DEATH(dispatcher_->Run(event::Dispatcher::RunType::NonBlock),
               "Timeout waiting for heartbeat ACK for seq_num=0");
}

TEST_F(HeartbeatMessageHandlerTest, ReceivedHeartbeatNack) {
  dispatcher_->Run(event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ(1, nats_conn_->published_msgs_.size());
  auto hb = nats_conn_->published_msgs_[0].mutable_heartbeat();
  EXPECT_EQ(0, hb->sequence_number());

  auto hb_nack = std::make_unique<messages::VizierMessage>();
  auto hb_nack_msg = hb_nack->mutable_heartbeat_nack();
  PL_UNUSED(hb_nack_msg);

  ASSERT_DEATH([&]() { auto s = heartbeat_handler_->HandleMessage(std::move(hb_nack)); }(),
               "Got a heartbeat NACK.");
}

TEST_F(HeartbeatMessageHandlerTest, HandleHeartbeat) {
  dispatcher_->Run(event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ(1, nats_conn_->published_msgs_.size());
  auto hb = nats_conn_->published_msgs_[0].mutable_heartbeat();
  EXPECT_EQ(0, hb->sequence_number());
  EXPECT_THAT(*hb->mutable_update_info(), EqualsProto(kAgentUpdateInfoSchemaNoTablets));

  time_system_->SetMonotonicTime(start_monotonic_time_ + std::chrono::milliseconds(5 * 4000));
  dispatcher_->Run(event::Dispatcher::RunType::NonBlock);

  auto hb_ack = std::make_unique<messages::VizierMessage>();
  auto hb_ack_msg = hb_ack->mutable_heartbeat_ack();
  hb_ack_msg->set_sequence_number(0);

  auto s = heartbeat_handler_->HandleMessage(std::move(hb_ack));

  time_system_->SetMonotonicTime(start_monotonic_time_ + std::chrono::milliseconds(5 * 5000 + 1));
  dispatcher_->Run(event::Dispatcher::RunType::NonBlock);

  EXPECT_EQ(3, nats_conn_->published_msgs_.size());
  hb = nats_conn_->published_msgs_[2].mutable_heartbeat();
  EXPECT_EQ(1, hb->sequence_number());
  // No schema should be included in subsequent heartbeats.
  EXPECT_EQ(0, hb->mutable_update_info()->schema().size());
}

}  // namespace agent
}  // namespace vizier
}  // namespace pl
