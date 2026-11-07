#ifndef CORE_RTL_SEXPR_UTILS_H_
#define CORE_RTL_SEXPR_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom {

// Classifies a top-level RTL chain entry in a GCC 2.7.2 RTL dump or `print_rtl` stream.
enum class RtlInsnKind {
  kInsn,
  kJumpInsn,
  kCallInsn,
  kCodeLabel,
  kBarrier,
  kNote,
  kOther,
};

// Represents a single parsed top-level RTL S-expression entry within a function dump.
struct RtlInsnEntry {
  RtlInsnKind kind = RtlInsnKind::kOther;
  int uid = -1;
  int prev_uid = -1;
  int next_uid = -1;
  bool is_sequence = false;
  bool in_annulled_branch_delay = false;
  std::string note_kind;
  std::string raw_sexpr;
};

// Represents the parsed RTL dump for a single function within a pass dump file.
struct RtlFunctionDump {
  RtlPassKind pass = RtlPassKind::kDbr;
  std::string function_name;
  std::string header_text;
  std::vector<RtlInsnEntry> insns;

  // Returns the number of active executable RTL instructions (`kInsn`, `kJumpInsn`, `kCallInsn`),
  // excluding `kNote`, `kCodeLabel`, and `kBarrier` entries.
  size_t ActiveInsnCount() const;

  // Returns the number of `(sequence [...])` delay-slot bundles (produced by `reorg.c` in `.dbr`).
  size_t SequenceCount() const;

  // Returns all `kNote` entries whose `note_kind` matches `target_note_kind`
  // (e.g., `"NOTE_INSN_LOOP_BEG"`, `"NOTE_INSN_LOOP_CONT"`, `"NOTE_INSN_PROLOGUE_END"`).
  std::vector<const RtlInsnEntry*> FindNotes(std::string_view target_note_kind) const;

  // Returns true if the function contains at least one `NOTE_INSN_LOOP_BEG` note.
  bool HasLoopNotes() const;
};

// Represents the parsed contents of a single GCC 2.7.2 RTL pass dump file (e.g. `<file>.c.dbr`).
class RtlPassDump {
 public:
  RtlPassDump() = default;
  RtlPassDump(RtlPassKind pass, std::string raw_text, std::string preamble_text,
              std::vector<RtlFunctionDump> functions);

  RtlPassKind Pass() const { return pass_; }
  const std::string& RawText() const { return raw_text_; }
  const std::string& PreambleText() const { return preamble_text_; }
  const std::vector<RtlFunctionDump>& Functions() const { return functions_; }

  const RtlFunctionDump* FindFunction(std::string_view func_name) const;

 private:
  RtlPassKind pass_ = RtlPassKind::kRtl;
  std::string raw_text_;
  std::string preamble_text_;
  std::vector<RtlFunctionDump> functions_;
};

// Represents a flattened item in an RTL instruction chain for basic-block partitioning and CFG
// construction. Shared between `GccRtlDumpInspector::BuildCanonicalCfgFromRtlDump` and
// `GccContext::DematerializeToCfg`.
struct FlatRtlItem {
  RtlInsnKind kind = RtlInsnKind::kOther;
  int uid = -1;
  std::string raw_pattern_for_targets;
  std::string pattern;
};

// -----------------------------------------------------------------------------
// Low-level RTL S-expression Lexing & Balanced Parenthesis Scanning
// -----------------------------------------------------------------------------

// Advances `*pos` past any ASCII whitespace in `text`.
void SkipRtlWhitespace(std::string_view text, size_t* pos);

// Skips leading whitespace and reads an identifier/number token delimited by whitespace or
// `()[]{}:/"`.
std::string_view ReadRtlToken(std::string_view text, size_t* pos);

// Updates the parenthesis nesting depth across `line`, tracking string literals via `*in_quotes`.
int UpdateRtlParenBalance(std::string_view line, int current_depth, bool* in_quotes);

// Given `text[open_pos] == '('`, returns the index of the matching closing `')'` (respecting string
// quotes and escapes), or `std::string_view::npos` if unbalanced.
size_t FindMatchingCloseParen(std::string_view text, size_t open_pos);

