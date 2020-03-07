/**
 * This file handles pattern matching. The idea behind this
 * is heavily copied from LLVM's pattern matching module.
 * https://github.com/llvm-mirror/llvm/blob/master/include/llvm/IR/PatternMatch.h
 * access at commit `e30b1c0c22a69971612e014f958ab33916c99f48`.
 *
 * Using the pattern matching interface is very simple.
 *
 * To match `r.latency == 10`, you have several options based on desired specificity,
 * here are a few:
 * ```
 * IRNode* expr; // initialized in the ASTvisitor as a FuncIR.
 * // Most specific
 * if (Match(expr, Equals(Column(), Int(10)))) {
 *    // handle case
 *    ...
 * }
 * // Match any int value
 * else if (Match(expr, Equals(Column(), Int()))) {
 *    // handle case
 *    ...
 * }
 * // Match any arbitrary value
 * else if (Match(expr, Equals(Column(), Value()))) {
 *    // handle case
 *    ...
 * }
 * ```
 *
 * New patterns must fit a specific structure.
 * 1. They must inherit from ParentMatch.
 * 2. They must call the ParentMatch constructor in their own constructor.
 * 3. They must implement Match()
 * 4. To be used properly, they must be specified with a function
 *    - see the Int() fns for an example of what this looks like.
 *
 * Likely for most new patterns you won't need to implement a new struct, but
 * rather you can use an existing struct to fit your use-case.
 */
#pragma once
#include <string>
#include <unordered_map>

#include "src/carnot/planner/ir/ir_nodes.h"
namespace pl {
namespace carnot {
namespace planner {

/**
 * @brief Match function that aliases the match function attribute of a pattern.
 */
template <typename Val, typename Pattern>
bool Match(const Val* node, const Pattern& P) {
  return const_cast<Pattern&>(P).Match(node);
}

/**
 * @brief The parent struct to all of the matching structs.
 * Contains an ordering value and a type for
 * easier data structure organization in the future.
 */
struct ParentMatch {
  virtual ~ParentMatch() = default;
  explicit ParentMatch(IRNodeType t) : type(t) {}

  /**
   * @brief Match returns true if the node passed in fits the pattern defined by the struct.
   * @param node: IRNode argument to examine.
   */
  virtual bool Match(const IRNode* node) const = 0;

