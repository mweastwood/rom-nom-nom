#ifndef CORE_C_AST_H_
#define CORE_C_AST_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rom_nom_nom {

// Represents a C type (primitive, typedef, pointer, or array).
struct CType {
  std::string name = "void";
  bool is_const = false;
  int pointer_depth = 0;

  std::string ToString() const;
  CType MakePointer() const;

  static CType Void();
  static CType S8();
  static CType U8();
  static CType S16();
  static CType U16();
  static CType S32();
  static CType U32();
  static CType F32();
  static CType F64();
  static CType Named(std::string type_name);
};

// Classification of C expressions.
enum class CExpressionKind {
  kIntegerLiteral,
  kFloatLiteral,
  kStringLiteral,
  kIdentifier,
  kUnaryExpression,
  kBinaryExpression,
  kAssignmentExpression,
  kCallExpression,
  kCastExpression,
  kMemberAccessExpression,
  kArrayIndexExpression,
  kTernaryExpression,
};

// Abstract base class for all C expressions.
class CExpression {
 public:
  virtual ~CExpression() = default;

  virtual CExpressionKind Kind() const = 0;
  virtual std::string ToString() const = 0;
  virtual std::unique_ptr<CExpression> Clone() const = 0;

  // Static helper factories
  static std::unique_ptr<CExpression> Integer(int64_t value, bool is_hex = false,
                                              bool is_unsigned = false);
  static std::unique_ptr<CExpression> Float(double value, bool is_float = true);
  static std::unique_ptr<CExpression> String(std::string value);
  static std::unique_ptr<CExpression> Identifier(std::string name);
  static std::unique_ptr<CExpression> Unary(std::string op, std::unique_ptr<CExpression> operand,
                                            bool is_prefix = true);
  static std::unique_ptr<CExpression> Binary(std::string op, std::unique_ptr<CExpression> lhs,
                                             std::unique_ptr<CExpression> rhs);
  static std::unique_ptr<CExpression> Assignment(std::string op, std::unique_ptr<CExpression> lhs,
                                                 std::unique_ptr<CExpression> rhs);
  static std::unique_ptr<CExpression> Call(std::unique_ptr<CExpression> callee,
                                           std::vector<std::unique_ptr<CExpression>> arguments);
  static std::unique_ptr<CExpression> Cast(CType target_type, std::unique_ptr<CExpression> operand);
  static std::unique_ptr<CExpression> MemberAccess(std::unique_ptr<CExpression> object,
                                                   std::string member_name, bool is_arrow = false);
  static std::unique_ptr<CExpression> ArrayIndex(std::unique_ptr<CExpression> array,
                                                 std::unique_ptr<CExpression> index);
  static std::unique_ptr<CExpression> Ternary(std::unique_ptr<CExpression> condition,
                                              std::unique_ptr<CExpression> true_expression,
                                              std::unique_ptr<CExpression> false_expression);
};

// Integer literal expression (e.g. 42, 0x801F2340).
class IntegerLiteralExpression : public CExpression {
 public:
  explicit IntegerLiteralExpression(int64_t value, bool is_hex = false, bool is_unsigned = false)
      : value_(value), is_hex_(is_hex), is_unsigned_(is_unsigned) {}

  CExpressionKind Kind() const override { return CExpressionKind::kIntegerLiteral; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  int64_t Value() const { return value_; }
  bool IsHex() const { return is_hex_; }
  bool IsUnsigned() const { return is_unsigned_; }

 private:
  int64_t value_;
  bool is_hex_;
  bool is_unsigned_;
};

// Floating point literal expression (e.g. 1.0f).
class FloatLiteralExpression : public CExpression {
 public:
  explicit FloatLiteralExpression(double value, bool is_float = true)
      : value_(value), is_float_(is_float) {}

