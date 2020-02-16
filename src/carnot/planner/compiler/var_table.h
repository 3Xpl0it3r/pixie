#pragma once
#include <memory>
#include <string>

#include <absl/container/flat_hash_map.h>
#include "src/carnot/planner/objects/qlobject.h"

namespace pl {
namespace carnot {
namespace compiler {
/**
 * @brief VarTable contains variables that are generated during processing.
 *
 */
// enable_shared_from_this allows you to make shared_ptrs of this object safely.
class VarTable : public std::enable_shared_from_this<VarTable> {
 public:
  /**
   * @brief Creates a vartable with no parent scope.
   *
   * @return std::shared_ptr<VarTable>
   */
  static std::shared_ptr<VarTable> Create();

  /**
   * @brief Creates a VarTable with the given parent_scope.
   *
   * @param parent_scope the parent scope of this new VarTable.
   * @return std::shared_ptr<VarTable>
   */
  static std::shared_ptr<VarTable> Create(std::shared_ptr<VarTable> parent_scope);

  /**
   * @brief Searches for the QLObject corresponding to the name. If it's not found in the current
   * context, we search the parent context. If it's not in the parent context, we return a nullptr.
   *
   * @param name
   * @return QLObjectPtr
   */
  QLObjectPtr Lookup(std::string_view name);
  /**
   * @brief Returns whether or not this VarTable has the name.
   *
   * @param name
   * @return true
   * @return false
   */
  bool HasVariable(std::string_view name);

  /**
   * @brief Adds a variable to this table. It adds to the current scope.
   *
   * @param name
   * @param ql_object
   */
  void Add(std::string_view name, QLObjectPtr ql_object);

  /**
   * @brief Create a Child scope of this VarTable. This enables lookups to search this table in the
   * case that the child table doesn't contain the results.
   *
   * @return std::shared_ptr<VarTable> The child Vartable.
   */
  std::shared_ptr<VarTable> CreateChild();

  std::shared_ptr<VarTable> parent_scope() { return parent_scope_; }

 protected:
  explicit VarTable(std::shared_ptr<VarTable> parent_scope) : parent_scope_(parent_scope) {}

  VarTable() : parent_scope_(nullptr) {}

 private:
  // The parent of this var table. If not set, then doesn't exist.
  std::shared_ptr<VarTable> parent_scope_;
  // The table that contains all of the scopes.
  absl::flat_hash_map<std::string, QLObjectPtr> scope_table_;
};

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