  IRNodeType type;
};

/**
 * @brief Match any possible node.
 * It evaluates to true no matter what you throw in there.
 */
struct AllMatch : public ParentMatch {
  AllMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode*) const override { return true; }
};

/**
 * @brief Match any valid IRNode.
 */
inline AllMatch Value() { return AllMatch(); }

/**
 * @brief matches
 *
 * @tparam t The IrNodeType
 */
template <IRNodeType t>
struct ClassMatch : public ParentMatch {
  ClassMatch() : ParentMatch(t) {}
  bool Match(const IRNode* node) const override { return node->type() == type; }
};

// Match an arbitrary Int value.
inline ClassMatch<IRNodeType::kInt> Int() { return ClassMatch<IRNodeType::kInt>(); }

// Match an arbitrary String value.
inline ClassMatch<IRNodeType::kString> String() { return ClassMatch<IRNodeType::kString>(); }

// Match an arbitrary Metadata value.
inline ClassMatch<IRNodeType::kMetadata> Metadata() { return ClassMatch<IRNodeType::kMetadata>(); }

// Match an arbitrary Metadata value.
inline ClassMatch<IRNodeType::kFunc> Func() { return ClassMatch<IRNodeType::kFunc>(); }

inline ClassMatch<IRNodeType::kMemorySource> MemorySource() {
  return ClassMatch<IRNodeType::kMemorySource>();
}
inline ClassMatch<IRNodeType::kMemorySink> MemorySink() {
  return ClassMatch<IRNodeType::kMemorySink>();
}
inline ClassMatch<IRNodeType::kLimit> Limit() { return ClassMatch<IRNodeType::kLimit>(); }

// Match an arbitrary MetadataLiteral value.
inline ClassMatch<IRNodeType::kMetadataLiteral> MetadataLiteral() {
  return ClassMatch<IRNodeType::kMetadataLiteral>();
}

// Match an arbitrary MetadataResolver operator.
inline ClassMatch<IRNodeType::kMetadataResolver> MetadataResolver() {
  return ClassMatch<IRNodeType::kMetadataResolver>();
}
inline ClassMatch<IRNodeType::kGRPCSource> GRPCSource() {
  return ClassMatch<IRNodeType::kGRPCSource>();
}
inline ClassMatch<IRNodeType::kGRPCSourceGroup> GRPCSourceGroup() {
  return ClassMatch<IRNodeType::kGRPCSourceGroup>();
}
inline ClassMatch<IRNodeType::kGRPCSink> GRPCSink() { return ClassMatch<IRNodeType::kGRPCSink>(); }

inline ClassMatch<IRNodeType::kJoin> Join() { return ClassMatch<IRNodeType::kJoin>(); }
inline ClassMatch<IRNodeType::kUnion> Union() { return ClassMatch<IRNodeType::kUnion>(); }
inline ClassMatch<IRNodeType::kTabletSourceGroup> TabletSourceGroup() {
  return ClassMatch<IRNodeType::kTabletSourceGroup>();
}

inline ClassMatch<IRNodeType::kGroupBy> GroupBy() { return ClassMatch<IRNodeType::kGroupBy>(); }
inline ClassMatch<IRNodeType::kRolling> Rolling() { return ClassMatch<IRNodeType::kRolling>(); }
inline ClassMatch<IRNodeType::kUDTFSource> UDTFSource() {
  return ClassMatch<IRNodeType::kUDTFSource>();
}
inline ClassMatch<IRNodeType::kUInt128> UInt128Value() {
  return ClassMatch<IRNodeType::kUInt128>();
}

/* Match GRPCSink with a specific source ID */
struct GRPCSinkWithSourceID : public ParentMatch {
  explicit GRPCSinkWithSourceID(int64_t source_id)
      : ParentMatch(IRNodeType::kGRPCSink), source_id_(source_id) {}
  bool Match(const IRNode* node) const override {
    return GRPCSink().Match(node) &&
           static_cast<const GRPCSinkIR*>(node)->destination_id() == source_id_;
  }

 private:
  int64_t source_id_;
};

/**
 * @brief Match a specific integer value.
 */
struct IntMatch : public ParentMatch {
  explicit IntMatch(const int64_t v) : ParentMatch(IRNodeType::kInt), val(v) {}

  bool Match(const IRNode* node) const override {
    if (node->type() == type) {
      auto iVal = static_cast<const IntIR*>(node);
      return iVal->val() == val;
    }
    return false;
  }

  int64_t val;
};

/**
 * @brief Match a specific integer value.
 */
inline IntMatch Int(const int64_t val) { return IntMatch(val); }

/**
 * @brief Match a tablet ID type.
 */
inline ClassMatch<IRNodeType::kString> TabletValue() { return String(); }

/**
 * @brief Match specific binary functions.
 *
 * @tparam LHS_t: the left hand type.
 * @tparam RHS_t: the right hand type.
 * @tparam op: the opcode to match for this Binary operator.
 * @tparam commmutable: whether we can swap left and right arguments.
 */
template <typename LHS_t, typename RHS_t, FuncIR::Opcode op, bool Commutable = false>
struct BinaryOpMatch : public ParentMatch {
  // The evaluation order is always stable, regardless of Commutability.
  // The LHS is always matched first.
  BinaryOpMatch(const LHS_t& LHS, const RHS_t& RHS)
      : ParentMatch(IRNodeType::kFunc), L(LHS), R(RHS) {}

  bool Match(const IRNode* node) const override {
    if (node->type() == IRNodeType::kFunc) {
      auto* F = static_cast<const FuncIR*>(node);
      if (F->opcode() == op && F->args().size() == 2) {
        return (L.Match(F->args()[0]) && R.Match(F->args()[1])) ||
               (Commutable && L.Match(F->args()[1]) && R.Match(F->args()[0]));
      }
    }
    return false;
  }

