#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/compiler.h"
#include "core/control_flow_graph.h"
#include "core/elf_reader.h"
#include "core/mips.h"
#include "core/relocation_applier.h"
#include "core/round_trip_runner.h"
#include "core/round_trip_test_cases.h"
#include "gtest/gtest.h"
#include "lifter/c_emitter.h"
#include "lifter/cfg_analysis_manager.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/cfg_pass_pipeline.h"
#include "lifter/decompilation_context.h"
#include "lifter/function_loader.h"
#include "lifter/gcc_rtl_dump_inspector.h"
#include "lifter/lifter_pipeline.h"

namespace rom_nom_nom {
namespace {

struct PhaseDescriptor {
  int inverse_phase_number;  // 1..12 (decompiler execution order)
  int gcc_stage_number;      // 12..1 (GCC forward execution order)
  RtlPassKind gcc_input_pass;
  RtlPassKind gcc_output_pass;
  std::string_view inverse_phase_name;
  std::string_view gcc_stage_name;
};

constexpr std::array<PhaseDescriptor, 12> kTwelvePhases = {{
    {1, 12, RtlPassKind::kJump2, RtlPassKind::kDbr, "InversePhase1_DelaySlotUnschedule",
     "GCCStage12_DbrReorg (.jump2 -> .dbr)"},
    {2, 11, RtlPassKind::kSched2, RtlPassKind::kJump2, "InversePhase2_CrossJumpTailDuplicate",
     "GCCStage11_Jump2CrossJump (.sched2 -> .jump2)"},
    {3, 10, RtlPassKind::kGreg, RtlPassKind::kSched2, "InversePhase3_PostReloadDeSchedule",
     "GCCStage10_Sched2 (.greg -> .sched2)"},
    {4, 9, RtlPassKind::kLreg, RtlPassKind::kGreg, "InversePhase4_GlobalRegAndReloadRecovery",
     "GCCStage9_GregReload (.lreg -> .greg)"},
    {5, 8, RtlPassKind::kSched, RtlPassKind::kLreg, "InversePhase5_LocalRegWebRecovery",
     "GCCStage8_LregLocalAlloc (.sched -> .lreg)"},
    {6, 7, RtlPassKind::kCombine, RtlPassKind::kSched, "InversePhase6_PreAllocDeSchedule",
     "GCCStage7_Sched1 (.combine -> .sched)"},
    {7, 6, RtlPassKind::kFlow, RtlPassKind::kCombine, "InversePhase7_InstructionDeCombine",
     "GCCStage6_Combine (.flow -> .combine)"},
    {8, 5, RtlPassKind::kCse2, RtlPassKind::kFlow, "InversePhase8_DataflowLifeRecovery",
     "GCCStage5_Flow (.cse2 -> .flow)"},
    {9, 4, RtlPassKind::kLoop, RtlPassKind::kCse2, "InversePhase9_PostLoopCseRecovery",
     "GCCStage4_Cse2 (.loop -> .cse2)"},
    {10, 3, RtlPassKind::kCse, RtlPassKind::kLoop, "InversePhase10_LoopStrengthUnReduce",
     "GCCStage3_LoopOpt (.cse -> .loop)"},
    {11, 2, RtlPassKind::kJump, RtlPassKind::kCse, "InversePhase11_BasicBlockCseRecovery",
     "GCCStage2_Cse1 (.jump -> .cse)"},
    {12, 1, RtlPassKind::kRtl, RtlPassKind::kJump, "InversePhase12_CfgStructureAndRtlRecovery",
     "GCCStage1_Jump1AndRtl (.rtl -> .jump)"},
}};

struct CfgStage1Comparison {
  bool zero_branch_likely = false;
  bool zero_non_nop_delay_slots = false;
  bool block_count_matches = false;
  bool branch_count_matches = false;
  bool active_instructions_match = false;
  size_t remaining_branch_likely = 0;
  size_t remaining_non_nop_delay_slots = 0;
  size_t unscheduled_block_count = 0;
  size_t expected_no_dbr_block_count = 0;
  std::string mismatch_detail;

  bool ValidPreReorgDomain() const { return zero_branch_likely && zero_non_nop_delay_slots; }

