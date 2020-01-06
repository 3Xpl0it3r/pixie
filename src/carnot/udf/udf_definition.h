#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/carnot/udf/udf_wrapper.h"
#include "src/common/base/base.h"
#include "src/shared/types/column_wrapper.h"

namespace pl {
namespace carnot {
namespace udf {

/**
 * Store definition of a UDF. This is meant ot be stored in the UDF registry and
 * includes things like execution wrappers.
 */
class UDFDefinition {
 public:
  UDFDefinition() = default;
  virtual ~UDFDefinition() = default;

  Status Init(const std::string& name) {
    name_ = name;
    return Status::OK();
  }
  /**
   * @return The overload dependent arguments that the registry uses to resolves UDFs.
   */
  virtual const std::vector<types::DataType>& RegistryArgTypes() = 0;

  /**
   * Gets an unowned pointer to definition.
   * @return The UDF definition.
   */
  virtual UDFDefinition* GetDefinition() { return this; }

  /**
   * Access internal variable name.
   * @return Returns the name of the UDF.
   */
  std::string name() { return name_; }

 private:
  std::string name_;
};

/**
 * Store the information for a single ScalarUDF.
 * TODO(zasgar): Also needs to store information like exec ptrs, etc.
 */
class ScalarUDFDefinition : public UDFDefinition {
 public:
  ScalarUDFDefinition() = default;
  ~ScalarUDFDefinition() override = default;

  /**
   * Init a UDF definition with the given name and type.
   *
   * @tparam TUDF the UDF class. Must be a ScalarUDF.
   * @param name The name of the UDF.
   * @return Status success/error.
   */
  template <typename TUDF>
  Status Init(const std::string& name) {
    PL_RETURN_IF_ERROR(UDFDefinition::Init(name));
    exec_return_type_ = ScalarUDFTraits<TUDF>::ReturnType();
    auto exec_arguments_array = ScalarUDFTraits<TUDF>::ExecArguments();
    exec_arguments_ = {begin(exec_arguments_array), end(exec_arguments_array)};
    exec_wrapper_fn_ = ScalarUDFWrapper<TUDF>::ExecBatch;
    exec_wrapper_arrow_fn_ = ScalarUDFWrapper<TUDF>::ExecBatchArrow;

    make_fn_ = ScalarUDFWrapper<TUDF>::Make;

    return Status::OK();
  }

  ScalarUDFDefinition* GetDefinition() override { return this; }

  std::unique_ptr<ScalarUDF> Make() { return make_fn_(); }

  Status ExecBatch(ScalarUDF* udf, FunctionContext* ctx,
                   const std::vector<const types::ColumnWrapper*>& inputs,
                   types::ColumnWrapper* output, int count) {
    return exec_wrapper_fn_(udf, ctx, inputs, output, count);
  }

  Status ExecBatchArrow(ScalarUDF* udf, FunctionContext* ctx,
                        const std::vector<arrow::Array*>& inputs, arrow::ArrayBuilder* output,
                        int count) {
    return exec_wrapper_arrow_fn_(udf, ctx, inputs, output, count);
  }

  /**
   * Access internal variable exec_return_type.
   * @return the stored return types of the exec function.
   */
  types::DataType exec_return_type() const { return exec_return_type_; }
  const std::vector<types::DataType>& exec_arguments() const { return exec_arguments_; }

  const std::vector<types::DataType>& RegistryArgTypes() override { return exec_arguments_; }
  size_t Arity() const { return exec_arguments_.size(); }
  const auto& exec_wrapper() const { return exec_wrapper_fn_; }

 private:
  std::vector<types::DataType> exec_arguments_;
  types::DataType exec_return_type_;
  std::function<std::unique_ptr<ScalarUDF>()> make_fn_;
  std::function<Status(ScalarUDF*, FunctionContext* ctx,
                       const std::vector<const types::ColumnWrapper*>& inputs,
                       types::ColumnWrapper* output, int count)>
      exec_wrapper_fn_;

  std::function<Status(ScalarUDF* udf, FunctionContext* ctx,
                       const std::vector<arrow::Array*>& inputs, arrow::ArrayBuilder* output,
                       int count)>
      exec_wrapper_arrow_fn_;
};

/**
 * Store the information for a single UDA.
 */
class UDADefinition : public UDFDefinition {
 public:
  UDADefinition() = default;
  ~UDADefinition() override = default;
  /**
   * Init a UDA definition with the given name and type.
   *
   * @tparam T the UDA class. Must be derived from UDA.
   * @param name The name of the UDA.
   * @return Status success/error.
   */
  template <typename T>
  Status Init(const std::string& name) {
    PL_RETURN_IF_ERROR(UDFDefinition::Init(name));
    auto update_arguments_array = UDATraits<T>::UpdateArgumentTypes();
    update_arguments_ = {update_arguments_array.begin(), update_arguments_array.end()};
    finalize_return_type_ = UDATraits<T>::FinalizeReturnType();
    make_fn_ = UDAWrapper<T>::Make;
    exec_batch_update_fn_ = UDAWrapper<T>::ExecBatchUpdate;
    exec_batch_update_arrow_fn_ = UDAWrapper<T>::ExecBatchUpdateArrow;

    merge_fn_ = UDAWrapper<T>::Merge;
    finalize_arrow_fn_ = UDAWrapper<T>::FinalizeArrow;
    finalize_value_fn = UDAWrapper<T>::FinalizeValue;
    return Status::OK();
  }

