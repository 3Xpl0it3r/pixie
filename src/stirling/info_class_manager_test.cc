#include <gtest/gtest.h>

#include "src/stirling/info_class_manager.h"
#include "src/stirling/seq_gen_connector.h"

namespace pl {
namespace stirling {

using types::DataType;
using types::PatternType;

TEST(DataElementTest, infoclass_element_proto_getters_test) {
  DataElement element("user_percentage", DataType::FLOAT64, PatternType::METRIC_GAUGE);

  EXPECT_EQ("user_percentage", std::string(element.name().data()));
  EXPECT_EQ(DataType::FLOAT64, element.type());

  stirlingpb::Element element_pb;
  element_pb = element.ToProto();
  EXPECT_EQ("user_percentage", element_pb.name());
  EXPECT_EQ(DataType::FLOAT64, element_pb.type());
}

TEST(InfoClassInfoSchemaTest, infoclass_mgr_proto_getters_test) {
  InfoClassManager info_class_mgr(SeqGenConnector::kSeq0Table);
  auto source = SeqGenConnector::Create("sequences");
  info_class_mgr.SetSourceConnector(source.get(), SeqGenConnector::kSeq0TableNum);

  EXPECT_EQ(SeqGenConnector::kSeq0Table.elements().size(),
            info_class_mgr.Schema().elements().size());
  EXPECT_EQ(SeqGenConnector::kSeq0Table.name(), info_class_mgr.name());

  stirlingpb::InfoClass info_class_pb;
  info_class_pb = info_class_mgr.ToProto();
  EXPECT_EQ(SeqGenConnector::kSeq0Table.elements().size(), info_class_pb.elements_size());
  EXPECT_EQ(SeqGenConnector::kSeq0Table.name(), info_class_pb.name());
  EXPECT_EQ(0, info_class_pb.id());

  info_class_mgr.SetSubscription(true);
  stirlingpb::InfoClass subscribe_pb;
  subscribe_pb = info_class_mgr.ToProto();
  EXPECT_TRUE(subscribe_pb.subscribed());
}

}  // namespace stirling
}  // namespace pl