  bool MatchesExactly() const {
    return zero_branch_likely && zero_non_nop_delay_slots && block_count_matches &&
           branch_count_matches && active_instructions_match;
  }
};

struct CfgStage2Comparison {
  bool gcc_jump2_modified_rtl = false;
  size_t sched2_active_insns = 0;
  size_t jump2_active_insns = 0;
  size_t cfg_before_tail_dup_blocks = 0;
  size_t cfg_after_tail_dup_blocks = 0;
  size_t cfg_before_tail_dup_active_insns = 0;
  size_t cfg_after_tail_dup_active_insns = 0;
  bool active_insn_delta_matches = false;
};

struct CaseInversionResult {
  std::string test_name;
  std::string function_name;
  bool decompiled_and_compiled_ok = false;
  bool bit_exact = false;
  bool structurally_isomorphic = false;
  bool final_dbr_rtl_matches = false;
  CfgStage1Comparison direct_cfg_stage1;
  CfgStage2Comparison direct_cfg_stage2;
  // Right-inverse check per phase: `f_k(g_k(y_k)) == y_k` (checks `gcc_output_pass` after
  // canonicalizing front-end-only `/v` `REG_USERVAR_P` and `/s` `MEM_IN_STRUCT_P` flags; for
  // Inverse Phase 1, also requires `CfgDeReorg` to produce a valid pre-`reorg.c` input in `X_11`
  // with 0 `BranchLikely` and 0 non-`nop` delay slots).
  std::array<bool, 12> phase_right_inverse_matches{};
  // Left-inverse check per phase: `g_k(f_k(x_{k-1})) == x_{k-1}` (checks `gcc_input_pass`).
  std::array<bool, 12> phase_left_inverse_matches{};
  std::array<bool, 12> gcc_stage_transformed_rtl{};
  std::optional<RtlPassKind> first_divergent_pass;
  std::string error_or_diag;
};

// Runs only the Inverse Phase 1 delay-slot unscheduling passes (excluding Inverse Phase 2's
// `CrossJumpTailDuplicatorPass`).
void RunInversePhase1DelaySlotUnschedulerOnly(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  CfgPassPipeline pipeline;
  pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<TargetHeaderDelaySlotReconcilerPass>());
  pipeline.AddPass(std::make_unique<RelaxDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>());
  pipeline.Run(cfg, analyses);
}

std::string NormalizeActiveInstructionForStage1Compare(const Instruction& inst) {
  if (inst.IsBranch() || inst.opcode == Opcode::kJ) {
    // Normalize PC-relative / absolute branch target addresses across `-O2` vs
    // `-O2 -fno-delayed-branch` since `nop` insertion shifts raw label addresses.
    return absl::StrFormat("op=%d_rs=%d_rt=%d_rd=%d_fd=%d_fs=%d_ft=%d",
                           static_cast<int>(inst.opcode),
                           inst.rs.has_value() ? static_cast<int>(*inst.rs) : -1,
                           inst.rt.has_value() ? static_cast<int>(*inst.rt) : -1,
                           inst.rd.has_value() ? static_cast<int>(*inst.rd) : -1,
                           inst.fd.has_value() ? static_cast<int>(*inst.fd) : -1,
                           inst.fs.has_value() ? static_cast<int>(*inst.fs) : -1,
                           inst.ft.has_value() ? static_cast<int>(*inst.ft) : -1);
  }
  return absl::StrFormat(
      "op=%d_rs=%d_rt=%d_rd=%d_sa=%d_imm=%d_fd=%d_fs=%d_ft=%d", static_cast<int>(inst.opcode),
      inst.rs.has_value() ? static_cast<int>(*inst.rs) : -1,
      inst.rt.has_value() ? static_cast<int>(*inst.rt) : -1,
      inst.rd.has_value() ? static_cast<int>(*inst.rd) : -1, static_cast<int>(inst.shift_amount),
      static_cast<int>(inst.immediate), inst.fd.has_value() ? static_cast<int>(*inst.fd) : -1,
      inst.fs.has_value() ? static_cast<int>(*inst.fs) : -1,
      inst.ft.has_value() ? static_cast<int>(*inst.ft) : -1);
}

size_t CountActiveInstructionsInCfg(const ControlFlowGraph& cfg) {
  size_t count = 0;
  for (const auto& blk : cfg.Blocks()) {
    for (const auto& inst : blk.instructions) {
      if (!inst.IsNop()) {
        ++count;
      }
    }
  }
  return count;
}

size_t CountBranchesInCfg(const ControlFlowGraph& cfg) {
  size_t branches = 0;
  for (const auto& blk : cfg.Blocks()) {
    for (const auto& inst : blk.instructions) {
      if (inst.IsBranch()) {
        ++branches;
      }
    }
  }
  return branches;
}

CfgStage1Comparison EvaluateDirectCfgStage1(const ControlFlowGraph& unscheduled_cfg,
                                            const ControlFlowGraph& no_dbr_cfg) {
  CfgStage1Comparison cmp;
  cmp.unscheduled_block_count = unscheduled_cfg.Blocks().size();
  cmp.expected_no_dbr_block_count = no_dbr_cfg.Blocks().size();

  for (const auto& blk : unscheduled_cfg.Blocks()) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      const Instruction& inst = blk.instructions[i];
      if (inst.IsBranchLikely()) {
        ++cmp.remaining_branch_likely;
      }
      if (inst.HasDelaySlot() && i + 1 < blk.instructions.size() &&
          !blk.instructions[i + 1].IsNop()) {
        ++cmp.remaining_non_nop_delay_slots;
      }
    }
  }

  cmp.zero_branch_likely = (cmp.remaining_branch_likely == 0);
  cmp.zero_non_nop_delay_slots = (cmp.remaining_non_nop_delay_slots == 0);
  cmp.block_count_matches = (cmp.unscheduled_block_count == cmp.expected_no_dbr_block_count);
  cmp.branch_count_matches =
      (CountBranchesInCfg(unscheduled_cfg) == CountBranchesInCfg(no_dbr_cfg));

  std::vector<uint32_t> rpo_unsched = unscheduled_cfg.ReversePostOrder();
  std::vector<uint32_t> rpo_no_dbr = no_dbr_cfg.ReversePostOrder();
  if (rpo_unsched.size() != rpo_no_dbr.size()) {
    cmp.active_instructions_match = false;
    cmp.mismatch_detail = absl::StrFormat("RPO block count %zu != expected %zu", rpo_unsched.size(),
                                          rpo_no_dbr.size());
    return cmp;
  }

  cmp.active_instructions_match = true;
  for (size_t b = 0; b < rpo_unsched.size(); ++b) {
    const BasicBlock* blk_u = unscheduled_cfg.GetBlock(rpo_unsched[b]);
    const BasicBlock* blk_n = no_dbr_cfg.GetBlock(rpo_no_dbr[b]);
    if (blk_u == nullptr || blk_n == nullptr) {
      cmp.active_instructions_match = false;
      break;
    }
    std::vector<std::string> active_u;
    for (const auto& inst : blk_u->instructions) {
      if (!inst.IsNop()) {
        active_u.push_back(NormalizeActiveInstructionForStage1Compare(inst));
      }
    }
    std::vector<std::string> active_n;
    for (const auto& inst : blk_n->instructions) {
      if (!inst.IsNop()) {
        active_n.push_back(NormalizeActiveInstructionForStage1Compare(inst));
      }
    }
    if (active_u != active_n) {
      cmp.active_instructions_match = false;
      cmp.mismatch_detail = absl::StrFormat("RPO block %zu active insn count %zu != expected %zu",
                                            b, active_u.size(), active_n.size());
      break;
    }
  }

  return cmp;
}

