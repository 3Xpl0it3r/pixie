#pragma once
#include <arrow/builder.h>
#include <arrow/type.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <functional>

#include "src/carnot/udf/base.h"
#include "src/common/base/base.h"
#include "src/shared/metadata/metadata_state.h"
#include "src/shared/types/column_wrapper.h"
#include "src/shared/types/proto/types.pb.h"
#include "src/shared/types/types.h"

namespace pl {
namespace carnot {
namespace udf {

/**
 * AnyUDF is the base class for all UDFs in carnot.
 */
class AnyUDF {
 public:
  virtual ~AnyUDF() = default;
};

/**
 * Any UDA is a base class for all UDAs in carnot.
 */
class AnyUDA {
 public:
  virtual ~AnyUDA() = default;
};

/**
 * ScalarUDF is a wrapper around a stateless function that can take one more more UDF values
 * and return a single UDF value.
 *
 * In the lifetime of a query, one more more instances may be created. The implementation should
 * take care not to store local state that can change functionality from call to call (ie. The
 * Exec function should be pure).
 *
 * The derived class must implement:
 *      UDFValue Exec(FunctionContext *ctx, UDFValue... value) {}
 * This function is called for each record for which this UDF needs to execute.
 *
 * The ScalarUDF can _optionally_ implement the following function:
 *      Status Init(FunctionContext *ctx, UDFValue... init_args) {}
 *  This function is called once during initialization of each instance (many instances
 *  may exists in a given query). The arguments are as provided by the query.
 */
class ScalarUDF : public AnyUDF {
 public:
  ~ScalarUDF() override = default;
};

/**
 * UDA is a stateful function that updates internal state bases on the input
 * values. It must be Merge-able with other UDAs of the same type.
 *
 * In the lifetime of the query one or more instances will be created. The Merge function
 * will be called to combine multiple instances together before destruction.
 *
 * The derived class must implement:
 *     void Update(FunctionContext *ctx, Args...) {}
 *     void Merge(FunctionContext *ctx, const SampleUDA& other) {}
 *     ReturnValue Finalize(FunctionContext *ctx) {}
 *
 * It may optionally implement:
 *     Status Init(FunctionContext *ctx, InitArgs...) {}
 *
 * All argument types must me valid UDFValueTypes.
 */
class UDA : public AnyUDA {
 public:
  ~UDA() override = default;
};

// SFINAE test for init fn.
// TODO(zasgar): We really want to also test the argument/return types.
template <typename T, typename = void>
struct has_udf_init_fn : std::false_type {};

template <typename T>
struct has_udf_init_fn<T, std::void_t<decltype(&T::Init)>> : std::true_type {
  static_assert(
      IsValidInitFn(&T::Init),
      "If an init functions exists it must have the form: Status Init(FunctionContext*, ...)");
};

/**
 * Checks to see if a valid looking Init Function exists.
 */
template <typename ReturnType, typename TUDF, typename... Types>
static constexpr bool IsValidInitFn(ReturnType (TUDF::*)(Types...)) {
  return false;
}

template <typename TUDF, typename... Types>
static constexpr bool IsValidInitFn(Status (TUDF::*)(FunctionContext*, Types...)) {
  return true;
}

/**
 * Checks to see if a valid looking Exec Function exists.
 */
template <typename ReturnType, typename TUDF, typename... Types>
static constexpr bool IsValidExecFunc(ReturnType (TUDF::*)(Types...)) {
  return false;
}

template <typename ReturnType, typename TUDF, typename... Types>
static constexpr bool IsValidExecFunc(ReturnType (TUDF::*)(FunctionContext*, Types...)) {
  return true;
}

template <typename ReturnType, typename TUDF, typename... Types>
static constexpr std::array<types::DataType, sizeof...(Types)> GetArgumentTypesHelper(
    ReturnType (TUDF::*)(FunctionContext*, Types...)) {
  return std::array<types::DataType, sizeof...(Types)>(
      {types::ValueTypeTraits<Types>::data_type...});
}

template <typename ReturnType, typename TUDF, typename... Types>
static constexpr types::DataType ReturnTypeHelper(ReturnType (TUDF::*)(Types...)) {
  return types::ValueTypeTraits<ReturnType>::data_type;
}

template <typename T, typename = void>
struct check_init_fn {};

template <typename T>
struct check_init_fn<T, typename std::enable_if_t<has_udf_init_fn<T>::value>> {
  static_assert(IsValidInitFn(&T::Init),
                "must have a valid Init fn, in form: Status Init(FunctionContext*, ...)");
};

/**
 * ScalarUDFTraits allows access to compile time traits of the class. For example,
 * argument types.
 * @tparam T A class that derives from ScalarUDF.
 */
template <typename T>
class ScalarUDFTraits {
 public:
  /**
   * Arguments types of Exec.
   * @return a vector of UDF data types.
   */
  static constexpr auto ExecArguments() { return GetArgumentTypesHelper(&T::Exec); }

