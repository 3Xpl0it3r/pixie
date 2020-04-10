#include "src/stirling/connection_tracker.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/socket.h>

#include "src/common/base/test_utils.h"
#include "src/stirling/mysql/test_utils.h"
#include "src/stirling/testing/event_generator.h"

namespace pl {
namespace stirling {

using ::testing::IsEmpty;
using ::testing::SizeIs;

using testing::kHTTP2EndStreamDataFrame;
using testing::kHTTP2EndStreamHeadersFrame;
using testing::kHTTPReq0;
using testing::kHTTPReq1;
using testing::kHTTPReq2;
using testing::kHTTPResp0;
using testing::kHTTPResp1;
using testing::kHTTPResp2;
using testing::kHTTPUpgradeReq;
using testing::kHTTPUpgradeResp;

class ConnectionTrackerTest : public ::testing::Test {
 protected:
  testing::RealClock real_clock_;
};

TEST_F(ConnectionTrackerTest, timestamp_test) {
  // Use mock clock to get precise timestamps.
  testing::MockClock mock_clock;
  testing::EventGenerator event_gen(&mock_clock);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> event0 = event_gen.InitSendEvent<kProtocolHTTP>("event0");
  std::unique_ptr<SocketDataEvent> event1 = event_gen.InitRecvEvent<kProtocolHTTP>("event1");
  std::unique_ptr<SocketDataEvent> event2 = event_gen.InitSendEvent<kProtocolHTTP>("event2");
  std::unique_ptr<SocketDataEvent> event3 = event_gen.InitRecvEvent<kProtocolHTTP>("event3");
  std::unique_ptr<SocketDataEvent> event4 = event_gen.InitSendEvent<kProtocolHTTP>("event4");
  std::unique_ptr<SocketDataEvent> event5 = event_gen.InitRecvEvent<kProtocolHTTP>("event5");
  struct socket_control_event_t close_event = event_gen.InitClose();

  ConnectionTracker tracker;
  EXPECT_EQ(0, tracker.last_bpf_timestamp_ns());
  tracker.AddControlEvent(conn);
  EXPECT_EQ(1, tracker.last_bpf_timestamp_ns());
  tracker.AddDataEvent(std::move(event0));
  EXPECT_EQ(2, tracker.last_bpf_timestamp_ns());
  tracker.AddDataEvent(std::move(event1));
  EXPECT_EQ(3, tracker.last_bpf_timestamp_ns());
  tracker.AddDataEvent(std::move(event5));
  EXPECT_EQ(7, tracker.last_bpf_timestamp_ns());
  tracker.AddDataEvent(std::move(event2));
  EXPECT_EQ(7, tracker.last_bpf_timestamp_ns());
  tracker.AddDataEvent(std::move(event3));
  EXPECT_EQ(7, tracker.last_bpf_timestamp_ns());
  tracker.AddDataEvent(std::move(event4));
  EXPECT_EQ(7, tracker.last_bpf_timestamp_ns());
  tracker.AddControlEvent(close_event);
  EXPECT_EQ(8, tracker.last_bpf_timestamp_ns());
}

// This test is of marginal value. Remove if it becomes hard to maintain.
TEST_F(ConnectionTrackerTest, info_string) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> event0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> event1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  std::unique_ptr<SocketDataEvent> event2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.AddDataEvent(std::move(event0));
  tracker.AddDataEvent(std::move(event1));
  tracker.AddDataEvent(std::move(event2));

  std::string debug_info = DebugString<http::ProtocolTraits>(tracker, "");

  std::string expected_output = R"(pid=12345 fd=3 gen=1
state=kCollecting
remote_addr=0.0.0.0:0
protocol=kProtocolHTTP
recv queue
  raw events=1
  parsed frames=0
send queue
  raw events=2
  parsed frames=0
)";

  EXPECT_EQ(expected_output, debug_info);

  tracker.ProcessToRecords<http::ProtocolTraits>();

  debug_info = DebugString<http::ProtocolTraits>(tracker, "");