  CExpressionKind Kind() const override { return CExpressionKind::kFloatLiteral; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  double Value() const { return value_; }
  bool IsFloat() const { return is_float_; }

 private:
  double value_;
  bool is_float_;
};

// String literal expression (e.g. "hello\n").
class StringLiteralExpression : public CExpression {
 public:
  explicit StringLiteralExpression(std::string value) : value_(std::move(value)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kStringLiteral; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const std::string& Value() const { return value_; }

 private:
  std::string value_;
};

// Identifier expression (e.g. variable name, global name).
class IdentifierExpression : public CExpression {
 public:
  explicit IdentifierExpression(std::string name) : name_(std::move(name)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kIdentifier; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const std::string& Name() const { return name_; }

 private:
  std::string name_;
};

// Unary operator expression (e.g. -x, ~x, !x, *ptr, &x).
class UnaryExpression : public CExpression {
 public:
  UnaryExpression(std::string op, std::unique_ptr<CExpression> operand, bool is_prefix = true)
      : op_(std::move(op)), operand_(std::move(operand)), is_prefix_(is_prefix) {}

  CExpressionKind Kind() const override { return CExpressionKind::kUnaryExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const std::string& Op() const { return op_; }
  const CExpression& Operand() const { return *operand_; }
  bool IsPrefix() const { return is_prefix_; }

 private:
  std::string op_;
  std::unique_ptr<CExpression> operand_;
  bool is_prefix_;
};

// Binary operator expression (e.g. a + b, a == b).
class BinaryExpression : public CExpression {
 public:
  BinaryExpression(std::string op, std::unique_ptr<CExpression> lhs,
                   std::unique_ptr<CExpression> rhs)
      : op_(std::move(op)), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kBinaryExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const std::string& Op() const { return op_; }
  const CExpression& Lhs() const { return *lhs_; }
  const CExpression& Rhs() const { return *rhs_; }

 private:
  std::string op_;
  std::unique_ptr<CExpression> lhs_;
  std::unique_ptr<CExpression> rhs_;
};

// Assignment expression (e.g. a = b, a += b).
class AssignmentExpression : public CExpression {
 public:
  AssignmentExpression(std::string op, std::unique_ptr<CExpression> lhs,
                       std::unique_ptr<CExpression> rhs)
      : op_(std::move(op)), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kAssignmentExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const std::string& Op() const { return op_; }
  const CExpression& Lhs() const { return *lhs_; }
  const CExpression& Rhs() const { return *rhs_; }

 private:
  std::string op_;
  std::unique_ptr<CExpression> lhs_;
  std::unique_ptr<CExpression> rhs_;
};

// Function call expression (e.g. func(a, b)).
class CallExpression : public CExpression {
 public:
  CallExpression(std::unique_ptr<CExpression> callee,
                 std::vector<std::unique_ptr<CExpression>> arguments)
      : callee_(std::move(callee)), arguments_(std::move(arguments)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kCallExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const CExpression& Callee() const { return *callee_; }
  const std::vector<std::unique_ptr<CExpression>>& Arguments() const { return arguments_; }

 private:
  std::unique_ptr<CExpression> callee_;
  std::vector<std::unique_ptr<CExpression>> arguments_;
};

// Explicit type cast expression (e.g. (u32)x, (Gfx*)p).
class CastExpression : public CExpression {
 public:
  CastExpression(CType target_type, std::unique_ptr<CExpression> operand)
      : target_type_(std::move(target_type)), operand_(std::move(operand)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kCastExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const CType& TargetType() const { return target_type_; }
  const CExpression& Operand() const { return *operand_; }

 private:
  CType target_type_;
  std::unique_ptr<CExpression> operand_;
};

// Member access expression (e.g. obj.field, ptr->field).
class MemberAccessExpression : public CExpression {
 public:
  MemberAccessExpression(std::unique_ptr<CExpression> object, std::string member_name,
                         bool is_arrow = false)
      : object_(std::move(object)), member_name_(std::move(member_name)), is_arrow_(is_arrow) {}

