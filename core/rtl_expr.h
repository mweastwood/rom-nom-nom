#ifndef ROM_NOM_NOM_CORE_RTL_EXPR_H_
#define ROM_NOM_NOM_CORE_RTL_EXPR_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

// All RTL expression opcodes emitted or consumed by KMC GCC 2.7.2 (MIPS).
enum class RtlOp {
  kUnknown = 0,
  kNil,
  kPc,
  kCc0,
  kReturn,

  // Top-level patterns and structural combinators.
  kSet,
  kParallel,
  kSequence,
  kUse,
  kClobber,
  kCall,
  kIfThenElse,
  kCond,

  // Top-level insn chain wrappers (when full insn S-expressions are parsed).
  kInsn,
  kJumpInsn,
  kCallInsn,
  kCodeLabel,
  kBarrier,
  kNote,

  // Lists, vectors, inline asm, and target-specific unspecs.
  kExprList,
  kInsnList,
  kAddrVec,
  kAddrDiffVec,
  kAsmInput,
  kAsmOperands,
  kUnspec,
  kUnspecVolatile,
  kUnalignedLoad,
  kUnalignedStore,

  // Registers, memory, and constants.
  kReg,
  kSubreg,
  kStrictLowPart,
  kMem,
  kScratch,
  kConstInt,
  kConstDouble,
  kConstString,
  kSymbolRef,
  kLabelRef,
  kConst,

  // Arithmetic, bitwise, and shift operations.
  kPlus,
  kMinus,
  kMult,
  kDiv,
  kUdiv,
  kMod,
  kUmod,
  kSmin,
  kSmax,
  kUmin,
  kUmax,
  kAnd,
  kIor,
  kXor,
  kNot,
  kNeg,
  kAbs,
  kFfs,
  kSqrt,
  kAshift,
  kAshiftrt,
  kLshiftrt,
  kRotate,
  kRotatert,
  kHigh,
  kLoSum,

  // Mode conversions and bit-field extractions.
  kSignExtend,
  kZeroExtend,
  kTruncate,
  kFloatExtend,
  kFloatTruncate,
  kFloat,
  kUnsignedFloat,
  kFix,
  kUnsignedFix,
  kSignExtract,
  kZeroExtract,

  // Relational comparisons.
  kEq,
  kNe,
  kLt,
  kLe,
  kGt,
  kGe,
  kLtu,
  kLeu,
  kGtu,
  kGeu,
};

// Machine modes and REG_NOTE kinds printed after ':' in GCC 2.7.2 RTL dumps.
enum class RtlMachineMode {
  kNone = 0,
  kVoid,
  kQi,
  kHi,
  kSi,
  kDi,
  kTi,
  kSf,
  kDf,
  kTf,
  kCc,
  kBlk,

  // Uppercase aliases for ergonomic pattern matching.
  kVOID = kVoid,
  kQI = kQi,
  kHI = kHi,
  kSI = kSi,
  kDI = kDi,
  kTI = kTi,
  kSF = kSf,
  kDF = kDf,
  kTF = kTf,
  kCC = kCc,
  kBLK = kBlk,

  // REG_NOTE modes used on expr_list / insn_list nodes.
  kRegDead,
  kRegUnused,
  kRegInc,
  kRegEquiv,
  kRegEqual,
  kRegNonneg,
  kRegNoConflict,
  kRegLibcall,
  kRegRetval,
  kRegWas0,
  kRegCcSetter,
  kRegCcUser,
  kRegLabel,
  kRegDepAnti,
  kRegDepOutput,

  kUnknown,
};

// RTL rtx header flag bits printed as `/s`, `/v`, `/u`, `/i`, `/f`.
struct RtlFlags {
  bool in_struct = false;      // `/s`
  bool volatil = false;        // `/v`
  bool unchanging = false;     // `/u`
  bool integrated = false;     // `/i`
  bool frame_related = false;  // `/f`

  bool Any() const { return in_struct || volatil || unchanging || integrated || frame_related; }