  expected_output = R"(pid=12345 fd=3 gen=1
state=kCollecting
remote_addr=0.0.0.0:0
protocol=kProtocolHTTP
recv queue
  raw events=0
  parsed frames=0
send queue
  raw events=0
  parsed frames=1
)";

  EXPECT_EQ(expected_output, debug_info);
}

TEST_F(ConnectionTrackerTest, ReqRespMatchingSimple) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> resp0 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> resp1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp1);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2);
  std::unique_ptr<SocketDataEvent> resp2 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp2);
  struct socket_control_event_t close_event = event_gen.InitClose();

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.AddDataEvent(std::move(req0));
  tracker.AddDataEvent(std::move(resp0));
  tracker.AddDataEvent(std::move(req1));
  tracker.AddDataEvent(std::move(resp1));
  tracker.AddDataEvent(std::move(req2));
  tracker.AddDataEvent(std::move(resp2));
  tracker.AddControlEvent(close_event);

  std::vector<http::Record> records = tracker.ProcessToRecords<http::ProtocolTraits>();

  ASSERT_EQ(3, records.size());

  EXPECT_EQ(records[0].req.http_req_path, "/index.html");
  EXPECT_EQ(records[0].resp.http_msg_body, "pixie");

  EXPECT_EQ(records[1].req.http_req_path, "/foo.html");
  EXPECT_EQ(records[1].resp.http_msg_body, "foo");

  EXPECT_EQ(records[2].req.http_req_path, "/bar.html");
  EXPECT_EQ(records[2].resp.http_msg_body, "bar");
}

TEST_F(ConnectionTrackerTest, DISABLED_ReqRespMatchingPipelined) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2);
  std::unique_ptr<SocketDataEvent> resp0 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  std::unique_ptr<SocketDataEvent> resp1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp1);
  std::unique_ptr<SocketDataEvent> resp2 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp2);
  struct socket_control_event_t close_event = event_gen.InitClose();

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.AddDataEvent(std::move(req0));
  tracker.AddDataEvent(std::move(req1));
  tracker.AddDataEvent(std::move(req2));
  tracker.AddDataEvent(std::move(resp0));
  tracker.AddDataEvent(std::move(resp1));
  tracker.AddDataEvent(std::move(resp2));
  tracker.AddControlEvent(close_event);

  std::vector<http::Record> records = tracker.ProcessToRecords<http::ProtocolTraits>();

  ASSERT_EQ(3, records.size());

  EXPECT_EQ(records[0].req.http_req_path, "/index.html");
  EXPECT_EQ(records[0].resp.http_msg_body, "pixie");

  EXPECT_EQ(records[1].req.http_req_path, "/foo.html");
  EXPECT_EQ(records[1].resp.http_msg_body, "foo");

  EXPECT_EQ(records[2].req.http_req_path, "/bar.html");
  EXPECT_EQ(records[2].resp.http_msg_body, "bar");
}

TEST_F(ConnectionTrackerTest, ReqRespMatchingSerializedMissingRequest) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> resp0 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> resp1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp1);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2);
  std::unique_ptr<SocketDataEvent> resp2 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp2);
  struct socket_control_event_t close_event = event_gen.InitClose();

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.AddDataEvent(std::move(req0));
  tracker.AddDataEvent(std::move(resp0));
  PL_UNUSED(req1);  // Missing event.
  tracker.AddDataEvent(std::move(resp1));
  tracker.AddDataEvent(std::move(req2));
  tracker.AddDataEvent(std::move(resp2));
  tracker.AddControlEvent(close_event);

  std::vector<http::Record> records = tracker.ProcessToRecords<http::ProtocolTraits>();

  ASSERT_EQ(2, records.size());

  EXPECT_EQ(records[0].req.http_req_path, "/index.html");
  EXPECT_EQ(records[0].resp.http_msg_body, "pixie");

  EXPECT_EQ(records[1].req.http_req_path, "/bar.html");
  EXPECT_EQ(records[1].resp.http_msg_body, "bar");
}

