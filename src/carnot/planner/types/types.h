#pragma once

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/strings/substitute.h>

#include "src/common/base/statusor.h"
#include "src/shared/types/magic_enum.h"
#include "src/shared/types/types.h"
#include "src/table_store/schema/relation.h"

namespace pl {
namespace carnot {
namespace planner {

using pl::table_store::schema::Relation;
using pl::types::DataType;
using pl::types::SemanticType;

// TODO(james): implement type hashing for the purpose of type checking.
class BaseType {
 public:
  virtual ~BaseType() {}
  virtual std::shared_ptr<BaseType> Copy() const = 0;
  virtual std::string DebugString() const = 0;
  virtual bool IsValueType() const { return false; }
};

using TypePtr = std::shared_ptr<BaseType>;

class ValueType : public BaseType {
  /**
   * @brief ValueType is the most basic type. It stores the primitive data type and the semantic
   * type.
   */
 public:
  DataType data_type() const { return data_type_; }
  SemanticType semantic_type() const { return semantic_type_; }

  TypePtr Copy() const override { return ValueType::Create(data_type_, semantic_type_); }

  bool operator==(const ValueType& other) const {
    return (data_type_ == other.data_type()) && (semantic_type_ == other.semantic_type());
  }

  std::string DebugString() const override {
    return absl::Substitute("ValueType($0, $1)", magic_enum::enum_name(data_type_),
                            magic_enum::enum_name(semantic_type_));
  }

  static std::shared_ptr<ValueType> Create(DataType data_type, SemanticType semantic_type) {
    return std::shared_ptr<ValueType>(new ValueType(data_type, semantic_type));
  }

  friend std::ostream& operator<<(std::ostream& os, const ValueType& val) {
    os << val.DebugString();
    return os;
  }

  bool IsValueType() const override { return true; }

 protected:
  explicit ValueType(DataType data_type, SemanticType semantic_type)
      : data_type_(data_type), semantic_type_(semantic_type) {}

 private:
  DataType data_type_;
  SemanticType semantic_type_;
};

class TableType : public BaseType {
  /**
   * @brief TableType stores column data types, mapping column names to there type.
   *
   * Currently, all Operators have a TableType and all expressions have a ValueType, but with the
   * data model changes we might want to extend the type system to make tags data there own type
   * structure.
   */
 public:
  static std::shared_ptr<TableType> Create() { return std::shared_ptr<TableType>(new TableType); }

  static std::shared_ptr<TableType> Create(Relation rel) {
    return std::shared_ptr<TableType>(new TableType(rel));
  }

  void AddColumn(std::string col_name, TypePtr type_) {
    map_.insert({col_name, type_});
    ordered_col_names_.push_back(col_name);
  }
  bool HasColumn(std::string col_name) const { return map_.find(col_name) != map_.end(); }
  bool RemoveColumn(std::string col_name) {
    auto col_to_remove = map_.find(col_name);
    if (col_to_remove == map_.end()) {
      return false;
    }
    map_.erase(col_to_remove);
    auto it = std::find(ordered_col_names_.begin(), ordered_col_names_.end(), col_name);
    ordered_col_names_.erase(it);
    return true;
  }

  bool RenameColumn(std::string old_col_name, std::string new_col_name) {
    DCHECK_NE(old_col_name, new_col_name);
    auto it = map_.find(old_col_name);
    if (it == map_.end()) {
      return false;
    }
    map_.insert({new_col_name, it->second});
    map_.erase(old_col_name);
    auto col_name_it =
        std::find(ordered_col_names_.begin(), ordered_col_names_.end(), old_col_name);
    *col_name_it = new_col_name;
    return true;
  }

  StatusOr<TypePtr> GetColumnType(std::string col_name) const {
    auto it = map_.find(col_name);
    if (it == map_.end()) {
      return Status(statuspb::INVALID_ARGUMENT,
                    absl::Substitute("cannot find column $0 in table type container", col_name));
    }
    return it->second;
  }

  TypePtr Copy() const override {
    auto copy = TableType::Create();
    for (const auto& [name, type] : map_) {
      copy->AddColumn(name, type);
    }
    return copy;
  }

  StatusOr<Relation> ToRelation() const {
    Relation r;
    for (const auto& [name, type] : *this) {
      if (!type->IsValueType()) {
        return error::Internal(
            "Can not convert TableType with non-ValueType columns into a relation");
      }
      auto val = std::static_pointer_cast<ValueType>(type);
      r.AddColumn(val->data_type(), name, val->semantic_type());
    }
    return r;
  }

  std::string DebugString() const override {
    std::vector<std::string> col_debug_strings;
    for (const auto& [name, type] : map_) {
      col_debug_strings.push_back(absl::Substitute("$0: $1", name, type->DebugString()));
    }
    return "TableType(" + absl::StrJoin(col_debug_strings, " | ") + ")";
  }

  class TableTypeIterator {
   public:
    using value_type = std::pair<const std::string, std::shared_ptr<BaseType>>;
    using difference_type = std::ptrdiff_t;
    using pointer = const std::pair<const std::string, std::shared_ptr<BaseType>>*;
    using reference = const std::pair<const std::string, std::shared_ptr<BaseType>>&;
    using iterator_category = std::input_iterator_tag;
    using vector_iterator = std::vector<std::string>::const_iterator;

    TableTypeIterator(const std::map<std::string, std::shared_ptr<BaseType>>& table_map,
                      vector_iterator curr)
        : table_map_(table_map), curr_(curr) {}

    TableTypeIterator& operator++() {
      ++curr_;
      return *this;
    }
    bool operator==(const TableTypeIterator& it) { return it.curr_ == curr_; }
    bool operator!=(const TableTypeIterator& it) { return it.curr_ != curr_; }

    reference operator*() const {
      auto it = table_map_.find(*curr_);
      return *it;
    }

    pointer operator->() const {
      auto it = table_map_.find(*curr_);
      return &(*it);
    }

   private:
    const std::map<std::string, std::shared_ptr<BaseType>>& table_map_;
    vector_iterator curr_;
  };
  using const_iterator = TableTypeIterator;
  const_iterator begin() const { return TableTypeIterator(map_, ordered_col_names_.begin()); }
  const_iterator end() const { return TableTypeIterator(map_, ordered_col_names_.end()); }

 protected:
  TableType() {}
  explicit TableType(const Relation& rel) {
    for (size_t i = 0; i < rel.NumColumns(); i++) {
      AddColumn(rel.col_names()[i],
                ValueType::Create(rel.col_types()[i], rel.col_semantic_types()[i]));
    }
  }

 private:
  std::map<std::string, std::shared_ptr<BaseType>> map_;
  std::vector<std::string> ordered_col_names_;
};

}  // namespace planner
}  // namespace carnot
}  // namespace pl