  bool operator==(const RtlFlags& other) const {
    return in_struct == other.in_struct && volatil == other.volatil &&
           unchanging == other.unchanging && integrated == other.integrated &&
           frame_related == other.frame_related;
  }
  bool operator!=(const RtlFlags& other) const { return !(*this == other); }
};

// Structured representation of a GCC 2.7.2 RTL S-expression node.
struct RtlExpr {
  // Descriptor for preserving exact operand token ordering on generic/container
  // opcodes (such as `insn`, `code_label`, `note`, `asm_operands`,
  // `addr_diff_vec`, or `kUnknown`).
  struct GenericItem {
    enum class Kind {
      kChildExpr,
      kBracketedChildSlice,
      kInt,
      kQuotedString,
      kParenQuotedString,
      kBareAtom,
      kBracketedRaw,
    };
    Kind kind = Kind::kBareAtom;
    size_t child_index = 0;
    size_t child_count = 0;
    int64_t int_val = 0;
    std::string text;

    bool operator==(const GenericItem& other) const {
      return kind == other.kind && child_index == other.child_index &&
             child_count == other.child_count && int_val == other.int_val && text == other.text;
    }
    bool operator!=(const GenericItem& other) const { return !(*this == other); }
  };

  RtlOp op = RtlOp::kNil;
  RtlMachineMode mode = RtlMachineMode::kNone;
  RtlFlags flags;

  // Primary integer payload:
  // - kConstInt: constant value
  // - kReg: register number (0..67 hard reg, >= 68 pseudo-reg)
  // - kSubreg: word offset
  // - kLabelRef / kInsnList: label or insn id
  // - kUnspec / kUnspecVolatile: unspec code
  // - kConstDouble: first integer word
  int64_t int_val = 0;
  int64_t int_val_high = 0;

  // Primary string payload:
  // - kSymbolRef: symbol name
  // - kConstString / kAsmInput: string literal
  // - kReg: optional explicit register name (e.g. "v0", "a0", "$f0")
  std::string str_val;

  // Child RTL sub-expressions in evaluation/operand order.
  std::vector<RtlExpr> children;

  // Formatting metadata for lossless S-expression round-tripping.
  bool symbol_paren_wrapped = true;
  bool bracketed_children = false;
  std::string raw_op;
  std::string raw_mode;
  std::vector<int64_t> extra_ints;
  std::vector<GenericItem> generic_items;

  bool operator==(const RtlExpr& other) const;
  bool operator!=(const RtlExpr& other) const { return !(*this == other); }

  // Parses an RTL S-expression into a structured RtlExpr tree.
  static absl::StatusOr<RtlExpr> Parse(std::string_view sexpr);

  // Formats this RtlExpr back to a canonical GCC 2.7.2 RTL S-expression string.
  std::string ToSexprString() const;

  // Formats this RtlExpr using GCC 2.7.2 `print_rtl` + `CanonicalizeRtlPattern`
  // spacing (`(parallel[ <e0> <e1> ... ] )` for bracketed `PARALLEL` nodes).
  std::string ToGccPrintRtlString() const;