TEST_F(ConnectionTrackerTest, ReqRespMatchingSerializedMissingResponse) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> resp0 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> resp1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp1);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2);
  std::unique_ptr<SocketDataEvent> resp2 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp2);
  struct socket_control_event_t close_event = event_gen.InitClose();

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.AddDataEvent(std::move(req0));
  tracker.AddDataEvent(std::move(resp0));
  tracker.AddDataEvent(std::move(req1));
  PL_UNUSED(req2);  // Missing event.
  tracker.AddDataEvent(std::move(req2));
  tracker.AddDataEvent(std::move(resp2));
  tracker.AddControlEvent(close_event);

  std::vector<http::Record> records = tracker.ProcessToRecords<http::ProtocolTraits>();

  ASSERT_EQ(2, records.size());

  EXPECT_EQ(records[0].req.http_req_path, "/index.html");
  EXPECT_EQ(records[0].resp.http_msg_body, "pixie");

  EXPECT_EQ(records[1].req.http_req_path, "/bar.html");
  EXPECT_EQ(records[1].resp.http_msg_body, "bar");
}

TEST_F(ConnectionTrackerTest, TrackerDisable) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> resp0 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> resp1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp1);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>("hello");
  std::unique_ptr<SocketDataEvent> resp2 =
      event_gen.InitRecvEvent<kProtocolHTTP>("hello to you too");
  std::unique_ptr<SocketDataEvent> req3 = event_gen.InitSendEvent<kProtocolHTTP>("good-bye");
  std::unique_ptr<SocketDataEvent> resp3 =
      event_gen.InitRecvEvent<kProtocolHTTP>("good-bye to you too");
  struct socket_control_event_t close_event = event_gen.InitClose();

  ConnectionTracker tracker;
  std::vector<http::Record> records;

  tracker.AddControlEvent(conn);
  tracker.AddDataEvent(std::move(req0));
  tracker.AddDataEvent(std::move(resp0));
  tracker.AddDataEvent(std::move(req1));
  tracker.AddDataEvent(std::move(resp1));

  records = tracker.ProcessToRecords<http::ProtocolTraits>();

  ASSERT_EQ(2, records.size());
  ASSERT_FALSE(tracker.IsZombie());

  // Say this connection is not interesting to follow anymore.
  tracker.Disable();

  // More events arrive.
  tracker.AddDataEvent(std::move(req2));
  tracker.AddDataEvent(std::move(resp2));

  records = tracker.ProcessToRecords<http::ProtocolTraits>();

  ASSERT_EQ(0, records.size());
  ASSERT_FALSE(tracker.IsZombie());

  tracker.AddDataEvent(std::move(req3));
  tracker.AddDataEvent(std::move(resp3));
  tracker.AddControlEvent(close_event);

  records = tracker.ProcessToRecords<http::ProtocolTraits>();

  ASSERT_EQ(0, records.size());
  ASSERT_TRUE(tracker.IsZombie());
}

TEST_F(ConnectionTrackerTest, TrackerHTTP101Disable) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> resp0 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPUpgradeReq);
  std::unique_ptr<SocketDataEvent> resp1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPUpgradeResp);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> resp2 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp1);
  std::unique_ptr<SocketDataEvent> req3 = event_gen.InitSendEvent<kProtocolHTTP>("good-bye");
  std::unique_ptr<SocketDataEvent> resp3 =
      event_gen.InitRecvEvent<kProtocolHTTP>("good-bye to you too");
  struct socket_control_event_t close_event = event_gen.InitClose();

  ConnectionTracker tracker;
  std::vector<http::Record> records;

  tracker.AddControlEvent(conn);
  tracker.AddDataEvent(std::move(req0));
  tracker.AddDataEvent(std::move(resp0));
  tracker.AddDataEvent(std::move(req1));
  tracker.AddDataEvent(std::move(resp1));

  records = tracker.ProcessToRecords<http::ProtocolTraits>();
  tracker.IterationPostTick();

  ASSERT_EQ(2, records.size());
  ASSERT_FALSE(tracker.IsZombie());

  // More events arrive after the connection Upgrade.
  tracker.AddDataEvent(std::move(req2));
  tracker.AddDataEvent(std::move(resp2));

  // Since we previously received connection Upgrade, this tracker should be disabled.
  // All future calls to ProcessToRecords() should produce no results.

  // TODO(oazizi): This is a bad test beyond this point,
  // because a disabled tracker would never call ProcessToRecords again in Stirling.
  // Currently, this causes a warning to fire that states ProcessToRecords should not be
  // run on a stream at EOS.
  // However, the test still passes, so we'll leave the test for now.

  records = tracker.ProcessToRecords<http::ProtocolTraits>();
  tracker.IterationPostTick();

  ASSERT_EQ(0, records.size());
  ASSERT_FALSE(tracker.IsZombie());

  tracker.AddDataEvent(std::move(req3));
  tracker.AddDataEvent(std::move(resp3));
  tracker.AddControlEvent(close_event);

  // The tracker should, however, still process the close event.

  records = tracker.ProcessToRecords<http::ProtocolTraits>();
  tracker.IterationPostTick();

  ASSERT_EQ(0, records.size());
  ASSERT_TRUE(tracker.IsZombie());
}

