#include <benchmark/benchmark.h>
#include <google/protobuf/text_format.h>
#include <sole.hpp>

#include "src/carnot/exec/expression_evaluator.h"
#include "src/carnot/exec/test_utils.h"
#include "src/carnot/plan/scalar_expression.h"
#include "src/carnot/planpb/plan.pb.h"
#include "src/carnot/planpb/test_proto.h"
#include "src/common/base/base.h"
#include "src/common/benchmark/benchmark.h"
#include "src/common/datagen/datagen.h"
#include "src/shared/types/arrow_adapter.h"
#include "src/shared/types/types.h"
#include "src/table_store/table/table_store.h"

using ScalarExpression = pl::carnot::plan::ScalarExpression;
using ScalarExpressionVector = std::vector<std::shared_ptr<ScalarExpression>>;
using pl::carnot::exec::ExecState;
using pl::carnot::exec::MockKelvinStubGenerator;
using pl::carnot::exec::ScalarExpressionEvaluator;
using pl::carnot::exec::ScalarExpressionEvaluatorType;
using pl::carnot::planpb::testutils::kAddScalarFuncNestedPbtxt;
using pl::carnot::planpb::testutils::kAddScalarFuncPbtxt;
using pl::carnot::planpb::testutils::kColumnReferencePbtxt;
using pl::carnot::planpb::testutils::kScalarInt64ValuePbtxt;
using pl::carnot::udf::FunctionContext;
using pl::carnot::udf::ScalarUDF;
using pl::carnot::udf::ScalarUDFRegistry;
using pl::carnot::udf::UDARegistry;
using pl::table_store::schema::RowBatch;
using pl::table_store::schema::RowDescriptor;
using pl::types::DataType;
using pl::types::Int64Value;
using pl::types::ToArrow;

class AddUDF : public ScalarUDF {
 public:
  Int64Value Exec(FunctionContext*, Int64Value v1, Int64Value v2) { return v1.val + v2.val; }
};

// NOLINTNEXTLINE : runtime/references.
void BM_ScalarExpressionTwoCols(benchmark::State& state,
                                const ScalarExpressionEvaluatorType& eval_type, const char* pbtxt) {
  pl::carnot::planpb::ScalarExpression se_pb;
  size_t data_size = state.range(0);

  google::protobuf::TextFormat::MergeFromString(pbtxt, &se_pb);
  auto s_or_se = pl::carnot::plan::ScalarExpression::FromProto(se_pb);
  CHECK(s_or_se.ok());
  std::shared_ptr<ScalarExpression> se = s_or_se.ConsumeValueOrDie();

  auto udf_registry = std::make_unique<ScalarUDFRegistry>("test_registry");
  auto uda_registry = std::make_unique<UDARegistry>("test_registry");
  auto table_store = std::make_shared<pl::carnot::exec::TableStore>();
  PL_CHECK_OK(udf_registry->Register<AddUDF>("add"));
  auto exec_state = std::make_unique<ExecState>(udf_registry.get(), uda_registry.get(), table_store,
                                                MockKelvinStubGenerator, sole::uuid4());

  auto in1 = pl::datagen::CreateLargeData<Int64Value>(data_size);
  auto in2 = pl::datagen::CreateLargeData<Int64Value>(data_size);

  RowDescriptor rd({DataType::INT64, DataType::INT64});
  auto input_rb = std::make_unique<RowBatch>(rd, in1.size());

  PL_CHECK_OK(input_rb->AddColumn(ToArrow(in1, arrow::default_memory_pool())));
  PL_CHECK_OK(input_rb->AddColumn(ToArrow(in2, arrow::default_memory_pool())));
  // NOLINTNEXTLINE : clang-analyzer-deadcode.DeadStores.
  for (auto _ : state) {
    RowDescriptor rd_output({DataType::INT64});
    RowBatch output_rb(rd_output, input_rb->num_rows());
    auto function_ctx = std::make_unique<pl::carnot::udf::FunctionContext>(nullptr);
    auto evaluator = ScalarExpressionEvaluator::Create({se}, eval_type, function_ctx.get());
    PL_CHECK_OK(evaluator->Open(exec_state.get()));
    PL_CHECK_OK(evaluator->Evaluate(exec_state.get(), *input_rb, &output_rb));
    PL_CHECK_OK(evaluator->Close(exec_state.get()));

    benchmark::DoNotOptimize(output_rb);
    CHECK_EQ(static_cast<size_t>(output_rb.ColumnAt(0)->length()), data_size);
  }
  state.SetBytesProcessed(int64_t(state.iterations()) * 2 * in1.size() * sizeof(int64_t));
}

BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, eval_col_arrow,
                  ScalarExpressionEvaluatorType::kArrowNative, kColumnReferencePbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);
BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, eval_col_native,
                  ScalarExpressionEvaluatorType::kVectorNative, kColumnReferencePbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);

BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, eval_const_arrow,
                  ScalarExpressionEvaluatorType::kArrowNative, kScalarInt64ValuePbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);
BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, eval_const_native,
                  ScalarExpressionEvaluatorType::kVectorNative, kScalarInt64ValuePbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);

BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, two_cols_add_nested_arrow,
                  ScalarExpressionEvaluatorType::kArrowNative, kAddScalarFuncNestedPbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);
BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, two_cols_add_nested_native,
                  ScalarExpressionEvaluatorType::kVectorNative, kAddScalarFuncNestedPbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);

BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, two_cols_simple_add_arrow,
                  ScalarExpressionEvaluatorType::kArrowNative, kAddScalarFuncNestedPbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);
BENCHMARK_CAPTURE(BM_ScalarExpressionTwoCols, two_cols_simple_add_vector,
                  ScalarExpressionEvaluatorType::kVectorNative, kAddScalarFuncNestedPbtxt)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 16);
