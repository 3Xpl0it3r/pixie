#pragma once
#include <string>
#include <utility>

#include "src/common/base/base.h"

namespace pl {
namespace utils {

template <size_t N>
void ReverseBytes(uint8_t* x, uint8_t* y) {
  for (size_t k = 0; k < N; k++) {
    y[k] = x[N - k - 1];
  }
}

template <typename TCharType, size_t N>
void ReverseBytes(TCharType (&x)[N], TCharType (&y)[N]) {
  uint8_t* x_bytes = reinterpret_cast<uint8_t*>(x);
  uint8_t* y_bytes = reinterpret_cast<uint8_t*>(y);
  ReverseBytes<N>(x_bytes, y_bytes);
}

template <typename T>
T ReverseBytes(T x) {
  T y;
  uint8_t* x_bytes = reinterpret_cast<uint8_t*>(&x);
  uint8_t* y_bytes = reinterpret_cast<uint8_t*>(&y);
  ReverseBytes<sizeof(T)>(x_bytes, y_bytes);
  return y;
}

/**
 * Convert a little-endian string of bytes to an integer.
 *
 * @tparam T The receiver int type.
 * @tparam N Number of bytes to process from the source buffer. N must be <= sizeof(T).
 * If N < sizeof(T), the remaining bytes (MSBs) are assumed to be zero.
 * @param buf The sequence of bytes.
 * @return The decoded int value.
 */
template <typename T, int N = sizeof(T)>
T LEndianBytesToInt(std::string_view buf) {
  // Doesn't make sense to process more bytes than the destination type.
  // Less bytes is okay, on the other hand, since the value will still fit.
  static_assert(N <= sizeof(T));

  // Source buffer must have enough bytes.
  DCHECK_GE(buf.size(), N);

  T result = 0;
  for (size_t i = 0; i < N; i++) {
    result = static_cast<uint8_t>(buf[N - 1 - i]) | (result << 8);
  }
  return result;
}

/**
 * Convert a little-endian string of bytes to a float/double.
 *
 * @tparam T The receiver float type.
 * @param buf The sequence of bytes.
 * @return The decoded float value.
 */
template <typename TFloatType>
TFloatType LEndianBytesToFloat(std::string_view buf) {
  // Source buffer must have enough bytes.
  DCHECK_GE(buf.size(), sizeof(TFloatType));

  return *reinterpret_cast<const TFloatType*>(buf.data());
}

/**
 * Convert an int to a little-endian string of bytes.
 *
 * @tparam TCharType The char type to use in the string (e.g. char vs uint8_t).
 * @param num The number to convert.
 * @param result the destination buffer.
 */
template <typename TCharType, size_t N>
void IntToLEndianBytes(int64_t num, TCharType (&result)[N]) {
  static_assert(N <= sizeof(int64_t));
  for (size_t i = 0; i < N; i++) {
    result[i] = (num >> (i * 8));
  }
}

/**
 * Convert a big-endian string of bytes to an integer.
 *
 * @tparam T The receiver int type.
 * @tparam N Number of bytes to process from the source buffer. N must be <= sizeof(T).
 * If N < sizeof(T), the remaining bytes (MSBs) are assumed to be zero.
 * @param buf The sequence of bytes.
 * @return The decoded int value.
 */
template <typename T, int N = sizeof(T)>
T BEndianBytesToInt(std::string_view buf) {
  // Doesn't make sense to process more bytes than the destination type.
  // Less bytes is okay, on the other hand, since the value will still fit.
  static_assert(N <= sizeof(T));

  // Source buffer must have enough bytes.
  DCHECK_GE(buf.size(), N);

  T result = 0;
  for (size_t i = 0; i < N; i++) {
    result = static_cast<uint8_t>(buf[i]) | (result << 8);
  }
  return result;
}

/**
 * Convert a big-endian string of bytes to a float/double.
 *
 * @tparam T The receiver float type.
 * @param buf The sequence of bytes.
 * @return The decoded float value.
 */
template <typename TFloatType>
TFloatType BEndianBytesToFloat(std::string_view buf) {
  // Source buffer must have enough bytes.
  DCHECK_GE(buf.size(), sizeof(TFloatType));

  TFloatType val = *reinterpret_cast<const TFloatType*>(buf.data());
  return ReverseBytes<TFloatType>(val);
}

}  // namespace utils
}  // namespace pl