TEST_F(ConnectionTrackerTest, stats_counter) {
  ConnectionTracker tracker;

  EXPECT_EQ(0, tracker.Stat(ConnectionTracker::CountStats::kDataEvent));

  tracker.IncrementStat(ConnectionTracker::CountStats::kDataEvent);
  EXPECT_EQ(1, tracker.Stat(ConnectionTracker::CountStats::kDataEvent));

  tracker.IncrementStat(ConnectionTracker::CountStats::kDataEvent);
  EXPECT_EQ(2, tracker.Stat(ConnectionTracker::CountStats::kDataEvent));
}

TEST_F(ConnectionTrackerTest, HTTP2ResetAfterStitchFailure) {
  testing::EventGenerator event_gen(&real_clock_);
  auto frame0 = event_gen.InitRecvEvent<kProtocolHTTP2>(kHTTP2EndStreamHeadersFrame);
  auto frame1 = event_gen.InitRecvEvent<kProtocolHTTP2>(kHTTP2EndStreamHeadersFrame);
  auto frame2 = event_gen.InitSendEvent<kProtocolHTTP2>(kHTTP2EndStreamDataFrame);
  auto frame3 = event_gen.InitSendEvent<kProtocolHTTP2>(kHTTP2EndStreamDataFrame);
  auto frame4 = event_gen.InitRecvEvent<kProtocolHTTP2>(kHTTP2EndStreamHeadersFrame);
  auto frame5 = event_gen.InitSendEvent<kProtocolHTTP2>(kHTTP2EndStreamDataFrame);

  ConnectionTracker tracker;

  tracker.AddDataEvent(std::move(frame0));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.resp_frames<http2::Frame>(), SizeIs(1));

  tracker.AddDataEvent(std::move(frame1));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  // Now we see two END_STREAM headers frame on stream ID 1, then that translate to 2 gRPC
  // response messages. That failure will cause stream being reset.
  EXPECT_THAT(tracker.resp_frames<http2::Frame>(), IsEmpty());

  tracker.AddDataEvent(std::move(frame2));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.req_frames<http2::Frame>(), SizeIs(1));

  tracker.AddDataEvent(std::move(frame3));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  // Ditto.
  EXPECT_THAT(tracker.req_frames<http2::Frame>(), IsEmpty());

  // Add a call to make sure things do not go haywire after resetting stream.
  tracker.AddDataEvent(std::move(frame4));
  tracker.AddDataEvent(std::move(frame5));
  auto records = tracker.ProcessToRecords<http2::ProtocolTraits>();
  // These 2 messages forms a matching req & resp.
  EXPECT_THAT(records, SizeIs(1));
}

