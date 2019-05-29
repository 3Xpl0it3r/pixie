#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <type_traits>

#include "src/carnot/udf/udf_wrapper.h"
#include "src/common/base/base.h"
#include "src/shared/types/types.h"

namespace pl {
namespace carnot {
namespace udf {

using testing::ElementsAre;

class ScalarUDF1 : ScalarUDF {
 public:
  types::Int64Value Exec(FunctionContext*, types::BoolValue, types::Int64Value) { return 0; }
};

class ScalarUDF1WithInit : ScalarUDF {
 public:
  Status Init(FunctionContext*, types::Int64Value) { return Status::OK(); }
  types::Int64Value Exec(FunctionContext*, types::BoolValue, types::BoolValue) { return 0; }
};

TEST(ScalarUDF, basic_tests) {
  EXPECT_EQ(types::DataType::INT64, ScalarUDFTraits<ScalarUDF1>::ReturnType());
  EXPECT_THAT(ScalarUDFTraits<ScalarUDF1>::ExecArguments(),
              ElementsAre(types::DataType::BOOLEAN, types::DataType::INT64));
  EXPECT_FALSE(ScalarUDFTraits<ScalarUDF1>::HasInit());
  EXPECT_TRUE(ScalarUDFTraits<ScalarUDF1WithInit>::HasInit());
}

TEST(UDFDataTypes, valid_tests) {
  EXPECT_TRUE((true == types::IsValidValueType<types::BoolValue>::value));
  EXPECT_TRUE((true == types::IsValidValueType<types::Int64Value>::value));
  EXPECT_TRUE((true == types::IsValidValueType<types::Float64Value>::value));
  EXPECT_TRUE((true == types::IsValidValueType<types::StringValue>::value));
}

class UDA1 : UDA {
 public:
  Status Init(FunctionContext*) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value) {}
  void Merge(FunctionContext*, const UDA1&) {}
  types::Int64Value Finalize(FunctionContext*) { return 0; }
};

class UDA1WithInit : UDA {
 public:
  Status Init(FunctionContext*, types::Int64Value) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value, types::Float64Value) {}
  void Merge(FunctionContext*, const UDA1WithInit&) {}
  types::Int64Value Finalize(FunctionContext*) { return 0; }
};

class UDAWithBadMerge1 : UDA {
 public:
  Status Init(FunctionContext*, types::Int64Value) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value, types::Float64Value) {}
  void Merge(const UDAWithBadMerge1&) {}
  types::Int64Value Finalize(FunctionContext*) { return 0; }
};

class UDAWithBadMerge2 : UDA {
 public:
  Status Init(FunctionContext*, types::Int64Value) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value, types::Float64Value) {}
  int Merge(FunctionContext*, const UDAWithBadMerge2&) { return 0; }
  types::Int64Value Finalize(FunctionContext*) { return 0; }
};

class UDAWithBadMerge3 : UDA {
 public:
  Status Init(FunctionContext*, types::Int64Value) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value, types::Float64Value) {}
  void Merge(FunctionContext*, const UDA&) {}
  types::Int64Value Finalize(FunctionContext*) { return 0; }
};

class UDAWithBadUpdate1 : UDA {
 public:
  Status Init(FunctionContext*) { return Status::OK(); }
  int Update(FunctionContext*, types::Int64Value) { return 0; }
  void Merge(FunctionContext*, const UDAWithBadUpdate1&) {}
  types::Int64Value Finalize(FunctionContext*) { return 0; }
};

class UDAWithBadUpdate2 : UDA {
 public:
  Status Init(FunctionContext*) { return Status::OK(); }
  void Update(types::Int64Value) {}
  void Merge(FunctionContext*, const UDAWithBadUpdate2&) {}
  types::Int64Value Finalize(FunctionContext*) { return 0; }
};

class UDAWithBadFinalize1 : UDA {
 public:
  Status Init(FunctionContext*) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value) {}
  void Merge(FunctionContext*, const UDAWithBadFinalize1&) {}
  void Finalize(FunctionContext*) {}
};