// Extracts the top-level parenthesized `(...)` S-expression elements inside a `[...]` vector body
// starting at or after `open_bracket_pos`.
std::vector<std::string_view> ExtractBracketedSexprElements(std::string_view text,
                                                            size_t open_bracket_pos);

// -----------------------------------------------------------------------------
// RTL Dump & Container S-Expression Parsing
// -----------------------------------------------------------------------------

// Parses a single top-level `(insn ...)`, `(jump_insn ...)`, `(call_insn ...)`, `(code_label ...)`,
// `(barrier ...)`, or `(note ...)` S-expression into an `RtlInsnEntry`.
RtlInsnEntry ParseRtlSexprEntry(std::string raw_sexpr);

// Parses the raw text of a GCC 2.7.2 RTL pass dump file into an `RtlPassDump`.
RtlPassDump ParseRtlPassDump(RtlPassKind pass, std::string_view raw_text);

// Builds a sequential canonical label map (`uid -> "L0", "L1", ...`) for all `kCodeLabel` entries
// in linear chain order.
absl::flat_hash_map<int, std::string> BuildSequentialLabelMap(absl::Span<const RtlInsnEntry> insns);
absl::flat_hash_map<int, std::string> BuildSequentialLabelMap(const RtlFunctionDump& dump);

// Normalizes an active RTL instruction S-expression for comparison across compilations by
// stripping the 3 UID numbers following `(insn...` and mapping `(label_ref <uid>)` to sequential
// function label IDs (`L0`, `L1`, ...).
std::string NormalizeInsnSexprForComparison(
    std::string_view sexpr, const absl::flat_hash_map<int, std::string>& label_uid_to_canon);

// Extracts the tag and flags (e.g. `"insn"`, `"insn/s"`, `"jump_insn"`) from a container
// S-expression `(insn/s ...)` without the leading `'('`.
std::string ExtractInsnTagWithFlags(std::string_view sexpr);

// Extracts the top-level `PATTERN(insn)` S-expression from a full `(insn UID PREV NEXT PATTERN
// ...)` dump entry.
std::string ExtractTopLevelPatternFromInsnSexpr(std::string_view sexpr);

// Extracts the trailing `REG_NOTES` S-expression (`(expr_list:...)` or `(insn_list:...)`) from a
// full `(insn UID PREV NEXT PATTERN CODE {name} LOG_LINKS REG_NOTES)` dump entry, or returns `""`
// if `REG_NOTES` is `(nil)` or absent.
std::string ExtractRegNotesFromInsnSexpr(std::string_view sexpr);

// Splits `<main_pattern> <trailing_reg_notes>` when `(expr_list:...` or `(insn_list:...` is
// appended after the primary pattern S-expression.
std::pair<std::string_view, std::string_view> SplitPatternAndTrailingRegNotes(
    std::string_view text);

// Strips any trailing `(expr_list:...` or `(insn_list:...` `REG_NOTES` from `pat`.
std::string StripTrailingRegNotesFromPattern(std::string_view pat);

// Rewrites `(label_ref <uid>)` occurrences in `pattern` using `label_map` (`uid -> "L0"`).
std::string CanonicalizeLabelRefsInPattern(std::string_view pattern,
                                           const absl::flat_hash_map<int, std::string>& label_map);

// Extracts `PATTERN(insn)` and any trailing `REG_NOTES(insn)` from `sexpr`, canonicalizing label
// references via `label_map` and collapsing whitespace.
std::string ExtractPatternAndNotesFromInsnSexpr(
    std::string_view sexpr, const absl::flat_hash_map<int, std::string>& label_map);

// Extracts all numeric `<uid>` values from `(label_ref <uid>)` occurrences in `pattern`.
std::vector<int> ExtractLabelRefsFromPattern(std::string_view pattern);

// Extracts all token strings `<tok>` (whether numeric `"25"` or symbolic `"L0"`) from
// `(label_ref <tok>)` occurrences in `text`.
std::vector<std::string> ExtractLabelRefTokens(std::string_view text);