bool DidGccStageTransformRtl(const GccCompilationDump& dump, std::string_view func_name,
                             RtlPassKind input_pass, RtlPassKind output_pass) {
  const RtlFunctionDump* in_fn = dump.GetFunctionPass(func_name, input_pass);
  const RtlFunctionDump* out_fn = dump.GetFunctionPass(func_name, output_pass);
  if (in_fn == nullptr || out_fn == nullptr) {
    return false;
  }
  GccCompilationDump in_as_out("", {{output_pass, RtlPassDump(output_pass, "", "", {*in_fn})}});
  return !dump.PassMatches(in_as_out, func_name, output_pass);
}

// Normalizes an active RTL instruction S-expression for Right-Inverse comparison (`f_k(g_k(y_k)) ==
// y_k`) by canonicalizing UIDs, `label_ref` numbers, virtual pseudo-registers (>= 72), and
// front-end-only C declaration flags (`/v` `REG_USERVAR_P`, `/f` pointer/function-value, `/i`
// integrated on `reg`, and `/s` `MEM_IN_STRUCT_P` on `mem`, while preserving `/v` `MEM_VOLATILE_P`
// on `mem` and `/s` `INSN_ANNULLED_BRANCH_P` on `insn`).
std::string NormalizeInsnSexprForRightInverse(std::string_view sexpr) {
  std::string out;
  out.reserve(sexpr.size());

  size_t i = 0;
  while (i < sexpr.size()) {
    if (absl::ascii_isspace(sexpr[i])) {
      if (!out.empty() && out.back() != ' ') {
        out.push_back(' ');
      }
      while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
        ++i;
      }
      continue;
    }

    if (sexpr[i] == '(') {
      std::string_view rest = sexpr.substr(i + 1);
      auto starts_with_tag = [&](std::string_view prefix) {
        if (!absl::StartsWith(rest, prefix)) {
          return false;
        }
        size_t after = prefix.size();
        return after < rest.size() &&
               (rest[after] == ' ' || rest[after] == '/' || rest[after] == ':');
      };

      if (starts_with_tag("insn") || starts_with_tag("jump_insn") || starts_with_tag("call_insn")) {
        out.push_back('(');
        ++i;
        std::string tag_buf;
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i])) {
          tag_buf.push_back(sexpr[i++]);
        }
        // Preserve `/s` (annulled branch delay slot) on insns, strip `/i` / `/v`.
        size_t slash_pos = tag_buf.find('/');
        if (slash_pos != std::string::npos) {
          bool has_s = absl::StrContains(tag_buf.substr(slash_pos + 1), 's');
          tag_buf.resize(slash_pos);
          if (has_s) {
            tag_buf.append("/s");
          }
        }
        out.append(tag_buf);
        for (int k = 0; k < 3; ++k) {
          size_t save = i;
          while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
            ++i;
          }
          size_t num_start = i;
          if (i < sexpr.size() && sexpr[i] == '-') {
            ++i;
          }
          while (i < sexpr.size() && absl::ascii_isdigit(sexpr[i])) {
            ++i;
          }
          if (i == num_start || (i == num_start + 1 && sexpr[num_start] == '-')) {
            i = save;
            break;
          }
        }
        continue;
      }

      if (absl::StartsWith(rest, "label_ref ")) {
        out.append("(label_ref)");
        i += 1 + std::string_view("label_ref ").size();
        while (i < sexpr.size() && sexpr[i] != ')') {
          ++i;
        }
        if (i < sexpr.size() && sexpr[i] == ')') {
          ++i;
        }
        continue;
      }

      if (absl::StartsWith(rest, "insn_list")) {
        out.push_back('(');
        ++i;
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != ')') {
          out.push_back(sexpr[i++]);
        }
        while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
          ++i;
        }
        while (i < sexpr.size() && absl::ascii_isdigit(sexpr[i])) {
          ++i;
        }
        continue;
      }

      if (starts_with_tag("mem")) {
        out.append("(mem");
        i += 1 + std::string_view("mem").size();
        bool is_volatile = false;
        while (i < sexpr.size() && sexpr[i] != ':' && !absl::ascii_isspace(sexpr[i]) &&
               sexpr[i] != ')') {
          if (sexpr[i] == 'v') {
            is_volatile = true;
          }
          ++i;
        }
        if (is_volatile) {
          out.append("/v");
        }
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != ')') {
          out.push_back(sexpr[i++]);
        }
        continue;
      }

      if (starts_with_tag("reg")) {
        out.append("(reg");
        i += 1 + std::string_view("reg").size();
        // Strip `/v`, `/i`, `/f` flags before `:MODE`.
        while (i < sexpr.size() && sexpr[i] != ':' && !absl::ascii_isspace(sexpr[i]) &&
               sexpr[i] != ')') {
          ++i;
        }
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != ')') {
          out.push_back(sexpr[i++]);
        }
        while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
          ++i;
        }
        size_t num_start = i;
        while (i < sexpr.size() && absl::ascii_isdigit(sexpr[i])) {
          ++i;
        }
        if (i > num_start) {
          int reg_num = -1;
          std::string_view num_str = sexpr.substr(num_start, i - num_start);
          if (absl::SimpleAtoi(num_str, &reg_num) && reg_num >= 72) {
            out.append(" pseudo");
          } else {
            out.push_back(' ');
            out.append(num_str);
          }
        }
        continue;
      }
    }

    out.push_back(sexpr[i++]);
  }

  return out;
}