  CExpressionKind Kind() const override { return CExpressionKind::kMemberAccessExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const CExpression& Object() const { return *object_; }
  const std::string& MemberName() const { return member_name_; }
  bool IsArrow() const { return is_arrow_; }

 private:
  std::unique_ptr<CExpression> object_;
  std::string member_name_;
  bool is_arrow_;
};

// Array index subscript expression (e.g. arr[index]).
class ArrayIndexExpression : public CExpression {
 public:
  ArrayIndexExpression(std::unique_ptr<CExpression> array, std::unique_ptr<CExpression> index)
      : array_(std::move(array)), index_(std::move(index)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kArrayIndexExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const CExpression& Array() const { return *array_; }
  const CExpression& Index() const { return *index_; }

 private:
  std::unique_ptr<CExpression> array_;
  std::unique_ptr<CExpression> index_;
};

// Ternary conditional expression (e.g. cond ? true_val : false_val).
class TernaryExpression : public CExpression {
 public:
  TernaryExpression(std::unique_ptr<CExpression> condition,
                    std::unique_ptr<CExpression> true_expression,
                    std::unique_ptr<CExpression> false_expression)
      : condition_(std::move(condition)),
        true_expression_(std::move(true_expression)),
        false_expression_(std::move(false_expression)) {}

  CExpressionKind Kind() const override { return CExpressionKind::kTernaryExpression; }
  std::string ToString() const override;
  std::unique_ptr<CExpression> Clone() const override;

  const CExpression& Condition() const { return *condition_; }
  const CExpression& TrueExpression() const { return *true_expression_; }
  const CExpression& FalseExpression() const { return *false_expression_; }

 private:
  std::unique_ptr<CExpression> condition_;
  std::unique_ptr<CExpression> true_expression_;
  std::unique_ptr<CExpression> false_expression_;
};

// Classification of C statements.
enum class CStatementKind {
  kCompoundStatement,
  kExpressionStatement,
  kVariableDeclarationStatement,
  kIfStatement,
  kWhileStatement,
  kDoWhileStatement,
  kForStatement,
  kReturnStatement,
  kBreakStatement,
  kContinueStatement,
  kGotoStatement,
  kLabelStatement,
  kSwitchStatement,
  kCaseStatement,
};

// Represents a case clause inside a switch statement.
struct SwitchCase {
  std::vector<int64_t> case_values;  // Empty if is_default == true
  bool is_default = false;
  std::unique_ptr<class CompoundStatement> body;

  SwitchCase Clone() const;
};

// Abstract base class for all C statements.
class CStatement {
 public:
  virtual ~CStatement() = default;

  virtual CStatementKind Kind() const = 0;
  virtual std::string ToString(int indent_level) const = 0;
  std::string ToString() const { return ToString(0); }
  virtual std::unique_ptr<CStatement> Clone() const = 0;

  // Static helper factories
  static std::unique_ptr<CStatement> Expression(std::unique_ptr<CExpression> expression);
  static std::unique_ptr<CStatement> VariableDeclaration(
      CType type, std::string name, std::unique_ptr<CExpression> initializer = nullptr);
  static std::unique_ptr<CStatement> If(std::unique_ptr<CExpression> condition,
                                        std::unique_ptr<CStatement> then_branch,
                                        std::unique_ptr<CStatement> else_branch = nullptr);
  static std::unique_ptr<CStatement> While(std::unique_ptr<CExpression> condition,
                                           std::unique_ptr<CStatement> body);
  static std::unique_ptr<CStatement> DoWhile(std::unique_ptr<CStatement> body,
                                             std::unique_ptr<CExpression> condition);
  static std::unique_ptr<CStatement> For(std::unique_ptr<CStatement> init,
                                         std::unique_ptr<CExpression> condition,
                                         std::unique_ptr<CExpression> step,
                                         std::unique_ptr<CStatement> body);
  static std::unique_ptr<CStatement> Return(std::unique_ptr<CExpression> return_value = nullptr);
  static std::unique_ptr<CStatement> Break();
  static std::unique_ptr<CStatement> Continue();
  static std::unique_ptr<CStatement> Goto(std::string label);
  static std::unique_ptr<CStatement> Label(std::string label);
  static std::unique_ptr<CStatement> Switch(std::unique_ptr<CExpression> condition,
                                            std::vector<SwitchCase> cases);
  static std::unique_ptr<CStatement> Case(int64_t value);
  static std::unique_ptr<CStatement> Default();
};

// Compound statement / block: { stmt1; stmt2; ... }
class CompoundStatement : public CStatement {
 public:
  CompoundStatement() = default;
  explicit CompoundStatement(std::vector<std::unique_ptr<CStatement>> statements)
      : statements_(std::move(statements)) {}