  LHS_t L;
  RHS_t R;
};

/**
 * @brief Match equals functions that match the left and right operators. It is commutative.
 */
template <typename LHS, typename RHS>
inline BinaryOpMatch<LHS, RHS, FuncIR::Opcode::eq, true> Equals(const LHS& L, const RHS& R) {
  return BinaryOpMatch<LHS, RHS, FuncIR::Opcode::eq, true>(L, R);
}

/**
 * @brief Match equals functions that match the left and right operators. It is commutative.
 */
template <typename LHS, typename RHS>
inline BinaryOpMatch<LHS, RHS, FuncIR::Opcode::logand, true> LogicalAnd(const LHS& L,
                                                                        const RHS& R) {
  return BinaryOpMatch<LHS, RHS, FuncIR::Opcode::logand, true>(L, R);
}

inline BinaryOpMatch<AllMatch, AllMatch, FuncIR::Opcode::logand, true> LogicalAnd() {
  return LogicalAnd(Value(), Value());
}

/**
 * @brief Match equals functions that match the left and right operators. It is commutative.
 */
template <typename LHS, typename RHS>
inline BinaryOpMatch<LHS, RHS, FuncIR::Opcode::lt, false> LessThan(const LHS& L, const RHS& R) {
  return BinaryOpMatch<LHS, RHS, FuncIR::Opcode::lt, false>(L, R);
}

/**
 * @brief Match subtract functions that match the left and right operators. It is notcommutative.
 */
template <typename LHS, typename RHS>
inline BinaryOpMatch<LHS, RHS, FuncIR::Opcode::sub, false> Subtract(const LHS& L, const RHS& R) {
  return BinaryOpMatch<LHS, RHS, FuncIR::Opcode::sub, false>(L, R);
}

/**
 * @brief Match modulo functions that match the left and right operators. It is notcommutative.
 */
template <typename LHS, typename RHS>
inline BinaryOpMatch<LHS, RHS, FuncIR::Opcode::mod, false> Modulo(const LHS& L, const RHS& R) {
  return BinaryOpMatch<LHS, RHS, FuncIR::Opcode::mod, false>(L, R);
}

/**
 * @brief Match any binary function.
 */
template <typename LHS_t, typename RHS_t, bool Commutable = false>
struct AnyBinaryOpMatch : public ParentMatch {
  // The evaluation order is always stable, regardless of Commutability.
  // The LHS is always matched first.
  AnyBinaryOpMatch(const LHS_t& LHS, const RHS_t& RHS)
      : ParentMatch(IRNodeType::kFunc), L(LHS), R(RHS) {}

  bool Match(const IRNode* node) const override {
    if (node->type() == type) {
      auto* F = static_cast<const FuncIR*>(node);
      if (F->args().size() == 2) {
        return (L.Match(F->args()[0]) && R.Match(F->args()[1])) ||
               (Commutable && L.Match(F->args()[1]) && R.Match(F->args()[0]));
      }
    }
    return false;
  }