bool RightInversePassMatches(const GccCompilationDump& orig_dump,
                             const GccCompilationDump& cand_dump, std::string_view func_name,
                             RtlPassKind output_pass) {
  const RtlFunctionDump* lhs_fn = orig_dump.GetFunctionPass(func_name, output_pass);
  const RtlFunctionDump* rhs_fn = cand_dump.GetFunctionPass(func_name, output_pass);
  if (lhs_fn == nullptr && rhs_fn == nullptr) {
    return true;
  }
  if (lhs_fn == nullptr || rhs_fn == nullptr) {
    return false;
  }
  std::vector<std::string> lhs_active;
  for (const auto& insn : lhs_fn->insns) {
    if (insn.kind == RtlInsnKind::kInsn || insn.kind == RtlInsnKind::kJumpInsn ||
        insn.kind == RtlInsnKind::kCallInsn) {
      lhs_active.push_back(NormalizeInsnSexprForRightInverse(insn.raw_sexpr));
    }
  }
  std::vector<std::string> rhs_active;
  for (const auto& insn : rhs_fn->insns) {
    if (insn.kind == RtlInsnKind::kInsn || insn.kind == RtlInsnKind::kJumpInsn ||
        insn.kind == RtlInsnKind::kCallInsn) {
      rhs_active.push_back(NormalizeInsnSexprForRightInverse(insn.raw_sexpr));
    }
  }
  return lhs_active == rhs_active;
}

class GccStageInversionSuiteAccumulator {
 public:
  static GccStageInversionSuiteAccumulator& Instance() {
    static GccStageInversionSuiteAccumulator instance;
    return instance;
  }

  void Record(CaseInversionResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(std::move(result));
  }

  std::string FormatReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t total = results_.size();
    if (total == 0) {
      return "";
    }

    size_t compiled_ok = 0;
    size_t bit_exact = 0;
    size_t isomorphic = 0;
    size_t dbr_rtl_right_inv = 0;
    size_t dbr_rtl_left_inv = 0;
    size_t all_12_right_inverse_exact = 0;
    size_t all_12_left_inverse_exact = 0;

    size_t stage1_zero_likely = 0;
    size_t stage1_zero_delay = 0;
    size_t stage1_valid_pre_reorg_domain = 0;
    size_t stage1_blocks_match = 0;
    size_t stage1_active_insns_match = 0;
    size_t stage1_full_cfg_exact = 0;

    size_t stage2_gcc_modified_count = 0;
    size_t stage2_delta_matched_when_modified = 0;
    size_t stage2_delta_matched_overall = 0;

    std::array<size_t, 12> phase_right_matches{};
    std::array<size_t, 12> phase_right_matches_when_transformed{};
    std::array<size_t, 12> phase_left_matches{};
    std::array<size_t, 12> phase_gcc_transformed_count{};
    absl::flat_hash_map<RtlPassKind, size_t> first_divergent_counts;

    for (const auto& r : results_) {
      if (r.decompiled_and_compiled_ok) ++compiled_ok;
      if (r.bit_exact) ++bit_exact;
      if (r.structurally_isomorphic) ++isomorphic;
      if (r.final_dbr_rtl_matches) ++dbr_rtl_left_inv;
      if (r.phase_right_inverse_matches[0]) ++dbr_rtl_right_inv;

      if (r.direct_cfg_stage1.zero_branch_likely) ++stage1_zero_likely;
      if (r.direct_cfg_stage1.zero_non_nop_delay_slots) ++stage1_zero_delay;
      if (r.direct_cfg_stage1.ValidPreReorgDomain()) ++stage1_valid_pre_reorg_domain;
      if (r.direct_cfg_stage1.block_count_matches) ++stage1_blocks_match;
      if (r.direct_cfg_stage1.active_instructions_match) ++stage1_active_insns_match;
      if (r.direct_cfg_stage1.MatchesExactly()) ++stage1_full_cfg_exact;

      if (r.direct_cfg_stage2.active_insn_delta_matches) ++stage2_delta_matched_overall;
      if (r.direct_cfg_stage2.gcc_jump2_modified_rtl) {
        ++stage2_gcc_modified_count;
        if (r.direct_cfg_stage2.active_insn_delta_matches) {
          ++stage2_delta_matched_when_modified;
        }
      }

      bool all_12_right = r.decompiled_and_compiled_ok;
      bool all_12_left = r.decompiled_and_compiled_ok;
      for (size_t i = 0; i < 12; ++i) {
        if (r.phase_right_inverse_matches[i]) {
          ++phase_right_matches[i];
        } else {
          all_12_right = false;
        }
        if (r.phase_left_inverse_matches[i]) {
          ++phase_left_matches[i];
        } else {
          all_12_left = false;
        }
        if (r.gcc_stage_transformed_rtl[i]) {
          ++phase_gcc_transformed_count[i];
          if (r.phase_right_inverse_matches[i]) {
            ++phase_right_matches_when_transformed[i];
          }
        }
      }
      if (all_12_right) ++all_12_right_inverse_exact;
      if (all_12_left) ++all_12_left_inverse_exact;
      if (r.first_divergent_pass.has_value()) {
        ++first_divergent_counts[*r.first_divergent_pass];
      }
    }