// TODO(yzhao): Add the same test for HTTPMessage.
TEST_F(ConnectionTrackerTest, HTTP2FramesCleanedUpAfterBreachingSizeLimit) {
  testing::EventGenerator event_gen(&real_clock_);
  auto frame0 = event_gen.InitRecvEvent<kProtocolHTTP2>(kHTTP2EndStreamHeadersFrame);
  auto frame1 = event_gen.InitSendEvent<kProtocolHTTP2>(kHTTP2EndStreamDataFrame);
  auto frame2 = event_gen.InitRecvEvent<kProtocolHTTP2>(kHTTP2EndStreamHeadersFrame);
  auto frame3 = event_gen.InitSendEvent<kProtocolHTTP2>(kHTTP2EndStreamDataFrame);

  ConnectionTracker tracker;

  FLAGS_messages_size_limit_bytes = 10000;

  tracker.AddDataEvent(std::move(frame0));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.resp_frames<http2::Frame>(), SizeIs(1));

  // Set to 0 so it can expire immediately.
  FLAGS_messages_size_limit_bytes = 0;

  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.resp_frames<http2::Frame>(), IsEmpty());

  FLAGS_messages_size_limit_bytes = 10000;
  tracker.AddDataEvent(std::move(frame1));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.req_frames<http2::Frame>(), SizeIs(1));

  FLAGS_messages_size_limit_bytes = 0;
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  // Ditto.
  EXPECT_THAT(tracker.req_frames<http2::Frame>(), IsEmpty());

  // Add a call to make sure things do not go haywire after resetting stream.
  tracker.AddDataEvent(std::move(frame2));
  tracker.AddDataEvent(std::move(frame3));
  auto records = tracker.ProcessToRecords<http2::ProtocolTraits>();
  // These 2 messages forms a matching req & resp.
  EXPECT_THAT(records, SizeIs(1));
}

TEST_F(ConnectionTrackerTest, HTTP2FramesErasedAfterExpiration) {
  testing::EventGenerator event_gen(&real_clock_);
  auto frame0 = event_gen.InitRecvEvent<kProtocolHTTP2>(kHTTP2EndStreamHeadersFrame);
  auto frame1 = event_gen.InitSendEvent<kProtocolHTTP2>(kHTTP2EndStreamDataFrame);
  auto frame2 = event_gen.InitRecvEvent<kProtocolHTTP2>(kHTTP2EndStreamHeadersFrame);
  auto frame3 = event_gen.InitSendEvent<kProtocolHTTP2>(kHTTP2EndStreamDataFrame);

  ConnectionTracker tracker;

  FLAGS_messages_size_limit_bytes = 10000;
  FLAGS_messages_expiration_duration_secs = 10000;

  tracker.AddDataEvent(std::move(frame0));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.resp_frames<http2::Frame>(), SizeIs(1));

  // Set to 0 so it can expire immediately.
  FLAGS_messages_expiration_duration_secs = 0;

  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.resp_frames<http2::Frame>(), IsEmpty());

  FLAGS_messages_expiration_duration_secs = 10000;
  tracker.AddDataEvent(std::move(frame1));
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  EXPECT_THAT(tracker.req_frames<http2::Frame>(), SizeIs(1));

  FLAGS_messages_expiration_duration_secs = 0;
  tracker.ProcessToRecords<http2::ProtocolTraits>();
  // Ditto.
  EXPECT_THAT(tracker.req_frames<http2::Frame>(), IsEmpty());

  // Add a call to make sure things do not go haywire after resetting stream.
  tracker.AddDataEvent(std::move(frame2));
  tracker.AddDataEvent(std::move(frame3));
  auto records = tracker.ProcessToRecords<http2::ProtocolTraits>();
  // These 2 messages forms a matching req & resp.
  EXPECT_THAT(records, SizeIs(1));
}