  LHS_t L;
  RHS_t R;
};

/**
 * @brief Matches any BinaryOperation that fits the Left and Right conditions
 * exactly (non-commutative).
 */
template <typename LHS, typename RHS>
inline AnyBinaryOpMatch<LHS, RHS, false> BinOp(const LHS& L, const RHS& R) {
  return AnyBinaryOpMatch<LHS, RHS>(L, R);
}

/**
 * @brief Match any binary op, no need to specify args.
 */
inline AnyBinaryOpMatch<AllMatch, AllMatch, false> BinOp() { return BinOp(Value(), Value()); }

/**
 * @brief Match any expression type.
 */
template <bool resolved>
struct ExpressionMatch : public ParentMatch {
  ExpressionMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override {
    if (node->IsExpression()) {
      return resolved == static_cast<const ExpressionIR*>(node)->IsDataTypeEvaluated();
    }
    return false;
  }
};
/**
 * @brief Match an expression that has been resolved.
 */
inline ExpressionMatch<true> ResolvedExpression() { return ExpressionMatch<true>(); }

/**
 * @brief Match any expression that has not yet been resolved.
 */
inline ExpressionMatch<false> UnresolvedExpression() { return ExpressionMatch<false>(); }

struct ExpressionMatchDataType : public ParentMatch {
  explicit ExpressionMatchDataType(types::DataType type)
      : ParentMatch(IRNodeType::kAny), type_(type) {}
  bool Match(const IRNode* node) const override {
    if (!node->IsExpression()) {
      return false;
    }
    const ExpressionIR* expr = static_cast<const ExpressionIR*>(node);
    return expr->IsDataTypeEvaluated() && expr->EvaluatedDataType() == type_;
  }
  types::DataType type_;
};

inline ExpressionMatchDataType Expression(types::DataType type) {
  return ExpressionMatchDataType(type);
}

/**
 * @brief Match a specifically typed expression that has a given resolution state.
 *
 * @tparam expression_type: the type of the node to match (must be an expression).
 * @tparam Resolved: expected resolution of pattern.
 */
template <IRNodeType expression_type, bool Resolved>
struct SpecificExpressionMatch : public ParentMatch {
  SpecificExpressionMatch() : ParentMatch(expression_type) {}
  bool Match(const IRNode* node) const override {
    if (node->IsExpression() && node->type() == expression_type) {
      return Resolved == static_cast<const ExpressionIR*>(node)->IsDataTypeEvaluated();
    }
    return false;
  }
};

/**
 * @brief Match a column that is not resolved.
 */
inline SpecificExpressionMatch<IRNodeType::kColumn, false> UnresolvedColumnType() {
  return SpecificExpressionMatch<IRNodeType::kColumn, false>();
}

/**
 * @brief Match a column that is resolved.
 */
inline SpecificExpressionMatch<IRNodeType::kColumn, true> ResolvedColumnType() {
  return SpecificExpressionMatch<IRNodeType::kColumn, true>();
}

/**
 * @brief Match a function that is not resolved.
 */
inline SpecificExpressionMatch<IRNodeType::kFunc, false> UnresolvedFuncType() {
  return SpecificExpressionMatch<IRNodeType::kFunc, false>();
}

/**
 * @brief Match a function that is resolved.
 */
inline SpecificExpressionMatch<IRNodeType::kFunc, true> ResolvedFuncType() {
  return SpecificExpressionMatch<IRNodeType::kFunc, true>();
}

/**
 * @brief Match metadata ir that has yet to resolve data type.
 */
inline SpecificExpressionMatch<IRNodeType::kMetadata, false> UnresolvedMetadataType() {
  return SpecificExpressionMatch<IRNodeType::kMetadata, false>();
}

/**
 * @brief Match a metadataIR node that has either been Resolved by a metadata
 * resolver node, or not.
 *
 * @tparam Resolved: whether the metadata has been resolved with a resovler node.
 */
template <bool Resolved>
struct MetadataIRMatch : public ParentMatch {
  MetadataIRMatch() : ParentMatch(IRNodeType::kMetadata) {}
  bool Match(const IRNode* node) const override {
    if (node->type() == IRNodeType::kMetadata) {
      return Resolved == static_cast<const MetadataIR*>(node)->HasMetadataResolver();
    }
    return false;
  }
};

/**
 * @brief Match a MetadataIR that doesn't have an associated MetadataResolver node.
 */
inline MetadataIRMatch<false> UnresolvedMetadataIR() { return MetadataIRMatch<false>(); }

/**
 * @brief Match Compile Time integer arithmetic
 * TODO(nserrino, philkuz) Generalize this better, currently just a special case for MemorySource
 * times.
 */
struct CompileTimeIntegerArithmetic : public ParentMatch {
  CompileTimeIntegerArithmetic() : ParentMatch(IRNodeType::kFunc) {}

  bool Match(const IRNode* node) const override;
  bool ArgMatches(const IRNode* arg) const;
};

// TODO(nserrino,philkuz) Move UDF function names into a centralized place.
const std::unordered_map<std::string, std::chrono::nanoseconds> kUnitTimeFnStr = {
    {"minutes", std::chrono::minutes(1)},           {"hours", std::chrono::hours(1)},
    {"seconds", std::chrono::seconds(1)},           {"days", std::chrono::hours(24)},
    {"microseconds", std::chrono::microseconds(1)}, {"milliseconds", std::chrono::milliseconds(1)}};

const char kTimeNowFnStr[] = "now";

/**
 * @brief Match Compile Time now() function
 */
struct CompileTimeNow : public ParentMatch {
  CompileTimeNow() : ParentMatch(IRNodeType::kFunc) {}