  // Static factory builders.
  static RtlExpr MakeNil();
  static RtlExpr MakePc();
  static RtlExpr MakeCc0();
  static RtlExpr MakeReturn();
  static RtlExpr MakeConstInt(int64_t val);
  static RtlExpr MakeReg(RtlMachineMode mode, int regno, RtlFlags flags = {});
  static RtlExpr MakeArgPointerReg(RtlMachineMode mode = RtlMachineMode::kSi, RtlFlags flags = {});
  static RtlExpr MakeSubreg(RtlMachineMode mode, RtlExpr inner, int word_num, RtlFlags flags = {});
  static RtlExpr MakeMem(RtlMachineMode mode, RtlExpr addr, RtlFlags flags = {});
  static RtlExpr MakeSymbolRef(RtlMachineMode mode, std::string symbol, bool paren_wrapped = true,
                               RtlFlags flags = {});
  static RtlExpr MakeLabelRef(int label_id, RtlMachineMode mode = RtlMachineMode::kNone);
  static RtlExpr MakeSymbolicLabelRef(std::string label_token,
                                      RtlMachineMode mode = RtlMachineMode::kNone);
  static RtlExpr MakeSet(RtlExpr dest, RtlExpr src);
  static RtlExpr MakeUnary(RtlOp op, RtlMachineMode mode, RtlExpr operand);
  static RtlExpr MakeBinary(RtlOp op, RtlMachineMode mode, RtlExpr lhs, RtlExpr rhs);
  static RtlExpr MakeIfThenElse(RtlExpr cond, RtlExpr then_expr, RtlExpr else_expr,
                                RtlMachineMode mode = RtlMachineMode::kNone);
  static RtlExpr MakeParallel(std::vector<RtlExpr> elements);
  static RtlExpr MakeSequence(std::vector<RtlExpr> sub_insns);
  static RtlExpr MakeInsnWrapper(RtlOp wrapper_op, RtlFlags flags, RtlExpr inner_pattern);
  static RtlExpr MakeInsnWrapper(RtlOp wrapper_op, RtlFlags flags,
                                 const struct RtlPatternAndNotes& inner);
  static RtlExpr MakeNote(std::string note_kind);
  static RtlExpr MakeUse(RtlExpr operand);
  static RtlExpr MakeClobber(RtlExpr operand);
  static RtlExpr MakeScratch(RtlMachineMode mode);
  static RtlExpr MakeCall(RtlExpr mem_fn, RtlExpr arg_size);

  // Structural queries and accessors.
  bool IsNil() const { return op == RtlOp::kNil; }
  bool IsReg() const { return op == RtlOp::kReg; }
  bool IsArgPointerReg() const {
    return op == RtlOp::kReg && int_val == 0 && (str_val == "argp" || str_val == "$arg");
  }
  bool IsPseudoReg() const { return op == RtlOp::kReg && int_val >= 68; }
  bool IsHardReg() const {
    return op == RtlOp::kReg && int_val >= 0 && int_val < 68 && !IsArgPointerReg();
  }
  int RegNo() const { return static_cast<int>(int_val); }
  void SetRegNo(int new_regno);

  bool IsSubreg() const { return op == RtlOp::kSubreg; }
  bool IsMem() const { return op == RtlOp::kMem && !children.empty(); }
  const RtlExpr& MemAddr() const { return children.at(0); }
  RtlExpr& MutableMemAddr() { return children.at(0); }

  bool IsConstInt() const { return op == RtlOp::kConstInt; }
  int64_t IntVal() const { return int_val; }

  bool IsSymbolRef() const { return op == RtlOp::kSymbolRef; }
  std::string_view SymbolName() const { return str_val; }

  bool IsLabelRef() const { return op == RtlOp::kLabelRef; }
  int LabelId() const { return static_cast<int>(int_val); }

  bool IsSet() const { return op == RtlOp::kSet && children.size() == 2; }
  const RtlExpr& SetDest() const { return children.at(0); }
  RtlExpr& MutableSetDest() { return children.at(0); }
  const RtlExpr& SetSrc() const { return children.at(1); }
  RtlExpr& MutableSetSrc() { return children.at(1); }

  bool IsParallel() const { return op == RtlOp::kParallel; }
  bool IsCallPattern() const;
  bool IsComparison() const;
  bool IsCommutative() const;

  // Pre-order traversal and in-place mutation helpers.
  void Visit(absl::FunctionRef<void(const RtlExpr&)> visitor) const;
  void Mutate(absl::FunctionRef<void(RtlExpr&)> mutator);
  void RewriteRegs(absl::FunctionRef<void(RtlExpr&)> mutator);
  void ReplaceRegNo(int from_regno, int to_regno);
  bool ContainsReg(int regno) const;

  // Opcode, mode, and MIPS hard-register name tables.
  static std::string_view HardRegName(int regno);
  static int ParseHardRegName(std::string_view name);
  static std::string_view OpToString(RtlOp op);
  static RtlOp ParseOp(std::string_view name);
  static std::string_view ModeToString(RtlMachineMode mode);
  static RtlMachineMode ParseMode(std::string_view name);
};