TEST_F(ConnectionTrackerTest, HTTPStuckEventsAreRemoved) {
  // Use incomplete data to make it stuck.
  testing::EventGenerator event_gen(&real_clock_);
  auto data0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(0, 10));
  auto data1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(10, 10));
  auto data2 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPReq0.substr(20, 10));
  auto data3 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPReq0.substr(30, 10));

  ConnectionTracker tracker;

  tracker.AddDataEvent(std::move(data0));
  tracker.ProcessToRecords<http::ProtocolTraits>();
  EXPECT_FALSE(tracker.req_data()->Empty<http::Message>());
  tracker.ProcessToRecords<http::ProtocolTraits>();
  EXPECT_FALSE(tracker.req_data()->Empty<http::Message>());
  tracker.ProcessToRecords<http::ProtocolTraits>();
  EXPECT_FALSE(tracker.req_data()->Empty<http::Message>());

  // The 4th time, the stuck condition is detected and all data is purged.
  tracker.ProcessToRecords<http::ProtocolTraits>();
  EXPECT_TRUE(tracker.req_data()->Empty<http::Message>());

  // Now the stuck count is reset, so the event is kept.
  tracker.AddDataEvent(std::move(data1));
  tracker.ProcessToRecords<http::ProtocolTraits>();
  EXPECT_FALSE(tracker.req_data()->Empty<http::Message>());
}

TEST_F(ConnectionTrackerTest, HTTPMessagesErasedAfterExpiration) {
  testing::EventGenerator event_gen(&real_clock_);
  auto frame0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  auto frame1 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);
  auto frame2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  auto frame3 = event_gen.InitRecvEvent<kProtocolHTTP>(kHTTPResp0);

  ConnectionTracker tracker;

  FLAGS_messages_size_limit_bytes = 10000;
  FLAGS_messages_expiration_duration_secs = 10000;

  tracker.AddDataEvent(std::move(frame0));
  tracker.ProcessToRecords<http::ProtocolTraits>();
  EXPECT_THAT(tracker.req_frames<http::Message>(), SizeIs(1));

  FLAGS_messages_expiration_duration_secs = 0;

  tracker.ProcessToRecords<http::ProtocolTraits>();
  EXPECT_THAT(tracker.req_frames<http::Message>(), IsEmpty());

  // TODO(yzhao): It's not possible to test the response messages, as they are immediately exported
  // without waiting for the requests.
}

TEST_F(ConnectionTrackerTest, MySQLMessagesErasedAfterExpiration) {
  testing::EventGenerator event_gen(&real_clock_);
  auto msg0 =
      event_gen.InitSendEvent<kProtocolMySQL>(mysql::testutils::GenRawPacket(0, "\x03SELECT"));

  ConnectionTracker tracker;

  FLAGS_messages_size_limit_bytes = 10000;
  FLAGS_messages_expiration_duration_secs = 10000;

  tracker.AddDataEvent(std::move(msg0));
  tracker.ProcessToRecords<mysql::ProtocolTraits>();
  EXPECT_THAT(tracker.req_frames<mysql::Packet>(), SizeIs(1));

  FLAGS_messages_expiration_duration_secs = 0;

  tracker.ProcessToRecords<mysql::ProtocolTraits>();
  EXPECT_THAT(tracker.req_frames<mysql::Packet>(), IsEmpty());
}

// Tests that tracker state is kDisabled if the remote address is in the cluster's CIDR range.
TEST_F(ConnectionTrackerTest, TrackerDisabledForIntraClusterRemoteEndpoint) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  conn.open.traffic_class.role = EndpointRole::kRoleClient;

  // Set an address that falls in the intra-cluster address range.
  testing::SetIPv4RemoteAddr(&conn, "1.2.3.4");

  CIDRBlock cidr;
  ASSERT_OK(ParseCIDRBlock("1.2.3.4/14", &cidr));
  std::vector cidrs = {cidr};

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.IterationPreTick(cidrs, /*proc_parser*/ nullptr, /*connections*/ nullptr);
  EXPECT_EQ(ConnectionTracker::State::kDisabled, tracker.state());
}

// Tests that client-side tracing is disabled if no cluster CIDR is specified.
TEST_F(ConnectionTrackerTest, TrackerDisabledForClientSideTracingWithNoCIDR) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  conn.open.traffic_class.role = EndpointRole::kRoleClient;
  testing::SetIPv4RemoteAddr(&conn, "1.2.3.4");

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.IterationPreTick({}, /*proc_parser*/ nullptr, /*connections*/ nullptr);
  EXPECT_EQ(ConnectionTracker::State::kDisabled, tracker.state());
}