    auto pct = [](size_t num, size_t den) -> double {
      return den == 0 ? 100.0 : (100.0 * static_cast<double>(num) / static_cast<double>(den));
    };

    std::string out;
    absl::StrAppendFormat(
        &out,
        "\n================================================================================\n"
        "              12-PHASE GCC RIGHT-INVERSE SUITE SUMMARY REPORT                   \n"
        "================================================================================\n"
        "Total Round-Trip Cases Evaluated       : %zu\n"
        "Decompiled & Recompiled Cleanly        : %zu / %zu (%.2f%%)\n"
        "Structurally Isomorphic Binary         : %zu / %zu (%.2f%%)\n"
        "Bit-Exact Final Binary (.o)            : %zu / %zu (%.2f%%)\n"
        "Stage 12 .dbr Right-Inverse RTL Match  : %zu / %zu (%.2f%%)\n"
        "All 12 Phases Right-Inverse RTL Match  : %zu / %zu (%.2f%%)\n"
        "All 12 Phases Left-Inverse RTL Match   : %zu / %zu (%.2f%%)\n"
        "--------------------------------------------------------------------------------\n"
        "DIRECT CFG STAGE 1 (CfgDeReorg Pre-reorg.c Domain & Right-Inverse Check):\n"
        "  Zero BranchLikely Remaining          : %zu / %zu (%.2f%%)\n"
        "  Zero Non-Nop Delay Slots             : %zu / %zu (%.2f%%)\n"
        "  Valid Pre-reorg.c Domain (Both Above): %zu / %zu (%.2f%%)\n"
        "  Matches -fno-delayed-branch Blocks   : %zu / %zu (%.2f%%)\n"
        "  Matches -fno-delayed-branch Insns    : %zu / %zu (%.2f%%)\n"
        "  Full Left-Inverse (-fno-delayed-br)  : %zu / %zu (%.2f%%)\n"
        "--------------------------------------------------------------------------------\n"
        "DIRECT CFG STAGE 2 (CrossJumpTailDuplicator vs .sched2 -> .jump2 Insn Delta):\n"
        "  Overall Active Insn Delta Match      : %zu / %zu (%.2f%%)\n"
        "  Cases Modified by GCC .jump2         : %zu / %zu (%.2f%%)\n"
        "--------------------------------------------------------------------------------\n"
        "RIGHT-INVERSE f_k(g_k(y_k)) == y_k BY PHASE (vs Left-Inverse g_k(f_k(x)) == x):\n",
        total, compiled_ok, total, pct(compiled_ok, total), isomorphic, total,
        pct(isomorphic, total), bit_exact, total, pct(bit_exact, total), dbr_rtl_right_inv, total,
        pct(dbr_rtl_right_inv, total), all_12_right_inverse_exact, total,
        pct(all_12_right_inverse_exact, total), all_12_left_inverse_exact, total,
        pct(all_12_left_inverse_exact, total), stage1_zero_likely, total,
        pct(stage1_zero_likely, total), stage1_zero_delay, total, pct(stage1_zero_delay, total),
        stage1_valid_pre_reorg_domain, total, pct(stage1_valid_pre_reorg_domain, total),
        stage1_blocks_match, total, pct(stage1_blocks_match, total), stage1_active_insns_match,
        total, pct(stage1_active_insns_match, total), stage1_full_cfg_exact, total,
        pct(stage1_full_cfg_exact, total), stage2_delta_matched_overall, total,
        pct(stage2_delta_matched_overall, total), stage2_delta_matched_when_modified,
        stage2_gcc_modified_count,
        pct(stage2_delta_matched_when_modified, stage2_gcc_modified_count));

    for (size_t i = 0; i < 12; ++i) {
      const auto& p = kTwelvePhases[i];
      absl::StrAppendFormat(
          &out,
          "  [Inv %2d <- GCC %2d] %-36s | Right: %3zu/%3zu (%6.2f%%) | Active: %3zu/%3zu (%6.2f%%) "
          "| Left: %3zu/%3zu (%6.2f%%)\n",
          p.inverse_phase_number, p.gcc_stage_number, p.gcc_stage_name, phase_right_matches[i],
          total, pct(phase_right_matches[i], total), phase_right_matches_when_transformed[i],
          phase_gcc_transformed_count[i],
          pct(phase_right_matches_when_transformed[i], phase_gcc_transformed_count[i]),
          phase_left_matches[i], total, pct(phase_left_matches[i], total));
    }