// A single entry in an instruction's `REG_NOTES` list (`expr_list` or
// `insn_list`).
struct RtlRegNote {
  RtlMachineMode kind = RtlMachineMode::kNone;
  RtlOp list_op = RtlOp::kExprList;
  RtlExpr datum;
  int insn_id = -1;
  std::string raw_kind;

  bool operator==(const RtlRegNote& other) const {
    return kind == other.kind && list_op == other.list_op && datum == other.datum &&
           insn_id == other.insn_id && raw_kind == other.raw_kind;
  }
  bool operator!=(const RtlRegNote& other) const { return !(*this == other); }
};

// Parsed representation of an `RtlInsn` pattern string, separating the core
// instruction pattern from any trailing `REG_NOTES` list (`(expr_list:...)` or
// `[REG_NOTES: ...]`) and optional `CALL_INSN_FUNCTION_USAGE` list
// (`(expr_list (use ...) ...)`).
struct RtlPatternAndNotes {
  RtlExpr pattern;
  std::vector<RtlRegNote> reg_notes;
  std::vector<RtlExpr> call_fusage;
  bool has_explicit_call_fusage = false;
  bool bracketed_notes = false;

  // Extracts `pattern`, optional `reg_notes`, and optional `call_fusage`
  // directly from a structured `(insn ...)`, `(jump_insn ...)`, or
  // `(call_insn ...)` element inside a `(sequence [...])` without
  // re-serializing child ASTs to strings.
  static absl::StatusOr<RtlPatternAndNotes> FromSequenceSubInsn(const RtlExpr& sub_insn);

  std::string ToSexprString() const;
  std::string ToGccPrintRtlString() const;
  void RewriteRegs(absl::FunctionRef<void(RtlExpr&)> mutator);
  void ReplaceRegNo(int from_regno, int to_regno);

  bool operator==(const RtlPatternAndNotes& other) const {
    return pattern == other.pattern && reg_notes == other.reg_notes &&
           call_fusage == other.call_fusage &&
           has_explicit_call_fusage == other.has_explicit_call_fusage &&
           bracketed_notes == other.bracketed_notes;
  }
  bool operator!=(const RtlPatternAndNotes& other) const { return !(*this == other); }
};

// Parses a `(expr_list (use ...) ... (nil))` `CALL_INSN_FUNCTION_USAGE` S-expression
// (or `(nil)`) into structured `(use ...)` / `(clobber ...)` elements.
absl::Status ParseCallFusageChainExpr(const RtlExpr& fusage_expr, std::vector<RtlExpr>* out_fusage);
absl::StatusOr<std::vector<RtlExpr>> ParseCallFusageChainSexpr(std::string_view fusage_sexpr);
RtlExpr BuildCallFusageChainExpr(absl::Span<const RtlExpr> call_fusage);

// Splits `pattern_with_notes` (either `<pattern> (expr_list:...)` or
// `<pattern> [REG_NOTES: (expr_list:...)]`, optionally followed by untyped
// `CALL_INSN_FUNCTION_USAGE` `(expr_list (use ...) ...)`) into its structured
// pattern, `REG_NOTES` list, and `call_fusage` list.
absl::StatusOr<RtlPatternAndNotes> ParsePatternAndNotes(std::string_view pattern_with_notes);

// Formats a structured pattern, optional `REG_NOTES` list, and optional
// `call_fusage` list back into the canonical `RtlInsn` pattern representation.
std::string FormatPatternAndNotes(const RtlExpr& pattern, absl::Span<const RtlRegNote> notes,
                                  bool bracketed = false,
                                  absl::Span<const RtlExpr> call_fusage = {});
std::string FormatGccPrintRtlPatternAndNotes(const RtlExpr& pattern,
                                             absl::Span<const RtlRegNote> notes,
                                             bool bracketed = false,
                                             absl::Span<const RtlExpr> call_fusage = {});

inline bool IsCallPattern(const RtlExpr& pat) {
  return pat.IsCallPattern();
}

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_CORE_RTL_EXPR_H_