// Tests that tracker state is kDisabled if the remote address is Unix domain socket.
TEST_F(ConnectionTrackerTest, TrackerDisabledForUnixDomainSocket) {
  testing::EventGenerator event_gen(&real_clock_);
  struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
  conn.open.traffic_class.role = EndpointRole::kRoleServer;
  conn.open.addr.sin6_family = AF_UNIX;

  CIDRBlock cidr;
  ASSERT_OK(ParseCIDRBlock("1.2.3.4/14", &cidr));
  std::vector cidrs = {cidr};

  ConnectionTracker tracker;
  tracker.AddControlEvent(conn);
  tracker.IterationPreTick(cidrs, /*proc_parser*/ nullptr, /*connections*/ nullptr);
  EXPECT_EQ(ConnectionTracker::State::kDisabled, tracker.state());
}

// Tests that tracker is disabled after mapping the addresses from IPv4 to IPv6.
TEST_F(ConnectionTrackerTest, TrackerDisabledAfterMapping) {
  {
    testing::EventGenerator event_gen(&real_clock_);
    struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
    conn.open.traffic_class.role = EndpointRole::kRoleClient;
    testing::SetIPv6RemoteAddr(&conn, "::ffff:1.2.3.4");

    CIDRBlock cidr;
    ASSERT_OK(ParseCIDRBlock("1.2.3.4/14", &cidr));
    std::vector cidrs = {cidr};

    ConnectionTracker tracker;
    tracker.AddControlEvent(conn);
    tracker.IterationPreTick(cidrs, /*proc_parser*/ nullptr, /*connections*/ nullptr);
    EXPECT_EQ(ConnectionTracker::State::kDisabled, tracker.state());
  }
  {
    testing::EventGenerator event_gen(&real_clock_);
    struct socket_control_event_t conn = event_gen.InitConn<kProtocolHTTP>();
    conn.open.traffic_class.role = EndpointRole::kRoleClient;
    testing::SetIPv4RemoteAddr(&conn, "1.2.3.4");

    CIDRBlock cidr;
    ASSERT_OK(ParseCIDRBlock("::ffff:1.2.3.4/120", &cidr));
    std::vector cidrs = {cidr};

    ConnectionTracker tracker;
    tracker.AddControlEvent(conn);
    tracker.IterationPreTick(cidrs, /*proc_parser*/ nullptr, /*connections*/ nullptr);
    EXPECT_EQ(ConnectionTracker::State::kDisabled, tracker.state());
  }
}

TEST_F(ConnectionTrackerTest, DisabledDueToParsingFailureRate) {
  using mysql::testutils::GenErr;
  using mysql::testutils::GenRawPacket;

  testing::EventGenerator event_gen(&real_clock_);
  auto req_frame0 = event_gen.InitSendEvent<kProtocolMySQL>(
      GenRawPacket(0, "\x44 0x44 is not a valid MySQL command"));
  auto resp_frame0 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame1 = event_gen.InitSendEvent<kProtocolMySQL>(
      GenRawPacket(0, "\x55 0x55 is not a valid MySQL command"));
  auto resp_frame1 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame2 = event_gen.InitSendEvent<kProtocolMySQL>(
      GenRawPacket(0, "\x66 0x66 is not a valid MySQL command"));
  auto resp_frame2 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame3 = event_gen.InitSendEvent<kProtocolMySQL>(
      GenRawPacket(0, "\x77 0x77 is not a valid MySQL command"));
  auto resp_frame3 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame4 = event_gen.InitSendEvent<kProtocolMySQL>(
      GenRawPacket(0, "\x88 0x88 is not a valid MySQL command"));
  auto resp_frame4 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame5 = event_gen.InitSendEvent<kProtocolMySQL>(
      GenRawPacket(0, "\x99 0x99 is not a valid MySQL command"));
  auto resp_frame5 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame6 = event_gen.InitSendEvent<kProtocolMySQL>(
      GenRawPacket(0, "\xaa 0xaa is not a valid MySQL command"));
  auto resp_frame6 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));

  ConnectionTracker tracker;
  std::vector<mysql::Record> records;

  tracker.AddDataEvent(std::move(req_frame0));
  tracker.AddDataEvent(std::move(resp_frame0));

  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kCollecting);
  EXPECT_EQ(records.size(), 0);

  tracker.AddDataEvent(std::move(req_frame1));
  tracker.AddDataEvent(std::move(resp_frame1));
  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kCollecting);
  EXPECT_EQ(records.size(), 0);

  tracker.AddDataEvent(std::move(req_frame2));
  tracker.AddDataEvent(std::move(resp_frame2));
  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kCollecting);
  EXPECT_EQ(records.size(), 0);

  tracker.AddDataEvent(std::move(req_frame3));
  tracker.AddDataEvent(std::move(resp_frame3));
  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kCollecting);
  EXPECT_EQ(records.size(), 0);

  tracker.AddDataEvent(std::move(req_frame4));
  tracker.AddDataEvent(std::move(resp_frame4));
  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kCollecting);
  EXPECT_EQ(records.size(), 0);

  // This request should push the error rate above the brink.
  tracker.AddDataEvent(std::move(req_frame5));
  tracker.AddDataEvent(std::move(resp_frame5));
  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kDisabled);
  EXPECT_EQ(records.size(), 0);
}

