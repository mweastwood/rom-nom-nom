#include "core/c_ast.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace rom_nom_nom {

namespace {

std::string Indent(int level) {
  return std::string(level * 4, ' ');
}

}  // namespace

std::string CType::ToString() const {
  std::string result;
  if (is_const) {
    result += "const ";
  }
  result += name;
  for (int i = 0; i < pointer_depth; ++i) {
    result += "*";
  }
  return result;
}

CType CType::MakePointer() const {
  CType ptr = *this;
  ptr.pointer_depth += 1;
  return ptr;
}

CType CType::Void() {
  return CType{.name = "void", .is_const = false, .pointer_depth = 0};
}

CType CType::S8() {
  return CType{.name = "s8", .is_const = false, .pointer_depth = 0};
}

CType CType::U8() {
  return CType{.name = "u8", .is_const = false, .pointer_depth = 0};
}

CType CType::S16() {
  return CType{.name = "s16", .is_const = false, .pointer_depth = 0};
}

CType CType::U16() {
  return CType{.name = "u16", .is_const = false, .pointer_depth = 0};
}

CType CType::S32() {
  return CType{.name = "s32", .is_const = false, .pointer_depth = 0};
}

CType CType::U32() {
  return CType{.name = "u32", .is_const = false, .pointer_depth = 0};
}

CType CType::F32() {
  return CType{.name = "f32", .is_const = false, .pointer_depth = 0};
}

CType CType::F64() {
  return CType{.name = "f64", .is_const = false, .pointer_depth = 0};
}

CType CType::Named(std::string type_name) {
  return CType{.name = std::move(type_name), .is_const = false, .pointer_depth = 0};
}

// --- CExpression Factory Implementations ---

std::unique_ptr<CExpression> CExpression::Integer(int64_t value, bool is_hex, bool is_unsigned) {
  return std::make_unique<IntegerLiteralExpression>(value, is_hex, is_unsigned);
}

std::unique_ptr<CExpression> CExpression::Float(double value, bool is_float) {
  return std::make_unique<FloatLiteralExpression>(value, is_float);
}

std::unique_ptr<CExpression> CExpression::String(std::string value) {
  return std::make_unique<StringLiteralExpression>(std::move(value));
}

std::unique_ptr<CExpression> CExpression::Identifier(std::string name) {
  return std::make_unique<IdentifierExpression>(std::move(name));
}

std::unique_ptr<CExpression> CExpression::Unary(std::string op,
                                                std::unique_ptr<CExpression> operand,
                                                bool is_prefix) {
  return std::make_unique<UnaryExpression>(std::move(op), std::move(operand), is_prefix);
}

std::unique_ptr<CExpression> CExpression::Binary(std::string op, std::unique_ptr<CExpression> lhs,
                                                 std::unique_ptr<CExpression> rhs) {
  return std::make_unique<BinaryExpression>(std::move(op), std::move(lhs), std::move(rhs));
}

std::unique_ptr<CExpression> CExpression::Assignment(std::string op,
                                                     std::unique_ptr<CExpression> lhs,
                                                     std::unique_ptr<CExpression> rhs) {
  return std::make_unique<AssignmentExpression>(std::move(op), std::move(lhs), std::move(rhs));
}

std::unique_ptr<CExpression> CExpression::Call(
    std::unique_ptr<CExpression> callee, std::vector<std::unique_ptr<CExpression>> arguments) {
  return std::make_unique<CallExpression>(std::move(callee), std::move(arguments));
}

std::unique_ptr<CExpression> CExpression::Cast(CType target_type,
                                               std::unique_ptr<CExpression> operand) {
  return std::make_unique<CastExpression>(std::move(target_type), std::move(operand));
}

std::unique_ptr<CExpression> CExpression::MemberAccess(std::unique_ptr<CExpression> object,
                                                       std::string member_name, bool is_arrow) {
  return std::make_unique<MemberAccessExpression>(std::move(object), std::move(member_name),
                                                  is_arrow);
}

std::unique_ptr<CExpression> CExpression::ArrayIndex(std::unique_ptr<CExpression> array,
                                                     std::unique_ptr<CExpression> index) {
  return std::make_unique<ArrayIndexExpression>(std::move(array), std::move(index));
}

std::unique_ptr<CExpression> CExpression::Ternary(std::unique_ptr<CExpression> condition,
                                                  std::unique_ptr<CExpression> true_expression,
                                                  std::unique_ptr<CExpression> false_expression) {
  return std::make_unique<TernaryExpression>(std::move(condition), std::move(true_expression),
                                             std::move(false_expression));
}

