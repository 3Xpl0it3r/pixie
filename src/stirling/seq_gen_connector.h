#pragma once

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "src/common/base/base.h"
#include "src/stirling/sequence_generator.h"
#include "src/stirling/source_connector.h"

namespace pl {
namespace stirling {

/**
 * @brief Sequence Generator connector
 */
class SeqGenConnector : public SourceConnector {
 public:
  ~SeqGenConnector() override = default;

  // clang-format off
  static constexpr DataElement kElementsSeq0[] = {
      {"time_", types::DataType::TIME64NS, types::PatternType::METRIC_COUNTER},
      {"x", types::DataType::INT64, types::PatternType::GENERAL},
      {"xmod10", types::DataType::INT64, types::PatternType::GENERAL_ENUM},
      {"xsquared", types::DataType::INT64, types::PatternType::GENERAL},
      {"fibonnaci", types::DataType::INT64, types::PatternType::GENERAL},
      {"PIx", types::DataType::FLOAT64, types::PatternType::GENERAL},
  };
  // clang-format on
  static constexpr auto kSeq0Table = DataTableSchema("sequence_generator0", kElementsSeq0);

  // clang-format off
  static constexpr DataElement kElementsSeq1[] = {
      {"time_", types::DataType::TIME64NS, types::PatternType::METRIC_COUNTER},
      {"x", types::DataType::INT64, types::PatternType::GENERAL},
      {"xmod8", types::DataType::INT64, types::PatternType::GENERAL}
  };
  // clang-format on
  static constexpr std::string_view kSeq1TabletizationKey = "xmod8";
  static constexpr auto kSeq1Table =
      DataTableSchema("sequence_generator1", kElementsSeq1, kSeq1TabletizationKey);

  static constexpr DataTableSchema kTablesArray[] = {kSeq0Table, kSeq1Table};
  static constexpr auto kTables = ArrayView<DataTableSchema>(kTablesArray);
  static constexpr uint32_t kSeq0TableNum = SourceConnector::TableNum(kTables, kSeq0Table);
  static constexpr uint32_t kSeq1TableNum = SourceConnector::TableNum(kTables, kSeq1Table);

  static constexpr std::chrono::milliseconds kDefaultSamplingPeriod{500};
  static constexpr std::chrono::milliseconds kDefaultPushPeriod{1000};

  static std::unique_ptr<SourceConnector> Create(std::string_view name) {
    return std::unique_ptr<SourceConnector>(new SeqGenConnector(name));
  }

  void SetSeed(uint32_t seed) { rng_.seed(seed); }

  void ConfigureNumRowsPerGet(uint32_t min_num_rows, uint32_t max_num_rows) {
    num_rows_min_ = min_num_rows;
    num_rows_max_ = max_num_rows;
  }

  void ConfigureNumRowsPerGet(uint32_t num_rows) { ConfigureNumRowsPerGet(num_rows, num_rows); }

 protected:
  explicit SeqGenConnector(std::string_view name)
      : SourceConnector(name, kTables, kDefaultSamplingPeriod, kDefaultPushPeriod),
        table0_lin_seq_(1, 1),
        table0_mod10_seq_(10),
        table0_square_seq_(1, 0, 0),
        table0_pi_seq_(3.14159, 0),
        table1_lin_seq_(2, 2),
        table1_mod8_seq_(8),
        rng_(37) {}

  Status InitImpl() override { return Status::OK(); }

  void TransferDataImpl(ConnectorContext* ctx, uint32_t table_num, DataTable* data_table) override;

  Status StopImpl() override { return Status::OK(); }

 private:
  void TransferDataTable0(uint32_t num_records, DataTable* data_table);
  void TransferDataTable1(uint32_t num_records, DataTable* data_table);

  TimeSequence<int64_t> table0_time_seq_;
  LinearSequence<int64_t> table0_lin_seq_;
  ModuloSequence<int64_t> table0_mod10_seq_;
  QuadraticSequence<int64_t> table0_square_seq_;
  LinearSequence<double> table0_pi_seq_;
  FibonacciSequence<int64_t> table0_fib_seq_;

  TimeSequence<int64_t> table1_time_seq_;
  LinearSequence<int64_t> table1_lin_seq_;
  ModuloSequence<int64_t> table1_mod8_seq_;

  std::default_random_engine rng_;
  uint32_t num_rows_min_ = 0;
  uint32_t num_rows_max_ = 10;
};

}  // namespace stirling
}  // namespace pl