  CStatementKind Kind() const override { return CStatementKind::kCompoundStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  void AddStatement(std::unique_ptr<CStatement> statement);
  bool IsEmpty() const { return statements_.empty(); }
  const std::vector<std::unique_ptr<CStatement>>& Statements() const { return statements_; }

 private:
  std::vector<std::unique_ptr<CStatement>> statements_;
};

// Expression statement: expr;
class ExpressionStatement : public CStatement {
 public:
  explicit ExpressionStatement(std::unique_ptr<CExpression> expression)
      : expression_(std::move(expression)) {}

  CStatementKind Kind() const override { return CStatementKind::kExpressionStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CExpression& Expression() const { return *expression_; }

 private:
  std::unique_ptr<CExpression> expression_;
};

// Local variable declaration: Type var = init;
class VariableDeclarationStatement : public CStatement {
 public:
  VariableDeclarationStatement(CType type, std::string name,
                               std::unique_ptr<CExpression> initializer = nullptr)
      : type_(std::move(type)), name_(std::move(name)), initializer_(std::move(initializer)) {}

  CStatementKind Kind() const override { return CStatementKind::kVariableDeclarationStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CType& Type() const { return type_; }
  const std::string& Name() const { return name_; }
  const CExpression* Initializer() const { return initializer_.get(); }

 private:
  CType type_;
  std::string name_;
  std::unique_ptr<CExpression> initializer_;
};

// If statement: if (cond) { then } else { else }
class IfStatement : public CStatement {
 public:
  IfStatement(std::unique_ptr<CExpression> condition, std::unique_ptr<CStatement> then_branch,
              std::unique_ptr<CStatement> else_branch = nullptr)
      : condition_(std::move(condition)),
        then_branch_(std::move(then_branch)),
        else_branch_(std::move(else_branch)) {}

  CStatementKind Kind() const override { return CStatementKind::kIfStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CExpression& Condition() const { return *condition_; }
  const CStatement& ThenBranch() const { return *then_branch_; }
  const CStatement* ElseBranch() const { return else_branch_.get(); }

 private:
  std::unique_ptr<CExpression> condition_;
  std::unique_ptr<CStatement> then_branch_;
  std::unique_ptr<CStatement> else_branch_;
};

// While loop: while (cond) { body }
class WhileStatement : public CStatement {
 public:
  WhileStatement(std::unique_ptr<CExpression> condition, std::unique_ptr<CStatement> body)
      : condition_(std::move(condition)), body_(std::move(body)) {}

  CStatementKind Kind() const override { return CStatementKind::kWhileStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CExpression& Condition() const { return *condition_; }
  const CStatement& Body() const { return *body_; }

 private:
  std::unique_ptr<CExpression> condition_;
  std::unique_ptr<CStatement> body_;
};

// Do-while loop: do { body } while (cond);
class DoWhileStatement : public CStatement {
 public:
  DoWhileStatement(std::unique_ptr<CStatement> body, std::unique_ptr<CExpression> condition)
      : body_(std::move(body)), condition_(std::move(condition)) {}

  CStatementKind Kind() const override { return CStatementKind::kDoWhileStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CStatement& Body() const { return *body_; }
  const CExpression& Condition() const { return *condition_; }

 private:
  std::unique_ptr<CStatement> body_;
  std::unique_ptr<CExpression> condition_;
};

// For loop: for (init; cond; step) { body }
class ForStatement : public CStatement {
 public:
  ForStatement(std::unique_ptr<CStatement> init, std::unique_ptr<CExpression> condition,
               std::unique_ptr<CExpression> step, std::unique_ptr<CStatement> body)
      : init_(std::move(init)),
        condition_(std::move(condition)),
        step_(std::move(step)),
        body_(std::move(body)) {}