  bool Match(const IRNode* node) const override;
};

/**
 * @brief Match Compile Time minutes(2), etc functions
 */
struct CompileTimeUnitTime : public ParentMatch {
  CompileTimeUnitTime() : ParentMatch(IRNodeType::kFunc) {}

  bool Match(const IRNode* node) const override;
};

/**
 * @brief Matches funcs we can execute at compile time.
 * TODO(nserrino, philkuz) Implement more robust constant-folding rather than just a few one-offs.
 */
template <bool CompileTime = false>
struct CompileTimeFuncMatch : public ParentMatch {
  bool match_compile_time = CompileTime;

  // The evaluation order is always stable, regardless of Commutability.
  // The LHS is always matched first.
  CompileTimeFuncMatch() : ParentMatch(IRNodeType::kFunc) {}

  bool Match(const IRNode* node) const override {
    if (!Func().Match(node)) {
      return false;
    }
    return match_compile_time == MatchCompileTimeFunc(static_cast<const FuncIR*>(node));
  }

 private:
  bool MatchCompileTimeFunc(const FuncIR* func) const {
    // TODO(nserrino): This selection of compile time evaluation is extremely limited.
    // We should add in more generalized constant folding at compile time.
    if (CompileTimeNow().Match(func)) {
      return true;
    }
    if (CompileTimeUnitTime().Match(func)) {
      return true;
    }
    if (CompileTimeIntegerArithmetic().Match(func)) {
      return true;
    }
    return false;
  }
};

/**
 * @brief Match compile-time function.
 */
inline CompileTimeFuncMatch<true> CompileTimeFunc() { return CompileTimeFuncMatch<true>(); }

/**
 * @brief Match run-time function.
 */
inline CompileTimeFuncMatch<false> RunTimeFunc() { return CompileTimeFuncMatch<false>(); }

/**
 * @brief Match any function that contains a compile time function inside.
 */
struct ContainsCompileTimeFunc : public ParentMatch {
  ContainsCompileTimeFunc() : ParentMatch(IRNodeType::kFunc) {}
  bool Match(const IRNode* node) const override;
};

/**
 * @brief Match any function with arguments that satisfy argMatcher and matches the specified
 * Resolution and CompileTime values.
 *
 * @tparam Arg_t
 * @tparam false
 * @tparam false
 */
template <typename Arg_t, bool Resolved = false, bool CompileTime = false>
struct AnyFuncAllArgsMatch : public ParentMatch {
  explicit AnyFuncAllArgsMatch(const Arg_t& argMatcher)
      : ParentMatch(IRNodeType::kFunc), argMatcher_(argMatcher) {}

  bool Match(const IRNode* node) const override {
    if (node->type() == type) {
      auto* F = static_cast<const FuncIR*>(node);
      CompileTimeFuncMatch<CompileTime> compile_or_rt_matcher;
      if (Resolved == F->IsDataTypeEvaluated() && compile_or_rt_matcher.Match(F)) {
        for (const auto a : F->args()) {
          if (!argMatcher_.Match(a)) {
            return false;
          }
        }
        return true;
      }
    }
    return false;
  }

  Arg_t argMatcher_;
};

/**
 * @brief Matches unresolved & runtime functions with args that satisfy
 * argMatcher.
 *
 * @tparam Arg_t: The type of the argMatcher.
 * @param argMatcher: The pattern that must be satisfied for all arguments.
 */
template <typename Arg_t>
inline AnyFuncAllArgsMatch<Arg_t, false> UnresolvedRTFuncMatchAllArgs(const Arg_t& argMatcher) {
  return AnyFuncAllArgsMatch<Arg_t, false, false>(argMatcher);
}

/**
 * @brief Matches any function that has an argument that matches the passed
 * in matcher and is a compile time function.
 *
 */
template <typename Arg_t, bool CompileTime = false>
struct AnyFuncAnyArgsMatch : public ParentMatch {
  explicit AnyFuncAnyArgsMatch(const Arg_t& argMatcher)
      : ParentMatch(IRNodeType::kFunc), argMatcher_(argMatcher) {}