    absl::StrAppend(
        &out,
        "--------------------------------------------------------------------------------\n"
        "FIRST DIVERGENT GCC PASS DISTRIBUTION (Chronological Forward Order):\n");
    for (RtlPassKind pass : GccRtlDumpInspector::AllPasses()) {
      size_t c = first_divergent_counts[pass];
      if (c > 0) {
        absl::StrAppendFormat(&out, "  %-10s : %3zu / %3zu (%.2f%%)\n",
                              GccRtlDumpInspector::PassExtension(pass), c, total, pct(c, total));
      }
    }
    absl::StrAppend(
        &out, "================================================================================\n");
    return out;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<CaseInversionResult> results_;
};

class GccStageInversionTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    auto runner_or = RoundTripRunner::Create();
    ASSERT_TRUE(runner_or.ok()) << runner_or.status();
    runner = runner_or->release();

    auto inspector_or = GccRtlDumpInspector::Create();
    ASSERT_TRUE(inspector_or.ok()) << inspector_or.status();
    inspector = new GccRtlDumpInspector(std::move(*inspector_or));

    const auto& cases = GetSharedRoundTripTestCases();
    std::vector<CaseInversionResult> precomputed(cases.size());
    std::atomic<size_t> next_idx{0};
    unsigned int hw_threads = std::thread::hardware_concurrency();
    const size_t num_workers =
        std::max<size_t>(1, std::min<size_t>(hw_threads == 0 ? 8 : hw_threads, 16));

    std::vector<std::thread> workers;
    workers.reserve(num_workers);
    for (size_t w = 0; w < num_workers; ++w) {
      workers.emplace_back([&]() {
        while (true) {
          size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
          if (idx >= cases.size()) {
            break;
          }
          precomputed[idx] = EvaluateTestCase(cases[idx]);
        }
      });
    }
    for (auto& t : workers) {
      t.join();
    }

    precomputed_results = new absl::flat_hash_map<std::string, CaseInversionResult>();
    precomputed_results->reserve(cases.size());
    for (auto& r : precomputed) {
      (*precomputed_results)[r.test_name] = std::move(r);
    }
  }

  static void TearDownTestSuite() {
    std::cout << GccStageInversionSuiteAccumulator::Instance().FormatReport() << std::flush;
    delete precomputed_results;
    precomputed_results = nullptr;
    delete inspector;
    inspector = nullptr;
    delete runner;
    runner = nullptr;
  }

  static CaseInversionResult EvaluateTestCase(const RoundTripTestCase& test_case) {
    CaseInversionResult res;
    res.test_name = test_case.test_name;
    res.function_name = test_case.function_name;

    static std::atomic<uint64_t> case_counter{0};
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    std::filesystem::path base_tmp = (test_tmpdir != nullptr && *test_tmpdir != '\0')
                                         ? std::filesystem::path(test_tmpdir)
                                         : std::filesystem::temp_directory_path();
    std::filesystem::path work_dir =
        base_tmp / absl::StrFormat("gcc_stage_inv_%d_%d", getpid(),
                                   case_counter.fetch_add(1, std::memory_order_relaxed));

    auto cleanup = [&]() {
      std::error_code ec;
      std::filesystem::remove_all(work_dir, ec);
    };

    auto prepared_or = runner->PrepareCase(test_case, work_dir);
    if (!prepared_or.ok()) {
      res.error_or_diag = std::string(prepared_or.status().message());
      cleanup();
      return res;
    }
    const PreparedRoundTripCase& prepared = *prepared_or;

    // 1. Build `-fno-delayed-branch` reference CFG (exact binary input to GCC Stage 12 `reorg.c`).
    std::vector<std::string> no_dbr_flags = test_case.options.c_flags;
    no_dbr_flags.push_back("-fno-delayed-branch");
    std::filesystem::path orig_no_dbr_o =
        work_dir / absl::StrCat("orig_no_dbr_", test_case.function_name, ".o");
    auto no_dbr_status = prepared.compiler->CompileC({
        .src_file = prepared.orig_c_path,
        .out_obj = orig_no_dbr_o,
        .opt_flags = no_dbr_flags,
        .include_dirs = {work_dir},
    });

    auto loaded_dbr_or = FunctionLoader::FromWords(
        test_case.function_name, prepared.patched_orig_words, test_case.options.base_vram,
        prepared.symbol_index.get(), /*game_config=*/nullptr, prepared.rodata_memory_reader);
    if (!loaded_dbr_or.ok()) {
      res.error_or_diag = std::string(loaded_dbr_or.status().message());
      cleanup();
      return res;
    }

    // 2. Inspect all 13 GCC RTL dumps on original C (`orig_dump`).
    GccRtlInspectOptions inspect_opts;
    inspect_opts.opt_flags = test_case.options.c_flags;
    inspect_opts.extra_types_header = test_case.options.extra_types_header;
    auto orig_dump_or = inspector->InspectC(test_case.original_c, inspect_opts);
    if (!orig_dump_or.ok()) {
      res.error_or_diag = std::string(orig_dump_or.status().message());
      cleanup();
      return res;
    }
    const GccCompilationDump& orig_dump = *orig_dump_or;

    // 3. Evaluate Direct CFG Stage 1 (`CfgDeReorg` Delay-Slot Unscheduler vs `-fno-delayed-branch`)
    // and Direct CFG Stage 2 (`CrossJumpTailDuplicatorPass` vs `.sched2` -> `.jump2`).
    DecompilationContext dbr_ctx(*loaded_dbr_or);
    if (dbr_ctx.BuildCfg().ok()) {
      RunInversePhase1DelaySlotUnschedulerOnly(dbr_ctx.MutableCfg(), &dbr_ctx.CfgAnalyses());

      if (no_dbr_status.ok()) {
        auto no_dbr_elf_or = ElfReader::LoadFromFile(orig_no_dbr_o);
        if (no_dbr_elf_or.ok()) {
          auto no_dbr_fn_or = no_dbr_elf_or->ExtractFunction(test_case.function_name);
          if (no_dbr_fn_or.ok()) {
            std::vector<uint32_t> no_dbr_words =
                RoundTripRunner::ApplyCandidateRelocations(prepared, *no_dbr_fn_or);
            auto loaded_no_dbr_or =
                FunctionLoader::FromWords(test_case.function_name, no_dbr_words,
                                          test_case.options.base_vram, prepared.symbol_index.get(),
                                          /*game_config=*/nullptr, prepared.rodata_memory_reader);
            if (loaded_no_dbr_or.ok()) {
              DecompilationContext no_dbr_ctx(*loaded_no_dbr_or);
              if (no_dbr_ctx.BuildCfg().ok()) {
                res.direct_cfg_stage1 = EvaluateDirectCfgStage1(dbr_ctx.Cfg(), no_dbr_ctx.Cfg());
              }
            }
          }
        }
      }

      res.direct_cfg_stage2.cfg_before_tail_dup_blocks = dbr_ctx.Cfg().Blocks().size();
      res.direct_cfg_stage2.cfg_before_tail_dup_active_insns =
          CountActiveInstructionsInCfg(dbr_ctx.Cfg());

      CrossJumpTailDuplicatorPass tail_dup_pass;
      tail_dup_pass.Run(dbr_ctx.MutableCfg(), &dbr_ctx.CfgAnalyses());

      res.direct_cfg_stage2.cfg_after_tail_dup_blocks = dbr_ctx.Cfg().Blocks().size();
      res.direct_cfg_stage2.cfg_after_tail_dup_active_insns =
          CountActiveInstructionsInCfg(dbr_ctx.Cfg());

      const RtlFunctionDump* orig_sched2 =
          orig_dump.GetFunctionPass(test_case.function_name, RtlPassKind::kSched2);
      const RtlFunctionDump* orig_jump2 =
          orig_dump.GetFunctionPass(test_case.function_name, RtlPassKind::kJump2);
      if (orig_sched2 != nullptr && orig_jump2 != nullptr) {
        res.direct_cfg_stage2.sched2_active_insns = orig_sched2->ActiveInsnCount();
        res.direct_cfg_stage2.jump2_active_insns = orig_jump2->ActiveInsnCount();
        res.direct_cfg_stage2.gcc_jump2_modified_rtl = DidGccStageTransformRtl(
            orig_dump, test_case.function_name, RtlPassKind::kSched2, RtlPassKind::kJump2);
        const ptrdiff_t gcc_merged_insns =
            static_cast<ptrdiff_t>(res.direct_cfg_stage2.sched2_active_insns) -
            static_cast<ptrdiff_t>(res.direct_cfg_stage2.jump2_active_insns);
        const ptrdiff_t cfg_restored_insns =
            static_cast<ptrdiff_t>(res.direct_cfg_stage2.cfg_after_tail_dup_active_insns) -
            static_cast<ptrdiff_t>(res.direct_cfg_stage2.cfg_before_tail_dup_active_insns);
        res.direct_cfg_stage2.active_insn_delta_matches = (gcc_merged_insns == cfg_restored_insns);
      } else {
        // `-O0` skips `.sched2` and `.jump2`.
        res.direct_cfg_stage2.active_insn_delta_matches =
            (res.direct_cfg_stage2.cfg_after_tail_dup_active_insns ==
             res.direct_cfg_stage2.cfg_before_tail_dup_active_insns);
      }
    }

    // 4. Run full `LifterPipeline` and compile lifted C with `-da` to inspect all 12 phases.
    LifterPipelineOptions lifter_opts;
    lifter_opts.includes = {"types.h"};
    lifter_opts.format_with_clang = false;
    LifterPipeline lifter(lifter_opts);

    auto decompile_or = lifter.DecompileFunction(*loaded_dbr_or);
    if (!decompile_or.ok()) {
      res.error_or_diag = std::string(decompile_or.status().message());
      cleanup();
      return res;
    }

    CTranslationUnit tu;
    tu.includes = {"types.h"};
    tu.symbol_index = prepared.symbol_index.get();
    tu.functions.push_back(std::move(*decompile_or->ast));

    CEmitterOptions emitter_opts;
    emitter_opts.includes = {"types.h"};
    emitter_opts.format_with_clang = false;
    std::string lifted_c = CEmitter::EmitTranslationUnit(tu, emitter_opts);

    auto cand_dump_or = inspector->InspectC(lifted_c, inspect_opts);
    if (!cand_dump_or.ok()) {
      res.error_or_diag = std::string(cand_dump_or.status().message());
      cleanup();
      return res;
    }
    const GccCompilationDump& cand_dump = *cand_dump_or;

    std::filesystem::path cand_c_path =
        work_dir / absl::StrCat("cand_", test_case.function_name, ".c");
    std::filesystem::path cand_o_path =
        work_dir / absl::StrCat("cand_", test_case.function_name, ".o");
    {
      std::ofstream ofs(cand_c_path, std::ios::out | std::ios::trunc);
      ofs.write(lifted_c.data(), static_cast<std::streamsize>(lifted_c.size()));
    }
    auto cand_compile_status = prepared.compiler->CompileC({
        .src_file = cand_c_path,
        .out_obj = cand_o_path,
        .opt_flags = test_case.options.c_flags,
        .include_dirs = {work_dir},
    });
    if (cand_compile_status.ok()) {
      auto cand_elf_or = ElfReader::LoadFromFile(cand_o_path);
      if (cand_elf_or.ok()) {
        auto cand_fn_or = cand_elf_or->ExtractFunction(test_case.function_name);
        if (cand_fn_or.ok()) {
          res.decompiled_and_compiled_ok = true;
          std::string diag;
          res.bit_exact = RoundTripRunner::CompareFunctionsBitExact(
              prepared.orig_func, *cand_fn_or, test_case.options.base_vram, prepared.full_orig_c,
              lifted_c, &diag);
          if (res.bit_exact) {
            res.structurally_isomorphic = true;
          } else {
            auto struct_cmp = RoundTripRunner::EvaluateStructuralIsomorphism(prepared, *cand_fn_or,
                                                                             lifted_c, &diag);
            res.structurally_isomorphic = struct_cmp.IsStructurallyIsomorphic();
          }
        }
      }
    }

    res.final_dbr_rtl_matches =
        orig_dump.PassMatches(cand_dump, test_case.function_name, RtlPassKind::kDbr);
    res.first_divergent_pass = std::nullopt;
    for (RtlPassKind pass : GccRtlDumpInspector::AllPasses()) {
      if (!RightInversePassMatches(orig_dump, cand_dump, test_case.function_name, pass)) {
        res.first_divergent_pass = pass;
        break;
      }
    }

    for (size_t i = 0; i < 12; ++i) {
      const auto& p = kTwelvePhases[i];
      res.phase_right_inverse_matches[i] =
          RightInversePassMatches(orig_dump, cand_dump, test_case.function_name, p.gcc_output_pass);
      res.phase_left_inverse_matches[i] =
          orig_dump.PassMatches(cand_dump, test_case.function_name, p.gcc_input_pass);
    }
    for (size_t i = 0; i < 12; ++i) {
      const auto& p = kTwelvePhases[i];
      res.gcc_stage_transformed_rtl[i] = !RightInversePassMatches(
          orig_dump,
          GccCompilationDump(
              "", {{p.gcc_output_pass,
                    RtlPassDump(p.gcc_output_pass, "", "",
                                orig_dump.GetFunctionPass(test_case.function_name,
                                                          p.gcc_input_pass) != nullptr
                                    ? std::vector<RtlFunctionDump>{*orig_dump.GetFunctionPass(
                                          test_case.function_name, p.gcc_input_pass)}
                                    : std::vector<RtlFunctionDump>{})}}),
          test_case.function_name, p.gcc_output_pass);
    }

    cleanup();
    return res;
  }

  static RoundTripRunner* runner;
  static GccRtlDumpInspector* inspector;
  static absl::flat_hash_map<std::string, CaseInversionResult>* precomputed_results;
};