  CStatementKind Kind() const override { return CStatementKind::kForStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CStatement* Init() const { return init_.get(); }
  const CExpression* Condition() const { return condition_.get(); }
  const CExpression* Step() const { return step_.get(); }
  const CStatement& Body() const { return *body_; }

 private:
  std::unique_ptr<CStatement> init_;
  std::unique_ptr<CExpression> condition_;
  std::unique_ptr<CExpression> step_;
  std::unique_ptr<CStatement> body_;
};

// Return statement: return [expr];
class ReturnStatement : public CStatement {
 public:
  explicit ReturnStatement(std::unique_ptr<CExpression> return_value = nullptr)
      : return_value_(std::move(return_value)) {}

  CStatementKind Kind() const override { return CStatementKind::kReturnStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CExpression* ReturnValue() const { return return_value_.get(); }

 private:
  std::unique_ptr<CExpression> return_value_;
};

// Break statement: break;
class BreakStatement : public CStatement {
 public:
  BreakStatement() = default;

  CStatementKind Kind() const override { return CStatementKind::kBreakStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;
};

// Continue statement: continue;
class ContinueStatement : public CStatement {
 public:
  ContinueStatement() = default;

  CStatementKind Kind() const override { return CStatementKind::kContinueStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;
};

// Goto statement: goto label;
class GotoStatement : public CStatement {
 public:
  explicit GotoStatement(std::string label) : label_(std::move(label)) {}

  CStatementKind Kind() const override { return CStatementKind::kGotoStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const std::string& Label() const { return label_; }

 private:
  std::string label_;
};

// Label statement: label:
class LabelStatement : public CStatement {
 public:
  explicit LabelStatement(std::string label) : label_(std::move(label)) {}

  CStatementKind Kind() const override { return CStatementKind::kLabelStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const std::string& Label() const { return label_; }

 private:
  std::string label_;
};

// Case statement: case val: or default:
class CaseStatement : public CStatement {
 public:
  explicit CaseStatement(int64_t value, bool is_default = false)
      : value_(value), is_default_(is_default) {}

  CStatementKind Kind() const override { return CStatementKind::kCaseStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  int64_t Value() const { return value_; }
  bool IsDefault() const { return is_default_; }

 private:
  int64_t value_ = 0;
  bool is_default_ = false;
};

// Switch statement: switch (cond) { case 0: ... break; default: ... }
class SwitchStatement : public CStatement {
 public:
  SwitchStatement(std::unique_ptr<CExpression> condition, std::vector<SwitchCase> cases)
      : condition_(std::move(condition)), cases_(std::move(cases)) {}

  CStatementKind Kind() const override { return CStatementKind::kSwitchStatement; }
  using CStatement::ToString;
  std::string ToString(int indent_level) const override;
  std::unique_ptr<CStatement> Clone() const override;

  const CExpression& Condition() const { return *condition_; }
  const std::vector<SwitchCase>& Cases() const { return cases_; }
  std::vector<SwitchCase>& MutableCases() { return cases_; }

 private:
  std::unique_ptr<CExpression> condition_;
  std::vector<SwitchCase> cases_;
};

// Represents a function parameter.
struct CParameter {
  CType type;
  std::string name;

  std::string ToString() const;
};

// Represents a full C function definition.
class FunctionDeclaration {
 public:
  FunctionDeclaration(CType return_type, std::string name, std::vector<CParameter> parameters,
                      std::unique_ptr<CompoundStatement> body, bool is_static = false)
      : return_type_(std::move(return_type)),
        name_(std::move(name)),
        parameters_(std::move(parameters)),
        body_(std::move(body)),
        is_static_(is_static) {}

  const CType& ReturnType() const { return return_type_; }
  const std::string& Name() const { return name_; }
  const std::vector<CParameter>& Parameters() const { return parameters_; }
  const CompoundStatement& Body() const { return *body_; }
  CompoundStatement* MutableBody() { return body_.get(); }
  bool IsStatic() const { return is_static_; }

  std::string Prototype() const;
  std::string ToString() const;

 private:
  CType return_type_;
  std::string name_;
  std::vector<CParameter> parameters_;
  std::unique_ptr<CompoundStatement> body_;
  bool is_static_;
};

}  // namespace rom_nom_nom

#endif  // CORE_C_AST_H_
