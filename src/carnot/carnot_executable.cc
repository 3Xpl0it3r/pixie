#include <fstream>
#include <iostream>
#include <string>

#include <parser.hpp>

#include "src/carnot/carnot.h"
#include "src/common/base/base.h"
#include "src/shared/types/column_wrapper.h"
#include "src/shared/types/type_utils.h"
#include "src/table_store/table_store.h"

DEFINE_string(input_file, gflags::StringFromEnv("INPUT_FILE", ""),
              "The csv containing data to run the query on.");

DEFINE_string(output_file, gflags::StringFromEnv("OUTPUT_FILE", ""),
              "The file path to write the output data to.");

DEFINE_string(query, gflags::StringFromEnv("QUERY", ""), "The query to run.");

DEFINE_int64(rowbatch_size, gflags::Int64FromEnv("ROWBATCH_SIZE", 0),
             "The size of the rowbatches.");

using pl::types::DataType;

namespace {
/**
 * Gets the corresponding pl::DataType from the string type in the csv.
 * @param type the string from the csv.
 * @return the pl::DataType.
 */
pl::StatusOr<DataType> GetTypeFromHeaderString(const std::string& type) {
  if (type == "int64") {
    return DataType::INT64;
  }
  if (type == "float64") {
    return DataType::FLOAT64;
  }
  if (type == "boolean") {
    return DataType::BOOLEAN;
  }
  if (type == "string") {
    return DataType::STRING;
  }
  if (type == "time64ns") {
    return DataType::TIME64NS;
  }
  return pl::error::InvalidArgument("Could not recognize type from header.");
}

std::string ValueToString(int64_t val) { return absl::StrFormat("%d", val); }

std::string ValueToString(double val) { return absl::StrFormat("%.2f", val); }

std::string ValueToString(std::string val) { return absl::StrFormat("%s", val); }

std::string ValueToString(bool val) { return absl::StrFormat("%s", val ? "true" : "false"); }

/**
 * Takes the value and converts it to the string representation.
 * @ param type The type of the value.
 * @ param val The value.
 * @return The string representation.
 */
template <DataType DT>
void AddStringValueToRow(std::vector<std::string>* row, arrow::Array* arr, int64_t idx) {
  using ArrowArrayType = typename pl::types::DataTypeTraits<DT>::arrow_array_type;

  auto val = ValueToString(pl::types::GetValue(static_cast<ArrowArrayType*>(arr), idx));
  row->push_back(val);
}

/**
 * Convert the csv at the given filename into a Carnot table.
 * @param filename The filename of the csv to convert.
 * @return The Carnot table.
 */
std::shared_ptr<pl::table_store::Table> GetTableFromCsv(const std::string& filename,
                                                        int64_t rb_size) {
  std::ifstream f(filename);
  aria::csv::CsvParser parser(f);

  // The schema of the columns.
  std::vector<pl::types::DataType> types;
  // The names of the columns.
  std::vector<std::string> names;

  // Get the columns types and names.
  auto row_idx = 0;
  for (auto& row : parser) {
    auto col_idx = 0;
    for (auto& field : row) {
      if (row_idx == 0) {
        auto type = GetTypeFromHeaderString(field).ConsumeValueOrDie();
        // Currently reading the first row, which should be the types of the columns.
        types.push_back(type);
      } else if (row_idx == 1) {  // Reading second row, should be the names of columns.
        names.push_back(field);
      }
      col_idx++;
    }
    row_idx++;
    if (row_idx > 1) {
      break;
    }
  }

  // Construct the table.
  pl::table_store::schema::Relation rel(types, names);
  auto table = std::make_shared<pl::table_store::Table>(rel);

  // Add rowbatches to the table.
  row_idx = 0;
  std::unique_ptr<std::vector<pl::types::SharedColumnWrapper>> batch;
  for (auto& row : parser) {
    if (row_idx % rb_size == 0) {
      if (batch) {
        auto s = table->TransferRecordBatch(std::move(batch));
        if (!s.ok()) {
          LOG(ERROR) << "Couldn't add record batch to table.";
        }
      }

      // Create new batch.
      batch = std::make_unique<std::vector<pl::types::SharedColumnWrapper>>();
      // Create vectors for each column.
      for (auto type : types) {
        auto wrapper = pl::types::ColumnWrapper::Make(type, 0);
        batch->push_back(wrapper);
      }
    }
    auto col_idx = 0;
    for (auto& field : row) {
      switch (types[col_idx]) {
        case DataType::INT64:
          static_cast<pl::types::Int64ValueColumnWrapper*>(batch->at(col_idx).get())
              ->Append(std::stoi(field));
          break;
        case DataType::FLOAT64:
          static_cast<pl::types::Float64ValueColumnWrapper*>(batch->at(col_idx).get())
              ->Append(std::stof(field));
          break;
        case DataType::BOOLEAN:
          static_cast<pl::types::BoolValueColumnWrapper*>(batch->at(col_idx).get())
              ->Append(field == "true");
          break;
        case DataType::STRING:
          static_cast<pl::types::StringValueColumnWrapper*>(batch->at(col_idx).get())
              ->Append(absl::StrFormat("%s", field));
          break;
        case DataType::TIME64NS:
          static_cast<pl::types::Time64NSValueColumnWrapper*>(batch->at(col_idx).get())
              ->Append(std::stoi(field));
          break;
        default:
          LOG(ERROR) << "Couldn't convert field to a ValueType.";
      }
      col_idx++;
    }
    row_idx++;
  }
  // Add the final batch to the table.
  if (batch->at(0)->Size() > 0) {
    auto s = table->TransferRecordBatch(std::move(batch));
    if (!s.ok()) {
      LOG(ERROR) << "Couldn't add record batch to table.";
    }
  }

  return table;
}

/**
 * Write the table to a CSV.
 * @param filename The name of the output CSV file.
 * @param table The table to write to a CSV.
 */
void TableToCsv(const std::string& filename, pl::table_store::Table* table) {
  std::ofstream output_csv;
  output_csv.open(filename);

  auto col_idxs = std::vector<int64_t>();
  for (int64_t i = 0; i < table->NumColumns(); i++) {
    col_idxs.push_back(i);
  }

  std::vector<std::string> output_col_names;
  for (size_t i = 0; i < col_idxs.size(); i++) {
    output_col_names.push_back(table->GetColumn(i)->name());
  }
  output_csv << absl::StrFormat("%s\n", absl::StrJoin(output_col_names, ","));

  for (auto i = 0; i < table->NumBatches(); i++) {
    auto rb = table->GetRowBatch(i, col_idxs, arrow::default_memory_pool()).ConsumeValueOrDie();
    for (auto row_idx = 0; row_idx < rb->num_rows(); row_idx++) {
      std::vector<std::string> row;
      for (size_t col_idx = 0; col_idx < col_idxs.size(); col_idx++) {
#define TYPE_CASE(_dt_) AddStringValueToRow<_dt_>(&row, rb->ColumnAt(col_idx).get(), row_idx)
        PL_SWITCH_FOREACH_DATATYPE(table->GetColumn(col_idx)->data_type(), TYPE_CASE);
#undef TYPE_CASE
      }
      output_csv << absl::StrFormat("%s\n", absl::StrJoin(row, ","));
    }
  }
  output_csv.close();
}

}  // namespace

int main(int argc, char* argv[]) {
  pl::InitEnvironmentOrDie(&argc, argv);

  auto filename = FLAGS_input_file;
  auto output_filename = FLAGS_output_file;
  auto query = FLAGS_query;
  auto rb_size = FLAGS_rowbatch_size;

  auto table = GetTableFromCsv(filename, rb_size);

  // Execute query.
  auto table_store = std::make_shared<pl::table_store::TableStore>();
  auto carnot_or_s = pl::carnot::Carnot::Create(table_store);
  if (!carnot_or_s.ok()) {
    LOG(FATAL) << "Carnot failed to init.";
  }
  auto carnot = carnot_or_s.ConsumeValueOrDie();
  table_store->AddTable("csv_table", table);
  auto exec_status = carnot->ExecuteQuery(query, pl::CurrentTimeNS());
  auto res = exec_status.ConsumeValueOrDie();

  // Write output table to CSV.
  auto output_table = res.output_tables_[0];
  TableToCsv(output_filename, output_table);

  pl::ShutdownEnvironmentOrDie();
  return 0;
}
