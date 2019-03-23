#pragma once

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <algorithm>
#include <numeric>
#include <string>

#include <memory>
#include <vector>

#include "src/common/base/base.h"
#include "src/shared/types/types.h"

namespace pl {

/*
 * Status adapter for arrow.
 */
template <>
inline Status StatusAdapter<arrow::Status>(const arrow::Status &s) noexcept {
  return Status(statuspb::UNKNOWN, s.message());
}

namespace types {

// The functions convert vector of UDF values to an arrow representation on
// the given MemoryPool.
template <typename TUDFValue>
inline std::shared_ptr<arrow::Array> ToArrow(const std::vector<TUDFValue> &data,
                                             arrow::MemoryPool *mem_pool) {
  DCHECK(mem_pool != nullptr);

  typename ValueTypeTraits<TUDFValue>::arrow_builder_type builder(mem_pool);
  PL_CHECK_OK(builder.Reserve(data.size()));
  for (const auto v : data) {
    builder.UnsafeAppend(v.val);
  }
  std::shared_ptr<arrow::Array> arr;
  PL_CHECK_OK(builder.Finish(&arr));
  return arr;
}

// Specialization of the above for strings.
template <>
inline std::shared_ptr<arrow::Array> ToArrow<StringValue>(const std::vector<StringValue> &data,
                                                          arrow::MemoryPool *mem_pool) {
  DCHECK(mem_pool != nullptr);
  arrow::StringBuilder builder(mem_pool);
  size_t total_size =
      std::accumulate(data.begin(), data.end(), 0ULL,
                      [](uint64_t sum, const std::string &str) { return sum + str.size(); });
  // This allocates space for null/ptrs/size.
  PL_CHECK_OK(builder.Reserve(data.size()));
  // This allocates space for the actual data.
  PL_CHECK_OK(builder.ReserveData(total_size));
  for (const auto val : data) {
    builder.UnsafeAppend(val);
  }
  std::shared_ptr<arrow::Array> arr;
  PL_CHECK_OK(builder.Finish(&arr));
  return arr;
}

/**
 * Find the UDFDataType for a given arrow type.
 * @param arrow_type The arrow type.
 * @return The UDFDataType.
 */
DataType ArrowToDataType(const arrow::Type::type &arrow_type);

arrow::Type::type ToArrowType(const DataType &udf_type);

int64_t ArrowTypeToBytes(const arrow::Type::type &arrow_type);

/**
 * Make an arrow builder based on UDFDataType and usng the passed in MemoryPool.
 * @param data_type The UDFDataType.
 * @param mem_pool The MemoryPool to use.
 * @return a unique_ptr to an array builder.
 */
std::unique_ptr<arrow::ArrayBuilder> MakeArrowBuilder(const DataType &data_type,
                                                      arrow::MemoryPool *mem_pool);

// The get value functions pluck out value at a specific index.
template <typename T>
inline auto GetValue(const T *arr, int64_t idx) {
  return arr->Value(idx);
}

// Specialization for string type.
template <>
inline auto GetValue<arrow::StringArray>(const arrow::StringArray *arr, int64_t idx) {
  return arr->GetString(idx);
}

// This function takes in a generic arrow::Array and then converts it to actual
// specific arrow::Array subtype. This function is unsafe and will produce wrong results (or crash)
// if used incorrectly.
template <types::DataType TExecArgType>
constexpr auto GetValueFromArrowArray(const arrow::Array *arg, int64_t idx) {
  // A sample transformation (for TExecArgType = types::DataType::INT64) is:
  // return GetValue(static_cast<arrow::Int64Array*>(arg), idx);
  using arrow_array_type = typename types::DataTypeTraits<TExecArgType>::arrow_array_type;
  return GetValue(static_cast<const arrow_array_type *>(arg), idx);
}

template <types::DataType T>
class ArrowArrayIterator
    : public std::iterator<std::forward_iterator_tag,
                           typename types::ValueTypeTraits<
                               typename types::DataTypeTraits<T>::value_type>::native_type> {
  using ReturnType =
      typename types::ValueTypeTraits<typename types::DataTypeTraits<T>::value_type>::native_type;

 public:
  ArrowArrayIterator();

  explicit ArrowArrayIterator(arrow::Array *array) : array_(array) {}

  ArrowArrayIterator(arrow::Array *array, int64_t idx) : array_(array), curr_idx_(idx) {}

  bool operator==(const ArrowArrayIterator<T> &iterator) const {
    return this->array_ == iterator.array_ && this->curr_idx_ == iterator.curr_idx_;
  }

  bool operator!=(const ArrowArrayIterator<T> &iterator) const {
    return this->array_ != iterator.array_ || this->curr_idx_ != iterator.curr_idx_;
  }

  ReturnType operator*() const { return (types::GetValueFromArrowArray<T>(array_, curr_idx_)); }

  ReturnType *operator->() const { return (types::GetValueFromArrowArray<T>(array_, curr_idx_)); }

  ArrowArrayIterator<T> &operator++() {
    curr_idx_++;

    return *this;
  }

  ArrowArrayIterator<T> begin() { return ArrowArrayIterator<T>(array_, 0); }

  ArrowArrayIterator<T> end() { return ArrowArrayIterator<T>(array_, array_->length()); }

  ArrowArrayIterator<T> operator++(int) {
    auto ret = *this;
    ++*this;
    return ret;
  }
  ArrowArrayIterator<T> operator+(int i) const {
    auto ret = ArrowArrayIterator<T>(array_, curr_idx_ + i);
    return ret;
  }

 private:
  arrow::Array *array_;
  int64_t curr_idx_ = 0;
};

/**
 * Search through the arrow array for the index of the first item equal or greater than the given
 * value.
 * @tparam T UDF datatype of the arrow array.
 * @param arr the arrow array to search through.
 * @param val the value to search for in the arrow array.
 * @return the index of the first item in the array equal to or greater than val.
 */
template <types::DataType T>
int64_t SearchArrowArrayGreaterThanOrEqual(
    arrow::Array *arr,
    typename types::ValueTypeTraits<typename types::DataTypeTraits<T>::value_type>::native_type
        val) {
  auto arr_iterator = ArrowArrayIterator<T>(arr);
  auto res = std::lower_bound(arr_iterator, arr_iterator.end(), val);
  if (res != arr_iterator.end()) {
    return std::distance(arr_iterator.begin(), res);
  }
  return -1;
}

/**
 * Search through the arrow array for the index of the first item less than the given value.
 * @tparam T UDF datatype of the arrow array.
 * @param arr the arrow array to search through.
 * @param val the value to search for in the arrow array.
 * @return the index of the first item in the array less than val.
 */
template <types::DataType T>
int64_t SearchArrowArrayLessThan(
    arrow::Array *arr,
    typename types::ValueTypeTraits<typename types::DataTypeTraits<T>::value_type>::native_type
        val) {
  auto res = SearchArrowArrayGreaterThanOrEqual<T>(arr, val);
  if (res == -1) {
    // Everything in the array is less than val.
    return arr->length();
  }
  if (res == 0) {
    // Nothing in the array is less than val.
    return -1;
  }
  // res points to an index that is geq than val. res - 1 should be the largest item less than
  // val. However, arr[res-1] may be a duplicate value, so we need to find the first instance of
  // arr[res-1] in the array.
  auto next_smallest = types::GetValueFromArrowArray<T>(arr, res - 1);
  return SearchArrowArrayGreaterThanOrEqual<T>(arr, next_smallest);
}

}  // namespace types
}  // namespace pl
