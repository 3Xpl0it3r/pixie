#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "src/carnot/compiler/registry_info.h"
#include "src/common/base/base.h"
#include "src/shared/types/types.h"
#include "src/table_store/table_store.h"

namespace pl {
namespace carnot {
namespace compiler {

using RelationMap = std::unordered_map<std::string, table_store::schema::Relation>;
class CompilerState : public NotCopyable {
 public:
  /**
   * CompilerState manages the state needed to compile a single query. A new one will
   * be constructed for every query compiled in Carnot and it will not be reused.
   */
  explicit CompilerState(std::unique_ptr<RelationMap> relation_map,
                         compiler::RegistryInfo* registry_info, types::Time64NSValue time_now)
      : relation_map_(std::move(relation_map)),
        registry_info_(registry_info),
        time_now_(time_now) {}

  CompilerState() = delete;

  RelationMap* relation_map() const { return relation_map_.get(); }
  compiler::RegistryInfo* registry_info() const { return registry_info_; }
  types::Time64NSValue time_now() const { return time_now_; }

  std::map<RegistryKey, int64_t> udf_to_id_map() const { return udf_to_id_map_; }
  std::map<RegistryKey, int64_t> uda_to_id_map() const { return uda_to_id_map_; }

  int64_t GetUDFID(const RegistryKey& key) {
    auto id = udf_to_id_map_.find(key);
    if (id != udf_to_id_map_.end()) {
      return id->second;
    }
    auto new_id = udf_to_id_map_.size();
    udf_to_id_map_[key] = new_id;
    return new_id;
  }

  int64_t GetUDAID(const RegistryKey& key) {
    auto id = uda_to_id_map_.find(key);
    if (id != uda_to_id_map_.end()) {
      return id->second;
    }
    auto new_id = uda_to_id_map_.size();
    uda_to_id_map_[key] = new_id;
    return new_id;
  }

 private:
  std::unique_ptr<RelationMap> relation_map_;
  compiler::RegistryInfo* registry_info_;
  types::Time64NSValue time_now_;
  // TODO(michelle): Update this map to handle init args, once we add init args to the compiler.
  std::map<RegistryKey, int64_t> udf_to_id_map_;
  std::map<RegistryKey, int64_t> uda_to_id_map_;
};

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