// --- IntegerLiteralExpression ---

std::string IntegerLiteralExpression::ToString() const {
  std::string suffix = is_unsigned_ ? "U" : "";
  if (is_hex_) {
    return absl::StrFormat("0x%X%s", static_cast<uint64_t>(value_), suffix);
  }
  return absl::StrFormat("%d%s", value_, suffix);
}

std::unique_ptr<CExpression> IntegerLiteralExpression::Clone() const {
  return std::make_unique<IntegerLiteralExpression>(value_, is_hex_, is_unsigned_);
}

// --- FloatLiteralExpression ---

std::string FloatLiteralExpression::ToString() const {
  std::string base = absl::StrFormat("%g", value_);
  if (base.find('.') == std::string::npos && base.find('e') == std::string::npos) {
    base += ".0";
  }
  if (is_float_) {
    base += "f";
  }
  return base;
}

std::unique_ptr<CExpression> FloatLiteralExpression::Clone() const {
  return std::make_unique<FloatLiteralExpression>(value_, is_float_);
}

// --- StringLiteralExpression ---

std::string StringLiteralExpression::ToString() const {
  return absl::StrFormat("\"%s\"", value_);
}

std::unique_ptr<CExpression> StringLiteralExpression::Clone() const {
  return std::make_unique<StringLiteralExpression>(value_);
}

// --- IdentifierExpression ---

std::string IdentifierExpression::ToString() const {
  return name_;
}

std::unique_ptr<CExpression> IdentifierExpression::Clone() const {
  return std::make_unique<IdentifierExpression>(name_);
}

// --- UnaryExpression ---

std::string UnaryExpression::ToString() const {
  if (is_prefix_) {
    return absl::StrFormat("%s%s", op_, operand_->ToString());
  }
  return absl::StrFormat("%s%s", operand_->ToString(), op_);
}

std::unique_ptr<CExpression> UnaryExpression::Clone() const {
  return std::make_unique<UnaryExpression>(op_, operand_->Clone(), is_prefix_);
}

// --- BinaryExpression ---

std::string BinaryExpression::ToString() const {
  return absl::StrFormat("(%s %s %s)", lhs_->ToString(), op_, rhs_->ToString());
}

std::unique_ptr<CExpression> BinaryExpression::Clone() const {
  return std::make_unique<BinaryExpression>(op_, lhs_->Clone(), rhs_->Clone());
}

// --- AssignmentExpression ---

std::string AssignmentExpression::ToString() const {
  return absl::StrFormat("%s %s %s", lhs_->ToString(), op_, rhs_->ToString());
}

std::unique_ptr<CExpression> AssignmentExpression::Clone() const {
  return std::make_unique<AssignmentExpression>(op_, lhs_->Clone(), rhs_->Clone());
}

// --- CallExpression ---

std::string CallExpression::ToString() const {
  std::vector<std::string> arg_strs;
  arg_strs.reserve(arguments_.size());
  for (const auto& arg : arguments_) {
    arg_strs.push_back(arg->ToString());
  }
  return absl::StrFormat("%s(%s)", callee_->ToString(), absl::StrJoin(arg_strs, ", "));
}

std::unique_ptr<CExpression> CallExpression::Clone() const {
  std::vector<std::unique_ptr<CExpression>> cloned_args;
  cloned_args.reserve(arguments_.size());
  for (const auto& arg : arguments_) {
    cloned_args.push_back(arg->Clone());
  }
  return std::make_unique<CallExpression>(callee_->Clone(), std::move(cloned_args));
}

// --- CastExpression ---

std::string CastExpression::ToString() const {
  return absl::StrFormat("(%s)%s", target_type_.ToString(), operand_->ToString());
}

std::unique_ptr<CExpression> CastExpression::Clone() const {
  return std::make_unique<CastExpression>(target_type_, operand_->Clone());
}

// --- MemberAccessExpression ---

std::string MemberAccessExpression::ToString() const {
  return absl::StrFormat("%s%s%s", object_->ToString(), is_arrow_ ? "->" : ".", member_name_);
}

std::unique_ptr<CExpression> MemberAccessExpression::Clone() const {
  return std::make_unique<MemberAccessExpression>(object_->Clone(), member_name_, is_arrow_);
}

// --- ArrayIndexExpression ---

std::string ArrayIndexExpression::ToString() const {
  return absl::StrFormat("%s[%s]", array_->ToString(), index_->ToString());
}

std::unique_ptr<CExpression> ArrayIndexExpression::Clone() const {
  return std::make_unique<ArrayIndexExpression>(array_->Clone(), index_->Clone());
}