class UDAWithBadFinalize2 : UDA {
 public:
  Status Init(FunctionContext*) { return Status::OK(); }
  void Update(FunctionContext*, types::Int64Value) {}
  void Merge(FunctionContext*, const UDAWithBadFinalize2&) {}
  types::Int64Value Finalize() { return 0; }
};

TEST(UDA, bad_merge_fn) {
  EXPECT_TRUE((false == IsValidMergeFn(&UDAWithBadMerge1::Merge)));
  EXPECT_TRUE((false == IsValidMergeFn(&UDAWithBadMerge2::Merge)));
  EXPECT_TRUE((false == IsValidMergeFn(&UDAWithBadMerge3::Merge)));
}

TEST(UDA, bad_update_fn) {
  EXPECT_TRUE((false == IsValidUpdateFn(&UDAWithBadUpdate1::Update)));
  EXPECT_TRUE((false == IsValidUpdateFn(&UDAWithBadUpdate2::Update)));
}

TEST(UDA, bad_finalize_fn) {
  EXPECT_TRUE((false == IsValidFinalizeFn(&UDAWithBadFinalize1::Finalize)));
  EXPECT_TRUE((false == IsValidFinalizeFn(&UDAWithBadFinalize2::Finalize)));
}

TEST(UDA, valid_uda) {
  EXPECT_EQ(types::DataType::INT64, UDATraits<UDA1>::FinalizeReturnType());
  EXPECT_THAT(UDATraits<UDA1>::UpdateArgumentTypes(), ElementsAre(types::DataType::INT64));

  EXPECT_EQ(types::DataType::INT64, UDATraits<UDA1WithInit>::FinalizeReturnType());
  EXPECT_THAT(UDATraits<UDA1WithInit>::UpdateArgumentTypes(),
              ElementsAre(types::DataType::INT64, types::DataType::FLOAT64));
}

TEST(BoolValue, value_tests) {
  // Test constructor init.
  types::BoolValue v(false);
  // Test == overload.
  // NOLINTNEXTLINE(readability/check).
  EXPECT_TRUE(v == false);
  // Test assignment.
  v = true;
  EXPECT_EQ(true, v.val);
  // Check type base type.
  bool base_type_check = (std::is_base_of_v<types::BaseValueType, types::BoolValue>);
  EXPECT_TRUE(base_type_check);
}

TEST(Int64Value, value_tests) {
  // Test constructor init.
  types::Int64Value v(12);
  // Test == overload.
  // NOLINTNEXTLINE(readability/check).
  EXPECT_TRUE(v == 12);
  // Test assignment.
  v = 24;
  EXPECT_EQ(24, v.val);
  // Check type base type.
  bool base_type_check = (std::is_base_of_v<types::BaseValueType, types::Int64Value>);
  EXPECT_TRUE(base_type_check);
}

TEST(Float64Value, value_tests) {
  // Test constructor init.
  types::Float64Value v(12.5);
  // Test == overload.
  // NOLINTNEXTLINE(readability/check).
  EXPECT_TRUE(v == 12.5);
  // Test assignment.
  v = 24.2;
  EXPECT_DOUBLE_EQ(24.2, v.val);
  // Check type base type.
  bool base_type_check = (std::is_base_of_v<types::BaseValueType, types::Float64Value>);
  EXPECT_TRUE(base_type_check);
}

TEST(StringValue, value_tests) {
  types::StringValue sv("abcd");

  // Test == overload.
  // NOLINTNEXTLINE(readability/check).
  EXPECT_TRUE("abcd" == sv);
  // Test assignment.
  sv = "def";
  EXPECT_EQ("def", sv);
  // Check type base type.
  bool base_type_check = (std::is_base_of_v<types::BaseValueType, types::StringValue>);
  EXPECT_TRUE(base_type_check);
}

}  // namespace udf
}  // namespace carnot
}  // namespace pl
