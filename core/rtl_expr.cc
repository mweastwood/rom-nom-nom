#include "core/rtl_expr.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"

namespace rom_nom_nom {
namespace {

struct OpEntry {
  RtlOp op;
  std::string_view name;
};

constexpr std::array<OpEntry, 87> kOpTable = {{
    {RtlOp::kNil, "nil"},
    {RtlOp::kPc, "pc"},
    {RtlOp::kCc0, "cc0"},
    {RtlOp::kReturn, "return"},
    {RtlOp::kSet, "set"},
    {RtlOp::kParallel, "parallel"},
    {RtlOp::kSequence, "sequence"},
    {RtlOp::kUse, "use"},
    {RtlOp::kClobber, "clobber"},
    {RtlOp::kCall, "call"},
    {RtlOp::kIfThenElse, "if_then_else"},
    {RtlOp::kCond, "cond"},
    {RtlOp::kInsn, "insn"},
    {RtlOp::kJumpInsn, "jump_insn"},
    {RtlOp::kCallInsn, "call_insn"},
    {RtlOp::kCodeLabel, "code_label"},
    {RtlOp::kBarrier, "barrier"},
    {RtlOp::kNote, "note"},
    {RtlOp::kExprList, "expr_list"},
    {RtlOp::kInsnList, "insn_list"},
    {RtlOp::kAddrVec, "addr_vec"},
    {RtlOp::kAddrDiffVec, "addr_diff_vec"},
    {RtlOp::kAsmInput, "asm_input"},
    {RtlOp::kAsmOperands, "asm_operands"},
    {RtlOp::kUnspec, "unspec"},
    {RtlOp::kUnspecVolatile, "unspec_volatile"},
    {RtlOp::kUnalignedLoad, "unaligned_load"},
    {RtlOp::kUnalignedStore, "unaligned_store"},
    {RtlOp::kReg, "reg"},
    {RtlOp::kSubreg, "subreg"},
    {RtlOp::kStrictLowPart, "strict_low_part"},
    {RtlOp::kMem, "mem"},
    {RtlOp::kScratch, "scratch"},
    {RtlOp::kConstInt, "const_int"},
    {RtlOp::kConstDouble, "const_double"},
    {RtlOp::kConstString, "const_string"},
    {RtlOp::kSymbolRef, "symbol_ref"},
    {RtlOp::kLabelRef, "label_ref"},
    {RtlOp::kConst, "const"},
    {RtlOp::kPlus, "plus"},
    {RtlOp::kMinus, "minus"},
    {RtlOp::kMult, "mult"},
    {RtlOp::kDiv, "div"},
    {RtlOp::kUdiv, "udiv"},
    {RtlOp::kMod, "mod"},
    {RtlOp::kUmod, "umod"},
    {RtlOp::kSmin, "smin"},
    {RtlOp::kSmax, "smax"},
    {RtlOp::kUmin, "umin"},
    {RtlOp::kUmax, "umax"},
    {RtlOp::kAnd, "and"},
    {RtlOp::kIor, "ior"},
    {RtlOp::kXor, "xor"},
    {RtlOp::kNot, "not"},
    {RtlOp::kNeg, "neg"},
    {RtlOp::kAbs, "abs"},
    {RtlOp::kFfs, "ffs"},
    {RtlOp::kSqrt, "sqrt"},
    {RtlOp::kAshift, "ashift"},
    {RtlOp::kAshiftrt, "ashiftrt"},
    {RtlOp::kLshiftrt, "lshiftrt"},
    {RtlOp::kRotate, "rotate"},
    {RtlOp::kRotatert, "rotatert"},
    {RtlOp::kHigh, "high"},
    {RtlOp::kLoSum, "lo_sum"},
    {RtlOp::kSignExtend, "sign_extend"},
    {RtlOp::kZeroExtend, "zero_extend"},
    {RtlOp::kTruncate, "truncate"},
    {RtlOp::kFloatExtend, "float_extend"},
    {RtlOp::kFloatTruncate, "float_truncate"},
    {RtlOp::kFloat, "float"},
    {RtlOp::kUnsignedFloat, "unsigned_float"},
    {RtlOp::kFix, "fix"},
    {RtlOp::kUnsignedFix, "unsigned_fix"},
    {RtlOp::kSignExtract, "sign_extract"},
    {RtlOp::kZeroExtract, "zero_extract"},
    {RtlOp::kEq, "eq"},
    {RtlOp::kNe, "ne"},
    {RtlOp::kLt, "lt"},
    {RtlOp::kLe, "le"},
    {RtlOp::kGt, "gt"},
    {RtlOp::kGe, "ge"},
    {RtlOp::kLtu, "ltu"},
    {RtlOp::kLeu, "leu"},
    {RtlOp::kGtu, "gtu"},
    {RtlOp::kGeu, "geu"},
    {RtlOp::kUnknown, ""},
}};

struct ModeEntry {
  RtlMachineMode mode;
  std::string_view name;
};

constexpr std::array<ModeEntry, 26> kModeTable = {{
    {RtlMachineMode::kVoid, "VOID"},
    {RtlMachineMode::kQi, "QI"},
    {RtlMachineMode::kHi, "HI"},
    {RtlMachineMode::kSi, "SI"},
    {RtlMachineMode::kDi, "DI"},
    {RtlMachineMode::kTi, "TI"},
    {RtlMachineMode::kSf, "SF"},
    {RtlMachineMode::kDf, "DF"},
    {RtlMachineMode::kTf, "TF"},
    {RtlMachineMode::kCc, "CC"},
    {RtlMachineMode::kBlk, "BLK"},
    {RtlMachineMode::kRegDead, "REG_DEAD"},
    {RtlMachineMode::kRegUnused, "REG_UNUSED"},
    {RtlMachineMode::kRegInc, "REG_INC"},
    {RtlMachineMode::kRegEquiv, "REG_EQUIV"},
    {RtlMachineMode::kRegEqual, "REG_EQUAL"},
    {RtlMachineMode::kRegNonneg, "REG_NONNEG"},
    {RtlMachineMode::kRegNoConflict, "REG_NO_CONFLICT"},
    {RtlMachineMode::kRegLibcall, "REG_LIBCALL"},
    {RtlMachineMode::kRegRetval, "REG_RETVAL"},
    {RtlMachineMode::kRegWas0, "REG_WAS_0"},
    {RtlMachineMode::kRegCcSetter, "REG_CC_SETTER"},
    {RtlMachineMode::kRegCcUser, "REG_CC_USER"},
    {RtlMachineMode::kRegLabel, "REG_LABEL"},
    {RtlMachineMode::kRegDepAnti, "REG_DEP_ANTI"},
    {RtlMachineMode::kRegDepOutput, "REG_DEP_OUTPUT"},
}};

constexpr std::array<std::string_view, 67> kHardRegNames = {{
    "$0",   "at",   "v0",   "v1",   "a0",   "a1",   "a2",    "a3",   "t0",   "t1",   "t2",   "t3",
    "t4",   "t5",   "t6",   "t7",   "s0",   "s1",   "s2",    "s3",   "s4",   "s5",   "s6",   "s7",
    "t8",   "t9",   "k0",   "k1",   "gp",   "sp",   "fp",    "ra",   "$f0",  "$f1",  "$f2",  "$f3",
    "$f4",  "$f5",  "$f6",  "$f7",  "$f8",  "$f9",  "$f10",  "$f11", "$f12", "$f13", "$f14", "$f15",
    "$f16", "$f17", "$f18", "$f19", "$f20", "$f21", "$f22",  "$f23", "$f24", "$f25", "$f26", "$f27",
    "$f28", "$f29", "$f30", "$f31", "hi",   "lo",   "accum",
}};

void SkipWhitespace(std::string_view text, size_t* pos) {
  while (*pos < text.size() && std::isspace(static_cast<unsigned char>(text[*pos])) != 0) {
    ++(*pos);
  }
}

bool IsAtomDelimiter(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '(' || c == ')' || c == '[' ||
         c == ']' || c == '"';
}

std::string_view ReadAtom(std::string_view text, size_t* pos) {
  SkipWhitespace(text, pos);
  size_t start = *pos;
  while (*pos < text.size() && !IsAtomDelimiter(text[*pos])) {
    ++(*pos);
  }
  return text.substr(start, *pos - start);
}

absl::StatusOr<std::string> ParseQuotedString(std::string_view text, size_t* pos) {
  SkipWhitespace(text, pos);
  if (*pos >= text.size() || text[*pos] != '"') {
    return absl::InvalidArgumentError("Expected '\"' at start of string");
  }
  ++(*pos);
  std::string out;
  while (*pos < text.size()) {
    char c = text[*pos];
    ++(*pos);
    if (c == '"') {
      return out;
    }
    if (c == '\\') {
      if (*pos >= text.size()) {
        return absl::InvalidArgumentError("Unterminated escape sequence in RTL string");
      }
      char esc = text[*pos];
      ++(*pos);
      switch (esc) {
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '"':
          out.push_back('"');
          break;
        default:
          out.push_back(esc);
          break;
      }
    } else {
      out.push_back(c);
    }
  }
  return absl::InvalidArgumentError("Unterminated quoted string in RTL sexpr");
}

std::string EscapeQuotedString(std::string_view str) {
  std::string out;
  out.reserve(str.size() + 2);
  out.push_back('"');
  for (char c : str) {
    switch (c) {
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '"':
        out.append("\\\"");
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  out.push_back('"');
  return out;
}

// Checks whether the token at `*pos` is a parenthesized string `("...")`.
bool PeekParenQuotedString(std::string_view text, size_t pos) {
  SkipWhitespace(text, &pos);
  if (pos >= text.size() || text[pos] != '(') {
    return false;
  }
  ++pos;
  SkipWhitespace(text, &pos);
  return pos < text.size() && text[pos] == '"';
}

absl::StatusOr<std::string> ParseParenQuotedString(std::string_view text, size_t* pos) {
  SkipWhitespace(text, pos);
  if (*pos >= text.size() || text[*pos] != '(') {
    return absl::InvalidArgumentError("Expected '(' before quoted string");
  }
  ++(*pos);
  auto str_or = ParseQuotedString(text, pos);
  if (!str_or.ok()) {
    return str_or.status();
  }
  SkipWhitespace(text, pos);
  if (*pos >= text.size() || text[*pos] != ')') {
    return absl::InvalidArgumentError("Expected ')' after quoted string");
  }
  ++(*pos);
  return *str_or;
}

absl::Status ParseHeaderToken(std::string_view header, RtlExpr* expr) {
  if (header.empty()) {
    return absl::InvalidArgumentError("Empty RTL opcode header");
  }

  // Split by ':' first to extract optional machine mode.
  std::string_view op_and_flags = header;
  std::string_view mode_str;
  size_t colon_pos = header.find(':');
  if (colon_pos != std::string_view::npos) {
    op_and_flags = header.substr(0, colon_pos);
    mode_str = header.substr(colon_pos + 1);
  }

  // Split op_and_flags by '/' to extract opcode and flags.
  std::string_view op_str = op_and_flags;
  size_t slash_pos = op_and_flags.find('/');
  if (slash_pos != std::string_view::npos) {
    op_str = op_and_flags.substr(0, slash_pos);
    std::string_view flags_part = op_and_flags.substr(slash_pos + 1);
    while (!flags_part.empty()) {
      size_t next_slash = flags_part.find('/');
      std::string_view flag_tok = flags_part.substr(0, next_slash);
      if (flag_tok == "s") {
        expr->flags.in_struct = true;
      } else if (flag_tok == "v") {
        expr->flags.volatil = true;
      } else if (flag_tok == "u") {
        expr->flags.unchanging = true;
      } else if (flag_tok == "i") {
        expr->flags.integrated = true;
      } else if (flag_tok == "f") {
        expr->flags.frame_related = true;
      } else {
        return absl::InvalidArgumentError(
            absl::StrCat("Unknown RTL flag '/", flag_tok, "' in ", header));
      }
      if (next_slash == std::string_view::npos) {
        break;
      }
      flags_part.remove_prefix(next_slash + 1);
    }
  }

  expr->op = RtlExpr::ParseOp(op_str);
  if (expr->op == RtlOp::kUnknown) {
    expr->raw_op = std::string(op_str);
  }

  if (!mode_str.empty()) {
    expr->mode = RtlExpr::ParseMode(mode_str);
    if (expr->mode == RtlMachineMode::kUnknown) {
      expr->raw_mode = std::string(mode_str);
    }
  } else {
    expr->mode = RtlMachineMode::kNone;
  }

  return absl::OkStatus();
}

void AppendHeaderToken(const RtlExpr& expr, std::string* out) {
  if (expr.op == RtlOp::kUnknown) {
    out->append(expr.raw_op);
  } else {
    out->append(RtlExpr::OpToString(expr.op));
  }
  // GCC 2.7.2 print_rtx prints flags in order: /s, /v, /u, /i, /f.
  if (expr.flags.in_struct) {
    out->append("/s");
  }
  if (expr.flags.volatil) {
    out->append("/v");
  }
  if (expr.flags.unchanging) {
    out->append("/u");
  }
  if (expr.flags.integrated) {
    out->append("/i");
  }
  if (expr.flags.frame_related) {
    out->append("/f");
  }
  if (expr.mode != RtlMachineMode::kNone) {
    out->push_back(':');
    if (expr.mode == RtlMachineMode::kUnknown) {
      out->append(expr.raw_mode);
    } else {
      out->append(RtlExpr::ModeToString(expr.mode));
    }
  }
}

bool UsesGenericItemsByDefault(RtlOp op) {
  switch (op) {
    case RtlOp::kCond:
    case RtlOp::kInsn:
    case RtlOp::kJumpInsn:
    case RtlOp::kCallInsn:
    case RtlOp::kCodeLabel:
    case RtlOp::kBarrier:
    case RtlOp::kNote:
    case RtlOp::kAsmOperands:
    case RtlOp::kAddrDiffVec:
    case RtlOp::kUnknown:
      return true;
    default:
      return false;
  }
}

absl::StatusOr<RtlExpr> ParseExprImpl(std::string_view text, size_t* pos);

absl::Status ParseGenericOperands(std::string_view text, size_t* pos, RtlExpr* expr) {
  while (true) {
    SkipWhitespace(text, pos);
    if (*pos >= text.size()) {
      return absl::InvalidArgumentError("Unexpected end of input inside RTL expression");
    }
    char c = text[*pos];
    if (c == ')') {
      ++(*pos);
      return absl::OkStatus();
    }
    if (c == '"') {
      auto str_or = ParseQuotedString(text, pos);
      if (!str_or.ok()) {
        return str_or.status();
      }
      RtlExpr::GenericItem item;
      item.kind = RtlExpr::GenericItem::Kind::kQuotedString;
      item.text = std::move(*str_or);
      expr->generic_items.push_back(std::move(item));
      continue;
    }
    if (PeekParenQuotedString(text, *pos)) {
      auto str_or = ParseParenQuotedString(text, pos);
      if (!str_or.ok()) {
        return str_or.status();
      }
      RtlExpr::GenericItem item;
      item.kind = RtlExpr::GenericItem::Kind::kParenQuotedString;
      item.text = std::move(*str_or);
      expr->generic_items.push_back(std::move(item));
      continue;
    }
    if (c == '(') {
      auto child_or = ParseExprImpl(text, pos);
      if (!child_or.ok()) {
        return child_or.status();
      }
      RtlExpr::GenericItem item;
      item.kind = RtlExpr::GenericItem::Kind::kChildExpr;
      item.child_index = expr->children.size();
      expr->children.push_back(std::move(*child_or));
      expr->generic_items.push_back(std::move(item));
      continue;
    }
    if (c == '[') {
      size_t bracket_start = *pos;
      ++(*pos);
      SkipWhitespace(text, pos);
      if (*pos < text.size() && (text[*pos] == '(' || text[*pos] == ']')) {
        size_t start_idx = expr->children.size();
        while (*pos < text.size() && text[*pos] != ']') {
          if (text[*pos] != '(') {
            return absl::InvalidArgumentError("Expected '(' or ']' inside RTL expression vector");
          }
          auto elem_or = ParseExprImpl(text, pos);
          if (!elem_or.ok()) {
            return elem_or.status();
          }
          expr->children.push_back(std::move(*elem_or));
          SkipWhitespace(text, pos);
        }
        if (*pos >= text.size() || text[*pos] != ']') {
          return absl::InvalidArgumentError("Unterminated '[' in RTL sexpr");
        }
        ++(*pos);
        RtlExpr::GenericItem item;
        item.kind = RtlExpr::GenericItem::Kind::kBracketedChildSlice;
        item.child_index = start_idx;
        item.child_count = expr->children.size() - start_idx;
        expr->generic_items.push_back(std::move(item));
      } else {
        // Non-expr bracketed payload like `[0 uses]` in code_label.
        int depth = 1;
        while (*pos < text.size() && depth > 0) {
          if (text[*pos] == '[') {
            ++depth;
          } else if (text[*pos] == ']') {
            --depth;
          }
          ++(*pos);
        }
        if (depth != 0) {
          return absl::InvalidArgumentError("Unterminated '[' in RTL sexpr");
        }
        RtlExpr::GenericItem item;
        item.kind = RtlExpr::GenericItem::Kind::kBracketedRaw;
        item.text = std::string(text.substr(bracket_start, *pos - bracket_start));
        expr->generic_items.push_back(std::move(item));
      }
      continue;
    }

    std::string_view atom = ReadAtom(text, pos);
    if (atom.empty()) {
      return absl::InvalidArgumentError("Unexpected token while parsing RTL expression");
    }
    int64_t val = 0;
    RtlExpr::GenericItem item;
    if (absl::SimpleAtoi(atom, &val)) {
      item.kind = RtlExpr::GenericItem::Kind::kInt;
      item.int_val = val;
    } else {
      item.kind = RtlExpr::GenericItem::Kind::kBareAtom;
      item.text = std::string(atom);
    }
    expr->generic_items.push_back(std::move(item));
  }
}

absl::StatusOr<RtlExpr> ParseExprImpl(std::string_view text, size_t* pos) {
  SkipWhitespace(text, pos);
  if (*pos >= text.size() || text[*pos] != '(') {
    return absl::InvalidArgumentError(
        absl::StrCat("Expected '(' at offset ", *pos, " in RTL sexpr: ", text));
  }
  ++(*pos);
  SkipWhitespace(text, pos);

  std::string_view header = ReadAtom(text, pos);
  RtlExpr expr;
  absl::Status header_status = ParseHeaderToken(header, &expr);
  if (!header_status.ok()) {
    return header_status;
  }

  if (UsesGenericItemsByDefault(expr.op)) {
    absl::Status status = ParseGenericOperands(text, pos, &expr);
    if (!status.ok()) {
      return status;
    }
    return expr;
  }

  switch (expr.op) {
    case RtlOp::kNil:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kReturn:
    case RtlOp::kScratch: {
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError(absl::StrCat("Expected ')' after ", header));
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kConstInt: {
      std::string_view num_tok = ReadAtom(text, pos);
      if (!absl::SimpleAtoi(num_tok, &expr.int_val)) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid const_int value: ", num_tok));
      }
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after const_int");
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kReg: {
      std::string_view reg_tok = ReadAtom(text, pos);
      if (!absl::SimpleAtoi(reg_tok, &expr.int_val)) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid register number: ", reg_tok));
      }
      SkipWhitespace(text, pos);
      if (*pos < text.size() && text[*pos] != ')') {
        std::string_view name_tok = ReadAtom(text, pos);
        expr.str_val = std::string(name_tok);
      }
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after reg");
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kSubreg: {
      auto inner_or = ParseExprImpl(text, pos);
      if (!inner_or.ok()) {
        return inner_or.status();
      }
      expr.children.push_back(std::move(*inner_or));
      std::string_view word_tok = ReadAtom(text, pos);
      if (!absl::SimpleAtoi(word_tok, &expr.int_val)) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid subreg word offset: ", word_tok));
      }
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after subreg");
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kSymbolRef: {
      SkipWhitespace(text, pos);
      if (PeekParenQuotedString(text, *pos)) {
        expr.symbol_paren_wrapped = true;
        auto sym_or = ParseParenQuotedString(text, pos);
        if (!sym_or.ok()) {
          return sym_or.status();
        }
        expr.str_val = std::move(*sym_or);
      } else if (*pos < text.size() && text[*pos] == '"') {
        expr.symbol_paren_wrapped = false;
        auto sym_or = ParseQuotedString(text, pos);
        if (!sym_or.ok()) {
          return sym_or.status();
        }
        expr.str_val = std::move(*sym_or);
      } else {
        return absl::InvalidArgumentError("Expected quoted symbol in symbol_ref");
      }
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after symbol_ref");
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kConstString:
    case RtlOp::kAsmInput: {
      auto str_or = ParseQuotedString(text, pos);
      if (!str_or.ok()) {
        return str_or.status();
      }
      expr.str_val = std::move(*str_or);
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after string operand");
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kLabelRef: {
      SkipWhitespace(text, pos);
      if (*pos < text.size() && text[*pos] == '(') {
        auto child_or = ParseExprImpl(text, pos);
        if (!child_or.ok()) {
          return child_or.status();
        }
        expr.children.push_back(std::move(*child_or));
      } else {
        std::string_view label_tok = ReadAtom(text, pos);
        if (label_tok.empty()) {
          return absl::InvalidArgumentError("Empty label_ref target");
        }
        if (absl::SimpleAtoi(label_tok, &expr.int_val)) {
          expr.str_val.clear();
        } else {
          expr.str_val = std::string(label_tok);
          if (label_tok.size() > 1 && label_tok.front() == 'L') {
            (void)absl::SimpleAtoi(label_tok.substr(1), &expr.int_val);
          }
        }
      }
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after label_ref");
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kInsnList: {
      SkipWhitespace(text, pos);
      if (*pos < text.size() && text[*pos] == '(') {
        auto first_or = ParseExprImpl(text, pos);
        if (!first_or.ok()) {
          return first_or.status();
        }
        expr.children.push_back(std::move(*first_or));
      } else {
        std::string_view id_tok = ReadAtom(text, pos);
        if (!absl::SimpleAtoi(id_tok, &expr.int_val)) {
          return absl::InvalidArgumentError(absl::StrCat("Invalid insn_list id: ", id_tok));
        }
      }
      auto next_or = ParseExprImpl(text, pos);
      if (!next_or.ok()) {
        return next_or.status();
      }
      expr.children.push_back(std::move(*next_or));
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after insn_list");
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kConstDouble: {
      SkipWhitespace(text, pos);
      if (*pos < text.size() && text[*pos] == '(') {
        auto chain_or = ParseExprImpl(text, pos);
        if (!chain_or.ok()) {
          return chain_or.status();
        }
        expr.children.push_back(std::move(*chain_or));
      }
      while (true) {
        SkipWhitespace(text, pos);
        if (*pos >= text.size()) {
          return absl::InvalidArgumentError("Unexpected end of input in const_double");
        }
        if (text[*pos] == ')') {
          ++(*pos);
          break;
        }
        std::string_view word_tok = ReadAtom(text, pos);
        int64_t word = 0;
        if (!absl::SimpleAtoi(word_tok, &word)) {
          return absl::InvalidArgumentError(
              absl::StrCat("Invalid const_double integer word: ", word_tok));
        }
        expr.extra_ints.push_back(word);
      }
      if (!expr.extra_ints.empty()) {
        expr.int_val = expr.extra_ints[0];
      }
      if (expr.extra_ints.size() >= 2) {
        expr.int_val_high = expr.extra_ints[1];
      }
      return expr;
    }

    case RtlOp::kParallel:
    case RtlOp::kSequence:
    case RtlOp::kAddrVec: {
      SkipWhitespace(text, pos);
      if (*pos < text.size() && text[*pos] == '[') {
        expr.bracketed_children = true;
        ++(*pos);
        while (true) {
          SkipWhitespace(text, pos);
          if (*pos >= text.size()) {
            return absl::InvalidArgumentError("Unterminated '[' in vector RTL expression");
          }
          if (text[*pos] == ']') {
            ++(*pos);
            break;
          }
          auto elem_or = ParseExprImpl(text, pos);
          if (!elem_or.ok()) {
            return elem_or.status();
          }
          expr.children.push_back(std::move(*elem_or));
        }
      } else {
        expr.bracketed_children = false;
        while (true) {
          SkipWhitespace(text, pos);
          if (*pos >= text.size() || text[*pos] == ')') {
            break;
          }
          auto elem_or = ParseExprImpl(text, pos);
          if (!elem_or.ok()) {
            return elem_or.status();
          }
          expr.children.push_back(std::move(*elem_or));
        }
      }
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError(absl::StrCat("Expected ')' after ", header));
      }
      ++(*pos);
      return expr;
    }

    case RtlOp::kUnspec:
    case RtlOp::kUnspecVolatile: {
      SkipWhitespace(text, pos);
      if (*pos < text.size() && text[*pos] == '[') {
        expr.bracketed_children = true;
        ++(*pos);
        while (true) {
          SkipWhitespace(text, pos);
          if (*pos >= text.size()) {
            return absl::InvalidArgumentError("Unterminated '[' in unspec");
          }
          if (text[*pos] == ']') {
            ++(*pos);
            break;
          }
          auto elem_or = ParseExprImpl(text, pos);
          if (!elem_or.ok()) {
            return elem_or.status();
          }
          expr.children.push_back(std::move(*elem_or));
        }
      } else {
        expr.bracketed_children = false;
        auto elem_or = ParseExprImpl(text, pos);
        if (!elem_or.ok()) {
          return elem_or.status();
        }
        expr.children.push_back(std::move(*elem_or));
      }
      std::string_view code_tok = ReadAtom(text, pos);
      if (!absl::SimpleAtoi(code_tok, &expr.int_val)) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid unspec code: ", code_tok));
      }
      SkipWhitespace(text, pos);
      if (*pos >= text.size() || text[*pos] != ')') {
        return absl::InvalidArgumentError("Expected ')' after unspec");
      }
      ++(*pos);
      return expr;
    }

    default: {
      // Standard child-expression-only operations (set, mem, plus, minus,
      // comparisons, conversions, call, if_then_else, expr_list, etc.).
      while (true) {
        SkipWhitespace(text, pos);
        if (*pos >= text.size()) {
          return absl::InvalidArgumentError(absl::StrCat("Unexpected end of input in ", header));
        }
        if (text[*pos] == ')') {
          ++(*pos);
          return expr;
        }
        if (text[*pos] != '(') {
          return absl::InvalidArgumentError(absl::StrCat(
              "Expected child '(' or closing ')' in ", header, " at offset ", *pos, " in: ", text));
        }
        auto child_or = ParseExprImpl(text, pos);
        if (!child_or.ok()) {
          return child_or.status();
        }
        expr.children.push_back(std::move(*child_or));
      }
    }
  }
}

void FormatExprImpl(const RtlExpr& expr, std::string* out) {
  out->push_back('(');
  AppendHeaderToken(expr, out);

  if (!expr.generic_items.empty() || UsesGenericItemsByDefault(expr.op)) {
    for (const auto& item : expr.generic_items) {
      out->push_back(' ');
      switch (item.kind) {
        case RtlExpr::GenericItem::Kind::kChildExpr:
          FormatExprImpl(expr.children.at(item.child_index), out);
          break;
        case RtlExpr::GenericItem::Kind::kBracketedChildSlice:
          out->push_back('[');
          for (size_t i = 0; i < item.child_count; ++i) {
            if (i > 0) {
              out->push_back(' ');
            }
            FormatExprImpl(expr.children.at(item.child_index + i), out);
          }
          out->push_back(']');
          break;
        case RtlExpr::GenericItem::Kind::kInt:
          absl::StrAppend(out, item.int_val);
          break;
        case RtlExpr::GenericItem::Kind::kQuotedString:
          out->append(EscapeQuotedString(item.text));
          break;
        case RtlExpr::GenericItem::Kind::kParenQuotedString:
          out->push_back('(');
          out->append(EscapeQuotedString(item.text));
          out->push_back(')');
          break;
        case RtlExpr::GenericItem::Kind::kBareAtom:
        case RtlExpr::GenericItem::Kind::kBracketedRaw:
          out->append(item.text);
          break;
      }
    }
    out->push_back(')');
    return;
  }

  switch (expr.op) {
    case RtlOp::kNil:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kReturn:
    case RtlOp::kScratch:
      break;

    case RtlOp::kConstInt:
      absl::StrAppend(out, " ", expr.int_val);
      break;

    case RtlOp::kReg: {
      absl::StrAppend(out, " ", expr.int_val);
      if (!expr.str_val.empty()) {
        absl::StrAppend(out, " ", expr.str_val);
      } else if (expr.int_val >= 0 && expr.int_val < 67) {
        std::string_view reg_name = RtlExpr::HardRegName(static_cast<int>(expr.int_val));
        if (!reg_name.empty()) {
          absl::StrAppend(out, " ", reg_name);
        }
      }
      break;
    }

    case RtlOp::kSubreg:
      if (!expr.children.empty()) {
        out->push_back(' ');
        FormatExprImpl(expr.children[0], out);
      }
      absl::StrAppend(out, " ", expr.int_val);
      break;

    case RtlOp::kSymbolRef:
      if (expr.symbol_paren_wrapped) {
        absl::StrAppend(out, " (", EscapeQuotedString(expr.str_val), ")");
      } else {
        absl::StrAppend(out, " ", EscapeQuotedString(expr.str_val));
      }
      break;

    case RtlOp::kConstString:
    case RtlOp::kAsmInput:
      absl::StrAppend(out, " ", EscapeQuotedString(expr.str_val));
      break;

    case RtlOp::kLabelRef:
      if (!expr.children.empty()) {
        out->push_back(' ');
        FormatExprImpl(expr.children[0], out);
      } else if (!expr.str_val.empty()) {
        absl::StrAppend(out, " ", expr.str_val);
      } else {
        absl::StrAppend(out, " ", expr.int_val);
      }
      break;

    case RtlOp::kInsnList:
      if (expr.children.size() == 2) {
        out->push_back(' ');
        FormatExprImpl(expr.children[0], out);
        out->push_back(' ');
        FormatExprImpl(expr.children[1], out);
      } else {
        absl::StrAppend(out, " ", expr.int_val);
        if (!expr.children.empty()) {
          out->push_back(' ');
          FormatExprImpl(expr.children[0], out);
        }
      }
      break;

    case RtlOp::kConstDouble:
      for (const auto& child : expr.children) {
        out->push_back(' ');
        FormatExprImpl(child, out);
      }
      if (!expr.extra_ints.empty()) {
        for (int64_t val : expr.extra_ints) {
          absl::StrAppend(out, " ", val);
        }
      } else {
        absl::StrAppend(out, " ", expr.int_val, " ", expr.int_val_high);
      }
      break;

    case RtlOp::kParallel:
    case RtlOp::kSequence:
    case RtlOp::kAddrVec:
      if (expr.bracketed_children) {
        out->append(" [");
        for (size_t i = 0; i < expr.children.size(); ++i) {
          if (i > 0) {
            out->push_back(' ');
          }
          FormatExprImpl(expr.children[i], out);
        }
        out->push_back(']');
      } else {
        for (const auto& child : expr.children) {
          out->push_back(' ');
          FormatExprImpl(child, out);
        }
      }
      break;

    case RtlOp::kUnspec:
    case RtlOp::kUnspecVolatile:
      if (expr.bracketed_children) {
        out->append(" [");
        for (size_t i = 0; i < expr.children.size(); ++i) {
          if (i > 0) {
            out->push_back(' ');
          }
          FormatExprImpl(expr.children[i], out);
        }
        out->push_back(']');
      } else {
        for (const auto& child : expr.children) {
          out->push_back(' ');
          FormatExprImpl(child, out);
        }
      }
      absl::StrAppend(out, " ", expr.int_val);
      break;

    default:
      for (const auto& child : expr.children) {
        out->push_back(' ');
        FormatExprImpl(child, out);
      }
      break;
  }

  out->push_back(')');
}

}  // namespace

bool RtlExpr::operator==(const RtlExpr& other) const {
  return op == other.op && mode == other.mode && flags == other.flags && int_val == other.int_val &&
         int_val_high == other.int_val_high && str_val == other.str_val &&
         children == other.children && symbol_paren_wrapped == other.symbol_paren_wrapped &&
         bracketed_children == other.bracketed_children && raw_op == other.raw_op &&
         raw_mode == other.raw_mode && extra_ints == other.extra_ints &&
         generic_items == other.generic_items;
}

absl::StatusOr<RtlExpr> RtlExpr::Parse(std::string_view sexpr) {
  size_t pos = 0;
  auto expr_or = ParseExprImpl(sexpr, &pos);
  if (!expr_or.ok()) {
    return expr_or.status();
  }
  SkipWhitespace(sexpr, &pos);
  if (pos != sexpr.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Trailing characters at offset ", pos, " in RTL sexpr: ", sexpr));
  }
  return expr_or;
}

std::string RtlExpr::ToSexprString() const {
  std::string out;
  FormatExprImpl(*this, &out);
  return out;
}

RtlExpr RtlExpr::MakeNil() {
  RtlExpr expr;
  expr.op = RtlOp::kNil;
  return expr;
}

RtlExpr RtlExpr::MakePc() {
  RtlExpr expr;
  expr.op = RtlOp::kPc;
  return expr;
}

RtlExpr RtlExpr::MakeCc0() {
  RtlExpr expr;
  expr.op = RtlOp::kCc0;
  return expr;
}

RtlExpr RtlExpr::MakeReturn() {
  RtlExpr expr;
  expr.op = RtlOp::kReturn;
  return expr;
}

RtlExpr RtlExpr::MakeConstInt(int64_t val) {
  RtlExpr expr;
  expr.op = RtlOp::kConstInt;
  expr.int_val = val;
  return expr;
}

RtlExpr RtlExpr::MakeReg(RtlMachineMode mode, int regno, RtlFlags flags) {
  RtlExpr expr;
  expr.op = RtlOp::kReg;
  expr.mode = mode;
  expr.flags = flags;
  expr.SetRegNo(regno);
  return expr;
}

RtlExpr RtlExpr::MakeSubreg(RtlMachineMode mode, RtlExpr inner, int word_num, RtlFlags flags) {
  RtlExpr expr;
  expr.op = RtlOp::kSubreg;
  expr.mode = mode;
  expr.flags = flags;
  expr.int_val = word_num;
  expr.children.push_back(std::move(inner));
  return expr;
}

RtlExpr RtlExpr::MakeMem(RtlMachineMode mode, RtlExpr addr, RtlFlags flags) {
  RtlExpr expr;
  expr.op = RtlOp::kMem;
  expr.mode = mode;
  expr.flags = flags;
  expr.children.push_back(std::move(addr));
  return expr;
}

RtlExpr RtlExpr::MakeSymbolRef(RtlMachineMode mode, std::string symbol, bool paren_wrapped,
                               RtlFlags flags) {
  RtlExpr expr;
  expr.op = RtlOp::kSymbolRef;
  expr.mode = mode;
  expr.flags = flags;
  expr.str_val = std::move(symbol);
  expr.symbol_paren_wrapped = paren_wrapped;
  return expr;
}

RtlExpr RtlExpr::MakeLabelRef(int label_id, RtlMachineMode mode) {
  RtlExpr expr;
  expr.op = RtlOp::kLabelRef;
  expr.mode = mode;
  expr.int_val = label_id;
  return expr;
}

RtlExpr RtlExpr::MakeSymbolicLabelRef(std::string label_token, RtlMachineMode mode) {
  RtlExpr expr;
  expr.op = RtlOp::kLabelRef;
  expr.mode = mode;
  if (label_token.size() > 1 && label_token.front() == 'L') {
    std::string_view token_view = label_token;
    (void)absl::SimpleAtoi(token_view.substr(1), &expr.int_val);
  }
  expr.str_val = std::move(label_token);
  return expr;
}

RtlExpr RtlExpr::MakeSet(RtlExpr dest, RtlExpr src) {
  RtlExpr expr;
  expr.op = RtlOp::kSet;
  expr.children.push_back(std::move(dest));
  expr.children.push_back(std::move(src));
  return expr;
}

RtlExpr RtlExpr::MakeUnary(RtlOp op, RtlMachineMode mode, RtlExpr operand) {
  RtlExpr expr;
  expr.op = op;
  expr.mode = mode;
  expr.children.push_back(std::move(operand));
  return expr;
}

RtlExpr RtlExpr::MakeBinary(RtlOp op, RtlMachineMode mode, RtlExpr lhs, RtlExpr rhs) {
  RtlExpr expr;
  expr.op = op;
  expr.mode = mode;
  expr.children.push_back(std::move(lhs));
  expr.children.push_back(std::move(rhs));
  return expr;
}

RtlExpr RtlExpr::MakeIfThenElse(RtlExpr cond, RtlExpr then_expr, RtlExpr else_expr,
                                RtlMachineMode mode) {
  RtlExpr expr;
  expr.op = RtlOp::kIfThenElse;
  expr.mode = mode;
  expr.children.push_back(std::move(cond));
  expr.children.push_back(std::move(then_expr));
  expr.children.push_back(std::move(else_expr));
  return expr;
}

RtlExpr RtlExpr::MakeParallel(std::vector<RtlExpr> elements) {
  RtlExpr expr;
  expr.op = RtlOp::kParallel;
  expr.bracketed_children = true;
  expr.children = std::move(elements);
  return expr;
}

RtlExpr RtlExpr::MakeUse(RtlExpr operand) {
  RtlExpr expr;
  expr.op = RtlOp::kUse;
  expr.children.push_back(std::move(operand));
  return expr;
}

RtlExpr RtlExpr::MakeClobber(RtlExpr operand) {
  RtlExpr expr;
  expr.op = RtlOp::kClobber;
  expr.children.push_back(std::move(operand));
  return expr;
}

RtlExpr RtlExpr::MakeScratch(RtlMachineMode mode) {
  RtlExpr expr;
  expr.op = RtlOp::kScratch;
  expr.mode = mode;
  return expr;
}

RtlExpr RtlExpr::MakeCall(RtlExpr mem_fn, RtlExpr arg_size) {
  RtlExpr expr;
  expr.op = RtlOp::kCall;
  expr.children.push_back(std::move(mem_fn));
  expr.children.push_back(std::move(arg_size));
  return expr;
}

void RtlExpr::SetRegNo(int new_regno) {
  int_val = new_regno;
  if (new_regno >= 0 && new_regno < 67) {
    str_val = std::string(HardRegName(new_regno));
  } else {
    str_val.clear();
  }
}

bool RtlExpr::IsComparison() const {
  switch (op) {
    case RtlOp::kEq:
    case RtlOp::kNe:
    case RtlOp::kLt:
    case RtlOp::kLe:
    case RtlOp::kGt:
    case RtlOp::kGe:
    case RtlOp::kLtu:
    case RtlOp::kLeu:
    case RtlOp::kGtu:
    case RtlOp::kGeu:
      return true;
    default:
      return false;
  }
}

bool RtlExpr::IsCommutative() const {
  switch (op) {
    case RtlOp::kPlus:
    case RtlOp::kMult:
    case RtlOp::kAnd:
    case RtlOp::kIor:
    case RtlOp::kXor:
    case RtlOp::kSmin:
    case RtlOp::kSmax:
    case RtlOp::kUmin:
    case RtlOp::kUmax:
    case RtlOp::kEq:
    case RtlOp::kNe:
      return true;
    default:
      return false;
  }
}

void RtlExpr::Visit(absl::FunctionRef<void(const RtlExpr&)> visitor) const {
  visitor(*this);
  for (const auto& child : children) {
    child.Visit(visitor);
  }
}

void RtlExpr::Mutate(absl::FunctionRef<void(RtlExpr&)> mutator) {
  mutator(*this);
  for (auto& child : children) {
    child.Mutate(mutator);
  }
}

void RtlExpr::RewriteRegs(absl::FunctionRef<void(RtlExpr&)> mutator) {
  if (op == RtlOp::kReg) {
    mutator(*this);
  }
  for (auto& child : children) {
    child.RewriteRegs(mutator);
  }
}

void RtlExpr::ReplaceRegNo(int from_regno, int to_regno) {
  RewriteRegs([from_regno, to_regno](RtlExpr& reg) {
    if (reg.RegNo() == from_regno) {
      reg.SetRegNo(to_regno);
    }
  });
}

bool RtlExpr::ContainsReg(int regno) const {
  bool found = false;
  Visit([regno, &found](const RtlExpr& node) {
    if (node.IsReg() && node.RegNo() == regno) {
      found = true;
    }
  });
  return found;
}

std::string_view RtlExpr::HardRegName(int regno) {
  if (regno >= 0 && regno < static_cast<int>(kHardRegNames.size())) {
    return kHardRegNames[regno];
  }
  return "";
}

int RtlExpr::ParseHardRegName(std::string_view name) {
  for (size_t i = 0; i < kHardRegNames.size(); ++i) {
    if (kHardRegNames[i] == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string_view RtlExpr::OpToString(RtlOp op) {
  for (const auto& entry : kOpTable) {
    if (entry.op == op) {
      return entry.name;
    }
  }
  return "";
}

RtlOp RtlExpr::ParseOp(std::string_view name) {
  for (const auto& entry : kOpTable) {
    if (entry.op != RtlOp::kUnknown && entry.name == name) {
      return entry.op;
    }
  }
  return RtlOp::kUnknown;
}

std::string_view RtlExpr::ModeToString(RtlMachineMode mode) {
  for (const auto& entry : kModeTable) {
    if (entry.mode == mode) {
      return entry.name;
    }
  }
  return "";
}

RtlMachineMode RtlExpr::ParseMode(std::string_view name) {
  if (name.empty()) {
    return RtlMachineMode::kNone;
  }
  for (const auto& entry : kModeTable) {
    if (entry.name == name) {
      return entry.mode;
    }
  }
  return RtlMachineMode::kUnknown;
}

std::string RtlPatternAndNotes::ToSexprString() const {
  return FormatPatternAndNotes(pattern, reg_notes, bracketed_notes);
}

void RtlPatternAndNotes::RewriteRegs(absl::FunctionRef<void(RtlExpr&)> mutator) {
  pattern.RewriteRegs(mutator);
  for (auto& note : reg_notes) {
    note.datum.RewriteRegs(mutator);
  }
}

void RtlPatternAndNotes::ReplaceRegNo(int from_regno, int to_regno) {
  RewriteRegs([from_regno, to_regno](RtlExpr& reg) {
    if (reg.RegNo() == from_regno) {
      reg.SetRegNo(to_regno);
    }
  });
}

absl::StatusOr<RtlPatternAndNotes> ParsePatternAndNotes(std::string_view pattern_with_notes) {
  constexpr std::string_view kNotesTag = " [REG_NOTES: ";
  std::string_view core_str = pattern_with_notes;
  std::string_view notes_str;
  bool bracketed = false;

  size_t tag_pos = pattern_with_notes.find(kNotesTag);
  if (tag_pos != std::string_view::npos) {
    bracketed = true;
    core_str = pattern_with_notes.substr(0, tag_pos);
    notes_str = pattern_with_notes.substr(tag_pos + kNotesTag.size());
    while (!notes_str.empty() && std::isspace(static_cast<unsigned char>(notes_str.back())) != 0) {
      notes_str.remove_suffix(1);
    }
    if (!absl::ConsumeSuffix(&notes_str, "]")) {
      return absl::InvalidArgumentError(
          absl::StrCat("Malformed [REG_NOTES: ...] suffix in: ", pattern_with_notes));
    }
  } else {
    size_t pos = 0;
    auto first_or = ParseExprImpl(pattern_with_notes, &pos);
    if (!first_or.ok()) {
      return first_or.status();
    }
    SkipWhitespace(pattern_with_notes, &pos);
    if (pos < pattern_with_notes.size()) {
      core_str = pattern_with_notes.substr(0, pos);
      notes_str = pattern_with_notes.substr(pos);
    }
  }

  auto pattern_or = RtlExpr::Parse(core_str);
  if (!pattern_or.ok()) {
    return pattern_or.status();
  }

  RtlPatternAndNotes out;
  out.pattern = std::move(*pattern_or);
  out.bracketed_notes = bracketed;

  if (!notes_str.empty()) {
    auto notes_expr_or = RtlExpr::Parse(notes_str);
    if (!notes_expr_or.ok()) {
      return notes_expr_or.status();
    }
    const RtlExpr* cur = &(*notes_expr_or);
    while (cur != nullptr && !cur->IsNil()) {
      if (cur->op == RtlOp::kExprList) {
        if (cur->children.size() != 2) {
          return absl::InvalidArgumentError("Expected 2 children in REG_NOTES expr_list node");
        }
        RtlRegNote note;
        note.kind = cur->mode;
        note.raw_kind = cur->raw_mode;
        note.list_op = RtlOp::kExprList;
        note.datum = cur->children[0];
        out.reg_notes.push_back(std::move(note));
        cur = &cur->children[1];
      } else if (cur->op == RtlOp::kInsnList) {
        RtlRegNote note;
        note.kind = cur->mode;
        note.raw_kind = cur->raw_mode;
        note.list_op = RtlOp::kInsnList;
        if (cur->children.size() == 2) {
          note.datum = cur->children[0];
          cur = &cur->children[1];
        } else if (cur->children.size() == 1) {
          note.insn_id = static_cast<int>(cur->int_val);
          cur = &cur->children[0];
        } else {
          return absl::InvalidArgumentError("Malformed insn_list node in REG_NOTES");
        }
        out.reg_notes.push_back(std::move(note));
      } else {
        return absl::InvalidArgumentError(
            absl::StrCat("Unexpected node in REG_NOTES list: ", cur->ToSexprString()));
      }
    }
  }

  return out;
}

std::string FormatPatternAndNotes(const RtlExpr& pattern, absl::Span<const RtlRegNote> notes,
                                  bool bracketed) {
  std::string out = pattern.ToSexprString();
  if (notes.empty()) {
    return out;
  }

  RtlExpr chain = RtlExpr::MakeNil();
  for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
    RtlExpr node;
    node.op = it->list_op;
    node.mode = it->kind;
    node.raw_mode = it->raw_kind;
    if (it->list_op == RtlOp::kInsnList && it->insn_id >= 0 && it->datum.IsNil()) {
      node.int_val = it->insn_id;
      node.children.push_back(std::move(chain));
    } else {
      node.children.push_back(it->datum);
      node.children.push_back(std::move(chain));
    }
    chain = std::move(node);
  }

  if (bracketed) {
    absl::StrAppend(&out, " [REG_NOTES: ", chain.ToSexprString(), "]");
  } else {
    absl::StrAppend(&out, " ", chain.ToSexprString());
  }
  return out;
}

}  // namespace rom_nom_nom
