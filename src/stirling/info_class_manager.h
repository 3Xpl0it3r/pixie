#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/common/base/base.h"
#include "src/shared/types/proto/types.pb.h"
#include "src/shared/types/type_utils.h"
#include "src/stirling/proto/stirling.pb.h"
#include "src/stirling/types.h"

namespace pl {
namespace stirling {

class ConnectorContext;
class SourceConnector;
class DataTable;

/**
 * InfoClassManager is the unit responsible for managing a data source, and its data transfers.
 *
 * InfoClassManager samples the data from the source, transferring the data to an internal table.
 * It also initiates data transfers to the TableStore.
 *
 * The InfoClassManager also serves as the State Manager for the entire data collector.
 *  - The Config unit uses the Schemas to publish available data to the Agent.
 *  - The Config unit changes the state of elements based on the Publish call from the Agent.
 *  - There is a 1:1 relationship with the Data Tables.
 *  - Each InfoClassManager points back to its SourceConnector.
 */
class InfoClassManager {
 public:
  InfoClassManager() = delete;
  /**
   * @brief Construct a new Info Class Manager object
   * SourceConnector constructs InfoClassManager objects with and adds Elements to it
   *
   * @param name Name of the InfoClass
   * @param source Pointer to the SourceConnector that created the InfoClassManager object.
   * This is required to identify an InfoClassManager parent source and also to generate
   * the publish proto.
   */
  explicit InfoClassManager(const DataTableSchema& schema) : schema_(schema) { id_ = global_id_++; }
  virtual ~InfoClassManager() = default;

  /**
   * @brief Source connector connected to this Info Class.
   *
   * @param source Pointer to source connector instance.
   */
  void SetSourceConnector(SourceConnector* source, uint32_t table_num) {
    source_ = source;
    source_table_num_ = table_num;
  }

  /**
   * @brief Data table connected to this Info Class.
   *
   * @param Pointer to data table instance.
   */
  void SetDataTable(DataTable* data_table) { data_table_ = data_table; }

  /**
   * @brief Get the schema of the InfoClass.
   *
   * @return DataTableSchema schema
   */
  const DataTableSchema& Schema() const { return schema_; }

  /**
   * @brief Get an Element object
   *
   * @param index
   * @return DataElement
   */
  const DataElement& GetElement(size_t index) const {
    DCHECK(index < schema_.elements().size());
    return schema_.elements()[index];
  }

  /**
   * @brief Generate a proto message based on the InfoClassManager.
   *
   * @return stirlingpb::InfoClass
   */
  stirlingpb::InfoClass ToProto() const;

  /**
   * @brief Configure sampling period.
   *
   * @param period Sampling period in ms.
   */
  void SetSamplingPeriod(std::chrono::milliseconds period) { sampling_period_ = period; }

  /**
   * @brief Configure sampling period.
   *
   * @param period Sampling period in ms.
   */
  void SetPushPeriod(std::chrono::milliseconds period) { push_period_ = period; }

  /**
   * @brief Returns true if sampling is required, for whatever reason (elapsed time, etc.).
   *
   * @return bool
   */
  bool SamplingRequired() const;

  /**
   * @brief Returns true if a data push is required, for whatever reason (elapsed time, occupancy,
   * etc.).
   *
   * @return bool
   */
  bool PushRequired() const;

  /**
   * @brief Samples the data from the Source and copies into local buffers.
   */
  void SampleData(ConnectorContext* ctx);

  /**
   * @brief Push data by using the callback.
   */
  void PushData(PushDataCallback agent_callback);

  /**
   * @brief Notify function to update state after making changes to the schema.
   * This will make sure changes are pushed to the Source Connector and Data Tables accordingly.
   */
  void Notify() {}

  /**
   * @brief Returns the next time the source needs to be sampled, according to the sampling period.
   *
   * @return std::chrono::milliseconds
   */
  std::chrono::steady_clock::time_point NextSamplingTime() const {
    return last_sampled_ + sampling_period_;
  }

  /**
   * @brief Returns the next time the data table needs to be pushed upstream, according to the push
   * period.
   *
   * @return std::chrono::milliseconds
   */
  std::chrono::steady_clock::time_point NextPushTime() const { return last_pushed_ + push_period_; }

  /**
   * @brief Convenience function to return current time in Milliseconds.
   *
   * @return milliseconds
   */
  static std::chrono::steady_clock::time_point CurrentTime() {
    return std::chrono::steady_clock::now();
  }

  /**
   * @brief Set the Subscription for the InfoClass.
   *
   * @param subscription
   */
  void SetSubscription(bool subscribed) { subscribed_ = subscribed; }

  std::string_view name() const { return schema_.name(); }
  const SourceConnector* source() const { return source_; }
  uint64_t id() { return id_; }
  bool subscribed() const { return subscribed_; }

 private:
  inline static std::atomic<uint64_t> global_id_ = 0;

  /**
   * Unique ID of the InfoClassManager instance. ID must never repeat, even after destruction.
   */
  uint64_t id_;

  /**
   * The schema of table associated with this Info Class manager.
   */
  const DataTableSchema& schema_;

  /**
   * Boolean indicating whether an agent has subscribed to the Info Class.
   */
  bool subscribed_ = false;

  /**
   * Pointer back to the source connector providing the data.
   */
  SourceConnector* source_ = nullptr;

  /**
   * Table number within source connector for this info class.
   */
  uint32_t source_table_num_;

  /**
   * Pointer to the data table where the data is stored.
   */
  DataTable* data_table_ = nullptr;

  /**
   * Sampling period.
   */
  std::chrono::milliseconds sampling_period_{kDefaultSamplingPeriod};

  /**
   * Keep track of when the source was last sampled.
   */
  std::chrono::steady_clock::time_point last_sampled_;

  /**
   * Statistics: count number of samples.
   */
  uint32_t sampling_count_ = 0;

  /**
   * Sampling period.
   */
  std::chrono::milliseconds push_period_{kDefaultPushPeriod};

  /**
   * Keep track of when the source was last sampled.
   */
  std::chrono::steady_clock::time_point last_pushed_;

  /**
   * Data push threshold, based number of records after which a push.
   */
  uint32_t occupancy_threshold_ = kDefaultOccupancyThreshold;

  /**
   * Data push threshold, based on percentage of buffer that is filled.
   */
  uint32_t occupancy_pct_threshold_ = kDefaultOccupancyPctThreshold;

  /**
   * Statistics: count number of pushes.
   */
  uint32_t push_count_ = 0;

 public:
  static constexpr uint32_t kDefaultOccupancyThreshold = 1024;
  static constexpr uint32_t kDefaultOccupancyPctThreshold = 100;

  // The sampling/push periods are overwritten by CreateSourceConnectors(),
  // which uses SourceConnector specific default values.
  // So don't read too much into these constants.
  // See the default constants in the individual source connectors instead.
  static constexpr uint32_t kDefaultSamplingPeriod = 100;
  static constexpr uint32_t kDefaultPushPeriod = 1000;
};

using InfoClassManagerVec = std::vector<std::unique_ptr<InfoClassManager>>;

}  // namespace stirling
}  // namespace pl
