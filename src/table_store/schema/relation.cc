#include <algorithm>
#include <string>
#include <utility>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/str_join.h>

#include "src/common/base/base.h"
#include "src/shared/types/type_utils.h"
#include "src/table_store/schema/relation.h"

namespace pl {
namespace table_store {
namespace schema {

using std::string;

Relation::Relation() = default;

Relation::Relation(ColTypeArray col_types, ColNameArray col_names)
    : col_types_(std::move(col_types)), col_names_(std::move(col_names)) {
  CHECK(col_types_.size() == col_names_.size()) << "Initialized with mismatched col names/sizes";
  absl::flat_hash_set<std::string> unique_names;
  for (const auto& col_name : col_names_) {
    DCHECK(!unique_names.contains(col_name))
        << absl::Substitute("Duplicate column name '$0' in relation", col_name);
    unique_names.insert(col_name);
  }
}

size_t Relation::NumColumns() const { return col_types_.size(); }

void Relation::AddColumn(const types::DataType& col_type, const std::string& col_name) {
  DCHECK(std::find(col_names_.begin(), col_names_.end(), col_name) == col_names_.end())
      << absl::Substitute("Column '$0' already exists", col_name);
  col_types_.push_back(col_type);
  col_names_.push_back(col_name);
}

bool Relation::HasColumn(size_t idx) const { return idx < col_types_.size(); }
int64_t Relation::GetColumnIndex(const std::string& col_name) const {
  auto it = std::find(col_names_.begin(), col_names_.end(), col_name);
  if (it == col_names_.end()) {
    return -1;
  }
  auto col_idx = std::distance(col_names_.begin(), it);
  return col_idx;
}

bool Relation::HasColumn(const std::string& col_name) const {
  return HasColumn(GetColumnIndex(col_name));
}

types::DataType Relation::GetColumnType(size_t idx) const {
  CHECK(HasColumn(idx)) << "Column does not exist";
  return col_types_[idx];
}

types::DataType Relation::GetColumnType(const std::string& col_name) const {
  return GetColumnType(GetColumnIndex(col_name));
}
std::string Relation::GetColumnName(size_t idx) const {
  CHECK(HasColumn(idx)) << absl::Substitute("Column $0 does not exist. Only $1 columns available.",
                                            idx, NumColumns());
  return col_names_[idx];
}

std::string Relation::DebugString() const {
  CHECK(col_types_.size() == col_names_.size()) << "Mismatched col names/sizes";
  std::vector<string> col_info_as_str;
  for (size_t i = 0; i < col_types_.size(); ++i) {
    col_info_as_str.push_back(absl::StrCat(col_names_[i], ":", types::ToString(col_types_[i])));
  }
  return "[" + absl::StrJoin(col_info_as_str, ", ") + "]";
}
StatusOr<Relation> Relation::MakeSubRelation(const std::vector<std::string>& columns) const {
  Relation new_relation;
  std::vector<std::string> missing_columns;
  for (auto& c : columns) {
    if (!HasColumn(c)) {
      missing_columns.push_back(c);
      continue;
    }
    auto col_type = GetColumnType(c);
    new_relation.AddColumn(col_type, c);
  }
  if (missing_columns.size() > 0) {
    return error::InvalidArgument("Columns {$0} are missing in table.",
                                  absl::StrJoin(missing_columns, ","));
  }
  return new_relation;
}
Status Relation::ToProto(table_store::schemapb::Relation* relation_proto) const {
  CHECK(relation_proto != nullptr);
  size_t num_columns = NumColumns();
  for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
    auto col_pb = relation_proto->add_columns();
    col_pb->set_column_type(GetColumnType(col_idx));
    col_pb->set_column_name(GetColumnName(col_idx));
  }
  return Status::OK();
}
Status Relation::FromProto(const table_store::schemapb::Relation* relation_pb) {
  if (NumColumns() != 0) {
    return error::AlreadyExists("Relation already has $0 columns. Can't init from proto.",
                                NumColumns());
  }
  for (int idx = 0; idx < relation_pb->columns_size(); ++idx) {
    auto column = relation_pb->columns(idx);
    AddColumn(column.column_type(), column.column_name());
  }
  return Status::OK();
}

}  // namespace schema
}  // namespace table_store
}  // namespace pl
