#include <gtest/gtest.h>

#include "src/shared/schema/utils.h"
#include "src/stirling/proto/stirling.pb.h"

namespace pl {

using stirling::stirlingpb::InfoClass;
using stirling::stirlingpb::Subscribe;

TEST(ConvertSubscribeProtoToRelationInfo, test_for_basic_subscription) {
  // Setup a test subscribe message.
  Subscribe subscribe_pb;
  // First info class with two columns.
  auto* info_class = subscribe_pb.add_subscribed_info_classes();
  info_class->set_name("rel1");
  info_class->set_id(0);
  info_class->set_subscribed(false);

  auto* elem0 = info_class->mutable_schema()->add_elements();
  elem0->set_type(types::INT64);
  elem0->set_name("col1");

  auto* elem1 = info_class->mutable_schema()->add_elements();
  elem1->set_type(types::STRING);
  elem1->set_name("col2");

  // Second relation with one column.
  info_class = subscribe_pb.add_subscribed_info_classes();
  info_class->set_name("rel2");
  info_class->set_id(1);
  info_class->set_subscribed(false);
  elem0 = info_class->mutable_schema()->add_elements();
  elem0->set_type(types::INT64);
  elem0->set_name("col1_2");

  // Do the conversion.
  const auto relation_info = ConvertSubscribePBToRelationInfo(subscribe_pb);

  // Test the results.
  ASSERT_EQ(2, relation_info.size());

  EXPECT_EQ(2, relation_info[0].relation.NumColumns());
  EXPECT_EQ(1, relation_info[1].relation.NumColumns());

  EXPECT_EQ(types::INT64, relation_info[0].relation.GetColumnType(0));
  EXPECT_EQ("col1", relation_info[0].relation.GetColumnName(0));

  EXPECT_EQ(types::STRING, relation_info[0].relation.GetColumnType(1));
  EXPECT_EQ("col2", relation_info[0].relation.GetColumnName(1));

  EXPECT_EQ(types::INT64, relation_info[1].relation.GetColumnType(0));
  EXPECT_EQ("col1_2", relation_info[1].relation.GetColumnName(0));

  EXPECT_EQ(0, relation_info[0].id);
  EXPECT_EQ(1, relation_info[1].id);

  EXPECT_EQ("rel1", relation_info[0].name);
  EXPECT_EQ("rel2", relation_info[1].name);
}

TEST(ConvertSubscribeProtoToRelationInfo, empty_subscribe_should_return_empty) {
  Subscribe subscribe_pb;
  const auto relation_info = ConvertSubscribePBToRelationInfo(subscribe_pb);
  ASSERT_EQ(0, relation_info.size());
}

}  // namespace pl
