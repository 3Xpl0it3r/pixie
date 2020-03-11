#include <vector>

#include "src/carnot/planner/objects/collection_object.h"
#include "src/carnot/planner/objects/dataframe.h"
#include "src/carnot/planner/objects/expr_object.h"
#include "src/carnot/planner/objects/funcobject.h"
#include "src/carnot/planner/objects/qlobject.h"

namespace pl {
namespace carnot {
namespace planner {
namespace compiler {
void QLObject::AddSubscriptMethod(std::shared_ptr<FuncObject> func_object) {
  DCHECK_EQ(func_object->name(), kSubscriptMethodName);
  DCHECK(func_object->arguments() == std::vector<std::string>{"key"})
      << absl::StrJoin(func_object->arguments(), ",");
  AddMethod(kSubscriptMethodName, func_object);
}

StatusOr<std::shared_ptr<QLObject>> QLObject::GetAttribute(const pypa::AstPtr& ast,
                                                           std::string_view attr) const {
  if (HasMethod(attr)) {
    return GetMethod(attr);
  }
  if (!HasNonMethodAttribute(attr)) {
    return CreateAstError(ast, "'$0' object has no attribute '$1'", name(), attr);
  }
  return GetAttributeImpl(ast, attr);
}

Status QLObject::AssignAttribute(std::string_view attr_name, QLObjectPtr object) {
  if (!CanAssignAttribute(attr_name)) {
    return CreateError("Cannot assign attribute $0 to object of type $1", attr_name, name());
  }
  attributes_[attr_name] = object;
  return Status::OK();
}

StatusOr<QLObjectPtr> QLObject::FromIRNode(IRNode* node, ASTVisitor* ast_visitor) {
  if (Match(node, Operator())) {
    return Dataframe::Create(static_cast<OperatorIR*>(node), ast_visitor);
  } else if (Match(node, Expression())) {
    return ExprObject::Create(static_cast<ExpressionIR*>(node), ast_visitor);
  } else {
    return node->CreateIRNodeError("Could not create QL object from IRNode of type $0",
                                   node->type_string());
  }
}

}  // namespace compiler
}  // namespace planner
}  // namespace carnot
}  // namespace pl
