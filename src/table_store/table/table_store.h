#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "absl/container/flat_hash_map.h"

#include "src/common/base/base.h"
#include "src/shared/types/column_wrapper.h"
#include "src/shared/types/hash_utils.h"
#include "src/table_store/schema/relation.h"
#include "src/table_store/table/table.h"
#include "src/table_store/table/tablets_group.h"

namespace pl {
namespace table_store {
using schema::Relation;

// NameTablet used as a key containing a Table name and Tablet ID.
struct NameTablet {
  bool operator==(const NameTablet& other) const {
    return name_ == other.name_ && tablet_id_ == other.tablet_id_;
  }

  template <typename H>
  friend H AbslHashValue(H h, const NameTablet& p) {
    return H::combine(std::move(h), p.name_, p.tablet_id_);
  }

  std::string name_;
  types::TabletID tablet_id_;
};

// TableIDTablet used as a key containing a Table ID and Tablet ID.
struct TableIDTablet {
  bool operator==(const TableIDTablet& other) const {
    return table_id_ == other.table_id_ && tablet_id_ == other.tablet_id_;
  }

  template <typename H>
  friend H AbslHashValue(H h, const TableIDTablet& p) {
    return H::combine(std::move(h), p.table_id_, p.tablet_id_);
  }

  uint64_t table_id_;
  types::TabletID tablet_id_;
};

// TableNameAndRelation contains a string name and a relation for a table.
struct NameRelationPair {
  std::string table_name;
  Relation relation;
};

/**
 * TableStore keeps track of the tables in our system.
 */
class TableStore {
 public:
  using RelationMap = std::unordered_map<std::string, table_store::schema::Relation>;

  TableStore() = default;

  /*
   * Gets the table associated with the given name, grabbing the default tablet.
   *
   * @ param table_name the name of the table to get
   * @ returns the associated table
   */
  table_store::Table* GetTable(const std::string& table_name,
                               const types::TabletID& tablet_id = kDefaultTablet);

  /**
   * @brief Get the Table according to table_id.
   *
   * @param table_id: the table_id to query.
   * @param tablet_id: the tablet_id to query upon.
   * @return table_store::Table*: the tablet associated with the table.
   */
  table_store::Table* GetTable(uint64_t table_id,
                               const types::TabletID& tablet_id = kDefaultTablet);

  /*
   * Add a table under the given name.
   *
   * This stores the data under the default tablet id.
   *
   * @ param table_name: the name of the table to add this under.
   * @ param table: the table to store.
   */
  void AddTable(const std::string& table_name, std::shared_ptr<table_store::Table> table);

  /*
   * Add a table under the given name and optionally tablet id.
   *
   * @ param table_name: the name of the table to add this under.
   * @ param table: the table to store.
   * @ param tablet_id: the optional id of the tablet to assign this to.
   */
  void AddTable(const std::string& table_name, const types::TabletID& tablet_id,
                std::shared_ptr<table_store::Table> table);

  /*
   * Add a table under the given name, with an assigned ID.
   *
   * @ param table_id: the unique ID of the table.
   * @ param table_name: the name of the table to add to.
   * @ param table: the table to store.
   */
  Status AddTable(uint64_t table_id, const std::string& table_name,
                  std::shared_ptr<table_store::Table> table);

  /*
   * Add a table under the given name, with an assigned ID.
   *
   * @ param table_id: the unique ID of the table.
   * @ param table_name: the name of the table to add to.
   * @ param tablet_id: the optional id of the tablet to assign this to.
   * @ param table: the table to store.
   */
  Status AddTable(uint64_t table_id, const std::string& table_name,
                  const types::TabletID& tablet_id, std::shared_ptr<table_store::Table> table);

  /**
   * @return A map of table name to relation representing the table's structure.
   */
  std::unique_ptr<RelationMap> GetRelationMap();

  /**
   * @brief Appends the record_batch to the sepcified table and tablet_id. If the table exists but
   * the tablet does not, then the method creates a new container for the tablet.
   * If the table doesn't exist, then the method errors out.
   *
   * @param table_id: the id of the table to append to.
   * @param tablet_id: the tablet within the table to append to.
   * @param record_batch: the data to append.
   * @return Status: error if anything goes wrong during the process.
   */
  Status AppendData(uint64_t table_id, types::TabletID tablet_id,
                    std::unique_ptr<pl::types::ColumnWrapperRecordBatch> record_batch);

  Status SchemaAsProto(schemapb::Schema* schema) const;

 private:
  /**
   * @brief Map the table_name to the relation passed in.
   *
   * @param table_name: the name of the table to set.
   * @param relation: the relation to set for the table_name.
   */
  void AddRelation(const std::string& table_name, const Relation& relation);

  /**
   * @brief Map the table_id to the table_name and relation. It's important to note that this does
   * not map the table_name to the relation. Use the other overloaded method to do so.
   *
   * @param table_id: the  table_id to set the relation and table_naem.
   * @param table_name: the name of the table to set for the table_id.
   * @param relation: the relation to set for the table_id.
   */
  void AddRelation(uint64_t table_id, const std::string& table_name, const Relation& relation);

  /**
   * @brief Create a New Tablet inside of the table with table_id
   *
   * @param table_id: table_id within which to create a tablet.
   * @param tablet_id: the tablet to create for the tablet.
   * @return StatusOr<Table*>: the table object or an error if the table is nonexistant.
   */
  StatusOr<Table*> CreateNewTable(uint64_t table_id, const types::TabletID& tablet_id);

  // The default value for tablets, when tablet is not specified.
  inline static types::TabletID kDefaultTablet = "";
  // Map a name to a table.
  absl::flat_hash_map<NameTablet, std::shared_ptr<Table>> name_to_table_map_;
  // Map an id to a table.
  absl::flat_hash_map<TableIDTablet, std::shared_ptr<Table>> id_to_table_map_;
  // Mapping from name to relation for adding new tablets.
  absl::flat_hash_map<std::string, Relation> name_to_relation_map_;
  // Mapping from id to name and relation pair for adding new tablets.
  absl::flat_hash_map<uint64_t, NameRelationPair> id_to_name_relation_pair_map_;
};

}  // namespace table_store
}  // namespace pl