RoundTripRunner* GccStageInversionTest::runner = nullptr;
GccRtlDumpInspector* GccStageInversionTest::inspector = nullptr;
absl::flat_hash_map<std::string, CaseInversionResult>* GccStageInversionTest::precomputed_results =
    nullptr;

TEST_P(GccStageInversionTest, SatisfiesRightInverseAcrossAllTwelvePhases) {
  const RoundTripTestCase& test_case = GetParam();
  CaseInversionResult res =
      (precomputed_results != nullptr && precomputed_results->contains(test_case.test_name))
          ? (*precomputed_results)[test_case.test_name]
          : EvaluateTestCase(test_case);
  GccStageInversionSuiteAccumulator::Instance().Record(res);

  ASSERT_TRUE(res.decompiled_and_compiled_ok) << res.error_or_diag;
  EXPECT_TRUE(res.direct_cfg_stage1.ValidPreReorgDomain())
      << "Direct CFG Stage 1 (`CfgDeReorg`) failed to map into pre-`reorg.c` domain X_11 for "
      << test_case.test_name
      << ": remaining_branch_likely=" << res.direct_cfg_stage1.remaining_branch_likely
      << ", remaining_non_nop_delay_slots=" << res.direct_cfg_stage1.remaining_non_nop_delay_slots;
  for (size_t i = 0; i < 12; ++i) {
    EXPECT_TRUE(res.phase_right_inverse_matches[i])
        << "Right-Inverse Phase " << kTwelvePhases[i].inverse_phase_number << " ("
        << kTwelvePhases[i].gcc_stage_name << ") output RTL f_k(g_k(y_k)) == y_k ("
        << GccRtlDumpInspector::PassExtension(kTwelvePhases[i].gcc_output_pass) << ") mismatch for "
        << test_case.test_name;
  }
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripTwelvePhaseSuite, GccStageInversionTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