  bool Match(const IRNode* node) const override {
    if (node->type() == type) {
      auto* F = static_cast<const FuncIR*>(node);
      CompileTimeFuncMatch<CompileTime> compile_or_rt_matcher;
      if (compile_or_rt_matcher.Match(F)) {
        for (const auto a : F->args()) {
          if (argMatcher_.Match(a)) {
            return true;
          }
        }
      }
    }
    return false;
  }
  Arg_t argMatcher_;
};

/**
 * @brief Matches runtime functions with any arg that satisfies
 * argMatcher.
 *
 * @tparam Arg_t: The type of the argMatcher.
 * @param argMatcher: The pattern that must be satisfied for all arguments.
 */
template <typename Arg_t>
inline AnyFuncAnyArgsMatch<Arg_t, false> FuncAnyArg(const Arg_t& argMatcher) {
  return AnyFuncAnyArgsMatch<Arg_t, false>(argMatcher);
}

/**
 * @brief Match a function with opcode op whose arguments satisfy the arg_matcher.
 *
 * @tparam Arg_t
 * @tparam false
 * @tparam false
 */
template <typename ArgMatcherType, FuncIR::Opcode op>
struct FuncAllArgsMatch : public ParentMatch {
  explicit FuncAllArgsMatch(const ArgMatcherType& arg_matcher)
      : ParentMatch(IRNodeType::kFunc), arg_matcher_(arg_matcher) {}

  bool Match(const IRNode* node) const override {
    if (node->type() == type) {
      auto* func = static_cast<const FuncIR*>(node);
      if (func->opcode() == op) {
        for (const auto a : func->args()) {
          if (!arg_matcher_.Match(a)) {
            return false;
          }
        }
        return true;
      }
    }
    return false;
  }

  ArgMatcherType arg_matcher_;
};

template <typename ArgMatcherType>
inline FuncAllArgsMatch<ArgMatcherType, FuncIR::Opcode::logand> AndFnMatchAll(
    const ArgMatcherType& arg_matcher) {
  return FuncAllArgsMatch<ArgMatcherType, FuncIR::Opcode::logand>(arg_matcher);
}

/**
 * @brief Match any node that is an expression.
 */
struct AnyExpressionMatch : public ParentMatch {
  AnyExpressionMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override { return node->IsExpression(); }
};

/**
 * @brief Match any node that is an expression.
 */
inline AnyExpressionMatch Expression() { return AnyExpressionMatch(); }

/**
 * @brief Match a MemorySource operation that has the expected relation status.
 *
 * @tparam HasRelation: whether the MemorySource should have a relation set or not.
 */
template <bool HasRelation = false>
struct SourceHasRelationMatch : public ParentMatch {
  SourceHasRelationMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override {
    if (!node->IsOperator()) {
      return false;
    }
    const OperatorIR* op = static_cast<const OperatorIR*>(node);
    return op->is_source() && op->IsRelationInit() == HasRelation;
  }
};

inline SourceHasRelationMatch<false> UnresolvedSource() { return SourceHasRelationMatch<false>(); }
inline SourceHasRelationMatch<true> ResolvedSource() { return SourceHasRelationMatch<true>(); }

struct SourceOperator : public ParentMatch {
  SourceOperator() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override {
    if (!node->IsOperator()) {
      return false;
    }
    const OperatorIR* op = static_cast<const OperatorIR*>(node);
    return op->is_source();
  }
};

/**
 * @brief Match any operator that matches the Relation Init status and the parent's
 * relation init status.
 *
 * @tparam ResolvedRelation: whether this operator should have a resolved relation.
 * @tparam ParentsOpResolved: whether the parent op relation should be resolved.
 */
template <bool ResolvedRelation = false, bool ParentOpResolved = false>
struct AnyRelationResolvedOpMatch : public ParentMatch {
  AnyRelationResolvedOpMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override {
    if (node->IsOperator()) {
      const OperatorIR* op_ir = static_cast<const OperatorIR*>(node);
      if (op_ir->HasParents() && op_ir->IsRelationInit() == ResolvedRelation) {
        for (OperatorIR* parent : op_ir->parents()) {
          if (parent->IsRelationInit() != ParentOpResolved) {
            return false;
          }
        }
        return true;
      }
    }
    return false;
  }
};

/**
 * @brief Match an operator of type Matcher that matches the Relation Init status and the parent's
 * relation init status.
 *
 * @tparam Matcher: the type of the matcher for the op.
 * @tparam ResolvedRelation: whether this operator should have a resolved relation.
 * @tparam ParentsOpResolved: whether the parent op relation should be resolved.
 */
template <typename Matcher, bool ResolvedRelation = false, bool ParentOpResolved = false>
struct RelationResolvedOpSpecialMatch : public ParentMatch {
  explicit RelationResolvedOpSpecialMatch(Matcher matcher)
      : ParentMatch(IRNodeType::kAny), matcher_(matcher) {}
  bool Match(const IRNode* node) const override {
    if (matcher_.Match(node)) {
      return AnyRelationResolvedOpMatch<ResolvedRelation, ParentOpResolved>().Match(node);
    }
    return false;
  }
  Matcher matcher_;
};

/**
 * @brief Match Any operator that doesn't have a relation but the parent does.
 */
inline AnyRelationResolvedOpMatch<false, true> UnresolvedReadyOp() {
  return AnyRelationResolvedOpMatch<false, true>();
}

/**
 * @brief Match a Join node that doesn't have a relation but it's parents do.
 */
template <typename Matcher>
inline RelationResolvedOpSpecialMatch<Matcher, false, true> UnresolvedReadyOp(Matcher m) {
  return RelationResolvedOpSpecialMatch<Matcher, false, true>(m);
}

struct MatchAnyOp : public ParentMatch {
  // The evaluation order is always stable, regardless of Commutability.
  // The LHS is always matched first.
  MatchAnyOp() : ParentMatch(IRNodeType::kAny) {}