// --- TernaryExpression ---

std::string TernaryExpression::ToString() const {
  return absl::StrFormat("(%s ? %s : %s)", condition_->ToString(), true_expression_->ToString(),
                         false_expression_->ToString());
}

std::unique_ptr<CExpression> TernaryExpression::Clone() const {
  return std::make_unique<TernaryExpression>(condition_->Clone(), true_expression_->Clone(),
                                             false_expression_->Clone());
}

// --- CStatement Factory Implementations ---

std::unique_ptr<CStatement> CStatement::Expression(std::unique_ptr<CExpression> expression) {
  return std::make_unique<ExpressionStatement>(std::move(expression));
}

std::unique_ptr<CStatement> CStatement::VariableDeclaration(
    CType type, std::string name, std::unique_ptr<CExpression> initializer) {
  return std::make_unique<VariableDeclarationStatement>(std::move(type), std::move(name),
                                                        std::move(initializer));
}

std::unique_ptr<CStatement> CStatement::If(std::unique_ptr<CExpression> condition,
                                           std::unique_ptr<CStatement> then_branch,
                                           std::unique_ptr<CStatement> else_branch) {
  return std::make_unique<IfStatement>(std::move(condition), std::move(then_branch),
                                       std::move(else_branch));
}

std::unique_ptr<CStatement> CStatement::While(std::unique_ptr<CExpression> condition,
                                              std::unique_ptr<CStatement> body) {
  return std::make_unique<WhileStatement>(std::move(condition), std::move(body));
}

std::unique_ptr<CStatement> CStatement::DoWhile(std::unique_ptr<CStatement> body,
                                                std::unique_ptr<CExpression> condition) {
  return std::make_unique<DoWhileStatement>(std::move(body), std::move(condition));
}

std::unique_ptr<CStatement> CStatement::For(std::unique_ptr<CStatement> init,
                                            std::unique_ptr<CExpression> condition,
                                            std::unique_ptr<CExpression> step,
                                            std::unique_ptr<CStatement> body) {
  return std::make_unique<ForStatement>(std::move(init), std::move(condition), std::move(step),
                                        std::move(body));
}

std::unique_ptr<CStatement> CStatement::Return(std::unique_ptr<CExpression> return_value) {
  return std::make_unique<ReturnStatement>(std::move(return_value));
}

std::unique_ptr<CStatement> CStatement::Break() {
  return std::make_unique<BreakStatement>();
}

std::unique_ptr<CStatement> CStatement::Continue() {
  return std::make_unique<ContinueStatement>();
}

std::unique_ptr<CStatement> CStatement::Goto(std::string label) {
  return std::make_unique<GotoStatement>(std::move(label));
}

std::unique_ptr<CStatement> CStatement::Label(std::string label) {
  return std::make_unique<LabelStatement>(std::move(label));
}

std::unique_ptr<CStatement> CStatement::Switch(std::unique_ptr<CExpression> condition,
                                               std::vector<SwitchCase> cases) {
  return std::make_unique<SwitchStatement>(std::move(condition), std::move(cases));
}

std::unique_ptr<CStatement> CStatement::Case(int64_t value) {
  return std::make_unique<CaseStatement>(value);
}

std::unique_ptr<CStatement> CStatement::Default() {
  return std::make_unique<CaseStatement>(0, /*is_default=*/true);
}

// --- CompoundStatement ---

void CompoundStatement::AddStatement(std::unique_ptr<CStatement> statement) {
  statements_.push_back(std::move(statement));
}

std::string CompoundStatement::ToString(int indent_level) const {
  std::string result = Indent(indent_level) + "{\n";
  for (const auto& stmt : statements_) {
    result += stmt->ToString(indent_level + 1);
  }
  result += Indent(indent_level) + "}\n";
  return result;
}

std::unique_ptr<CStatement> CompoundStatement::Clone() const {
  std::vector<std::unique_ptr<CStatement>> cloned;
  cloned.reserve(statements_.size());
  for (const auto& stmt : statements_) {
    cloned.push_back(stmt->Clone());
  }
  return std::make_unique<CompoundStatement>(std::move(cloned));
}

// --- ExpressionStatement ---

std::string ExpressionStatement::ToString(int indent_level) const {
  return Indent(indent_level) + expression_->ToString() + ";\n";
}

std::unique_ptr<CStatement> ExpressionStatement::Clone() const {
  return std::make_unique<ExpressionStatement>(expression_->Clone());
}

// --- VariableDeclarationStatement ---

