#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "src/carnot/compiler/metadata_handler.h"
#include "src/common/testing/testing.h"
#include "src/shared/types/proto/types.pb.h"

namespace pl {
namespace carnot {
namespace compiler {
TEST(MetadataPropertyTests, retrieval) {
  auto md_handle = MetadataHandler::Create();
  EXPECT_TRUE(md_handle->HasProperty("service_name"));
  EXPECT_TRUE(md_handle->HasProperty("pod_name"));
  EXPECT_TRUE(md_handle->HasProperty("pid"));
  EXPECT_OK(md_handle->GetProperty("pod_name"));
  EXPECT_OK(md_handle->GetProperty("service_name"));
  EXPECT_OK(md_handle->GetProperty("pid"));
}

TEST(MetadataPropertyTests, types) {
  auto md_handle = MetadataHandler::Create();

  auto id_prop = md_handle->GetProperty("container_id").ConsumeValueOrDie();
  EXPECT_EQ(types::DataType::STRING, id_prop->column_type());

  auto name_prop = md_handle->GetProperty("pod_name").ConsumeValueOrDie();
  EXPECT_EQ(types::DataType::STRING, name_prop->column_type());

  auto int64_prop = md_handle->GetProperty("pid").ConsumeValueOrDie();
  EXPECT_EQ(types::DataType::INT64, int64_prop->column_type());
}

class MetadataHandlerTests : public ::testing::Test {
 protected:
  void SetUp() { md_handler = MetadataHandler::Create(); }
  std::unique_ptr<MetadataHandler> md_handler;
};

class MetadataGetPropertyTests : public MetadataHandlerTests,
                                 public ::testing::WithParamInterface<std::string> {};

TEST_P(MetadataGetPropertyTests, has_property) {
  std::string property_name = GetParam();
  EXPECT_TRUE(md_handler->HasProperty(property_name));
  auto property_status = md_handler->GetProperty(property_name);
  EXPECT_OK(property_status);
}

std::vector<std::string> metadata_strs = {"service_name", "service_id",   "pod_name",
                                          "pod_id",       "container_id", "deployment_id"};

INSTANTIATE_TEST_SUITE_P(GetPropertyTestSuites, MetadataGetPropertyTests,
                         ::testing::ValuesIn(metadata_strs));

class MetadataAliasPropertyTests
    : public MetadataHandlerTests,
      public ::testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(MetadataAliasPropertyTests, has_property) {
  std::string alias;
  std::string property_name;
  std::tie(alias, property_name) = GetParam();
  EXPECT_TRUE(md_handler->HasProperty(property_name));
  auto property_status = md_handler->GetProperty(property_name);
  EXPECT_OK(property_status);
  EXPECT_TRUE(md_handler->HasProperty(alias));
  auto alias_status = md_handler->GetProperty(alias);
  EXPECT_OK(alias_status);
  EXPECT_EQ(alias_status.ValueOrDie(), property_status.ValueOrDie());
}
std::vector<std::tuple<std::string, std::string>> alias_to_original = {
    {"service", "service_name"}, {"pod", "pod_name"}, {"deployment", "deployment_name"}};

INSTANTIATE_TEST_SUITE_P(AliasPropertyTestSuites, MetadataAliasPropertyTests,
                         ::testing::ValuesIn(alias_to_original));

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
