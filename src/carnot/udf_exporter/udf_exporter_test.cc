#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>

#include <tuple>
#include <unordered_map>
#include <vector>

#include <pypa/parser/parser.hh>

#include "src/carnot/udf_exporter/udf_exporter.h"
#include "src/common/testing/testing.h"
#include "src/shared/types/proto/types.pb.h"

namespace pl {
namespace carnot {
namespace udfexporter {
using ::testing::_;

class UDFExporterTest : public ::testing::Test {};

TEST_F(UDFExporterTest, udf_proto) {
  // Sanity checks in lieu of full tests.
  auto registry_info = ExportUDFInfo().ConsumeValueOrDie();

  // Contains a math op.
  auto udf_status = registry_info->GetUDF("add", {types::FLOAT64, types::FLOAT64});
  EXPECT_OK(udf_status);
  EXPECT_TRUE(udf_status.ConsumeValueOrDie() == types::FLOAT64);

  // Contains a string op.
  udf_status = registry_info->GetUDF("pluck", {types::STRING, types::STRING});
  EXPECT_OK(udf_status);
  EXPECT_TRUE(udf_status.ConsumeValueOrDie() == types::STRING);

  // Contains metadata ops.
  udf_status = registry_info->GetUDF("upid_to_pod_id", {types::UINT128});
  EXPECT_OK(udf_status);
  EXPECT_TRUE(udf_status.ConsumeValueOrDie() == types::STRING);

  // Contains aggregate ops.
  auto uda_status = registry_info->GetUDA("count", {types::BOOLEAN});
  EXPECT_OK(uda_status);
  EXPECT_TRUE(uda_status.ConsumeValueOrDie() == types::INT64);
}

}  // namespace udfexporter
}  // namespace carnot
}  // namespace pl