std::string VariableDeclarationStatement::ToString(int indent_level) const {
  std::string result = Indent(indent_level) + type_.ToString() + " " + name_;
  if (initializer_ != nullptr) {
    result += " = " + initializer_->ToString();
  }
  result += ";\n";
  return result;
}

std::unique_ptr<CStatement> VariableDeclarationStatement::Clone() const {
  return std::make_unique<VariableDeclarationStatement>(
      type_, name_, initializer_ ? initializer_->Clone() : nullptr);
}

// --- IfStatement ---

std::string IfStatement::ToString(int indent_level) const {
  std::string result = Indent(indent_level) + "if (" + condition_->ToString() + ") ";
  if (then_branch_->Kind() == CStatementKind::kCompoundStatement) {
    // Trim leading indent from compound statement
    std::string then_str = then_branch_->ToString(indent_level);
    size_t first_brace = then_str.find('{');
    result += then_str.substr(first_brace);
  } else {
    result += "\n" + then_branch_->ToString(indent_level + 1);
  }

  if (else_branch_ != nullptr) {
    // If then_branch ended with newline, remove it to put else on same line or next line
    if (!result.empty() && result.back() == '\n') {
      result.pop_back();
      result += " ";
    }
    result += "else ";
    if (else_branch_->Kind() == CStatementKind::kCompoundStatement ||
        else_branch_->Kind() == CStatementKind::kIfStatement) {
      std::string else_str = else_branch_->ToString(indent_level);
      size_t first_char = else_str.find_first_not_of(' ');
      result += else_str.substr(first_char);
    } else {
      result += "\n" + else_branch_->ToString(indent_level + 1);
    }
  }
  return result;
}

std::unique_ptr<CStatement> IfStatement::Clone() const {
  return std::make_unique<IfStatement>(condition_->Clone(), then_branch_->Clone(),
                                       else_branch_ ? else_branch_->Clone() : nullptr);
}

// --- WhileStatement ---

std::string WhileStatement::ToString(int indent_level) const {
  std::string result = Indent(indent_level) + "while (" + condition_->ToString() + ") ";
  if (body_->Kind() == CStatementKind::kCompoundStatement) {
    std::string body_str = body_->ToString(indent_level);
    size_t first_brace = body_str.find('{');
    result += body_str.substr(first_brace);
  } else {
    result += "\n" + body_->ToString(indent_level + 1);
  }
  return result;
}

std::unique_ptr<CStatement> WhileStatement::Clone() const {
  return std::make_unique<WhileStatement>(condition_->Clone(), body_->Clone());
}

// --- DoWhileStatement ---

std::string DoWhileStatement::ToString(int indent_level) const {
  std::string result = Indent(indent_level) + "do ";
  if (body_->Kind() == CStatementKind::kCompoundStatement) {
    std::string body_str = body_->ToString(indent_level);
    size_t first_brace = body_str.find('{');
    result += body_str.substr(first_brace);
  } else {
    result += "\n" + body_->ToString(indent_level + 1);
  }
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
    result += " ";
  }
  result += "while (" + condition_->ToString() + ");\n";
  return result;
}

std::unique_ptr<CStatement> DoWhileStatement::Clone() const {
  return std::make_unique<DoWhileStatement>(body_->Clone(), condition_->Clone());
}

// --- ForStatement ---

std::string ForStatement::ToString(int indent_level) const {
  std::string init_str;
  if (init_ != nullptr) {
    init_str = init_->ToString(0);
    // Remove newline and trailing semicolon if present
    while (!init_str.empty() && (init_str.back() == '\n' || init_str.back() == ';')) {
      init_str.pop_back();
    }
  }
  std::string cond_str = condition_ ? condition_->ToString() : "";
  std::string step_str = step_ ? step_->ToString() : "";

  std::string result =
      Indent(indent_level) + absl::StrFormat("for (%s; %s; %s) ", init_str, cond_str, step_str);
  if (body_->Kind() == CStatementKind::kCompoundStatement) {
    std::string body_str = body_->ToString(indent_level);
    size_t first_brace = body_str.find('{');
    result += body_str.substr(first_brace);
  } else {
    result += "\n" + body_->ToString(indent_level + 1);
  }
  return result;
}

std::unique_ptr<CStatement> ForStatement::Clone() const {
  return std::make_unique<ForStatement>(init_ ? init_->Clone() : nullptr,
                                        condition_ ? condition_->Clone() : nullptr,
                                        step_ ? step_->Clone() : nullptr, body_->Clone());
}

// --- ReturnStatement ---

std::string ReturnStatement::ToString(int indent_level) const {
  if (return_value_ != nullptr) {
    return Indent(indent_level) + "return " + return_value_->ToString() + ";\n";
  }
  return Indent(indent_level) + "return;\n";
}