  UDADefinition* GetDefinition() override { return this; }

  const std::vector<types::DataType>& RegistryArgTypes() override { return update_arguments_; }

  const std::vector<types::DataType>& update_arguments() { return update_arguments_; }
  types::DataType finalize_return_type() const { return finalize_return_type_; }

  std::unique_ptr<UDA> Make() { return make_fn_(); }

  Status ExecBatchUpdate(UDA* uda, FunctionContext* ctx,
                         const std::vector<const types::ColumnWrapper*>& inputs) {
    return exec_batch_update_fn_(uda, ctx, inputs);
  }
  Status ExecBatchUpdateArrow(UDA* uda, FunctionContext* ctx,
                              const std::vector<const arrow::Array*>& inputs) {
    return exec_batch_update_arrow_fn_(uda, ctx, inputs);
  }

  Status Merge(UDA* uda1, UDA* uda2, FunctionContext* ctx) { return merge_fn_(uda1, uda2, ctx); }
  Status FinalizeValue(UDA* uda, FunctionContext* ctx, types::BaseValueType* output) {
    return finalize_value_fn(uda, ctx, output);
  }
  Status FinalizeArrow(UDA* uda, FunctionContext* ctx, arrow::ArrayBuilder* output) {
    return finalize_arrow_fn_(uda, ctx, output);
  }

 private:
  std::vector<types::DataType> update_arguments_;
  types::DataType finalize_return_type_;

  std::function<std::unique_ptr<UDA>()> make_fn_;
  std::function<Status(UDA* uda, FunctionContext* ctx,
                       const std::vector<const types::ColumnWrapper*>& inputs)>
      exec_batch_update_fn_;

  std::function<Status(UDA* uda, FunctionContext* ctx,
                       const std::vector<const arrow::Array*>& inputs)>
      exec_batch_update_arrow_fn_;

  std::function<Status(UDA* uda, FunctionContext* ctx, arrow::ArrayBuilder* output)>
      finalize_arrow_fn_;
  std::function<Status(UDA* uda, FunctionContext* ctx, types::BaseValueType* output)>
      finalize_value_fn;
  std::function<Status(UDA* uda1, UDA* uda2, FunctionContext* ctx)> merge_fn_;
};

class UDTFDefinition : public UDFDefinition {
 public:
  UDTFDefinition() = default;
  ~UDTFDefinition() override = default;
  /**
   * Init a UDTF definition with the given name and type.
   *
   * @tparam T the UDTF class. Must be derived from UDTF.
   * @param name The name of the UDTF.
   * @return Status success/error.
   */
  template <typename T>
  Status Init(const std::string& name) {
    auto factory = std::make_unique<GenericUDTFFactory<T>>();
    return Init<T>(std::move(factory), name);
  }

  /**
   * Init a UDTF definition with the given factory function.
   * @tparam T The UDTF def.
   * @param factory The UDTF factory.
   * @param name The name of the UDTF.
   * @return Status
   */
  template <typename T>
  Status Init(std::unique_ptr<UDTFFactory> factory, const std::string& name) {
    factory_ = std::move(factory);
    // Check to make sure it's a valid UDTF.
    UDTFChecker<T> checker;
    PL_UNUSED(checker);

    PL_RETURN_IF_ERROR(UDFDefinition::Init(name));
    exec_init_ = UDTFWrapper<T>::Init;
    exec_batch_update_ = UDTFWrapper<T>::ExecBatchUpdate;

    auto init_args = UDTFTraits<T>::InitArguments();
    init_arguments_ = {init_args.begin(), init_args.end()};

    auto output_relation = UDTFTraits<T>::OutputRelation();
    output_relation_ = {output_relation.begin(), output_relation.end()};

    executor_ = UDTFTraits<T>::Executor();

    return Status::OK();
  }

  const std::vector<types::DataType>& RegistryArgTypes() override {
    // UDTF's can't be overloaded.
    return args_types;
  }

  UDTFDefinition* GetDefinition() override { return this; }

  std::unique_ptr<AnyUDTF> Make() { return factory_->Make(); }

  Status ExecInit(AnyUDTF* udtf, FunctionContext* ctx,
                  const std::vector<const types::BaseValueType*>& args) {
    return exec_init_(udtf, ctx, args);
  }

  bool ExecBatchUpdate(AnyUDTF* udtf, FunctionContext* ctx, int max_gen_records,
                       std::vector<arrow::ArrayBuilder*>* outputs) {
    return exec_batch_update_(udtf, ctx, max_gen_records, outputs);
  }

  const std::vector<UDTFArg>& init_arguments() { return init_arguments_; }
  const std::vector<ColInfo>& output_relation() { return output_relation_; }
  udfspb::UDTFSourceExecutor executor() { return executor_; }

 private:
  std::unique_ptr<UDTFFactory> factory_;
  std::function<Status(AnyUDTF*, FunctionContext*, const std::vector<const types::BaseValueType*>&)>
      exec_init_;
  std::function<bool(AnyUDTF* udtf, FunctionContext* ctx, int max_gen_records,
                     std::vector<arrow::ArrayBuilder*>* outputs)>
      exec_batch_update_;
  std::vector<UDTFArg> init_arguments_;
  std::vector<ColInfo> output_relation_;
  udfspb::UDTFSourceExecutor executor_;
  const std::vector<types::DataType>
      args_types{};  // Empty arg types because UDTF's can't be overloaded.
};

}  // namespace udf
}  // namespace carnot
}  // namespace pl
