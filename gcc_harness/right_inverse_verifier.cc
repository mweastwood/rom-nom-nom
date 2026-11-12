#include "gcc_harness/right_inverse_verifier.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_sexpr_utils.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gcc_harness/toy_subfunction_inverses.h"

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {
struct GccLangIndepOption {
  const char* string;
  int* variable;
  int on_value;
};
extern GccLangIndepOption f_options[52];
extern int obey_regdecls;
extern int flag_delayed_branch;
extern int flag_schedule_insns;
extern int flag_schedule_insns_after_reload;
extern int flag_expensive_optimizations;
void restore_reg_data(rtx_def* first);
void reg_scan(rtx_def* f, int nregs, int repeat);
void regclass_init();
void regclass(rtx_def* f, int nregs);
void stupid_life_analysis(rtx_def* f, int nregs, FILE* file);
}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace rom_nom_nom::gcc_harness {
namespace {

const SubfunctionInverseProvider* DefaultInverseProvider() {
  static const auto* const default_toy_inverses = new ToySubfunctionInverses();
  return default_toy_inverses;
}

std::string FormatWordsSummary(absl::Span<const uint32_t> words) {
  std::vector<std::string> hex_words;
  hex_words.reserve(words.size());
  for (uint32_t w : words) {
    hex_words.push_back(absl::StrFormat("0x%08x", w));
  }
  return absl::StrJoin(hex_words, ", ");
}

std::string ExtractInstructionLines(std::string_view asm_text) {
  std::vector<std::string> lines;
  for (std::string_view raw : absl::StrSplit(asm_text, '\n')) {
    const size_t hash_pos = raw.find('#');
    if (hash_pos != std::string_view::npos) {
      raw = raw.substr(0, hash_pos);
    }
    std::string_view line = absl::StripAsciiWhitespace(raw);
    if (line.empty() || line.front() == '.') {
      continue;
    }
    if (line.back() == ':' && line.find_first_of(" \t,()") == std::string_view::npos) {
      continue;
    }
    lines.emplace_back(line);
  }
  return absl::StrJoin(lines, "\n");
}

std::string StripFileDirectives(std::string_view asm_text) {
  std::vector<std::string> lines;
  for (std::string_view raw : absl::StrSplit(asm_text, '\n')) {
    std::string_view trimmed = absl::StripAsciiWhitespace(raw);
    if (absl::StartsWith(trimmed, ".file")) {
      continue;
    }
    lines.emplace_back(raw);
  }
  return absl::StrJoin(lines, "\n");
}

}  // namespace

RightInverseVerifier::RightInverseVerifier(GccContext* ctx, FunctionHooks* hooks,
                                           const SubfunctionInverseProvider* inverses)
    : ctx_(ctx),
      hooks_(hooks),
      inverses_(inverses != nullptr ? inverses : DefaultInverseProvider()) {}

RightInverseResult RightInverseVerifier::VerifyRtlPassRightInverse(
    const RtlControlFlowGraph& target_x, InverseCfgFn inverse_g, ForwardHookFn forward_f,
    RtlPassKind target_pass) const {
  RightInverseResult result;
  result.expected_summary = target_x.FormatSummary();

  const RtlControlFlowGraph lifted_y = inverse_g(target_x);
  if (!target_x.rpo_blocks.empty() && lifted_y.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  ctx_->MaterializeFromCfg(lifted_y);
  forward_f(ctx_, hooks_);

  const RtlControlFlowGraph actual_fx = ctx_->DematerializeToCfg(ctx_->GetInsns(), target_pass);

  result.actual_summary = actual_fx.FormatSummary();
  result.cfg_comparison = actual_fx.CompareAgainst(target_x);
  result.matches = result.cfg_comparison.MatchesExactly();
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  return result;
}

// -----------------------------------------------------------------------------
// Stage 0: Final Assembly (`final.c`, `mips.c`) & GAS (`tc-mips.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyGasMipsIpRightInverse(uint32_t word_x) const {
  RightInverseResult result;
  result.expected_words = {word_x};
  result.expected_summary = FormatWordsSummary(result.expected_words);

  const std::string asm_y = inverses_->InvertGasMipsIp(word_x);
  if (asm_y.empty()) {
    result.matches = false;
    result.diff_detail = "InvertGasMipsIp returned empty string";
    return result;
  }
  const uint32_t actual_word = hooks_->RunGasMipsIp(asm_y);
  result.actual_words = {actual_word};
  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (actual_word == word_x);
  if (!result.matches) {
    result.diff_detail =
        absl::StrFormat("mips_ip(%s) got 0x%08x != expected 0x%08x", asm_y, actual_word, word_x);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyGasMacroRightInverse(
    absl::Span<const uint32_t> macro_words_x) const {
  RightInverseResult result;
  result.expected_words.assign(macro_words_x.begin(), macro_words_x.end());
  result.expected_summary = FormatWordsSummary(macro_words_x);

  const std::string macro_asm_y = inverses_->InvertGasMacro(macro_words_x);
  if (macro_words_x.empty() || absl::StripAsciiWhitespace(macro_asm_y).empty()) {
    result.matches = false;
    result.diff_detail = "InvertGasMacro returned empty assembly string";
    return result;
  }
  result.actual_words = hooks_->RunGasMacro(macro_asm_y);
  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (result.actual_words == result.expected_words);
  if (!result.matches) {
    result.diff_detail =
        absl::StrFormat("GAS macro(%s) words got [%s] != expected [%s]", macro_asm_y,
                        result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyGasAppendInsnRightInverse(
    absl::Span<const uint32_t> words_x) const {
  RightInverseResult result;
  result.expected_words.assign(words_x.begin(), words_x.end());
  result.expected_summary = FormatWordsSummary(words_x);

  const std::vector<std::string> asm_lines_y = inverses_->InvertGasAppendInsn(words_x);
  if (!words_x.empty() && asm_lines_y.empty()) {
    result.matches = false;
    result.diff_detail = "InvertGasAppendInsn returned empty assembly lines";
    return result;
  }
  result.actual_words = hooks_->RunGasAppendInsnReorder(asm_lines_y);
  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (result.actual_words == result.expected_words);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("append_insn words got [%s] != expected [%s]",
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyFinalScanInsnRightInverse(
    std::string_view asm_line_x) const {
  RightInverseResult result;
  const std::string normalized_expected = ExtractInstructionLines(asm_line_x);
  result.expected_summary = normalized_expected.empty()
                                ? std::string(absl::StripAsciiWhitespace(asm_line_x))
                                : normalized_expected;

  const std::string rtl_pat_y = inverses_->InvertFinalScanInsn(asm_line_x);
  if (rtl_pat_y.empty()) {
    result.matches = false;
    result.diff_detail = "InvertFinalScanInsn returned empty RTL pattern";
    return result;
  }
  ctx_->BeginFunction("final_scan_verify_fn");
  Rtx insn = ctx_->EmitPattern(rtl_pat_y);
  const std::string emitted = hooks_->RunFinalScanInsn(insn);
  result.actual_summary = ExtractInstructionLines(emitted);
  if (absl::StrContains(asm_line_x, "$L")) {
    for (size_t pos = 0; (pos = result.actual_summary.find(".L", pos)) != std::string::npos;
         pos += 2) {
      result.actual_summary[pos] = '$';
    }
  }
  if (asm_line_x.find('\t') == std::string_view::npos) {
    std::replace(result.actual_summary.begin(), result.actual_summary.end(), '\t', ' ');
  }
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("final_scan_insn(%s) got '%s' != expected '%s'", rtl_pat_y,
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyFunctionPrologueEpilogueRightInverse(
    std::string_view asm_text_x) const {
  RightInverseResult result;
  const PrologueEpilogueFrameSpec spec = inverses_->InvertFunctionPrologueEpilogue(asm_text_x);
  if (spec.var_size < 0) {
    result.expected_summary = std::string(asm_text_x);
    result.matches = false;
    result.diff_detail = "InvertFunctionPrologueEpilogue returned invalid var_size < 0";
    return result;
  }
  std::string fn_name = "harness_func";
  for (std::string_view raw : absl::StrSplit(asm_text_x, '\n')) {
    std::string_view line = absl::StripAsciiWhitespace(raw);
    if (absl::StartsWith(line, ".ent") && line.size() > 4 &&
        absl::ascii_isspace(static_cast<unsigned char>(line[4]))) {
      std::string_view sym = absl::StripAsciiWhitespace(line.substr(4));
      if (!sym.empty()) {
        fn_name = std::string(sym);
        break;
      }
    }
  }
  ctx_->BeginFunction(fn_name);
  const std::string emitted =
      hooks_->RunFunctionPrologueEpilogue(spec.var_size, spec.saved_gpr_mask, spec.saved_fpr_mask);

  if (absl::StrContains(asm_text_x, ".frame")) {
    result.expected_summary = StripFileDirectives(asm_text_x);
    result.actual_summary = StripFileDirectives(emitted);
  } else {
    result.expected_summary = ExtractInstructionLines(asm_text_x);
    result.actual_summary = ExtractInstructionLines(emitted);
  }
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat(
        "function_prologue/epilogue(var_size=%d, mask=0x%08x, fmask=0x%08x) got:\n%s\n!= "
        "expected:\n%s",
        spec.var_size, spec.saved_gpr_mask, spec.saved_fpr_mask, result.actual_summary,
        result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyFinalAndGasStageRightInverse(
    absl::Span<const uint32_t> machine_words_x) const {
  RightInverseResult result;
  result.expected_words.assign(machine_words_x.begin(), machine_words_x.end());
  result.expected_summary = FormatWordsSummary(machine_words_x);

  const RtlControlFlowGraph lifted_dbr = inverses_->InvertFinalAndGasFunction(machine_words_x);
  if (machine_words_x.empty() || lifted_dbr.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "InvertFinalAndGasFunction returned empty RtlControlFlowGraph";
    return result;
  }

  ctx_->MaterializeFromCfg(lifted_dbr);
  const std::string combined_asm = hooks_->RunFinal();
  result.actual_words = hooks_->RunGasMdAssemble(combined_asm);

  result.actual_summary = FormatWordsSummary(result.actual_words);
  result.matches = (result.actual_words == result.expected_words);
  if (!result.matches) {
    result.diff_detail =
        absl::StrFormat("Machine words got [%s] != expected [%s]\nasm:\n%s", result.actual_summary,
                        result.expected_summary, combined_asm);
  }
  return result;
}

namespace {

bool RelocImmediateMatchesExpected(const Stage0Relocation& rel, uint32_t sym_vram, uint32_t gp_vram,
                                   const std::vector<uint32_t>& expected_words) {
  if (rel.word_index >= expected_words.size()) {
    return false;
  }
  const uint32_t full_addr = static_cast<uint32_t>(static_cast<int64_t>(sym_vram) + rel.addend);
  const uint32_t exp_word = expected_words[rel.word_index];
  switch (rel.kind) {
    case Stage0RelocKind::kMips26:
      return (exp_word & 0x03ffffffu) == ((full_addr >> 2) & 0x03ffffffu);
    case Stage0RelocKind::kMipsHi16:
      return (exp_word & 0xffffu) == (((full_addr + 0x8000u) >> 16) & 0xffffu);
    case Stage0RelocKind::kMipsLo16:
      return (exp_word & 0xffffu) == (full_addr & 0xffffu);
    case Stage0RelocKind::kMipsGpRel16:
    case Stage0RelocKind::kMipsLiteral: {
      if (gp_vram == 0u) {
        return false;
      }
      const int32_t gp_delta = static_cast<int32_t>(full_addr - gp_vram);
      if (gp_delta < -32768 || gp_delta > 32767) {
        return false;
      }
      return (exp_word & 0xffffu) == (static_cast<uint32_t>(gp_delta) & 0xffffu);
    }
    case Stage0RelocKind::kMips32:
      return exp_word == full_addr;
  }
  return false;
}

RightInverseResult CompareStage0FunctionInputs(const Stage0FunctionInput& input_x,
                                               const Stage0FunctionInput& actual_stage0,
                                               std::string_view combined_asm) {
  RightInverseResult result;
  result.expected_words = input_x.machine_words;
  result.expected_summary = FormatWordsSummary(result.expected_words);
  result.actual_words = actual_stage0.machine_words;

  if (actual_stage0.jump_tables.size() != input_x.jump_tables.size()) {
    result.matches = false;
    result.diff_detail =
        absl::StrFormat("Jump table count got %zu != expected %zu\nasm:\n%s",
                        actual_stage0.jump_tables.size(), input_x.jump_tables.size(), combined_asm);
    return result;
  }

  // If `input_x` represents a linked N64 ROM function (`base_vram != 0` with
  // already-linked relocations), resolve `actual_stage0.relocations` against
  // `vram_to_symbol`, `func_<08x>`, jump-table `table_vram`, and constant-pool
  // `entry_vram` addresses before comparing `.text` words.
  const bool is_linked_rom_input = input_x.base_vram != 0 && input_x.relocations.empty();
  if (is_linked_rom_input) {
    if (actual_stage0.constant_pool.size() != input_x.constant_pool.size()) {
      result.matches = false;
      result.diff_detail = absl::StrFormat("Constant-pool count got %zu != expected %zu\nasm:\n%s",
                                           actual_stage0.constant_pool.size(),
                                           input_x.constant_pool.size(), combined_asm);
      return result;
    }

    // Enforce MIPS O32 ABI rule: every `R_MIPS_HI16` relocation must have a
    // matching `R_MIPS_LO16` relocation for the same `(symbol_name, addend)`.
    for (const Stage0Relocation& rel : actual_stage0.relocations) {
      if (rel.kind != Stage0RelocKind::kMipsHi16) {
        continue;
      }
      bool has_matching_lo16 = false;
      for (const Stage0Relocation& cand : actual_stage0.relocations) {
        if (cand.kind == Stage0RelocKind::kMipsLo16 && cand.symbol_name == rel.symbol_name &&
            cand.addend == rel.addend) {
          has_matching_lo16 = true;
          break;
        }
      }
      if (!has_matching_lo16) {
        result.matches = false;
        result.diff_detail = absl::StrFormat(
            "Orphaned R_MIPS_HI16 relocation for '%s'+%d at word %zu without matching "
            "R_MIPS_LO16\nasm:\n%s",
            rel.symbol_name, static_cast<int>(rel.addend), rel.word_index, combined_asm);
        return result;
      }
    }

    absl::flat_hash_map<std::string, std::vector<const Stage0Relocation*>> relocs_by_sym;
    for (const Stage0Relocation& rel : actual_stage0.relocations) {
      relocs_by_sym[rel.symbol_name].push_back(&rel);
    }

    absl::flat_hash_map<std::string, uint32_t> sym_to_vram;
    for (const auto& [vram, sym] : input_x.vram_to_symbol) {
      sym_to_vram[sym] = vram;
    }

    // Bijectively match `actual_stage0.jump_tables` to `input_x.jump_tables` by
    // exact `target_word_indices` AND concrete `table_vram` relocation match.
    const size_t num_jts = actual_stage0.jump_tables.size();
    std::vector<bool> used_exp_jt(num_jts, false);
    std::vector<size_t> matched_exp_jt(num_jts, 0);
    std::function<bool(size_t)> match_jts = [&](size_t act_idx) -> bool {
      if (act_idx == num_jts) {
        return true;
      }
      const Stage0JumpTable& act_jt = actual_stage0.jump_tables[act_idx];
      const auto rel_it = relocs_by_sym.find(act_jt.label_name);
      if (rel_it == relocs_by_sym.end() || rel_it->second.empty()) {
        return false;
      }
      for (size_t exp_idx = 0; exp_idx < num_jts; ++exp_idx) {
        if (used_exp_jt[exp_idx]) {
          continue;
        }
        const Stage0JumpTable& exp_jt = input_x.jump_tables[exp_idx];
        if (exp_jt.table_vram == 0u || act_jt.target_word_indices != exp_jt.target_word_indices) {
          continue;
        }
        bool relocs_compatible = true;
        for (const Stage0Relocation* rel : rel_it->second) {
          if (!RelocImmediateMatchesExpected(*rel, exp_jt.table_vram, input_x.gp_vram,
                                             input_x.machine_words)) {
            relocs_compatible = false;
            break;
          }
        }
        if (!relocs_compatible) {
          continue;
        }
        used_exp_jt[exp_idx] = true;
        matched_exp_jt[act_idx] = exp_idx;
        if (match_jts(act_idx + 1)) {
          return true;
        }
        used_exp_jt[exp_idx] = false;
      }
      return false;
    };
    if (!match_jts(0)) {
      result.matches = false;
      result.diff_detail = absl::StrFormat(
          "Jump table target_word_indices / table_vram mismatch in linked-ROM "
          "verification\nasm:\n%s",
          combined_asm);
      return result;
    }
    for (size_t act_idx = 0; act_idx < num_jts; ++act_idx) {
      sym_to_vram[actual_stage0.jump_tables[act_idx].label_name] =
          input_x.jump_tables[matched_exp_jt[act_idx]].table_vram;
    }

    // Bijectively match `actual_stage0.constant_pool` to `input_x.constant_pool`
    // by exact `(byte_size, words)` AND concrete `entry_vram` relocation match.
    const size_t num_cps = actual_stage0.constant_pool.size();
    std::vector<bool> used_exp_cp(num_cps, false);
    std::vector<size_t> matched_exp_cp(num_cps, 0);
    std::function<bool(size_t)> match_cps = [&](size_t act_idx) -> bool {
      if (act_idx == num_cps) {
        return true;
      }
      const Stage0ConstantPoolEntry& act_cp = actual_stage0.constant_pool[act_idx];
      const auto rel_it = relocs_by_sym.find(act_cp.label_name);
      if (rel_it == relocs_by_sym.end() || rel_it->second.empty()) {
        return false;
      }
      for (size_t exp_idx = 0; exp_idx < num_cps; ++exp_idx) {
        if (used_exp_cp[exp_idx]) {
          continue;
        }
        const Stage0ConstantPoolEntry& exp_cp = input_x.constant_pool[exp_idx];
        if (exp_cp.entry_vram == 0u || act_cp.byte_size != exp_cp.byte_size ||
            act_cp.words != exp_cp.words) {
          continue;
        }
        bool relocs_compatible = true;
        for (const Stage0Relocation* rel : rel_it->second) {
          if (!RelocImmediateMatchesExpected(*rel, exp_cp.entry_vram, input_x.gp_vram,
                                             input_x.machine_words)) {
            relocs_compatible = false;
            break;
          }
        }
        if (!relocs_compatible) {
          continue;
        }
        used_exp_cp[exp_idx] = true;
        matched_exp_cp[act_idx] = exp_idx;
        if (match_cps(act_idx + 1)) {
          return true;
        }
        used_exp_cp[exp_idx] = false;
      }
      return false;
    };
    if (!match_cps(0)) {
      result.matches = false;
      result.diff_detail = absl::StrFormat(
          "Constant-pool entry / entry_vram mismatch in linked-ROM verification\nasm:\n%s",
          combined_asm);
      return result;
    }
    for (size_t act_idx = 0; act_idx < num_cps; ++act_idx) {
      sym_to_vram[actual_stage0.constant_pool[act_idx].label_name] =
          input_x.constant_pool[matched_exp_cp[act_idx]].entry_vram;
    }

    auto resolve_sym_vram = [&](const std::string& sym, uint32_t* out_vram) -> bool {
      if (auto it = sym_to_vram.find(sym); it != sym_to_vram.end()) {
        *out_vram = it->second;
        return true;
      }
      if (absl::StartsWith(sym, "func_") && sym.size() == 13) {
        uint32_t parsed_vram = 0;
        if (absl::SimpleHexAtoi(sym.substr(5), &parsed_vram)) {
          *out_vram = parsed_vram;
          return true;
        }
      }
      return false;
    };
    absl::flat_hash_set<size_t> relocated_indices;
    absl::flat_hash_map<size_t, uint32_t> hi16_page_by_word;
    absl::flat_hash_map<std::string, size_t> jt_sym_to_idx;
    for (size_t jt_i = 0; jt_i < actual_stage0.jump_tables.size(); ++jt_i) {
      if (!actual_stage0.jump_tables[jt_i].label_name.empty()) {
        jt_sym_to_idx[actual_stage0.jump_tables[jt_i].label_name] = jt_i;
      }
    }
    absl::flat_hash_map<size_t, size_t> word_to_jt_idx;

    for (const Stage0Relocation& rel : actual_stage0.relocations) {
      if (rel.word_index >= result.actual_words.size()) {
        result.matches = false;
        result.diff_detail =
            absl::StrFormat("Relocation word_index %zu out of range (%zu)\nasm:\n%s",
                            rel.word_index, result.actual_words.size(), combined_asm);
        return result;
      }
      relocated_indices.insert(rel.word_index);
      uint32_t sym_vram = 0;
      if (!resolve_sym_vram(rel.symbol_name, &sym_vram)) {
        result.matches = false;
        result.diff_detail = absl::StrFormat(
            "Unresolved relocation symbol '%s' at word %zu in linked-ROM verification\nasm:\n%s",
            rel.symbol_name, rel.word_index, combined_asm);
        return result;
      }
      if (rel.kind == Stage0RelocKind::kMipsHi16) {
        const uint32_t hi_w = result.actual_words[rel.word_index];
        if (((hi_w >> 26) & 0x3fu) != 0x0fu || ((hi_w >> 21) & 0x1fu) != 0u ||
            ((hi_w >> 16) & 0x1fu) == 0u) {
          result.matches = false;
          result.diff_detail = absl::StrFormat(
              "R_MIPS_HI16 relocation for '%s' at word %zu is not on a valid lui instruction\n"
              "asm:\n%s",
              rel.symbol_name, rel.word_index, combined_asm);
          return result;
        }
        const uint32_t full_addr =
            static_cast<uint32_t>(static_cast<int64_t>(sym_vram) + rel.addend);
        hi16_page_by_word[rel.word_index] = ((full_addr + 0x8000u) >> 16) & 0xffffu;
      } else if (const auto jt_it = jt_sym_to_idx.find(rel.symbol_name);
                 jt_it != jt_sym_to_idx.end()) {
        word_to_jt_idx[rel.word_index] = jt_it->second;
      }
    }

    // Compute CFG- and GPR-level must-equal `R_MIPS_HI16` 64KB page provenance over
    // `result.actual_words` + `actual_stage0.jump_tables` so every `R_MIPS_LO16` base
    // register `rs` is guaranteed to hold a matching `R_MIPS_HI16`-backed page on all
    // reaching paths.
    struct HiProvState {
      bool reachable = false;
      std::array<std::optional<uint32_t>, 32> hi_page{};
      std::array<std::vector<size_t>, 32> jt_indices{};
    };
    const size_t num_words = result.actual_words.size();
    std::vector<HiProvState> in_state(num_words);
    std::vector<HiProvState> pre_exec_state(num_words);

    auto kill_gpr = [](HiProvState* st, uint32_t reg) {
      if (reg > 0u && reg < 32u) {
        st->hi_page[reg] = std::nullopt;
        st->jt_indices[reg].clear();
      }
    };

    auto merge_state = [](const HiProvState& src, HiProvState* dst) -> bool {
      if (!src.reachable) {
        return false;
      }
      if (!dst->reachable) {
        *dst = src;
        return true;
      }
      bool changed = false;
      for (size_t r = 1; r < 32; ++r) {
        if (dst->hi_page[r].has_value() &&
            (!src.hi_page[r].has_value() || *src.hi_page[r] != *dst->hi_page[r])) {
          dst->hi_page[r] = std::nullopt;
          changed = true;
        }
        for (size_t jt_i : src.jt_indices[r]) {
          if (std::find(dst->jt_indices[r].begin(), dst->jt_indices[r].end(), jt_i) ==
              dst->jt_indices[r].end()) {
            dst->jt_indices[r].push_back(jt_i);
            changed = true;
          }
        }
      }
      return changed;
    };

    auto is_branch_or_jump_word = [](uint32_t w) -> bool {
      const uint32_t op = (w >> 26) & 0x3fu;
      const uint32_t rs = (w >> 21) & 0x1fu;
      const uint32_t rt = (w >> 16) & 0x1fu;
      const uint32_t funct = w & 0x3fu;
      if (op == 0x02u || op == 0x03u || (op >= 0x04u && op <= 0x07u) ||
          (op >= 0x14u && op <= 0x17u)) {
        return true;
      }
      if (op == 0x00u && (funct == 0x08u || funct == 0x09u)) {
        return true;
      }
      if (op == 0x01u && (rt <= 0x03u || (rt >= 0x10u && rt <= 0x13u))) {
        return true;
      }
      if (op == 0x11u && rs == 0x08u && rt <= 0x03u) {
        return true;
      }
      return false;
    };

    auto exec_word = [&](uint32_t w, size_t w_idx, HiProvState* st) {
      const uint32_t op = (w >> 26) & 0x3fu;
      const uint32_t rs = (w >> 21) & 0x1fu;
      const uint32_t rt = (w >> 16) & 0x1fu;
      const uint32_t rd = (w >> 11) & 0x1fu;
      const uint32_t sa = (w >> 6) & 0x1fu;
      const uint32_t funct = w & 0x3fu;
      const int16_t simm = static_cast<int16_t>(w & 0xffffu);

      if (op == 0x0fu) {
        if (rt != 0u) {
          if (const auto hi_it = hi16_page_by_word.find(w_idx); hi_it != hi16_page_by_word.end()) {
            st->hi_page[rt] = hi_it->second;
            st->jt_indices[rt].clear();
          } else {
            kill_gpr(st, rt);
          }
        }
        return;
      }

      if (op == 0x00u) {
        if (sa == 0u && (funct == 0x21u || funct == 0x2du || funct == 0x25u) &&
            (rs == 0u || rt == 0u)) {
          if (rd != 0u) {
            const uint32_t src = (rs != 0u) ? rs : rt;
            if (src == 0u) {
              kill_gpr(st, rd);
            } else {
              st->hi_page[rd] = st->hi_page[src];
              st->jt_indices[rd] = st->jt_indices[src];
            }
          }
          return;
        }
        if (sa == 0u && (funct == 0x21u || funct == 0x2du) && rs != 0u && rt != 0u) {
          if (rd != 0u) {
            const bool rs_has_jt = !st->jt_indices[rs].empty();
            const bool rt_has_jt = !st->jt_indices[rt].empty();
            const bool rs_has_hi = st->hi_page[rs].has_value();
            const bool rt_has_hi = st->hi_page[rt].has_value();
            if (rs != rt && (rs_has_jt != rt_has_jt)) {
              std::vector<size_t> next_jts = rs_has_jt ? st->jt_indices[rs] : st->jt_indices[rt];
              kill_gpr(st, rd);
              st->jt_indices[rd] = std::move(next_jts);
            } else if (rs != rt && (rs_has_hi != rt_has_hi)) {
              const uint32_t page = rs_has_hi ? *st->hi_page[rs] : *st->hi_page[rt];
              kill_gpr(st, rd);
              st->hi_page[rd] = page;
            } else {
              kill_gpr(st, rd);
            }
          }
          return;
        }
        if (funct == 0x08u || funct == 0x0cu || funct == 0x0du || funct == 0x0fu ||
            funct == 0x11u || funct == 0x13u || (funct >= 0x18u && funct <= 0x1fu) ||
            (funct >= 0x30u && funct <= 0x37u)) {
          return;
        }
        kill_gpr(st, rd);
        return;
      }

      if (op == 0x09u || op == 0x19u) {
        if (rt != 0u) {
          if (const auto jt_it = word_to_jt_idx.find(w_idx); jt_it != word_to_jt_idx.end()) {
            kill_gpr(st, rt);
            st->jt_indices[rt] = {jt_it->second};
          } else if (!relocated_indices.contains(w_idx) && simm == 0 && rs != 0u) {
            st->hi_page[rt] = st->hi_page[rs];
            st->jt_indices[rt] = st->jt_indices[rs];
          } else {
            kill_gpr(st, rt);
          }
        }
        return;
      }

      if ((op >= 0x08u && op <= 0x0eu) || op == 0x18u) {
        kill_gpr(st, rt);
        return;
      }

      if (op >= 0x10u && op <= 0x12u) {
        if (rs <= 0x02u) {
          kill_gpr(st, rt);
        }
        return;
      }

      if (op == 0x23u || op == 0x27u || op == 0x37u) {
        if (rt != 0u) {
          if (const auto jt_it = word_to_jt_idx.find(w_idx); jt_it != word_to_jt_idx.end()) {
            kill_gpr(st, rt);
            st->jt_indices[rt] = {jt_it->second};
          } else if (rs != 0u && !st->jt_indices[rs].empty()) {
            std::vector<size_t> next_jts = st->jt_indices[rs];
            kill_gpr(st, rt);
            st->jt_indices[rt] = std::move(next_jts);
          } else {
            kill_gpr(st, rt);
          }
        }
        return;
      }

      if ((op >= 0x20u && op <= 0x26u) || op == 0x1au || op == 0x1bu || op == 0x30u ||
          op == 0x34u || op == 0x38u || op == 0x3cu) {
        kill_gpr(st, rt);
      }
    };

    std::vector<bool> is_delay_slot(num_words, false);
    for (size_t pc = 0; pc < num_words;) {
      if (is_branch_or_jump_word(result.actual_words[pc]) && pc + 1 < num_words) {
        is_delay_slot[pc + 1] = true;
        pc += 2;
      } else {
        ++pc;
      }
    }

    auto run_provenance_from_seed = [&](size_t seed_pc,
                                        const std::vector<bool>* forbidden_targets) {
      if (seed_pc >= num_words) {
        return;
      }
      in_state[seed_pc].reachable = true;
      std::vector<size_t> worklist = {seed_pc};
      std::vector<bool> in_queue(num_words, false);
      in_queue[seed_pc] = true;
      size_t q_head = 0;

      auto propagate = [&](size_t target_pc, const HiProvState& st) {
        if (target_pc >= num_words ||
            (forbidden_targets != nullptr && (*forbidden_targets)[target_pc])) {
          return;
        }
        if (merge_state(st, &in_state[target_pc]) && !in_queue[target_pc]) {
          in_queue[target_pc] = true;
          worklist.push_back(target_pc);
        }
      };

      while (q_head < worklist.size()) {
        const size_t pc = worklist[q_head++];
        in_queue[pc] = false;
        merge_state(in_state[pc], &pre_exec_state[pc]);
        const uint32_t w = result.actual_words[pc];

        if (!is_branch_or_jump_word(w)) {
          HiProvState next_st = in_state[pc];
          exec_word(w, pc, &next_st);
          propagate(pc + 1, next_st);
          continue;
        }
        if (pc + 1 >= num_words) {
          continue;
        }

        const size_t d_pc = pc + 1;
        const uint32_t d_w = result.actual_words[d_pc];
        const uint32_t op = (w >> 26) & 0x3fu;
        const uint32_t rs = (w >> 21) & 0x1fu;
        const uint32_t rt = (w >> 16) & 0x1fu;
        const uint32_t rd = (w >> 11) & 0x1fu;
        const uint32_t funct = w & 0x3fu;
        const int16_t simm = static_cast<int16_t>(w & 0xffffu);

        HiProvState slot_in = in_state[pc];
        if (op == 0x03u || (op == 0x01u && (rt & 0x1cu) == 0x10u)) {
          kill_gpr(&slot_in, 31u);
        } else if (op == 0x00u && funct == 0x09u) {
          kill_gpr(&slot_in, rd);
        }
        merge_state(slot_in, &pre_exec_state[d_pc]);

        const bool is_likely =
            (op >= 0x14u && op <= 0x17u) ||
            (op == 0x01u && (rt == 0x02u || rt == 0x03u || rt == 0x12u || rt == 0x13u)) ||
            (op == 0x11u && rs == 0x08u && (rt == 0x02u || rt == 0x03u));
        const bool is_cond_branch = is_likely || (op >= 0x04u && op <= 0x07u) ||
                                    (op == 0x01u && (rt <= 0x01u || rt == 0x10u || rt == 0x11u)) ||
                                    (op == 0x11u && rs == 0x08u && rt <= 0x01u);

        if (is_likely) {
          HiProvState taken_st = slot_in;
          exec_word(d_w, d_pc, &taken_st);
          const int64_t target_pc = static_cast<int64_t>(pc) + 1 + static_cast<int64_t>(simm);
          if (target_pc >= 0 && static_cast<size_t>(target_pc) < num_words) {
            propagate(static_cast<size_t>(target_pc), taken_st);
          }
          propagate(pc + 2, slot_in);
        } else if (is_cond_branch) {
          HiProvState after_d = slot_in;
          exec_word(d_w, d_pc, &after_d);
          const int64_t target_pc = static_cast<int64_t>(pc) + 1 + static_cast<int64_t>(simm);
          if (target_pc >= 0 && static_cast<size_t>(target_pc) < num_words) {
            propagate(static_cast<size_t>(target_pc), after_d);
          }
          const bool is_uncond_branch = (op == 0x04u && rs == rt) ||
                                        (op == 0x01u && rs == 0u && (rt == 0x01u || rt == 0x11u));
          if (!is_uncond_branch) {
            propagate(pc + 2, after_d);
          }
        } else if (op == 0x03u || (op == 0x00u && funct == 0x09u)) {
          HiProvState after_call = slot_in;
          exec_word(d_w, d_pc, &after_call);
          for (uint32_t caller_reg = 1; caller_reg <= 15; ++caller_reg) {
            kill_gpr(&after_call, caller_reg);
          }
          kill_gpr(&after_call, 24u);
          kill_gpr(&after_call, 25u);
          kill_gpr(&after_call, 31u);
          propagate(pc + 2, after_call);
        } else if (op == 0x02u) {
          HiProvState after_d = slot_in;
          exec_word(d_w, d_pc, &after_d);
          if (!relocated_indices.contains(pc)) {
            const size_t target_pc = static_cast<size_t>(w & 0x03ffffffu);
            propagate(target_pc, after_d);
          }
        } else if (op == 0x00u && funct == 0x08u && rs != 31u) {
          const std::vector<size_t> target_jts = in_state[pc].jt_indices[rs];
          HiProvState after_d = slot_in;
          exec_word(d_w, d_pc, &after_d);
          for (size_t jt_i : target_jts) {
            if (jt_i < actual_stage0.jump_tables.size()) {
              for (size_t target_pc : actual_stage0.jump_tables[jt_i].target_word_indices) {
                propagate(target_pc, after_d);
              }
            }
          }
        }
      }
    };

    if (num_words > 0) {
      run_provenance_from_seed(/*seed_pc=*/0, /*forbidden_targets=*/nullptr);
      std::vector<bool> entry_reachable(num_words, false);
      for (size_t i = 0; i < num_words; ++i) {
        entry_reachable[i] = pre_exec_state[i].reachable;
      }
      for (size_t i = 0; i < num_words; ++i) {
        if (!pre_exec_state[i].reachable && !is_delay_slot[i]) {
          run_provenance_from_seed(i, &entry_reachable);
        }
      }
    }

    for (const Stage0Relocation& rel : actual_stage0.relocations) {
      uint32_t sym_vram = 0;
      resolve_sym_vram(rel.symbol_name, &sym_vram);
      const uint32_t full_addr = static_cast<uint32_t>(static_cast<int64_t>(sym_vram) + rel.addend);
      if (rel.kind == Stage0RelocKind::kMips26) {
        result.actual_words[rel.word_index] =
            (result.actual_words[rel.word_index] & 0xfc000000u) | ((full_addr >> 2) & 0x03ffffffu);
      } else if (rel.kind == Stage0RelocKind::kMipsHi16) {
        const uint32_t hi = ((full_addr + 0x8000u) >> 16) & 0xffffu;
        result.actual_words[rel.word_index] =
            (result.actual_words[rel.word_index] & 0xffff0000u) | hi;
      } else if (rel.kind == Stage0RelocKind::kMipsLo16) {
        const uint32_t req_hi = ((full_addr + 0x8000u) >> 16) & 0xffffu;
        const uint32_t lo_w = result.actual_words[rel.word_index];
        const uint32_t base_rs = (lo_w >> 21) & 0x1fu;
        const HiProvState& pre_st = pre_exec_state[rel.word_index];
        if (base_rs == 0u || !pre_st.reachable || !pre_st.hi_page[base_rs].has_value() ||
            *pre_st.hi_page[base_rs] != req_hi) {
          result.matches = false;
          result.diff_detail = absl::StrFormat(
              "Orphaned R_MIPS_LO16 relocation for '%s'+%d at word %zu: base register $%u lacks "
              "R_MIPS_HI16 backing on page 0x%04x\nasm:\n%s",
              rel.symbol_name, static_cast<int>(rel.addend), rel.word_index, base_rs, req_hi,
              combined_asm);
          return result;
        }
        result.actual_words[rel.word_index] =
            (result.actual_words[rel.word_index] & 0xffff0000u) | (full_addr & 0xffffu);
      } else if (rel.kind == Stage0RelocKind::kMipsGpRel16 ||
                 rel.kind == Stage0RelocKind::kMipsLiteral) {
        if (input_x.gp_vram == 0u) {
          result.matches = false;
          result.diff_detail =
              absl::StrFormat("GP-relative relocation for '%s' requires non-zero gp_vram\nasm:\n%s",
                              rel.symbol_name, combined_asm);
          return result;
        }
        const int32_t gp_delta = static_cast<int32_t>(full_addr - input_x.gp_vram);
        if (gp_delta < -32768 || gp_delta > 32767) {
          result.matches = false;
          result.diff_detail =
              absl::StrFormat("GP-relative offset %d out of 16-bit signed range for '%s'\nasm:\n%s",
                              gp_delta, rel.symbol_name, combined_asm);
          return result;
        }
        result.actual_words[rel.word_index] = (result.actual_words[rel.word_index] & 0xffff0000u) |
                                              (static_cast<uint32_t>(gp_delta) & 0xffffu);
      } else if (rel.kind == Stage0RelocKind::kMips32) {
        result.actual_words[rel.word_index] = full_addr;
      }
    }
    // Also relocate intra-function `j $L<n>` words (`op == 0x02`) to `input_x.base_vram`.
    for (size_t idx = 0; idx < result.actual_words.size();) {
      const uint32_t w = result.actual_words[idx];
      if (!relocated_indices.contains(idx) && ((w >> 26) & 0x3fu) == 0x02u) {
        const uint32_t local_word_idx = w & 0x03ffffffu;
        const uint32_t target_vram = input_x.base_vram + local_word_idx * 4u;
        result.actual_words[idx] = 0x08000000u | ((target_vram >> 2) & 0x03ffffffu);
      }
      const uint32_t op = (w >> 26) & 0x3fu;
      const uint32_t funct = w & 0x3fu;
      const bool has_delay =
          (op == 0x01u || op == 0x02u || op == 0x03u || (op >= 0x04u && op <= 0x07u) ||
           (op >= 0x14u && op <= 0x17u) || (op == 0x00u && (funct == 0x08u || funct == 0x09u)) ||
           (op == 0x11u && ((w >> 21) & 0x1fu) == 0x08u));
      idx += has_delay ? 2u : 1u;
    }
  }

  // Account for 16-byte (4-word) `.text` section trailing alignment padding when
  // `input_x.machine_words` carries section alignment `nop`s.
  const size_t padded_actual_len = (result.actual_words.size() + 3u) & ~size_t{3u};
  if (input_x.machine_words.size() > result.actual_words.size() &&
      input_x.machine_words.size() == padded_actual_len) {
    bool trailing_all_zero = true;
    for (size_t i = result.actual_words.size(); i < input_x.machine_words.size(); ++i) {
      if (input_x.machine_words[i] != 0x00000000u) {
        trailing_all_zero = false;
        break;
      }
    }
    if (trailing_all_zero) {
      result.actual_words.resize(padded_actual_len, 0x00000000u);
    }
  }

  result.actual_summary = FormatWordsSummary(result.actual_words);
  if (result.actual_words != result.expected_words) {
    result.matches = false;
    result.diff_detail =
        absl::StrFormat("Machine words got [%s] != expected [%s]\nasm:\n%s", result.actual_summary,
                        result.expected_summary, combined_asm);
    return result;
  }

  if (!is_linked_rom_input) {
    for (size_t i = 0; i < input_x.jump_tables.size(); ++i) {
      if (actual_stage0.jump_tables[i].target_word_indices !=
          input_x.jump_tables[i].target_word_indices) {
        result.matches = false;
        result.diff_detail = absl::StrFormat(
            "Jump table %zu target_word_indices mismatch\nasm:\n%s", i, combined_asm);
        return result;
      }
    }
    if (actual_stage0.constant_pool.size() != input_x.constant_pool.size()) {
      result.matches = false;
      result.diff_detail = absl::StrFormat("Constant-pool count got %zu != expected %zu\nasm:\n%s",
                                           actual_stage0.constant_pool.size(),
                                           input_x.constant_pool.size(), combined_asm);
      return result;
    }
    if (actual_stage0.relocations.size() != input_x.relocations.size()) {
      result.matches = false;
      result.diff_detail = absl::StrFormat("Relocation count got %zu != expected %zu\nasm:\n%s",
                                           actual_stage0.relocations.size(),
                                           input_x.relocations.size(), combined_asm);
      return result;
    }

    absl::flat_hash_map<std::string, size_t> expected_jt_by_label;
    for (size_t i = 0; i < input_x.jump_tables.size(); ++i) {
      expected_jt_by_label[input_x.jump_tables[i].label_name] = i;
    }
    absl::flat_hash_map<std::string, size_t> actual_jt_by_label;
    for (size_t i = 0; i < actual_stage0.jump_tables.size(); ++i) {
      actual_jt_by_label[actual_stage0.jump_tables[i].label_name] = i;
    }

    absl::flat_hash_map<std::string, const Stage0ConstantPoolEntry*> expected_cp_by_label;
    for (const auto& cp : input_x.constant_pool) {
      expected_cp_by_label[cp.label_name] = &cp;
    }
    absl::flat_hash_map<std::string, const Stage0ConstantPoolEntry*> actual_cp_by_label;
    for (const auto& cp : actual_stage0.constant_pool) {
      actual_cp_by_label[cp.label_name] = &cp;
    }

    for (size_t i = 0; i < input_x.relocations.size(); ++i) {
      const Stage0Relocation& exp_r = input_x.relocations[i];
      const Stage0Relocation& act_r = actual_stage0.relocations[i];
      if (act_r.word_index != exp_r.word_index || act_r.kind != exp_r.kind ||
          act_r.addend != exp_r.addend) {
        result.matches = false;
        result.diff_detail = absl::StrFormat(
            "Relocation %zu mismatch: actual(word=%zu, kind=%d, sym=%s, add=%d) != "
            "expected(word=%zu, kind=%d, sym=%s, add=%d)\nasm:\n%s",
            i, act_r.word_index, static_cast<int>(act_r.kind), act_r.symbol_name,
            static_cast<int>(act_r.addend), exp_r.word_index, static_cast<int>(exp_r.kind),
            exp_r.symbol_name, static_cast<int>(exp_r.addend), combined_asm);
        return result;
      }
      if (auto exp_jt = expected_jt_by_label.find(exp_r.symbol_name);
          exp_jt != expected_jt_by_label.end()) {
        auto act_jt = actual_jt_by_label.find(act_r.symbol_name);
        if (act_jt == actual_jt_by_label.end() ||
            actual_stage0.jump_tables[act_jt->second].target_word_indices !=
                input_x.jump_tables[exp_jt->second].target_word_indices) {
          result.matches = false;
          result.diff_detail = absl::StrFormat(
              "Relocation %zu jump-table target mismatch for '%s' vs '%s'\nasm:\n%s", i,
              act_r.symbol_name, exp_r.symbol_name, combined_asm);
          return result;
        }
      } else if (auto exp_cp = expected_cp_by_label.find(exp_r.symbol_name);
                 exp_cp != expected_cp_by_label.end()) {
        auto act_cp = actual_cp_by_label.find(act_r.symbol_name);
        const bool pool_entry_matches = (act_cp != actual_cp_by_label.end() &&
                                         act_cp->second->byte_size == exp_cp->second->byte_size &&
                                         act_cp->second->words == exp_cp->second->words);
        if (!pool_entry_matches) {
          result.matches = false;
          result.diff_detail =
              absl::StrFormat("Relocation %zu constant-pool mismatch for '%s' vs '%s'\nasm:\n%s", i,
                              act_r.symbol_name, exp_r.symbol_name, combined_asm);
          return result;
        }
      } else if (act_r.symbol_name != exp_r.symbol_name) {
        result.matches = false;
        result.diff_detail =
            absl::StrFormat("Relocation %zu symbol mismatch: got '%s' != expected '%s'\nasm:\n%s",
                            i, act_r.symbol_name, exp_r.symbol_name, combined_asm);
        return result;
      }
    }
  }

  result.matches = true;
  return result;
}

bool IsRerunCseAfterLoopEnabled() {
  int rerun_cse = flag_expensive_optimizations;
  for (const auto& opt : f_options) {
    if (opt.string != nullptr && std::strcmp(opt.string, "rerun-cse-after-loop") == 0 &&
        opt.variable != nullptr) {
      rerun_cse = *opt.variable;
      break;
    }
  }
  return rerun_cse != 0;
}

int PassPipelineRank(RtlPassKind pass) {
  switch (pass) {
    case RtlPassKind::kRtl:
      return 0;
    case RtlPassKind::kJump:
      return 1;
    case RtlPassKind::kCse:
      return 2;
    case RtlPassKind::kLoop:
      return 3;
    case RtlPassKind::kCse2:
      return 4;
    case RtlPassKind::kFlow:
      return 5;
    case RtlPassKind::kCombine:
      return 6;
    case RtlPassKind::kSched:
      return 7;
    case RtlPassKind::kLreg:
      return 8;
    case RtlPassKind::kGreg:
      return 9;
    case RtlPassKind::kSched2:
      return 10;
    case RtlPassKind::kJump2:
      return 11;
    case RtlPassKind::kDbr:
      return 12;
  }
  return 12;
}

int FirstReplaySubfunctionIndexAfterPass(RtlPassKind pass) {
  const auto all_subs = AllSubfunctionIds();
  auto index_of = [&](SubfunctionId target) -> int {
    for (size_t i = 0; i < all_subs.size(); ++i) {
      if (all_subs[i] == target) {
        return static_cast<int>(i);
      }
    }
    return static_cast<int>(all_subs.size());
  };
  switch (pass) {
    case RtlPassKind::kRtl:
      return index_of(SubfunctionId::kJumpRedirect);
    case RtlPassKind::kJump:
      return index_of(SubfunctionId::kFoldRtx);
    case RtlPassKind::kCse:
      return index_of(SubfunctionId::kScanLoop);
    case RtlPassKind::kLoop:
      return index_of(SubfunctionId::kCseBasicBlockCse2);
    case RtlPassKind::kCse2:
      return index_of(SubfunctionId::kFindBasicBlocks);
    case RtlPassKind::kFlow:
      return index_of(SubfunctionId::kSimplifyRtx);
    case RtlPassKind::kCombine:
      return index_of(SubfunctionId::kScheduleBlockSched1);
    case RtlPassKind::kSched:
      return index_of(SubfunctionId::kUpdateEquivRegs);
    case RtlPassKind::kLreg:
      return index_of(SubfunctionId::kGlobalAlloc);
    case RtlPassKind::kGreg:
      return index_of(SubfunctionId::kScheduleBlockSched2);
    case RtlPassKind::kSched2:
      return index_of(SubfunctionId::kDoCrossJump);
    case RtlPassKind::kJump2:
      return index_of(SubfunctionId::kFillSimpleDelaySlots);
    case RtlPassKind::kDbr:
      return index_of(SubfunctionId::kMakeReturnInsns) + 1;
  }
  return index_of(SubfunctionId::kMakeReturnInsns) + 1;
}

CompilationProfile BuildEffectiveStage0CompilationProfile(const Stage0FunctionInput& input_x) {
  CompilationProfile effective_profile = input_x.profile;
  if (input_x.base_vram != 0 && input_x.relocations.empty()) {
    for (const auto& [vram, sym] : input_x.vram_to_symbol) {
      if (sym.empty()) {
        continue;
      }
      const auto sz_it = input_x.vram_to_symbol_size.find(vram);
      if (sz_it != input_x.vram_to_symbol_size.end() && sz_it->second > 0) {
        effective_profile.gp_symbol_sizes.try_emplace(sym, sz_it->second);
      }
    }
  }
  return effective_profile;
}

}  // namespace

RightInverseResult RightInverseVerifier::VerifyFinalAndGasStageRightInverse(
    const Stage0FunctionInput& input_x) const {
  RightInverseResult result;
  result.expected_words = input_x.machine_words;
  result.expected_summary = FormatWordsSummary(result.expected_words);

  const RtlControlFlowGraph lifted_dbr = inverses_->InvertFinalAndGasFunction(input_x);
  if (input_x.machine_words.empty() || lifted_dbr.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "InvertFinalAndGasFunction(Stage0FunctionInput) returned empty CFG";
    return result;
  }

  const CompilationProfile effective_profile = BuildEffectiveStage0CompilationProfile(input_x);
  hooks_->ApplyCompilationProfile(effective_profile);
  ctx_->MaterializeFromCfg(lifted_dbr);
  std::string bad_insn;
  if (!hooks_->ValidateMaterializedInsnsRecognizable(&bad_insn)) {
    result.matches = false;
    result.diff_detail =
        absl::StrCat("Unrecognizable RTL insn under compilation profile: ", bad_insn);
    return result;
  }
  const std::string combined_asm = hooks_->RunFinal();
  const Stage0FunctionInput actual_stage0 = hooks_->RunGasMdAssembleWithMetadata(combined_asm);
  return CompareStage0FunctionInputs(input_x, actual_stage0, combined_asm);
}

RightInverseResult RightInverseVerifier::VerifySuffixRightInverse(
    RtlPassKind target_pass, const Stage0FunctionInput& input_x) const {
  RightInverseResult fail_result;
  fail_result.expected_words = input_x.machine_words;
  fail_result.expected_summary = FormatWordsSummary(fail_result.expected_words);

  if (input_x.machine_words.empty()) {
    fail_result.matches = false;
    fail_result.diff_detail = "VerifySuffixRightInverse given empty machine_words";
    return fail_result;
  }

  // Instantiate a completely fresh `GccContext` and `FunctionHooks` configured
  // solely from `effective_profile` so zero forward-compilation oracle state
  // leaks from the caller's `ctx_` / `hooks_`.
  const CompilationProfile effective_profile = BuildEffectiveStage0CompilationProfile(input_x);
  GccContext fresh_ctx;
  FunctionHooks fresh_hooks(&fresh_ctx);
  fresh_hooks.ApplyCompilationProfile(effective_profile);

  const bool opt_enabled = (optimize > 0);
  const bool dbr_enabled = opt_enabled && (flag_delayed_branch != 0);
  const bool sched2_enabled = opt_enabled && (flag_schedule_insns_after_reload != 0);
  const bool sched1_enabled = opt_enabled && (flag_schedule_insns != 0);
  const bool cse2_enabled = opt_enabled && IsRerunCseAfterLoopEnabled();

  RtlControlFlowGraph y = inverses_->InvertFinalAndGasFunction(input_x);
  if (y.rpo_blocks.empty()) {
    fail_result.matches = false;
    fail_result.diff_detail = "InvertFinalAndGasFunction returned empty CFG";
    return fail_result;
  }
  y.pass = RtlPassKind::kDbr;

  const int target_rank = PassPipelineRank(target_pass);
  auto step_fail = [&](std::string_view step_name) -> RightInverseResult {
    fail_result.matches = false;
    fail_result.diff_detail = absl::StrCat(step_name, " returned empty RtlControlFlowGraph");
    return fail_result;
  };

  if (target_rank <= PassPipelineRank(RtlPassKind::kJump2)) {
    if (dbr_enabled) {
      y = inverses_->InvertDbrSchedule(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertDbrSchedule");
    }
    y.pass = RtlPassKind::kJump2;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kSched2)) {
    if (opt_enabled) {
      y = inverses_->InvertDoCrossJump(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertDoCrossJump");
    }
    y.pass = RtlPassKind::kSched2;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kGreg)) {
    if (sched2_enabled) {
      y = inverses_->InvertScheduleBlock(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertScheduleBlock (sched2)");
    }
    y.pass = RtlPassKind::kGreg;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kLreg)) {
    y = inverses_->InvertAlterReg(y);
    if (y.rpo_blocks.empty()) return step_fail("InvertAlterReg");
    y = inverses_->InvertReload(y);
    if (y.rpo_blocks.empty()) return step_fail("InvertReload");
    if (opt_enabled) {
      y = inverses_->InvertGlobalAlloc(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertGlobalAlloc");
    }
    y.pass = RtlPassKind::kLreg;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kSched)) {
    if (opt_enabled) {
      y = inverses_->InvertBlockAlloc(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertBlockAlloc");
      y = inverses_->InvertUpdateEquivRegs(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertUpdateEquivRegs");
    }
    y.pass = RtlPassKind::kSched;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kCombine)) {
    if (sched1_enabled) {
      y = inverses_->InvertScheduleBlock(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertScheduleBlock (sched1)");
    }
    y.pass = RtlPassKind::kCombine;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kFlow)) {
    if (opt_enabled) {
      y = inverses_->InvertTryCombine(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertTryCombine");
    }
    y.pass = RtlPassKind::kFlow;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kCse2)) {
    if (opt_enabled) {
      y = inverses_->InvertFindBasicBlocks(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertFindBasicBlocks");
      y = inverses_->InvertPropagateBlock(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertPropagateBlock");
    }
    y.pass = RtlPassKind::kCse2;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kLoop)) {
    if (cse2_enabled) {
      y = inverses_->InvertCseBasicBlock(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertCseBasicBlock (cse2)");
    }
    y.pass = RtlPassKind::kLoop;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kCse)) {
    if (opt_enabled) {
      y = inverses_->InvertStrengthReduce(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertStrengthReduce");
      y = inverses_->InvertScanLoop(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertScanLoop");
    }
    y.pass = RtlPassKind::kCse;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kJump)) {
    if (opt_enabled) {
      y = inverses_->InvertCseBasicBlock(y);
      if (y.rpo_blocks.empty()) return step_fail("InvertCseBasicBlock (cse1)");
    }
    y.pass = RtlPassKind::kJump;
  }
  if (target_rank <= PassPipelineRank(RtlPassKind::kRtl)) {
    y = inverses_->InvertJump1Optimize(y);
    if (y.rpo_blocks.empty()) return step_fail("InvertJump1Optimize");
    y = inverses_->InvertJumpRedirect(y);
    if (y.rpo_blocks.empty()) return step_fail("InvertJumpRedirect");
    y = inverses_->InvertInstantiateVirtualRegs(y);
    if (y.rpo_blocks.empty()) return step_fail("InvertInstantiateVirtualRegs");
    y.pass = RtlPassKind::kRtl;
  }

  // Reset `fresh_ctx` and replay forward passes $F_{0..s}(y_s)$ strictly under
  // `effective_profile`.
  fresh_ctx.BeginFunction("harness_func");
  fresh_hooks.ApplyCompilationProfile(effective_profile);
  fresh_ctx.MaterializeFromCfg(y);
  {
    std::string bad_insn;
    if (!fresh_hooks.ValidateMaterializedInsnsRecognizable(&bad_insn)) {
      fail_result.matches = false;
      fail_result.diff_detail =
          absl::StrCat("Unrecognizable lifted RTL insn under compilation profile: ", bad_insn);
      return fail_result;
    }
  }

  RtlControlFlowGraph current_cfg = y;
  const auto all_subs = AllSubfunctionIds();
  const int start_sub_idx = FirstReplaySubfunctionIndexAfterPass(target_pass);
  int end_sub_idx = 0;
  for (size_t i = 0; i < all_subs.size(); ++i) {
    if (all_subs[i] == SubfunctionId::kMakeReturnInsns) {
      end_sub_idx = static_cast<int>(i);
      break;
    }
  }
  for (int idx = start_sub_idx; idx <= end_sub_idx; ++idx) {
    current_cfg =
        fresh_hooks
            .ExecuteSubfunctionBoundary(all_subs[idx], current_cfg, {},
                                        effective_profile.optimize_level, effective_profile.c_flags)
            .post_state;
  }

  fresh_hooks.ApplyCompilationProfile(effective_profile);
  fresh_ctx.MaterializeFromCfg(current_cfg);
  {
    std::string bad_insn;
    if (!fresh_hooks.ValidateMaterializedInsnsRecognizable(&bad_insn)) {
      fail_result.matches = false;
      fail_result.diff_detail =
          absl::StrCat("Unrecognizable post-replay RTL insn under compilation profile: ", bad_insn);
      return fail_result;
    }
  }
  const std::string combined_asm = fresh_hooks.RunFinal();
  const Stage0FunctionInput actual_stage0 = fresh_hooks.RunGasMdAssembleWithMetadata(combined_asm);
  RightInverseResult res = CompareStage0FunctionInputs(input_x, actual_stage0, combined_asm);
  if (!res.matches) {
    absl::StrAppend(&res.diff_detail, "\nlifted y_s (", RtlPassExtension(target_pass), "):\n",
                    y.FormatSummary());
  }
  return res;
}

// -----------------------------------------------------------------------------
// Stage 1: Delayed-Branch Scheduling (`reorg.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyMakeReturnInsnsRightInverse(
    const RtlControlFlowGraph& dbr_return_cfg_x) const {
  return VerifyRtlPassRightInverse(
      dbr_return_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertMakeReturnInsns(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunMakeReturnInsns(); },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyRelaxDelaySlotsRightInverse(
    const RtlControlFlowGraph& relaxed_cfg_x) const {
  return VerifyRtlPassRightInverse(
      relaxed_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertRelaxDelaySlots(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunRelaxDelaySlots(); },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyFillEagerDelaySlotsRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  return VerifyRtlPassRightInverse(
      scheduled_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertFillEagerDelaySlots(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunFillEagerDelaySlots(); },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyFillSimpleDelaySlotsRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  return VerifyRtlPassRightInverse(
      scheduled_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertFillSimpleDelaySlots(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunFillSimpleDelaySlots(/*non_jumps_p=*/0);
      },
      RtlPassKind::kDbr);
}

RightInverseResult RightInverseVerifier::VerifyDbrStageRightInverse(
    const RtlControlFlowGraph& dbr_cfg_x) const {
  return VerifyRtlPassRightInverse(
      dbr_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertDbrSchedule(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunDbrSchedule(); },
      RtlPassKind::kDbr);
}

// -----------------------------------------------------------------------------
// Stage 2: Post-Reload Jump Optimization & Cross-Jumping (`jump.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyDoCrossJumpRightInverse(
    const RtlControlFlowGraph& jump2_cfg_x) const {
  return VerifyRtlPassRightInverse(
      jump2_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertDoCrossJump(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunJumpOptimize(/*cross_jump=*/1, /*noop_moves=*/1,
                               /*after_regscan=*/0);
      },
      RtlPassKind::kJump2);
}

RightInverseResult RightInverseVerifier::VerifyJump2CrossJumpStageRightInverse(
    const RtlControlFlowGraph& jump2_cfg_x) const {
  return VerifyDoCrossJumpRightInverse(jump2_cfg_x);
}

// -----------------------------------------------------------------------------
// Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyScheduleBlockRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  return VerifyRtlPassRightInverse(
      scheduled_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertScheduleBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunScheduleBlock(0); },
      scheduled_cfg_x.pass);
}

RightInverseResult RightInverseVerifier::VerifySched2StageRightInverse(
    const RtlControlFlowGraph& sched2_cfg_x) const {
  return VerifyRtlPassRightInverse(
      sched2_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertScheduleBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunScheduleInsns(); },
      RtlPassKind::kSched2);
}

RightInverseResult RightInverseVerifier::VerifySched1StageRightInverse(
    const RtlControlFlowGraph& sched1_cfg_x) const {
  return VerifyRtlPassRightInverse(
      sched1_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertScheduleBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunScheduleInsns(); },
      RtlPassKind::kSched);
}

RightInverseResult RightInverseVerifier::VerifySchedStageRightInverse(
    const RtlControlFlowGraph& scheduled_cfg_x) const {
  if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
    return VerifySched1StageRightInverse(scheduled_cfg_x);
  }
  return VerifySched2StageRightInverse(scheduled_cfg_x);
}

// -----------------------------------------------------------------------------
// Stage 4 & Stage 5: Local & Global Register Allocation (`local-alloc.c`,
// `global.c`, `reload1.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyAlterRegRightInverse(
    const RtlControlFlowGraph& allocated_cfg_x) const {
  std::vector<int> altered_regnos;
  return VerifyRtlPassRightInverse(
      allocated_cfg_x,
      [this, &altered_regnos](const RtlControlFlowGraph& x) {
        RtlControlFlowGraph lifted_y = inverses_->InvertAlterReg(x, /*pseudo_regno=*/72);
        altered_regnos.clear();
        for (const auto& [regno, _] : lifted_y.sideband.reg_renumber) {
          if (regno >= kMipsFirstPseudoRegister) {
            altered_regnos.push_back(regno);
          }
        }
        std::sort(altered_regnos.begin(), altered_regnos.end());
        if (altered_regnos.empty()) {
          altered_regnos.push_back(72);
        }
        return lifted_y;
      },
      [&altered_regnos](GccContext* /*ctx*/, FunctionHooks* hooks) {
        for (int regno : altered_regnos) {
          hooks->RunAlterReg(regno, /*from_reg=*/-1);
        }
      },
      allocated_cfg_x.pass);
}

RightInverseResult RightInverseVerifier::VerifyReloadRightInverse(
    const RtlControlFlowGraph& reload_cfg_x) const {
  RightInverseResult result;
  result.expected_summary = reload_cfg_x.FormatSummary();

  const RtlControlFlowGraph lifted_y = inverses_->InvertReload(reload_cfg_x);
  if (!reload_cfg_x.rpo_blocks.empty() && lifted_y.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  const SubfunctionBoundarySnapshot replayed =
      hooks_->ExecuteSubfunctionBoundary(SubfunctionId::kReload, lifted_y);
  result.actual_summary = replayed.post_state.FormatSummary();
  result.cfg_comparison = replayed.post_state.CompareAgainst(reload_cfg_x);
  result.matches = result.cfg_comparison.MatchesExactly();
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  return result;
}

RightInverseResult RightInverseVerifier::VerifyGlobalAllocRightInverse(
    const RtlControlFlowGraph& greg_cfg_x) const {
  RightInverseResult result;
  result.expected_summary = greg_cfg_x.FormatSummary();

  const RtlControlFlowGraph lifted_y = inverses_->InvertGlobalAlloc(greg_cfg_x);
  if (!greg_cfg_x.rpo_blocks.empty() && lifted_y.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  const SubfunctionBoundarySnapshot replayed =
      hooks_->ExecuteSubfunctionBoundary(SubfunctionId::kGlobalAlloc, lifted_y);
  result.actual_summary = replayed.post_state.FormatSummary();
  result.cfg_comparison = replayed.post_state.CompareAgainst(greg_cfg_x);
  const bool reg_renumber_matches =
      (replayed.post_state.sideband.reg_renumber == greg_cfg_x.sideband.reg_renumber);
  result.matches = result.cfg_comparison.MatchesExactly() && reg_renumber_matches;
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  if (result.cfg_comparison.MatchesExactly() && !reg_renumber_matches) {
    result.diff_detail = "sideband.reg_renumber mismatch after kGlobalAlloc";
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyGlobalReloadStageRightInverse(
    const RtlControlFlowGraph& greg_cfg_x) const {
  return VerifyRtlPassRightInverse(
      greg_cfg_x,
      [this](const RtlControlFlowGraph& x) {
        RtlControlFlowGraph pre_alter = inverses_->InvertAlterReg(x, /*pseudo_regno=*/72);
        if (!x.rpo_blocks.empty() && pre_alter.rpo_blocks.empty()) {
          return RtlControlFlowGraph{};
        }
        RtlControlFlowGraph pre_reload = inverses_->InvertReload(pre_alter);
        if (!pre_alter.rpo_blocks.empty() && pre_reload.rpo_blocks.empty()) {
          return RtlControlFlowGraph{};
        }
        return inverses_->InvertGlobalAlloc(pre_reload);
      },
      [](GccContext* ctx, FunctionHooks* hooks) {
        if (optimize > 0) {
          hooks->RunGlobalAlloc();
        } else {
          Rtx head = ctx->GetInsns();
          if (head != nullptr) {
            obey_regdecls = 1;
            restore_reg_data(head);
            max_regno = max_reg_num();
            reg_scan(head, max_regno, 0);
            regclass_init();
            regclass(head, max_regno);
            stupid_life_analysis(head, max_regno, nullptr);
            hooks->RunReload(head, /*global=*/0);
          }
        }
      },
      RtlPassKind::kGreg);
}

RightInverseResult RightInverseVerifier::VerifyBlockAllocRightInverse(
    const RtlControlFlowGraph& lreg_cfg_x) const {
  RightInverseResult result;
  result.expected_summary = lreg_cfg_x.FormatSummary();

  const RtlControlFlowGraph lifted_y = inverses_->InvertBlockAlloc(lreg_cfg_x);
  if (!lreg_cfg_x.rpo_blocks.empty() && lifted_y.rpo_blocks.empty()) {
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  const SubfunctionBoundarySnapshot replayed =
      hooks_->ExecuteSubfunctionBoundary(SubfunctionId::kBlockAlloc, lifted_y);
  result.actual_summary = replayed.post_state.FormatSummary();
  result.cfg_comparison = replayed.post_state.CompareAgainst(lreg_cfg_x);
  const bool reg_renumber_matches =
      (replayed.post_state.sideband.reg_renumber == lreg_cfg_x.sideband.reg_renumber);
  result.matches = result.cfg_comparison.MatchesExactly() && reg_renumber_matches;
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  if (result.cfg_comparison.MatchesExactly() && !reg_renumber_matches) {
    result.diff_detail = "sideband.reg_renumber mismatch after kBlockAlloc";
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyUpdateEquivRegsRightInverse(
    const RtlControlFlowGraph& equiv_cfg_x) const {
  return VerifyRtlPassRightInverse(
      equiv_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertUpdateEquivRegs(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunUpdateEquivRegs(); },
      RtlPassKind::kLreg);
}

RightInverseResult RightInverseVerifier::VerifyLocalAllocStageRightInverse(
    const RtlControlFlowGraph& lreg_cfg_x) const {
  return VerifyRtlPassRightInverse(
      lreg_cfg_x,
      [this](const RtlControlFlowGraph& x) {
        RtlControlFlowGraph pre_block = inverses_->InvertBlockAlloc(x);
        if (!x.rpo_blocks.empty() && pre_block.rpo_blocks.empty()) {
          return RtlControlFlowGraph{};
        }
        return inverses_->InvertUpdateEquivRegs(pre_block);
      },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        if (optimize > 0) {
          hooks->RunLocalAlloc();
        }
      },
      RtlPassKind::kLreg);
}

// -----------------------------------------------------------------------------
// Stage 7: Instruction Combination (`combine.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifySimplifyRtxRightInverse(
    std::string_view simplified_rtx_x) const {
  RightInverseResult result;
  result.expected_summary = std::string(absl::StripAsciiWhitespace(simplified_rtx_x));

  const std::string unsimplified_y = inverses_->InvertSimplifyRtx(simplified_rtx_x);
  ctx_->BeginFunction("simplify_rtx_verify_fn");
  Rtx rtx_y = ctx_->ParseRtx(unsimplified_y);
  Rtx simplified = hooks_->RunSimplifyRtx(rtx_y, rtx_y != nullptr ? rtx_y->mode : kSiMode);
  result.actual_summary = ctx_->FormatRtx(simplified);
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("simplify_rtx(%s) got %s != expected %s", unsimplified_y,
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifySubstRightInverse(
    std::string_view subst_rtx_x) const {
  RightInverseResult result;
  result.expected_summary = std::string(absl::StripAsciiWhitespace(subst_rtx_x));

  const SubstInverseSpec spec = inverses_->InvertSubst(subst_rtx_x);
  ctx_->BeginFunction("subst_verify_fn");
  Rtx target = ctx_->ParseRtx(spec.target_expr);
  Rtx from = ctx_->ParseRtx(spec.from_reg);
  Rtx to = ctx_->ParseRtx(spec.to_expr);
  Rtx actual = hooks_->RunSubst(target, from, to, /*in_dest=*/0,
                                /*unique_copy=*/0);
  result.actual_summary = ctx_->FormatRtx(actual);
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail =
        absl::StrFormat("subst(%s, %s, %s) got %s != expected %s", spec.target_expr, spec.from_reg,
                        spec.to_expr, result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyTryCombineRightInverse(
    const RtlControlFlowGraph& combine_cfg_x) const {
  return VerifyRtlPassRightInverse(
      combine_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertTryCombine(x); },
      [](GccContext* ctx, FunctionHooks* hooks) {
        hooks->RunFlowAnalysis();
        for (Rtx insn = ctx->GetInsns(); insn != nullptr; insn = insn->fld[2].rtx) {
          if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
              insn->fld[5].rtx != nullptr && insn->fld[5].rtx->fld[0].rtx != nullptr) {
            hooks->RunTryCombine(insn, insn->fld[5].rtx->fld[0].rtx, nullptr);
          }
        }
      },
      RtlPassKind::kCombine);
}

RightInverseResult RightInverseVerifier::VerifyCombineStageRightInverse(
    const RtlControlFlowGraph& combine_cfg_x) const {
  return VerifyRtlPassRightInverse(
      combine_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertTryCombine(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunCombineInstructions(); },
      RtlPassKind::kCombine);
}

// -----------------------------------------------------------------------------
// Stage 8: Dataflow & Life Analysis (`flow.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyFindBasicBlocksRightInverse(
    const RtlControlFlowGraph& flow_cfg_x) const {
  return VerifyRtlPassRightInverse(
      flow_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertFindBasicBlocks(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunFindBasicBlocks(); },
      RtlPassKind::kFlow);
}

RightInverseResult RightInverseVerifier::VerifyPropagateBlockRightInverse(
    const RtlControlFlowGraph& flow_cfg_x) const {
  return VerifyRtlPassRightInverse(
      flow_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertPropagateBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunLifeAnalysis(); },
      RtlPassKind::kFlow);
}

RightInverseResult RightInverseVerifier::VerifyFlowStageRightInverse(
    const RtlControlFlowGraph& flow_cfg_x) const {
  return VerifyRtlPassRightInverse(
      flow_cfg_x,
      [this](const RtlControlFlowGraph& x) {
        return inverses_->InvertPropagateBlock(inverses_->InvertFindBasicBlocks(x));
      },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunFlowAnalysis(); },
      RtlPassKind::kFlow);
}

// -----------------------------------------------------------------------------
// Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyFoldRtxRightInverse(
    std::string_view folded_rtx_x) const {
  RightInverseResult result;
  result.expected_summary = std::string(absl::StripAsciiWhitespace(folded_rtx_x));

  const std::string unfolded_y = inverses_->InvertFoldRtx(folded_rtx_x);
  ctx_->BeginFunction("fold_rtx_verify_fn");
  Rtx rtx_y = ctx_->ParseRtx(unfolded_y);
  Rtx folded = hooks_->RunFoldRtx(rtx_y, /*insn=*/nullptr);
  result.actual_summary = ctx_->FormatRtx(folded);
  result.matches = (result.actual_summary == result.expected_summary);
  if (!result.matches) {
    result.diff_detail = absl::StrFormat("fold_rtx(%s) got %s != expected %s", unfolded_y,
                                         result.actual_summary, result.expected_summary);
  }
  return result;
}

RightInverseResult RightInverseVerifier::VerifyCseBasicBlockRightInverse(
    const RtlControlFlowGraph& cse_cfg_x) const {
  return VerifyRtlPassRightInverse(
      cse_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertCseBasicBlock(x); },
      [](GccContext* ctx, FunctionHooks* hooks) {
        hooks->RunCseBasicBlock(ctx->GetInsns(), /*to=*/nullptr,
                                /*next_branch=*/nullptr, /*around_loop=*/0);
      },
      cse_cfg_x.pass);
}

RightInverseResult RightInverseVerifier::VerifyCse1StageRightInverse(
    const RtlControlFlowGraph& cse_cfg_x) const {
  return VerifyRtlPassRightInverse(
      cse_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertCseBasicBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunCseMain(/*first=*/nullptr, /*nregs=*/0, /*after_loop=*/0);
      },
      RtlPassKind::kCse);
}

RightInverseResult RightInverseVerifier::VerifyCse2StageRightInverse(
    const RtlControlFlowGraph& cse2_cfg_x) const {
  return VerifyRtlPassRightInverse(
      cse2_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertCseBasicBlock(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunCseMain(/*first=*/nullptr, /*nregs=*/0, /*after_loop=*/1);
      },
      RtlPassKind::kCse2);
}

// -----------------------------------------------------------------------------
// Stage 10: Loop Optimization (`loop.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyScanLoopRightInverse(
    const RtlControlFlowGraph& loop_cfg_x) const {
  return VerifyRtlPassRightInverse(
      loop_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertScanLoop(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunScanLoop(/*loop_start=*/nullptr, /*end=*/nullptr,
                           /*nregs=*/0);
      },
      RtlPassKind::kLoop);
}

RightInverseResult RightInverseVerifier::VerifyLoopStageRightInverse(
    const RtlControlFlowGraph& loop_cfg_x) const {
  return VerifyRtlPassRightInverse(
      loop_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertScanLoop(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunLoopOptimize(/*first=*/nullptr, /*unroll_p=*/0);
      },
      RtlPassKind::kLoop);
}

RightInverseResult RightInverseVerifier::VerifyStrengthReduceRightInverse(
    const RtlControlFlowGraph& loop_sr_cfg_x) const {
  return VerifyRtlPassRightInverse(
      loop_sr_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertStrengthReduce(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) { hooks->RunStrengthReduce(); },
      RtlPassKind::kLoop);
}

// -----------------------------------------------------------------------------
// Stage 12: Initial Jump Optimization (`jump.c`)
// -----------------------------------------------------------------------------

RightInverseResult RightInverseVerifier::VerifyJumpRedirectRightInverse(
    const RtlControlFlowGraph& jump_cfg_x) const {
  return VerifyRtlPassRightInverse(
      jump_cfg_x, [this](const RtlControlFlowGraph& x) { return inverses_->InvertJumpRedirect(x); },
      [](GccContext* ctx, FunctionHooks* hooks) {
        Rtx first_jump = nullptr;
        Rtx last_label = nullptr;
        for (Rtx insn = ctx->GetInsns(); insn != nullptr; insn = insn->fld[2].rtx) {
          if (insn->code == kJumpInsn && first_jump == nullptr) {
            first_jump = insn;
          } else if (insn->code == kCodeLabel) {
            last_label = insn;
          }
        }
        if (first_jump != nullptr && last_label != nullptr) {
          hooks->RunRedirectJump(first_jump, last_label);
        }
        hooks->RunJumpOptimize(/*cross_jump=*/0, /*noop_moves=*/0,
                               /*after_regscan=*/1);
      },
      RtlPassKind::kJump);
}

RightInverseResult RightInverseVerifier::VerifyJump1StageRightInverse(
    const RtlControlFlowGraph& jump_cfg_x) const {
  return VerifyRtlPassRightInverse(
      jump_cfg_x,
      [this](const RtlControlFlowGraph& x) { return inverses_->InvertJump1Optimize(x); },
      [](GccContext* /*ctx*/, FunctionHooks* hooks) {
        hooks->RunJumpOptimize(/*cross_jump=*/0, /*noop_moves=*/0,
                               /*after_regscan=*/1);
      },
      RtlPassKind::kJump);
}

// -----------------------------------------------------------------------------
// Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`)
// -----------------------------------------------------------------------------

namespace {

RightInverseResult BuildCfgRightInverseResult(const RtlControlFlowGraph& expected_x,
                                              const RtlControlFlowGraph& actual_fx) {
  RightInverseResult result;
  result.expected_summary = expected_x.FormatSummary();
  result.actual_summary = actual_fx.FormatSummary();
  result.cfg_comparison = actual_fx.CompareAgainst(expected_x);
  result.matches = result.cfg_comparison.MatchesExactly();
  result.diff_detail = result.cfg_comparison.mismatch_detail;
  return result;
}

RightInverseResult BuildFailedStage13Result(const RtlControlFlowGraph& expected_x,
                                            const absl::Status& status) {
  RightInverseResult result;
  result.expected_summary = expected_x.FormatSummary();
  result.matches = false;
  result.diff_detail = std::string(status.message());
  return result;
}

}  // namespace

RightInverseResult RightInverseVerifier::VerifyInstantiateVirtualRegsRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  const RtlControlFlowGraph pre_y = inverses_->InvertInstantiateVirtualRegs(post_cfg_x);
  if (!post_cfg_x.rpo_blocks.empty() && pre_y.rpo_blocks.empty()) {
    RightInverseResult result;
    result.expected_summary = post_cfg_x.FormatSummary();
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunInstantiateVirtualRegs(*ctx_, pre_y);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandFunctionEndRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandFunctionEnd(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandFunctionEnd(*ctx_, lifted_y->body_cfg, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandReturnRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandReturn(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandReturn(*ctx_, lifted_y->ret_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandExprRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandExpr(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunExpandExpr(
      *ctx_, *lifted_y->expr, lifted_y->func_decl, lifted_y->target_pseudo_regno);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandAssignmentRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandAssignment(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandAssignment(*ctx_, *lifted_y->lhs, *lifted_y->rhs, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyDoStoreFlagRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertDoStoreFlag(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunDoStoreFlag(*ctx_, *lifted_y->comparison_expr, lifted_y->func_decl,
                             lifted_y->target_pseudo_regno, lifted_y->only_cheap);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyDoJumpRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertDoJump(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunDoJump(*ctx_, *lifted_y->cond_expr, lifted_y->func_decl, lifted_y->jump_if_true,
                        lifted_y->jump_if_false);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandCallRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandCall(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunExpandCall(
      *ctx_, lifted_y->call_expr, lifted_y->caller_decl, lifted_y->return_type,
      lifted_y->target_pseudo_regno, lifted_y->ignore_result);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandCondRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandCond(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandCond(*ctx_, lifted_y->if_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandLoopRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandLoop(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandLoop(*ctx_, *lifted_y->loop_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandCaseRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandCase(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandCase(*ctx_, lifted_y->switch_stmt, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandDeclRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandDecl(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx =
      hooks_->RunExpandDecl(*ctx_, lifted_y->var_decl, lifted_y->func_decl);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifyExpandFunctionStartRightInverse(
    const RtlControlFlowGraph& post_cfg_x) const {
  auto lifted_y = inverses_->InvertExpandFunctionStart(post_cfg_x);
  if (!lifted_y.ok()) {
    return BuildFailedStage13Result(post_cfg_x, lifted_y.status());
  }
  const RtlControlFlowGraph actual_fx = hooks_->RunExpandFunctionStart(*ctx_, *lifted_y);
  return BuildCfgRightInverseResult(post_cfg_x, actual_fx);
}

RightInverseResult RightInverseVerifier::VerifySubfunctionBoundaryRightInverse(
    const SubfunctionBoundarySnapshot& snap) const {
  switch (snap.subfunction_id) {
    case SubfunctionId::kFunctionPrologueEpilogue: {
      RightInverseResult result;
      result.expected_summary = absl::StrJoin(snap.post_asm_lines, "\n");
      const PrologueEpilogueFrameSpec spec =
          inverses_->InvertFunctionPrologueEpilogue(result.expected_summary);
      if (spec.var_size < 0) {
        result.matches = false;
        result.diff_detail = "InvertFunctionPrologueEpilogue returned invalid var_size < 0";
        return result;
      }
      RtlControlFlowGraph cand_pre = snap.post_state;
      cand_pre.sideband.frame_size_bytes = spec.var_size;
      cand_pre.sideband.saved_gpr_regnos.clear();
      cand_pre.sideband.saved_fpr_regnos.clear();
      for (int r = 0; r < 32; ++r) {
        if ((spec.saved_gpr_mask >> r) & 1u) {
          cand_pre.sideband.saved_gpr_regnos.push_back(r);
        }
        if ((spec.saved_fpr_mask >> r) & 1u) {
          cand_pre.sideband.saved_fpr_regnos.push_back(32 + r);
        }
      }
      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, cand_pre);
      result.actual_summary = absl::StrJoin(replayed.post_asm_lines, "\n");
      result.matches = (replayed.post_asm_lines == snap.post_asm_lines);
      if (!result.matches) {
        result.diff_detail =
            absl::StrFormat("kFunctionPrologueEpilogue asm lines got:\n%s\n!= expected:\n%s",
                            result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kFinalScanInsn: {
      RightInverseResult result;
      result.expected_summary = absl::StrJoin(snap.post_asm_lines, "\n");
      RtlControlFlowGraph cand_pre =
          inverses_->InvertFinalScanInsnStream(snap.post_asm_lines, snap.post_state.sideband);
      if (!snap.post_asm_lines.empty() && cand_pre.rpo_blocks.empty()) {
        result.matches = false;
        result.diff_detail = "InvertFinalScanInsnStream returned empty RtlControlFlowGraph";
        return result;
      }
      cand_pre.pass = snap.post_state.pass;

      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, cand_pre);
      result.actual_summary = absl::StrJoin(replayed.post_asm_lines, "\n");
      result.matches = (replayed.post_asm_lines == snap.post_asm_lines);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kFinalScanInsn asm lines got:\n%s\n!= expected:\n%s",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kGasMipsIp: {
      RightInverseResult result;
      result.expected_words = snap.post_machine_words;
      result.expected_summary = FormatWordsSummary(snap.post_machine_words);
      std::vector<std::string> cand_asm_lines;
      cand_asm_lines.reserve(snap.post_machine_words.size());
      for (uint32_t w : snap.post_machine_words) {
        std::string asm_y = inverses_->InvertGasMipsIp(w);
        if (asm_y.empty()) {
          result.matches = false;
          result.diff_detail = "InvertGasMipsIp returned empty string";
          return result;
        }
        cand_asm_lines.push_back(std::move(asm_y));
      }
      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, snap.post_state, cand_asm_lines);
      result.actual_words = replayed.post_machine_words;
      result.actual_summary = FormatWordsSummary(replayed.post_machine_words);
      result.matches = (replayed.post_machine_words == snap.post_machine_words);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kGasMipsIp words got [%s] != expected [%s]",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kGasMacro: {
      RightInverseResult result;
      result.expected_words = snap.post_machine_words;
      result.expected_summary = FormatWordsSummary(snap.post_machine_words);
      std::vector<std::string> cand_asm_lines;
      const auto words_span = absl::MakeConstSpan(snap.post_machine_words);
      size_t i = 0;
      while (i < words_span.size()) {
        bool matched_multi = false;
        const size_t max_len = std::min<size_t>(12, words_span.size() - i);
        for (size_t len = max_len; len >= 2; --len) {
          const auto slice = words_span.subspan(i, len);
          const std::string macro_asm = inverses_->InvertGasMacro(slice);
          if (!macro_asm.empty() && absl::MakeConstSpan(hooks_->RunGasMacro(macro_asm)) == slice) {
            cand_asm_lines.push_back(macro_asm);
            i += len;
            matched_multi = true;
            break;
          }
        }
        if (matched_multi) {
          continue;
        }
        const std::string single_asm = inverses_->InvertGasMacro(words_span.subspan(i, 1));
        if (single_asm.empty()) {
          result.matches = false;
          result.diff_detail = absl::StrFormat(
              "InvertGasMacro returned empty string at word index %zu (0x%08x)", i, words_span[i]);
          return result;
        }
        cand_asm_lines.push_back(single_asm);
        ++i;
      }
      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, snap.post_state, cand_asm_lines);
      result.actual_words = replayed.post_machine_words;
      result.actual_summary = FormatWordsSummary(replayed.post_machine_words);
      result.matches = (replayed.post_machine_words == snap.post_machine_words);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kGasMacro words got [%s] != expected [%s]",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    case SubfunctionId::kGasAppendInsn: {
      RightInverseResult result;
      result.expected_words = snap.post_machine_words;
      result.expected_summary = FormatWordsSummary(snap.post_machine_words);

      const std::vector<std::string> cand_asm_lines =
          inverses_->InvertGasAppendInsn(snap.post_machine_words, snap.c_flags);
      if (!snap.post_machine_words.empty() && cand_asm_lines.empty()) {
        result.matches = false;
        result.diff_detail = "InvertGasAppendInsn returned empty assembly lines";
        return result;
      }

      const SubfunctionBoundarySnapshot replayed =
          hooks_->ExecuteSubfunctionBoundary(snap, snap.post_state, cand_asm_lines);
      result.actual_words = replayed.post_machine_words;
      result.actual_summary = FormatWordsSummary(replayed.post_machine_words);
      result.matches = (replayed.post_machine_words == snap.post_machine_words);
      if (!result.matches) {
        result.diff_detail = absl::StrFormat("kGasAppendInsn words got [%s] != expected [%s]",
                                             result.actual_summary, result.expected_summary);
      }
      return result;
    }

    default:
      break;
  }

  RtlControlFlowGraph cand_pre = snap.post_state;
  switch (snap.subfunction_id) {
    case SubfunctionId::kInstantiateVirtualRegs:
      cand_pre = inverses_->InvertInstantiateVirtualRegs(snap.post_state);
      break;
    case SubfunctionId::kJumpRedirect:
      cand_pre = inverses_->InvertJumpRedirect(snap.post_state);
      break;
    case SubfunctionId::kJump1Optimize:
      cand_pre = inverses_->InvertJump1Optimize(snap.post_state);
      break;
    case SubfunctionId::kFoldRtx: {
      bool called_fold = false;
      for (auto& blk : cand_pre.rpo_blocks) {
        for (std::string& pat : blk.instructions) {
          const std::string clean = StripTrailingRegNotesFromPattern(pat);
          if (!absl::StartsWith(clean, "(set ")) {
            continue;
          }
          called_fold = true;
          const std::string notes = pat.size() > clean.size() ? pat.substr(clean.size()) : "";
          const std::string unfolded = inverses_->InvertFoldRtx(clean);
          if (!absl::StartsWith(unfolded, "(set ")) {
            RightInverseResult result;
            result.expected_summary = snap.post_state.FormatSummary();
            result.matches = false;
            result.diff_detail = absl::StrFormat(
                "InvertFoldRtx returned invalid pattern '%s' for '%s'", unfolded, clean);
            return result;
          }
          if (snap.optimize_level > 0) {
            pat = absl::StrCat(unfolded, notes);
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      if (!called_fold) {
        RightInverseResult result;
        result.expected_summary = snap.post_state.FormatSummary();
        result.matches = false;
        result.diff_detail = "No (set ...) instruction found for kFoldRtx";
        return result;
      }
      break;
    }
    case SubfunctionId::kCseBasicBlockCse1:
    case SubfunctionId::kCseBasicBlockCse2:
      cand_pre = inverses_->InvertCseBasicBlock(snap.post_state);
      break;
    case SubfunctionId::kScanLoop:
      cand_pre = inverses_->InvertScanLoop(snap.post_state);
      break;
    case SubfunctionId::kStrengthReduce:
      cand_pre = inverses_->InvertStrengthReduce(snap.post_state);
      break;
    case SubfunctionId::kFindBasicBlocks:
      cand_pre = inverses_->InvertFindBasicBlocks(snap.post_state);
      break;
    case SubfunctionId::kPropagateBlock:
      cand_pre = inverses_->InvertPropagateBlock(snap.post_state);
      break;
    case SubfunctionId::kSimplifyRtx: {
      bool called_simplify = false;
      for (auto& blk : cand_pre.rpo_blocks) {
        for (std::string& pat : blk.instructions) {
          const std::string clean = StripTrailingRegNotesFromPattern(pat);
          const std::string notes = pat.size() > clean.size() ? pat.substr(clean.size()) : "";
          if (absl::StartsWith(clean, "(set ") && clean.back() == ')') {
            size_t pos = std::string_view("(set ").size();
            int depth = 0;
            for (; pos < clean.size(); ++pos) {
              if (clean[pos] == '(') ++depth;
              if (clean[pos] == ')' && --depth == 0) {
                ++pos;
                break;
              }
            }
            const std::string_view clean_view(clean);
            std::string_view dst = absl::StripAsciiWhitespace(clean_view.substr(5, pos - 5));
            std::string_view rhs =
                absl::StripAsciiWhitespace(clean_view.substr(pos, clean.size() - pos - 1));
            const bool is_compound_rhs =
                dst != "(pc)" && absl::StartsWith(rhs, "(") && !absl::StartsWith(rhs, "(reg") &&
                !absl::StartsWith(rhs, "(subreg") && !absl::StartsWith(rhs, "(const_int") &&
                !absl::StartsWith(rhs, "(mem") && !absl::StartsWith(rhs, "(clobber") &&
                !absl::StartsWith(rhs, "(call") && !absl::StartsWith(rhs, "(if_then_else") &&
                !absl::StartsWith(rhs, "(label_ref") && rhs != "(pc)";
            if (is_compound_rhs) {
              called_simplify = true;
              const std::string unsimp = inverses_->InvertSimplifyRtx(rhs);
              if (unsimp.empty()) {
                RightInverseResult result;
                result.expected_summary = snap.post_state.FormatSummary();
                result.matches = false;
                result.diff_detail =
                    absl::StrFormat("InvertSimplifyRtx returned empty pattern for '%s'", rhs);
                return result;
              }
              if (snap.optimize_level > 0) {
                pat = absl::StrFormat("(set %s %s)%s", dst, unsimp, notes);
              }
            }
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      if (!called_simplify) {
        const std::string probe = inverses_->InvertSimplifyRtx("(reg:SI 2 v0)");
        if (probe.empty()) {
          RightInverseResult result;
          result.expected_summary = snap.post_state.FormatSummary();
          result.matches = false;
          result.diff_detail = "InvertSimplifyRtx returned empty pattern";
          return result;
        }
      }
      break;
    }
    case SubfunctionId::kSubst: {
      bool called_subst = false;
      for (auto& blk : cand_pre.rpo_blocks) {
        for (std::string& pat : blk.instructions) {
          const std::string clean = StripTrailingRegNotesFromPattern(pat);
          if (!absl::StartsWith(clean, "(set ") || clean.back() != ')') {
            continue;
          }
          called_subst = true;
          const std::string notes = pat.size() > clean.size() ? pat.substr(clean.size()) : "";
          const SubstInverseSpec spec = inverses_->InvertSubst(clean);
          if (spec.target_expr.empty() || spec.from_reg.empty() || spec.to_expr.empty()) {
            RightInverseResult result;
            result.expected_summary = snap.post_state.FormatSummary();
            result.matches = false;
            result.diff_detail = absl::StrFormat("InvertSubst returned empty spec for '%s'", clean);
            return result;
          }
          size_t pos = std::string_view("(set ").size();
          int depth = 0;
          for (; pos < clean.size(); ++pos) {
            if (clean[pos] == '(') ++depth;
            if (clean[pos] == ')' && --depth == 0) {
              ++pos;
              break;
            }
          }
          std::string combined = spec.target_expr;
          const size_t from_pos = combined.find(spec.from_reg, pos);
          if (from_pos == std::string::npos) {
            RightInverseResult result;
            result.expected_summary = snap.post_state.FormatSummary();
            result.matches = false;
            result.diff_detail =
                absl::StrFormat("InvertSubst target_expr '%s' missing from_reg '%s' in RHS",
                                spec.target_expr, spec.from_reg);
            return result;
          }
          if (snap.optimize_level > 0) {
            combined.replace(from_pos, spec.from_reg.size(), spec.to_expr);
            pat = absl::StrCat(combined, notes);
          }
        }
        blk.SyncRtlInsnsFromInstructions();
      }
      if (!called_subst) {
        const SubstInverseSpec probe = inverses_->InvertSubst("(set (reg:SI 2 v0) (reg:SI 4 a0))");
        if (probe.target_expr.empty() || probe.from_reg.empty() || probe.to_expr.empty()) {
          RightInverseResult result;
          result.expected_summary = snap.post_state.FormatSummary();
          result.matches = false;
          result.diff_detail = "InvertSubst returned empty spec";
          return result;
        }
      }
      break;
    }
    case SubfunctionId::kTryCombine:
      cand_pre = inverses_->InvertTryCombine(snap.post_state);
      break;
    case SubfunctionId::kScheduleBlockSched1:
    case SubfunctionId::kScheduleBlockSched2:
      cand_pre = inverses_->InvertScheduleBlock(snap.post_state);
      break;
    case SubfunctionId::kUpdateEquivRegs:
      cand_pre = inverses_->InvertUpdateEquivRegs(snap.post_state);
      break;
    case SubfunctionId::kBlockAlloc:
      cand_pre = inverses_->InvertBlockAlloc(snap.post_state);
      break;
    case SubfunctionId::kGlobalAlloc:
      cand_pre = inverses_->InvertGlobalAlloc(snap.post_state);
      break;
    case SubfunctionId::kReload:
      cand_pre = inverses_->InvertReload(snap.post_state);
      break;
    case SubfunctionId::kAlterReg:
      cand_pre = inverses_->InvertAlterReg(snap.post_state);
      break;
    case SubfunctionId::kDoCrossJump:
      cand_pre = inverses_->InvertDoCrossJump(snap.post_state);
      break;
    case SubfunctionId::kFillSimpleDelaySlots:
      cand_pre = inverses_->InvertFillSimpleDelaySlots(snap.post_state);
      break;
    case SubfunctionId::kFillEagerDelaySlots:
      cand_pre = inverses_->InvertFillEagerDelaySlots(snap.post_state);
      break;
    case SubfunctionId::kRelaxDelaySlots:
      cand_pre = inverses_->InvertRelaxDelaySlots(snap.post_state);
      break;
    case SubfunctionId::kMakeReturnInsns:
      cand_pre = inverses_->InvertMakeReturnInsns(snap.post_state);
      break;
    default:
      break;
  }

  if (!snap.post_state.rpo_blocks.empty() && cand_pre.rpo_blocks.empty()) {
    RightInverseResult result;
    result.expected_summary = snap.post_state.FormatSummary();
    result.matches = false;
    result.diff_detail = "Inverse returned empty RtlControlFlowGraph";
    return result;
  }

  const SubfunctionBoundarySnapshot replayed = hooks_->ExecuteSubfunctionBoundary(snap, cand_pre);
  RightInverseResult result = BuildCfgRightInverseResult(snap.post_state, replayed.post_state);
  if (result.matches &&
      (snap.subfunction_id == SubfunctionId::kGlobalAlloc ||
       snap.subfunction_id == SubfunctionId::kBlockAlloc) &&
      replayed.post_state.sideband.reg_renumber != snap.post_state.sideband.reg_renumber) {
    result.matches = false;
    result.diff_detail = snap.subfunction_id == SubfunctionId::kGlobalAlloc
                             ? "sideband.reg_renumber mismatch after kGlobalAlloc"
                             : "sideband.reg_renumber mismatch after kBlockAlloc";
  }
  return result;
}

}  // namespace rom_nom_nom::gcc_harness