TEST_F(ConnectionTrackerTest, DisabledDueToStitchingFailureRate) {
  using mysql::testutils::GenErr;
  using mysql::testutils::GenRawPacket;

  testing::EventGenerator event_gen(&real_clock_);
  auto req_frame0 = event_gen.InitSendEvent<kProtocolMySQL>(GenRawPacket(0, "\x03 A"));
  auto resp_frame0 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame1 = event_gen.InitSendEvent<kProtocolMySQL>(GenRawPacket(0, "\x03 B"));
  auto resp_frame1 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame2 = event_gen.InitSendEvent<kProtocolMySQL>(GenRawPacket(0, "\x03 C"));
  auto resp_frame2 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame3 = event_gen.InitSendEvent<kProtocolMySQL>(GenRawPacket(0, "\x03 D"));
  auto resp_frame3 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame4 = event_gen.InitSendEvent<kProtocolMySQL>(GenRawPacket(0, "\x03 E"));
  auto resp_frame4 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame5 = event_gen.InitSendEvent<kProtocolMySQL>(GenRawPacket(0, "\x03 F"));
  auto resp_frame5 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));
  auto req_frame6 = event_gen.InitSendEvent<kProtocolMySQL>(GenRawPacket(0, "\x03 G"));
  auto resp_frame6 = event_gen.InitRecvEvent<kProtocolMySQL>(GenRawPacket(1, ""));

  ConnectionTracker tracker;
  std::vector<mysql::Record> records;

  tracker.AddDataEvent(std::move(req_frame0));
  tracker.AddDataEvent(std::move(resp_frame0));
  tracker.AddDataEvent(std::move(req_frame1));
  tracker.AddDataEvent(std::move(resp_frame1));
  tracker.AddDataEvent(std::move(req_frame2));
  tracker.AddDataEvent(std::move(resp_frame2));
  tracker.AddDataEvent(std::move(req_frame3));
  tracker.AddDataEvent(std::move(resp_frame3));
  tracker.AddDataEvent(std::move(req_frame4));
  tracker.AddDataEvent(std::move(resp_frame4));

  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kCollecting);
  EXPECT_EQ(records.size(), 0);

  // This request should push the error rate above the brink.
  tracker.AddDataEvent(std::move(req_frame5));
  tracker.AddDataEvent(std::move(resp_frame5));
  records = tracker.ProcessToRecords<mysql::ProtocolTraits>();
  tracker.IterationPostTick();

  EXPECT_EQ(tracker.state(), ConnectionTracker::State::kDisabled);
  EXPECT_EQ(tracker.disable_reason(),
            "Connection does not appear to produce valid records of protocol kProtocolMySQL");
  EXPECT_EQ(records.size(), 0);
}

}  // namespace stirling
}  // namespace pl
