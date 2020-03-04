#include "src/stirling/data_stream.h"

#include <utility>

#include "src/common/testing/testing.h"
#include "src/stirling/testing/event_generator.h"

namespace pl {
namespace stirling {

using ::testing::IsEmpty;
using ::testing::SizeIs;

using testing::kHTTPReq0;
using testing::kHTTPReq1;
using testing::kHTTPReq2;

class DataStreamTest : public ::testing::Test {
 protected:
  testing::MockClock real_clock_;
};

TEST_F(DataStreamTest, LostEvent) {
  testing::EventGenerator event_gen(&real_clock_);
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req3 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req4 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req5 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);

  DataStream stream;

  // Start off with no lost events.
  stream.AddData(std::move(req0));
  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_THAT(stream.Frames<http::Message>(), SizeIs(1));
  EXPECT_FALSE(stream.IsStuck());

  // Now add some lost events - should get skipped over.
  PL_UNUSED(req1);  // Lost event.
  stream.AddData(std::move(req2));
  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_THAT(stream.Frames<http::Message>(), SizeIs(2));
  EXPECT_FALSE(stream.IsStuck());

  // Some more requests, and another lost request (this time undetectable).
  stream.AddData(std::move(req3));
  PL_UNUSED(req4);
  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_THAT(stream.Frames<http::Message>(), SizeIs(3));
  EXPECT_FALSE(stream.IsStuck());

  // Now the lost event should be detected.
  stream.AddData(std::move(req5));
  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_THAT(stream.Frames<http::Message>(), SizeIs(4));
  EXPECT_FALSE(stream.IsStuck());
}

TEST_F(DataStreamTest, StuckTemporarily) {
  testing::EventGenerator event_gen(&real_clock_);

  // First request is missing a few bytes from its start.
  std::unique_ptr<SocketDataEvent> req0a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(0, kHTTPReq0.length() - 10));
  std::unique_ptr<SocketDataEvent> req0b =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(kHTTPReq0.length() - 10, 10));
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2);

  DataStream stream;
  stream.AddData(std::move(req0a));

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_THAT(stream.Frames<http::Message>(), IsEmpty());

  // Remaining data arrives in time, so stuck count never gets high enough to flush events.
  stream.AddData(std::move(req0b));
  stream.AddData(std::move(req1));
  stream.AddData(std::move(req2));

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  const auto& requests = stream.Frames<http::Message>();
  ASSERT_THAT(requests, SizeIs(3));
  EXPECT_EQ(requests[0].http_req_path, "/index.html");
  EXPECT_EQ(requests[1].http_req_path, "/foo.html");
  EXPECT_EQ(requests[2].http_req_path, "/bar.html");
}

TEST_F(DataStreamTest, StuckTooLong) {
  testing::EventGenerator event_gen(&real_clock_);

  // First request is missing a few bytes from its start.
  std::unique_ptr<SocketDataEvent> req0a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(0, kHTTPReq0.length() - 10));
  std::unique_ptr<SocketDataEvent> req0b =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(kHTTPReq0.length() - 10, 10));
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2);

  DataStream stream;
  stream.AddData(std::move(req0a));

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_THAT(stream.Frames<http::Message>(), IsEmpty());

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_THAT(stream.Frames<http::Message>(), IsEmpty());

  // Remaining data does not arrive in time, so stuck recovery has already removed req0a.
  // req0b will be noticed as invalid and cleared out as well.
  stream.AddData(std::move(req0b));
  stream.AddData(std::move(req1));
  stream.AddData(std::move(req2));

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  const auto& requests = stream.Frames<http::Message>();
  ASSERT_THAT(requests, SizeIs(2));
  EXPECT_EQ(requests[0].http_req_path, "/foo.html");
  EXPECT_EQ(requests[1].http_req_path, "/bar.html");
}

TEST_F(DataStreamTest, PartialMessageRecovery) {
  testing::EventGenerator event_gen(&real_clock_);
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req1a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1.substr(0, kHTTPReq1.length() / 2));
  std::unique_ptr<SocketDataEvent> req1b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq1.substr(kHTTPReq1.length() / 2, kHTTPReq1.length()));
  std::unique_ptr<SocketDataEvent> req2 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2);

  DataStream stream;
  stream.AddData(std::move(req0));
  stream.AddData(std::move(req1a));
  PL_UNUSED(req1b);  // Missing event.
  stream.AddData(std::move(req2));

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  const auto& requests = stream.Frames<http::Message>();
  ASSERT_THAT(requests, SizeIs(2));
  EXPECT_EQ(requests[0].http_req_path, "/index.html");
  EXPECT_EQ(requests[1].http_req_path, "/bar.html");
}

TEST_F(DataStreamTest, HeadAndMiddleMissing) {
  testing::EventGenerator event_gen(&real_clock_);
  std::unique_ptr<SocketDataEvent> req0b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq0.substr(kHTTPReq0.length() / 2, kHTTPReq0.length()));
  std::unique_ptr<SocketDataEvent> req1a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1.substr(0, kHTTPReq1.length() / 2));
  std::unique_ptr<SocketDataEvent> req1b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq1.substr(kHTTPReq1.length() / 2, kHTTPReq1.length()));
  std::unique_ptr<SocketDataEvent> req2a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2.substr(0, kHTTPReq2.length() / 2));
  std::unique_ptr<SocketDataEvent> req2b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq2.substr(kHTTPReq2.length() / 2, kHTTPReq2.length()));

  DataStream stream;
  stream.AddData(std::move(req0b));
  stream.AddData(std::move(req1a));
  PL_UNUSED(req1b);  // Missing event.
  stream.AddData(std::move(req2a));
  stream.AddData(std::move(req2b));

  // The presence of a missing event should trigger the stream to make forward progress.

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  const auto& requests = stream.Frames<http::Message>();
  ASSERT_THAT(requests, SizeIs(1));
  EXPECT_EQ(requests[0].http_req_path, "/bar.html");
}

