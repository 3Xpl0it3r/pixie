#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>
#include <vector>

#include "src/table_store/schema/relation.h"
#include "src/table_store/schema/row_descriptor.h"
#include "src/table_store/table/table_store.h"

namespace pl {
namespace table_store {

using table_store::Column;
using table_store::Table;
using table_store::schema::RowDescriptor;
using types::ColumnWrapperRecordBatch;

class TabletsGroupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rel1 = schema::Relation({types::DataType::BOOLEAN, types::DataType::FLOAT64},
                            {"table1col1", "table1col2"});
    rel2 =
        schema::Relation({types::DataType::INT64, types::DataType::FLOAT64, types::DataType::INT64},
                         {"table2col1", "table2col2", "table2col3"});
    tablet1 = std::make_shared<Table>(rel1);
    tablet2 = std::make_shared<Table>(rel2);
  }

  std::shared_ptr<Table> tablet1;
  std::shared_ptr<Table> tablet2;
  schema::Relation rel1;
  schema::Relation rel2;
};

TEST_F(TabletsGroupTest, TabletIdNotFoundMissing) {
  types::TabletID tablet_id1 = "123";
  types::TabletID tablet_id2 = "456";

  auto table = TabletsGroup(rel1);
  // Only add tablet_id1.
  table.AddTablet(tablet_id1, tablet1);

  EXPECT_NE(table.GetTablet(tablet_id1), nullptr);
  EXPECT_EQ(table.GetTablet(tablet_id2), nullptr);
}

}  // namespace table_store
}  // namespace pl