  bool Match(const IRNode* node) const override { return node->IsOperator(); }
};

inline MatchAnyOp Operator() { return MatchAnyOp(); }

/**
 * @brief Match map operator.
 */
inline ClassMatch<IRNodeType::kMap> Map() { return ClassMatch<IRNodeType::kMap>(); }

/**
 * @brief Match drop operator.
 */
inline ClassMatch<IRNodeType::kDrop> Drop() { return ClassMatch<IRNodeType::kDrop>(); }

/**
 * @brief Match blocking_agg operator.
 */
inline ClassMatch<IRNodeType::kBlockingAgg> BlockingAgg() {
  return ClassMatch<IRNodeType::kBlockingAgg>();
}

/**
 * @brief Match Filter operator.
 */
inline ClassMatch<IRNodeType::kFilter> Filter() { return ClassMatch<IRNodeType::kFilter>(); }

struct ColumnMatch : public ParentMatch {
  ColumnMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override {
    return node->IsExpression() && static_cast<const ExpressionIR*>(node)->IsColumn();
  }
};

inline ColumnMatch ColumnNode() { return ColumnMatch(); }

template <bool MatchName, bool MatchIdx>
struct ColumnPropMatch : public ParentMatch {
  explicit ColumnPropMatch(const std::string& name, int64_t idx)
      : ParentMatch(IRNodeType::kColumn), name_(name), idx_(idx) {}
  bool Match(const IRNode* node) const override {
    if (ColumnNode().Match(node)) {
      const ColumnIR* col_node = static_cast<const ColumnIR*>(node);
      // If matchName, check match name.
      // If MatchIdx, then check the idx.
      return (!MatchName || col_node->col_name() == name_) &&
             (!MatchIdx || col_node->container_op_parent_idx() == idx_);
    }
    return false;
  }
  const std::string& name_;
  int64_t idx_;
};

inline ColumnPropMatch<true, false> ColumnNode(const std::string& name) {
  return ColumnPropMatch<true, false>(name, 0);
}
inline ColumnPropMatch<true, true> ColumnNode(const std::string& name, int64_t parent_idx) {
  return ColumnPropMatch<true, true>(name, parent_idx);
}

struct DataMatch : public ParentMatch {
  DataMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override {
    return node->IsExpression() && static_cast<const ExpressionIR*>(node)->IsData();
  }
};

inline DataMatch DataNode() { return DataMatch(); }

struct BlockingOperatorMatch : public ParentMatch {
  BlockingOperatorMatch() : ParentMatch(IRNodeType::kAny) {}
  bool Match(const IRNode* node) const override {
    return node->IsOperator() && static_cast<const OperatorIR*>(node)->IsBlocking();
  }
};

inline BlockingOperatorMatch BlockingOperator() { return BlockingOperatorMatch(); }

/**
 * @brief Matches two operators in sequence.
 *
 */
template <typename ParentType, typename ChildType>
struct OperatorChainMatch : public ParentMatch {
  OperatorChainMatch(ParentType parent, ChildType child)
      : ParentMatch(IRNodeType::kAny), parent_(parent), child_(child) {}
  bool Match(const IRNode* node) const override {
    if (!node->IsOperator()) {
      return false;
    }
    auto op_node = static_cast<const OperatorIR*>(node);
    if (op_node->Children().size() != 1 || !parent_.Match(op_node)) {
      return false;
    }
    return child_.Match(op_node->Children()[0]);
  }