TEST_F(DataStreamTest, LateArrivalPlusMissingEvents) {
  testing::EventGenerator event_gen(&real_clock_);
  std::unique_ptr<SocketDataEvent> req0a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(0, kHTTPReq0.length() / 2));
  std::unique_ptr<SocketDataEvent> req0b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq0.substr(kHTTPReq0.length() / 2, kHTTPReq0.length()));
  std::unique_ptr<SocketDataEvent> req1a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1.substr(0, kHTTPReq1.length() / 2));
  std::unique_ptr<SocketDataEvent> req1b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq1.substr(kHTTPReq1.length() / 2, kHTTPReq1.length()));
  std::unique_ptr<SocketDataEvent> req2a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq2.substr(0, kHTTPReq2.length() / 2));
  std::unique_ptr<SocketDataEvent> req2b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq2.substr(kHTTPReq2.length() / 2, kHTTPReq2.length()));
  std::unique_ptr<SocketDataEvent> req3a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0.substr(0, kHTTPReq0.length() / 2));
  std::unique_ptr<SocketDataEvent> req3b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq0.substr(kHTTPReq0.length() / 2, kHTTPReq0.length()));
  std::unique_ptr<SocketDataEvent> req4a =
      event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1.substr(0, kHTTPReq1.length() / 2));
  std::unique_ptr<SocketDataEvent> req4b = event_gen.InitSendEvent<kProtocolHTTP>(
      kHTTPReq1.substr(kHTTPReq1.length() / 2, kHTTPReq1.length()));

  DataStream stream;
  stream.AddData(std::move(req0a));
  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  ASSERT_THAT(stream.Frames<http::Message>(), IsEmpty());

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  ASSERT_THAT(stream.Frames<http::Message>(), IsEmpty());

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  ASSERT_THAT(stream.Frames<http::Message>(), IsEmpty());

  stream.AddData(std::move(req0b));
  stream.AddData(std::move(req1a));
  stream.AddData(std::move(req1b));
  PL_UNUSED(req2a);  // Missing event.
  PL_UNUSED(req2b);  // Missing event.
  stream.AddData(std::move(req3a));
  stream.AddData(std::move(req3b));
  stream.AddData(std::move(req4a));
  stream.AddData(std::move(req4b));

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  const auto& requests = stream.Frames<http::Message>();
  ASSERT_THAT(requests, SizeIs(3));
  EXPECT_EQ(requests[0].http_req_path, "/foo.html");
  EXPECT_EQ(requests[1].http_req_path, "/index.html");
  EXPECT_EQ(requests[2].http_req_path, "/foo.html");
}

// This test checks that various stats updated on each call ProcessBytesToFrames()
// are updated correctly.
TEST_F(DataStreamTest, Stats) {
  testing::EventGenerator event_gen(&real_clock_);
  std::unique_ptr<SocketDataEvent> req0 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req1 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> req2bad =
      event_gen.InitSendEvent<kProtocolHTTP>("This is not a valid HTTP message");
  std::unique_ptr<SocketDataEvent> req3 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq0);
  std::unique_ptr<SocketDataEvent> req4 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> req5 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);
  std::unique_ptr<SocketDataEvent> req6bad =
      event_gen.InitSendEvent<kProtocolHTTP>("Another malformed message");
  std::unique_ptr<SocketDataEvent> req7 = event_gen.InitSendEvent<kProtocolHTTP>(kHTTPReq1);

  DataStream stream;
  stream.AddData(std::move(req0));
  stream.AddData(std::move(req1));
  stream.AddData(std::move(req2bad));

  EXPECT_EQ(stream.stat_raw_data_gaps(), 0);
  EXPECT_EQ(stream.stat_invalid_frames(), 0);
  EXPECT_EQ(stream.stat_valid_frames(), 0);

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_EQ(stream.Frames<http::Message>().size(), 2);
  EXPECT_EQ(stream.stat_raw_data_gaps(), 0);
  EXPECT_EQ(stream.stat_invalid_frames(), 1);
  EXPECT_EQ(stream.stat_valid_frames(), 2);

  stream.AddData(std::move(req3));
  PL_UNUSED(req4);  // Skip req4 as missing event.
  stream.AddData(std::move(req5));
  stream.AddData(std::move(req6bad));
  stream.AddData(std::move(req7));

  // Note that we don't expect req7 to be parsed, because an invalid frame means
  // all subsequent data is purged.

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);
  EXPECT_EQ(stream.Frames<http::Message>().size(), 4);
  EXPECT_EQ(stream.stat_raw_data_gaps(), 1);
  EXPECT_EQ(stream.stat_invalid_frames(), 2);
  EXPECT_EQ(stream.stat_valid_frames(), 4);
}

TEST_F(DataStreamTest, CannotSwitchType) {
  DataStream stream;

  stream.ProcessBytesToFrames<http::Message>(MessageType::kRequest);

#if DCHECK_IS_ON()
  EXPECT_DEATH(stream.ProcessBytesToFrames<http2::Frame>(MessageType::kRequest),
               "ConnectionTracker cannot change the type it holds during runtime");
#else
  EXPECT_THROW(stream.ProcessBytesToFrames<http2::Frame>(MessageType::kRequest), std::exception);
#endif
}

}  // namespace stirling
}  // namespace pl