  /**
   * Return types of the Exec function
   * @return A types::UDFDataType which is the return type of the Exec function.
   */
  static constexpr types::DataType ReturnType() { return ReturnTypeHelper(&T::Exec); }

  /**
   * Checks if the UDF has an Init function.
   * @return true if it has an Init function.
   */
  static constexpr bool HasInit() { return has_udf_init_fn<T>::value; }

 private:
  struct check_valid_udf {
    static_assert(std::is_base_of_v<ScalarUDF, T>, "UDF must be derived from ScalarUDF");
    static_assert(IsValidExecFunc(&T::Exec),
                  "must have a valid Exec fn, in form: UDFValue Exec(FunctionContext*, ...)");

   private:
    static constexpr check_init_fn<T> check_init_{};
  } check_;
};

/**
 * These are function type checkers for UDAs. Ideally these would all be pure
 * SFINAE templates, but the overload makes the code a bit easier to read.
 */

/**
 * Checks to see if a valid looking Update function exists.
 */
template <typename ReturnType, typename TUDA, typename... Types>
static constexpr bool IsValidUpdateFn(ReturnType (TUDA::*)(Types...)) {
  return false;
}

template <typename TUDA, typename... Types>
static constexpr bool IsValidUpdateFn(void (TUDA::*)(FunctionContext*, Types...)) {
  return true;
}

/**
 * Checks to see if a valid looking Merge Function exists.
 */
template <typename ReturnType, typename TUDA, typename... Types>
static constexpr bool IsValidMergeFn(ReturnType (TUDA::*)(Types...)) {
  return false;
}

template <typename TUDA>
static constexpr bool IsValidMergeFn(void (TUDA::*)(FunctionContext*, const TUDA&)) {
  return true;
}

/**
 * Checks to see if a valid looking Finalize Function exists.
 */
template <typename ReturnType, typename TUDA, typename... Types>
static constexpr bool IsValidFinalizeFn(ReturnType (TUDA::*)(Types...)) {
  return false;
}

template <typename ReturnType, typename TUDA, typename... Types>
static constexpr bool IsValidFinalizeFn(ReturnType (TUDA::*)(FunctionContext*)) {
  return static_cast<bool>(types::IsValidValueType<ReturnType>::value);
}

/**
 * ScalarUDFTraits allows access to compile time traits of a given UDA.
 * @tparam T A class that derives from UDA.
 */
template <typename T>
class UDATraits {
 public:
  static constexpr auto UpdateArgumentTypes() { return GetArgumentTypesHelper<void>(&T::Update); }
  static constexpr types::DataType FinalizeReturnType() { return ReturnTypeHelper(&T::Finalize); }

  /**
   * Checks if the UDA has an Init function.
   * @return true if it has an Init function.
   */
  static constexpr bool HasInit() { return has_udf_init_fn<T>::value; }

 private:
  /**
   * Static asserts to validate that the UDA is well formed.
   */
  struct check_valid_uda {
    static_assert(std::is_base_of_v<UDA, T>, "UDA must be derived from UDA");
    static_assert(IsValidUpdateFn(&T::Update),
                  "must have a valid Update fn, in form: void Update(FunctionContext*, ...)");
    static_assert(IsValidMergeFn(&T::Merge),
                  "must have a valid Merge fn, in form: void Merge(FunctionContext*, const UDA&)");
    static_assert(IsValidFinalizeFn(&T::Finalize),
                  "must have a valid Finalize fn, in form: ReturnType Finalize(FunctionContext*)");
    static constexpr check_init_fn<T> check_init_{};
  } check_;
};

}  // namespace udf
}  // namespace carnot
}  // namespace pl