std::unique_ptr<CStatement> ReturnStatement::Clone() const {
  return std::make_unique<ReturnStatement>(return_value_ ? return_value_->Clone() : nullptr);
}

// --- BreakStatement ---

std::string BreakStatement::ToString(int indent_level) const {
  return Indent(indent_level) + "break;\n";
}

std::unique_ptr<CStatement> BreakStatement::Clone() const {
  return std::make_unique<BreakStatement>();
}

// --- ContinueStatement ---

std::string ContinueStatement::ToString(int indent_level) const {
  return Indent(indent_level) + "continue;\n";
}

std::unique_ptr<CStatement> ContinueStatement::Clone() const {
  return std::make_unique<ContinueStatement>();
}

// --- GotoStatement ---

std::string GotoStatement::ToString(int indent_level) const {
  return Indent(indent_level) + "goto " + label_ + ";\n";
}

std::unique_ptr<CStatement> GotoStatement::Clone() const {
  return std::make_unique<GotoStatement>(label_);
}

// --- LabelStatement ---

std::string LabelStatement::ToString(int /*indent_level*/) const {
  return label_ + ":\n";
}

std::unique_ptr<CStatement> LabelStatement::Clone() const {
  return std::make_unique<LabelStatement>(label_);
}

// --- SwitchCase ---

SwitchCase SwitchCase::Clone() const {
  SwitchCase copy;
  copy.case_values = case_values;
  copy.is_default = is_default;
  if (body != nullptr) {
    copy.body = std::unique_ptr<CompoundStatement>(
        static_cast<CompoundStatement*>(body->Clone().release()));
  }
  return copy;
}

// --- CaseStatement ---

std::string CaseStatement::ToString(int indent_level) const {
  if (is_default_) {
    return Indent(indent_level) + "default:\n";
  }
  return Indent(indent_level) + "case " + std::to_string(value_) + ":\n";
}

std::unique_ptr<CStatement> CaseStatement::Clone() const {
  return std::make_unique<CaseStatement>(value_, is_default_);
}

// --- SwitchStatement ---

std::string SwitchStatement::ToString(int indent_level) const {
  std::string result = Indent(indent_level) + "switch (" + condition_->ToString() + ") {\n";
  for (const auto& switch_case : cases_) {
    for (int64_t val : switch_case.case_values) {
      result += Indent(indent_level + 1) + "case " + std::to_string(val) + ":\n";
    }
    if (switch_case.is_default) {
      result += Indent(indent_level + 1) + "default:\n";
    }
    if (switch_case.body != nullptr && !switch_case.body->IsEmpty()) {
      for (const auto& stmt : switch_case.body->Statements()) {
        result += stmt->ToString(indent_level + 2);
      }
    }
  }
  result += Indent(indent_level) + "}\n";
  return result;
}

std::unique_ptr<CStatement> SwitchStatement::Clone() const {
  std::vector<SwitchCase> cloned_cases;
  cloned_cases.reserve(cases_.size());
  for (const auto& c : cases_) {
    cloned_cases.push_back(c.Clone());
  }
  return std::make_unique<SwitchStatement>(condition_->Clone(), std::move(cloned_cases));
}

// --- CParameter ---

std::string CParameter::ToString() const {
  return type.ToString() + " " + name;
}

// --- FunctionDeclaration ---

std::string FunctionDeclaration::Prototype() const {
  std::vector<std::string> parameter_strings;
  parameter_strings.reserve(parameters_.size());
  for (const auto& parameter : parameters_) {
    parameter_strings.push_back(parameter.ToString());
  }
  std::string prefix = is_static_ ? "static " : "";
  std::string parameters_joined =
      parameter_strings.empty() ? "void" : absl::StrJoin(parameter_strings, ", ");
  return absl::StrFormat("%s%s %s(%s);\n", prefix, return_type_.ToString(), name_,
                         parameters_joined);
}

std::string FunctionDeclaration::ToString() const {
  std::vector<std::string> parameter_strings;
  parameter_strings.reserve(parameters_.size());
  for (const auto& parameter : parameters_) {
    parameter_strings.push_back(parameter.ToString());
  }
  std::string prefix = is_static_ ? "static " : "";
  std::string parameters_joined =
      parameter_strings.empty() ? "void" : absl::StrJoin(parameter_strings, ", ");
  std::string header =
      absl::StrFormat("%s%s %s(%s) ", prefix, return_type_.ToString(), name_, parameters_joined);
  return header + body_->ToString(0);
}

}  // namespace rom_nom_nom