// Parses the inner instruction entries of a `(sequence [(jump_insn ...) (insn ...)])` entry.
std::vector<RtlInsnEntry> ParseSequenceInnerEntries(const RtlInsnEntry& seq_entry);

// Decomposes a canonical `(sequence [(jump_insn <jump_pat>) (<delay_tag> <delay_pat>)])` pattern
// into its inner jump pattern, delay-slot container tag (e.g. `"insn"` or `"insn/s"`), and
// delay-slot pattern. Returns true on success.
bool ExtractOuterSequenceParts(std::string_view seq_pat, std::string* out_jump_pat,
                               std::string* out_delay_tag, std::string* out_delay_pat);

// -----------------------------------------------------------------------------
// Canonical `RtlControlFlowGraph` Construction from RTL Streams / Dumps
// -----------------------------------------------------------------------------

// Partitions a flattened linear RTL item stream into basic blocks in exact linear order and
// populates jump/fallthrough successor and predecessor edges.
RtlControlFlowGraph BuildCanonicalCfgFromFlatRtlItems(
    RtlPassKind pass, absl::Span<const FlatRtlItem> items,
    absl::Span<const RtlInsnEntry> raw_insns_for_tablejumps = {});

// Builds the canonical basic-block `RtlControlFlowGraph` from a parsed `RtlFunctionDump` (uniform
// across all 13 GCC 2.7.2 RTL passes `.rtl` .. `.dbr`).
RtlControlFlowGraph BuildCanonicalCfgFromRtlDump(const RtlFunctionDump& dump);

// -----------------------------------------------------------------------------
// RTL Pattern Classification, Branch Inversion, and MIPS Register Formatting
// -----------------------------------------------------------------------------

// Returns true if `pat` is a jump or return RTL pattern (`(return)`, `(set (pc) ...)`, or a
// `parallel` jump/return).
bool IsJumpPatternText(std::string_view pat);

// Returns true if `pat` is an unconditional jump, tablejump, or return RTL pattern.
bool IsUnconditionalJumpOrReturnPatternText(std::string_view pat);

// Returns true if `pat` is a call RTL pattern (`(call ...)`, `(set ... (call ...))`, or `parallel`
// call).
bool IsCallPatternText(std::string_view pat);

// Inverts an RTL comparison code (`"eq" <-> "ne"`, `"lt" <-> "ge"`, `"gt" <-> "le"`,
// `"ltu" <-> "geu"`, `"gtu" <-> "leu"`). Returns `cmp_code` unchanged if not a recognized
// comparison code.
std::string InvertRtlConditionCode(std::string_view cmp_code);

// Reverses the comparison operator inside `(if_then_else (<cmp>:...` in `pat`.
std::string InvertBranchConditionInRtlPattern(std::string_view pat);

// Returns the GCC 2.7.2 MIPS RTL register name for GPR `regno` (`0..31`, e.g. `"$0"`, `"v0"`,
// `"sp"`, `"ra"`), or `""` if `regno >= 32`.
std::string_view MipsGprRtlName(uint32_t regno);

// Formats a MIPS hardware or pseudo register as a GCC 2.7.2 RTL S-expression, e.g.
// `"(reg:SI 2 v0)"`, `"(reg/i:SI 2 v0)"`, or `"(reg:SI 72)"`.
std::string FormatHardRegRtl(uint32_t regno, std::string_view mode = "SI", bool integrated = false);
inline std::string FormatHardRegRtl(uint32_t regno, bool integrated) {
  return FormatHardRegRtl(regno, "SI", integrated);
}

// Parses a MIPS assembly register token (`"$0"`, `"$zero"`, `"$2"`, `"$v0"`, `"$sp"`, `"$31"`,
// etc.) into its hardware register number `0..31`, or `-1` if invalid.
int ParseMipsAsmRegNumber(std::string_view tok);

// Scans all instruction patterns in `cfg` and returns the highest pseudo-register number (`>= 67`)
// referenced in `(reg...:M <regno>)`, or `-1` if no pseudo-registers are present.
int MaxPseudoRegInCfg(const RtlControlFlowGraph& cfg);

}  // namespace rom_nom_nom

#endif  // CORE_RTL_SEXPR_UTILS_H_