 private:
  ParentType parent_;
  ChildType child_;
};

template <typename ParentType, typename ChildType>
inline OperatorChainMatch<ParentType, ChildType> OperatorChain(ParentType parent, ChildType child) {
  return OperatorChainMatch(parent, child);
}

template <JoinIR::JoinType Type>
struct JoinMatch : public ParentMatch {
  JoinMatch() : ParentMatch(IRNodeType::kJoin) {}
  bool Match(const IRNode* node) const override {
    if (!Join().Match(node)) {
      return false;
    }
    auto join = static_cast<const JoinIR*>(node);
    return join->join_type() == Type;
  }

 private:
  std::string join_type_;
};

inline JoinMatch<JoinIR::JoinType::kRight> RightJoin() {
  return JoinMatch<JoinIR::JoinType::kRight>();
}

template <typename OpType, typename ParentType>
struct ParentOfOpMatcher : public ParentMatch {
  explicit ParentOfOpMatcher(OpType op_matcher, ParentType parent_matcher)
      : ParentMatch(IRNodeType::kAny), op_matcher_(op_matcher), parent_matcher_(parent_matcher) {}
  bool Match(const IRNode* node) const override {
    if (!op_matcher_.Match(node)) {
      return false;
    }
    // Make sure that we can cast into operator.
    DCHECK(Operator().Match(node));
    auto op = static_cast<const OperatorIR*>(node);
    for (const auto& p : op->parents()) {
      if (!parent_matcher_.Match(p)) {
        return false;
      }
    }
    return true;
  }

  OpType op_matcher_;
  ParentType parent_matcher_;
};
template <typename OpType, typename ParentType>
inline ParentOfOpMatcher<OpType, ParentType> OperatorWithParent(OpType op_matcher,
                                                                ParentType parent_matcher) {
  return ParentOfOpMatcher<OpType, ParentType>(op_matcher, parent_matcher);
}

template <bool OutputColumnsAreSet>
struct OutputColumnsJoinMatcher : public ParentMatch {
  OutputColumnsJoinMatcher() : ParentMatch(IRNodeType::kJoin) {}
  bool Match(const IRNode* node) const override {
    if (!Join().Match(node)) {
      return false;
    }
    auto join = static_cast<const JoinIR*>(node);
    if (OutputColumnsAreSet) {
      return join->output_columns().size() != 0;
    }
    return join->output_columns().size() == 0;
  }
};

inline OutputColumnsJoinMatcher<false> UnsetOutputColumnsJoin() {
  return OutputColumnsJoinMatcher<false>();
}

struct DataOfType : public ParentMatch {
  explicit DataOfType(types::DataType type) : ParentMatch(IRNodeType::kAny), type_(type) {}

  bool Match(const IRNode* node) const override {
    if (!DataNode().Match(node)) {
      return false;
    }
    auto data = static_cast<const DataIR*>(node);
    return data->EvaluatedDataType() == type_;
  }

  types::DataType type_;
};

}  // namespace planner
}  // namespace carnot
}  // namespace pl
