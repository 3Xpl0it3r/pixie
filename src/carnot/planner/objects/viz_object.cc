#include "src/carnot/planner/objects/viz_object.h"

#include <utility>
#include "src/carnot/planner/objects/expr_object.h"

namespace pl {
namespace carnot {
namespace planner {
namespace compiler {

Status VisualizationObject::Init() {
  // Setup methods.
  PL_ASSIGN_OR_RETURN(std::shared_ptr<FuncObject> vega_fn,
                      FuncObject::Create(kVegaAttrId, {"vega_spec"}, {},
                                         /* has_variable_len_args */ false,
                                         /* has_variable_len_kwargs */ false,
                                         std::bind(&VegaHandler::Eval, std::placeholders::_1,
                                                   std::placeholders::_2, std::placeholders::_3),
                                         ast_visitor()));

  AddMethod(kVegaAttrId, vega_fn);
  return Status::OK();
}

StatusOr<std::shared_ptr<VisualizationObject>> VisualizationObject::Create(
    ASTVisitor* ast_visitor) {
  auto viz_object = std::shared_ptr<VisualizationObject>(new VisualizationObject(ast_visitor));
  PL_RETURN_IF_ERROR(viz_object->Init());
  return viz_object;
}

StatusOr<QLObjectPtr> VegaHandler::Eval(const pypa::AstPtr&, const ParsedArgs& args,
                                        ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(StringIR * vega_spec_ir, GetArgAs<StringIR>(args, "vega_spec"));
  std::string vega_spec = vega_spec_ir->str();
  return FuncObject::Create(VisualizationObject::kVegaAttrId, {"fn"}, {},
                            /* has_variable_len_args */ false,
                            /* has_variable_len_kwargs */ false,
                            std::bind(&VegaHandler::NestedFn, vega_spec, std::placeholders::_1,
                                      std::placeholders::_2, std::placeholders::_3),
                            visitor);
}

StatusOr<QLObjectPtr> VegaHandler::NestedFn(std::string spec, const pypa::AstPtr& ast,
                                            const ParsedArgs& args, ASTVisitor*) {
  auto fn = args.GetArg("fn");
  PL_ASSIGN_OR_RETURN(auto func, GetCallMethod(ast, fn));

  auto viz_spec = std::make_unique<VizSpec>();
  viz_spec->vega_spec = spec;
  PL_RETURN_IF_ERROR(func->AddVizSpec(std::move(viz_spec)));
  return std::static_pointer_cast<QLObject>(func);
}

}  // namespace compiler
}  // namespace planner
}  // namespace carnot
}  // namespace pl
