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
#include "core/compiler_flags.h"
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
#include "lifter/inverse_compiler_pipeline.h"
#include "lifter/lifter_pipeline.h"

namespace rom_nom_nom {
namespace {

struct CfgStage1Comparison {
  bool zero_branch_likely = false;
  bool zero_non_nop_delay_slots = false;
  bool block_count_matches = false;
  bool branch_count_matches = false;
  bool successors_match = false;
  bool active_instructions_match = false;
  size_t remaining_branch_likely = 0;
  size_t remaining_non_nop_delay_slots = 0;
  size_t unscheduled_block_count = 0;
  size_t expected_no_dbr_block_count = 0;
  std::string mismatch_detail;

  bool ValidPreReorgDomain() const { return zero_branch_likely && zero_non_nop_delay_slots; }

  bool MatchesExactly() const {
    return zero_branch_likely && zero_non_nop_delay_slots && block_count_matches &&
           branch_count_matches && successors_match && active_instructions_match;
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
  // Direct RtlCfg -> RTL comparison for Stage 0 (`InverseFinalAndGasStage`: initial binary CFG
  // lifted to `.dbr` RtlCfg vs `.dbr` RTL dump).
  RtlCfgPassComparison direct_rtl_stage0_comparison{};
  // Direct RtlCfg -> RTL comparison per stage (`Stages 1..12` vs `.jump2` .. `.rtl`).
  std::array<RtlCfgPassComparison, 12> direct_rtl_stage_comparisons{};
  // Exact Right-inverse check per phase (`Stages 1..12`): `f_k(g_k(y_k)) == y_k` (exact
  // S-expression match on `source_gcc_pass`).
  std::array<bool, 12> phase_right_inverse_matches{};
  // Exact Left-inverse check per phase (`Stages 1..12`): `g_k(f_k(x_{k-1})) == x_{k-1}` (exact
  // S-expression match on `target_gcc_pass`).
  std::array<bool, 12> phase_left_inverse_matches{};
  std::array<bool, 12> gcc_stage_transformed_rtl{};
  // Stage 13 (`InverseRtlToCAstStage` <- `GCCStage0_Frontend (C AST -> .rtl)`) topology and exact
  // S-expression match on `.00.rtl`.
  RtlCfgPassComparison stage13_rtl_topo_comparison{};
  bool stage13_rtl_matches = false;
  std::optional<RtlPassKind> first_divergent_pass;
  std::string error_or_diag;
};

std::string FormatActiveInstructionForStage1Compare(const Instruction& inst) {
  if (inst.IsBranch() || inst.opcode == Opcode::kJ) {
    // Target basic block edges are verified exactly via `successors_match` on RPO block indices.
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
    const Instruction* term = blk.Terminator();
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      const Instruction& inst = blk.instructions[i];
      if (inst.IsBranchLikely()) {
        ++cmp.remaining_branch_likely;
      }
      if (&inst == term && inst.HasDelaySlot() && i + 1 < blk.instructions.size() &&
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
    cmp.successors_match = false;
    cmp.active_instructions_match = false;
    auto summarize_cfg = [](const ControlFlowGraph& g, const std::vector<uint32_t>& rpo) {
      std::vector<std::string> blks;
      for (size_t i = 0; i < rpo.size(); ++i) {
        const BasicBlock* blk = g.GetBlock(rpo[i]);
        if (blk == nullptr) continue;
        std::vector<std::string> insns;
        for (const auto& inst : blk->instructions) {
          if (!inst.IsNop()) insns.push_back(inst.Disassemble());
        }
        blks.push_back(absl::StrFormat("B%zu(%s)", i, absl::StrJoin(insns, "; ")));
      }
      return absl::StrJoin(blks, " | ");
    };
    cmp.mismatch_detail = absl::StrFormat(
        "RPO blks %zu != exp %zu: got {%s} vs exp {%s}", rpo_unsched.size(), rpo_no_dbr.size(),
        summarize_cfg(unscheduled_cfg, rpo_unsched), summarize_cfg(no_dbr_cfg, rpo_no_dbr));
    return cmp;
  }

  absl::flat_hash_map<uint32_t, size_t> unsched_id_to_rpo;
  absl::flat_hash_map<uint32_t, size_t> no_dbr_id_to_rpo;
  for (size_t b = 0; b < rpo_unsched.size(); ++b) {
    unsched_id_to_rpo[rpo_unsched[b]] = b;
    no_dbr_id_to_rpo[rpo_no_dbr[b]] = b;
  }

  cmp.successors_match = true;
  cmp.active_instructions_match = true;
  for (size_t b = 0; b < rpo_unsched.size(); ++b) {
    const BasicBlock* blk_u = unscheduled_cfg.GetBlock(rpo_unsched[b]);
    const BasicBlock* blk_n = no_dbr_cfg.GetBlock(rpo_no_dbr[b]);
    if (blk_u == nullptr || blk_n == nullptr) {
      cmp.successors_match = false;
      cmp.active_instructions_match = false;
      break;
    }
    std::vector<size_t> succ_u;
    succ_u.reserve(blk_u->successors.size());
    for (uint32_t s : blk_u->successors) {
      auto it = unsched_id_to_rpo.find(s);
      succ_u.push_back(it != unsched_id_to_rpo.end() ? it->second : static_cast<size_t>(-1));
    }
    std::vector<size_t> succ_n;
    succ_n.reserve(blk_n->successors.size());
    for (uint32_t s : blk_n->successors) {
      auto it = no_dbr_id_to_rpo.find(s);
      succ_n.push_back(it != no_dbr_id_to_rpo.end() ? it->second : static_cast<size_t>(-1));
    }
    if (succ_u != succ_n) {
      cmp.successors_match = false;
      if (cmp.mismatch_detail.empty()) {
        auto summarize_cfg = [](const ControlFlowGraph& g, const std::vector<uint32_t>& rpo) {
          std::vector<std::string> blks;
          for (size_t i = 0; i < rpo.size(); ++i) {
            const BasicBlock* blk = g.GetBlock(rpo[i]);
            if (blk == nullptr) continue;
            std::vector<std::string> insns;
            for (const auto& inst : blk->instructions) {
              if (!inst.IsNop()) insns.push_back(inst.Disassemble());
            }
            blks.push_back(absl::StrFormat("B%zu(%s)", i, absl::StrJoin(insns, "; ")));
          }
          return absl::StrJoin(blks, " | ");
        };
        cmp.mismatch_detail = absl::StrFormat(
            "RPO blk %zu edges got [%s] != exp [%s]: got {%s} vs exp {%s}", b,
            absl::StrJoin(succ_u, ","), absl::StrJoin(succ_n, ","),
            summarize_cfg(unscheduled_cfg, rpo_unsched), summarize_cfg(no_dbr_cfg, rpo_no_dbr));
      }
    }

    std::vector<std::string> active_u;
    std::vector<std::string> disasm_u;
    for (const auto& inst : blk_u->instructions) {
      if (!inst.IsNop()) {
        active_u.push_back(FormatActiveInstructionForStage1Compare(inst));
        disasm_u.push_back(inst.Disassemble());
      }
    }
    std::vector<std::string> active_n;
    std::vector<std::string> disasm_n;
    for (const auto& inst : blk_n->instructions) {
      if (!inst.IsNop()) {
        active_n.push_back(FormatActiveInstructionForStage1Compare(inst));
        disasm_n.push_back(inst.Disassemble());
      }
    }
    if (active_u != active_n) {
      cmp.active_instructions_match = false;
      if (cmp.mismatch_detail.empty()) {
        auto summarize_cfg = [](const ControlFlowGraph& g, const std::vector<uint32_t>& rpo) {
          std::vector<std::string> blks;
          for (size_t i = 0; i < rpo.size(); ++i) {
            const BasicBlock* blk = g.GetBlock(rpo[i]);
            if (blk == nullptr) continue;
            std::vector<std::string> insns;
            for (const auto& inst : blk->instructions) {
              if (!inst.IsNop()) insns.push_back(inst.Disassemble());
            }
            blks.push_back(absl::StrFormat("B%zu(%s)", i, absl::StrJoin(insns, "; ")));
          }
          return absl::StrJoin(blks, " | ");
        };
        cmp.mismatch_detail = absl::StrFormat(
            "RPO blk %zu: got [%s] vs exp [%s] (full got {%s} vs exp {%s})", b,
            absl::StrJoin(disasm_u, "; "), absl::StrJoin(disasm_n, "; "),
            summarize_cfg(unscheduled_cfg, rpo_unsched), summarize_cfg(no_dbr_cfg, rpo_no_dbr));
      }
    }
  }

  return cmp;
}

// Compares two RTL function dumps for exact S-expression equality across all RTL entries without
// any lossy heuristic normalization.
bool ExactPassMatches(const GccCompilationDump& orig_dump, const GccCompilationDump& cand_dump,
                      std::string_view func_name, RtlPassKind pass) {
  const RtlFunctionDump* lhs_fn = orig_dump.GetFunctionPass(func_name, pass);
  const RtlFunctionDump* rhs_fn = cand_dump.GetFunctionPass(func_name, pass);
  if (lhs_fn == nullptr && rhs_fn == nullptr) {
    return true;
  }
  if (lhs_fn == nullptr || rhs_fn == nullptr) {
    return false;
  }
  if (lhs_fn->insns.size() != rhs_fn->insns.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs_fn->insns.size(); ++i) {
    if (absl::StripAsciiWhitespace(lhs_fn->insns[i].raw_sexpr) !=
        absl::StripAsciiWhitespace(rhs_fn->insns[i].raw_sexpr)) {
      return false;
    }
  }
  return true;
}

bool DidGccStageTransformRtl(const GccCompilationDump& dump, std::string_view func_name,
                             RtlPassKind input_pass, RtlPassKind output_pass) {
  const RtlFunctionDump* in_fn = dump.GetFunctionPass(func_name, input_pass);
  const RtlFunctionDump* out_fn = dump.GetFunctionPass(func_name, output_pass);
  if (in_fn == nullptr || out_fn == nullptr) {
    return false;
  }
  GccCompilationDump in_as_out("", {{output_pass, RtlPassDump(output_pass, "", "", {*in_fn})}});
  return !ExactPassMatches(dump, in_as_out, func_name, output_pass);
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

    size_t bit_exact = 0;
    size_t isomorphic = 0;
    size_t stage13_topo_present = 0;
    size_t stage13_topo_match = 0;
    size_t stage13_rtl_exact = 0;
    size_t all_13_rtl_dumps_exact = 0;
    size_t all_14_stages_topo_match = 0;
    size_t all_14_stages_exact_match = 0;

    size_t direct_rtl_stage0_present = 0;
    size_t direct_rtl_stage0_topo_cnt_match = 0;
    size_t direct_rtl_stage0_exact_match = 0;

    std::array<size_t, 12> direct_rtl_present_count{};
    std::array<size_t, 12> direct_rtl_topo_cnt_match{};
    std::array<size_t, 12> direct_rtl_exact_match{};

    for (const auto& r : results_) {
      if (r.bit_exact) ++bit_exact;
      if (r.structurally_isomorphic) ++isomorphic;
      if (r.stage13_rtl_matches) ++stage13_rtl_exact;

      bool case_all_14_topo = r.decompiled_and_compiled_ok;
      bool case_all_14_exact = r.decompiled_and_compiled_ok && r.stage13_rtl_matches;

      if (r.stage13_rtl_topo_comparison.pass_present) {
        ++stage13_topo_present;
        if (r.stage13_rtl_topo_comparison.MatchesTopologyAndInsnCounts()) {
          ++stage13_topo_match;
        } else {
          case_all_14_topo = false;
        }
      } else {
        case_all_14_topo = false;
      }

      bool all_13_rtl = r.decompiled_and_compiled_ok && r.stage13_rtl_matches;
      if (r.direct_rtl_stage0_comparison.pass_present) {
        ++direct_rtl_stage0_present;
        if (r.direct_rtl_stage0_comparison.MatchesTopologyAndInsnCounts()) {
          ++direct_rtl_stage0_topo_cnt_match;
        } else {
          case_all_14_topo = false;
        }
        if (r.direct_rtl_stage0_comparison.MatchesExactly()) {
          ++direct_rtl_stage0_exact_match;
        } else {
          case_all_14_exact = false;
        }
      }
      for (size_t i = 0; i < 12; ++i) {
        const auto& dc = r.direct_rtl_stage_comparisons[i];
        if (dc.pass_present) {
          ++direct_rtl_present_count[i];
          if (dc.MatchesTopologyAndInsnCounts()) {
            ++direct_rtl_topo_cnt_match[i];
          } else {
            case_all_14_topo = false;
          }
          if (dc.MatchesExactly()) {
            ++direct_rtl_exact_match[i];
          } else {
            case_all_14_exact = false;
          }
        }
        if (!r.phase_right_inverse_matches[i] || !r.phase_left_inverse_matches[i]) {
          all_13_rtl = false;
        }
      }
      if (case_all_14_topo) {
        ++all_14_stages_topo_match;
      }
      if (case_all_14_exact) {
        ++all_14_stages_exact_match;
      }
      if (all_13_rtl) {
        ++all_13_rtl_dumps_exact;
      }
    }

    auto pct = [](size_t num, size_t den) -> double {
      return den == 0 ? 100.0 : (100.0 * static_cast<double>(num) / static_cast<double>(den));
    };

    const auto all_stages = InverseCompilerPipeline::AllStageDescriptors();
    std::string out;
    absl::StrAppendFormat(
        &out,
        "\n========================================================================================"
        "==================\n"
        "                       14-STAGE INVERSE COMPILER PIPELINE REPORT (%zu CASES)\n"
        "========================================================================================"
        "==================\n"
        "Stage | Inverse Stage Name & Target                          | Topology Match       | "
        "Exact Insn/RTL Match\n"
        "------+------------------------------------------------------+----------------------+-----"
        "----------------\n",
        total);

    auto format_stage_row = [&](int stage_idx, std::string_view name, std::string_view transition,
                                size_t topo_num, size_t topo_den, size_t exact_num,
                                size_t exact_den) {
      std::string label = absl::StrFormat("%-30s (%s)", name, transition);
      absl::StrAppendFormat(&out,
                            "  %2d  | %-52s | %3zu / %3zu (%6.2f%%)  | %3zu / %3zu (%6.2f%%)\n",
                            stage_idx, label, topo_num, topo_den, pct(topo_num, topo_den),
                            exact_num, exact_den, pct(exact_num, exact_den));
    };

    format_stage_row(0, all_stages[0].stage_name, ".o -> .dbr", direct_rtl_stage0_topo_cnt_match,
                     direct_rtl_stage0_present, direct_rtl_stage0_exact_match,
                     direct_rtl_stage0_present);
    for (size_t i = 0; i < 12; ++i) {
      const auto& p = all_stages[i + 1];
      std::string trans =
          absl::StrFormat("%s -> %s", GccRtlDumpInspector::PassExtension(*p.source_gcc_pass),
                          GccRtlDumpInspector::PassExtension(*p.target_gcc_pass));
      format_stage_row(p.inverse_stage_index, p.stage_name, trans, direct_rtl_topo_cnt_match[i],
                       direct_rtl_present_count[i], direct_rtl_exact_match[i],
                       direct_rtl_present_count[i]);
    }
    format_stage_row(13, all_stages[13].stage_name, ".rtl -> C AST", stage13_topo_match,
                     stage13_topo_present, stage13_rtl_exact, total);

    absl::StrAppendFormat(
        &out,
        "------+------------------------------------------------------+----------------------+-----"
        "----------------\n"
        " ALL  | %-52s | %3zu / %3zu (%6.2f%%)  | %3zu / %3zu (%6.2f%%)\n"
        "------+------------------------------------------------------+----------------------+-----"
        "----------------\n"
        "END-TO-END RECOMPILATION OF LIFTED C (Stage 13 C AST -> GCC Forward Compile):\n"
        "  Lifted C -> Exact Match Across All 13 GCC RTL Dumps : %3zu / %3zu (%6.2f%%)\n"
        "  Lifted C -> Structurally Isomorphic Final .o Binary : %3zu / %3zu (%6.2f%%)\n"
        "  Lifted C -> Bit-Exact Final .o Binary               : %3zu / %3zu (%6.2f%%)\n"
        "========================================================================================"
        "==================\n",
        "End-to-End Across All 14 Stages (0..13)", all_14_stages_topo_match, total,
        pct(all_14_stages_topo_match, total), all_14_stages_exact_match, total,
        pct(all_14_stages_exact_match, total), all_13_rtl_dumps_exact, total,
        pct(all_13_rtl_dumps_exact, total), isomorphic, total, pct(isomorphic, total), bit_exact,
        total, pct(bit_exact, total));
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
    absl::flat_hash_set<std::string> active_test_names;
    if (const auto* unit_test = ::testing::UnitTest::GetInstance(); unit_test != nullptr) {
      for (int s = 0; s < unit_test->total_test_suite_count(); ++s) {
        const auto* suite = unit_test->GetTestSuite(s);
        if (suite == nullptr) continue;
        for (int t = 0; t < suite->total_test_count(); ++t) {
          const auto* info = suite->GetTestInfo(t);
          if (info != nullptr && info->should_run()) {
            std::string_view full_name = info->name();
            const size_t slash = full_name.rfind('/');
            if (slash != std::string_view::npos) {
              active_test_names.emplace(full_name.substr(slash + 1));
            } else {
              active_test_names.emplace(full_name);
            }
          }
        }
      }
    }
    std::vector<size_t> active_indices;
    active_indices.reserve(cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
      if (active_test_names.empty() || active_test_names.contains(cases[i].test_name)) {
        active_indices.push_back(i);
      }
    }

    std::vector<CaseInversionResult> precomputed(active_indices.size());
    std::atomic<size_t> next_idx{0};
    unsigned int hw_threads = std::thread::hardware_concurrency();
    const size_t num_workers =
        std::max<size_t>(1, std::min<size_t>(hw_threads == 0 ? 8 : hw_threads, 32));

    std::vector<std::thread> workers;
    workers.reserve(num_workers);
    for (size_t w = 0; w < num_workers; ++w) {
      workers.emplace_back([&]() {
        while (true) {
          size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
          if (idx >= active_indices.size()) {
            break;
          }
          precomputed[idx] = EvaluateTestCase(cases[active_indices[idx]]);
        }
      });
    }
    for (auto& t : workers) {
      t.join();
    }

    precomputed_results = new absl::flat_hash_map<std::string, CaseInversionResult>();
    precomputed_results->reserve(precomputed.size());
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
    std::vector<std::string> no_dbr_flags;
    for (const auto& flag : test_case.options.c_flags) {
      if (!absl::StartsWith(flag, "-Wa,")) {
        no_dbr_flags.push_back(flag);
      }
    }
    no_dbr_flags.push_back("-fno-delayed-branch");
    no_dbr_flags.push_back("-Wa,-g");
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
    loaded_dbr_or->compiler_flags = ParsedCompilerFlags::Parse(test_case.options.c_flags);

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

    // 3. Execute the production 14-stage `InverseCompilerPipeline` with per-stage snapshot hooks.
    DecompilationContext dbr_ctx(*loaded_dbr_or);
    ControlFlowGraph stage1_unscheduled_cfg;
    InverseCompilerPipeline pipeline = InverseCompilerPipeline::BuildDefaultPipeline();
    absl::Status pipeline_status = pipeline.Run(&dbr_ctx, [&](const InverseStageDescriptor& stage,
                                                              const DecompilationContext& ctx) {
      if (stage.id == InverseStageId::kStage00FinalAndGas) {
        stage1_unscheduled_cfg = ctx.Cfg();
        CfgAnalysisManager stage1_analyses(&stage1_unscheduled_cfg);
        CfgDeReorg::UnscheduleDelaySlotsOnly(&stage1_unscheduled_cfg, &stage1_analyses);
      }
      if (stage.target_gcc_pass.has_value() && ctx.HasRtlCfg()) {
        const RtlCfgPassComparison cmp = GccRtlDumpInspector::CompareRtlCfgToDump(
            ctx.RtlCfg(),
            orig_dump.GetFunctionPass(test_case.function_name, *stage.target_gcc_pass));
        if (stage.inverse_stage_index == 0) {
          res.direct_rtl_stage0_comparison = cmp;
        } else if (stage.inverse_stage_index >= 1 && stage.inverse_stage_index <= 12) {
          res.direct_rtl_stage_comparisons[stage.inverse_stage_index - 1] = cmp;
        }
      }
      if (stage.id == InverseStageId::kStage01DbrDelaySlot) {
        if (no_dbr_status.ok()) {
          auto no_dbr_elf_or = ElfReader::LoadFromFile(orig_no_dbr_o);
          if (no_dbr_elf_or.ok()) {
            auto no_dbr_fn_or = no_dbr_elf_or->ExtractFunction(test_case.function_name);
            if (no_dbr_fn_or.ok()) {
              std::vector<uint32_t> no_dbr_words =
                  RoundTripRunner::ApplyCandidateRelocations(prepared, *no_dbr_fn_or);
              std::function<std::optional<uint32_t>(uint32_t)> no_dbr_rodata_reader =
                  prepared.rodata_memory_reader;
              const ElfSection* rodata_sec = no_dbr_elf_or->FindSection(".rodata");
              if (rodata_sec == nullptr) {
                rodata_sec = no_dbr_elf_or->FindSection(".rdata");
              }
              uint32_t rodata_vram = 0;
              if (auto it = prepared.sym_to_vram.find(".rodata");
                  it != prepared.sym_to_vram.end()) {
                rodata_vram = it->second;
              } else if (auto it_r = prepared.sym_to_vram.find(".rdata");
                         it_r != prepared.sym_to_vram.end()) {
                rodata_vram = it_r->second;
              }
              if (rodata_sec != nullptr && rodata_vram != 0) {
                std::vector<uint8_t> rodata_bytes = rodata_sec->data;
                const uint32_t text_offset = no_dbr_fn_or->section_offset;
                const uint32_t text_size =
                    static_cast<uint32_t>(no_dbr_fn_or->raw_words.size() * 4);
                const uint32_t base_vram = test_case.options.base_vram;
                no_dbr_rodata_reader = [rodata_vram, rodata_bytes = std::move(rodata_bytes),
                                        text_offset, text_size,
                                        base_vram](uint32_t vram) -> std::optional<uint32_t> {
                  if (vram >= rodata_vram && vram + 4 <= rodata_vram + rodata_bytes.size()) {
                    const uint32_t offset = vram - rodata_vram;
                    const uint32_t word = (static_cast<uint32_t>(rodata_bytes[offset]) << 24) |
                                          (static_cast<uint32_t>(rodata_bytes[offset + 1]) << 16) |
                                          (static_cast<uint32_t>(rodata_bytes[offset + 2]) << 8) |
                                          static_cast<uint32_t>(rodata_bytes[offset + 3]);
                    if (word >= 0x80000000) {
                      return word;
                    }
                    if (word >= text_offset && word < text_offset + text_size && (word & 3) == 0) {
                      return base_vram + (word - text_offset);
                    }
                    return word;
                  }
                  return std::nullopt;
                };
              }
              auto loaded_no_dbr_or = FunctionLoader::FromWords(
                  test_case.function_name, no_dbr_words, test_case.options.base_vram,
                  prepared.symbol_index.get(), /*game_config=*/nullptr, no_dbr_rodata_reader);
              if (loaded_no_dbr_or.ok()) {
                DecompilationContext no_dbr_ctx(*loaded_no_dbr_or);
                if (no_dbr_ctx.BuildCfg().ok()) {
                  res.direct_cfg_stage1 =
                      EvaluateDirectCfgStage1(stage1_unscheduled_cfg, no_dbr_ctx.Cfg());
                }
              }
            }
          }
        }
        res.direct_cfg_stage2.cfg_before_tail_dup_blocks = ctx.RtlCfg().BlockCount();
        res.direct_cfg_stage2.cfg_before_tail_dup_active_insns = ctx.RtlCfg().TotalInsnCount();
      } else if (stage.id == InverseStageId::kStage02Jump2CrossJump) {
        res.direct_cfg_stage2.cfg_after_tail_dup_blocks = ctx.RtlCfg().BlockCount();
        res.direct_cfg_stage2.cfg_after_tail_dup_active_insns = ctx.RtlCfg().TotalInsnCount();
        const RtlFunctionDump* orig_sched2 =
            orig_dump.GetFunctionPass(test_case.function_name, RtlPassKind::kSched2);
        const RtlFunctionDump* orig_jump2 =
            orig_dump.GetFunctionPass(test_case.function_name, RtlPassKind::kJump2);
        if (orig_sched2 != nullptr && orig_jump2 != nullptr) {
          const RtlControlFlowGraph exp_sched2 =
              GccRtlDumpInspector::BuildCanonicalCfgFromRtlDump(*orig_sched2);
          const RtlControlFlowGraph exp_jump2 =
              GccRtlDumpInspector::BuildCanonicalCfgFromRtlDump(*orig_jump2);
          res.direct_cfg_stage2.sched2_active_insns = exp_sched2.TotalInsnCount();
          res.direct_cfg_stage2.jump2_active_insns = exp_jump2.TotalInsnCount();
          res.direct_cfg_stage2.gcc_jump2_modified_rtl = DidGccStageTransformRtl(
              orig_dump, test_case.function_name, RtlPassKind::kSched2, RtlPassKind::kJump2);
          const ptrdiff_t gcc_merged_insns =
              static_cast<ptrdiff_t>(res.direct_cfg_stage2.sched2_active_insns) -
              static_cast<ptrdiff_t>(res.direct_cfg_stage2.jump2_active_insns);
          const ptrdiff_t cfg_restored_insns =
              static_cast<ptrdiff_t>(res.direct_cfg_stage2.cfg_after_tail_dup_active_insns) -
              static_cast<ptrdiff_t>(res.direct_cfg_stage2.cfg_before_tail_dup_active_insns);
          res.direct_cfg_stage2.active_insn_delta_matches =
              (gcc_merged_insns == cfg_restored_insns);
        } else {
          res.direct_cfg_stage2.active_insn_delta_matches =
              (res.direct_cfg_stage2.cfg_after_tail_dup_active_insns ==
               res.direct_cfg_stage2.cfg_before_tail_dup_active_insns);
        }
      }
    });

    if (!pipeline_status.ok() || !dbr_ctx.HasAst()) {
      res.error_or_diag = std::string(pipeline_status.message());
      cleanup();
      return res;
    }

    CTranslationUnit tu;
    tu.includes = {"types.h"};
    tu.symbol_index = prepared.symbol_index.get();
    tu.functions.push_back(std::move(*dbr_ctx.TakeAst()));

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
        ExactPassMatches(orig_dump, cand_dump, test_case.function_name, RtlPassKind::kDbr);
    const RtlFunctionDump* orig_rtl_fn =
        orig_dump.GetFunctionPass(test_case.function_name, RtlPassKind::kRtl);
    const RtlFunctionDump* cand_rtl_fn =
        cand_dump.GetFunctionPass(test_case.function_name, RtlPassKind::kRtl);
    if (cand_rtl_fn != nullptr) {
      const RtlControlFlowGraph cand_rtl_cfg =
          GccRtlDumpInspector::BuildCanonicalCfgFromRtlDump(*cand_rtl_fn);
      res.stage13_rtl_topo_comparison =
          GccRtlDumpInspector::CompareRtlCfgToDump(cand_rtl_cfg, orig_rtl_fn);
    }
    res.stage13_rtl_matches =
        ExactPassMatches(orig_dump, cand_dump, test_case.function_name, RtlPassKind::kRtl);
    res.first_divergent_pass = std::nullopt;
    for (RtlPassKind pass : GccRtlDumpInspector::AllPasses()) {
      if (!ExactPassMatches(orig_dump, cand_dump, test_case.function_name, pass)) {
        res.first_divergent_pass = pass;
        break;
      }
    }

    const auto rtl_stages = InverseCompilerPipeline::RtlStageDescriptors();
    for (size_t i = 0; i < 12; ++i) {
      const auto& p = rtl_stages[i + 1];
      res.phase_right_inverse_matches[i] =
          ExactPassMatches(orig_dump, cand_dump, test_case.function_name, *p.source_gcc_pass);
      res.phase_left_inverse_matches[i] =
          ExactPassMatches(orig_dump, cand_dump, test_case.function_name, *p.target_gcc_pass);
      res.gcc_stage_transformed_rtl[i] = DidGccStageTransformRtl(
          orig_dump, test_case.function_name, *p.target_gcc_pass, *p.source_gcc_pass);
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

TEST_P(GccStageInversionTest, SatisfiesRightInverseAcrossAllFourteenStages) {
  const RoundTripTestCase& test_case = GetParam();
  CaseInversionResult res =
      (precomputed_results != nullptr && precomputed_results->contains(test_case.test_name))
          ? (*precomputed_results)[test_case.test_name]
          : EvaluateTestCase(test_case);
  GccStageInversionSuiteAccumulator::Instance().Record(res);

  ASSERT_TRUE(res.decompiled_and_compiled_ok) << res.error_or_diag;
  EXPECT_TRUE(res.direct_cfg_stage1.MatchesExactly())
      << "Direct CFG Stage 1 (`CfgDeReorg`) failed exact match for " << test_case.test_name
      << ": remaining_branch_likely=" << res.direct_cfg_stage1.remaining_branch_likely
      << ", remaining_non_nop_delay_slots=" << res.direct_cfg_stage1.remaining_non_nop_delay_slots
      << ", detail=" << res.direct_cfg_stage1.mismatch_detail;
  for (size_t s : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{4}, size_t{5}, size_t{6},
                   size_t{7}, size_t{8}, size_t{9}, size_t{10}, size_t{11}, size_t{12}}) {
    const RtlCfgPassComparison& rtl_cmp =
        (s == 0) ? res.direct_rtl_stage0_comparison : res.direct_rtl_stage_comparisons[s - 1];
    if (rtl_cmp.pass_present) {
      EXPECT_TRUE(rtl_cmp.MatchesTopologyAndInsnCounts())
          << "Direct RtlCfg -> RTL Stage " << s << " failed topology + instruction count match for "
          << test_case.test_name << ": " << rtl_cmp.mismatch_detail;
      EXPECT_TRUE(rtl_cmp.MatchesExactly())
          << "Direct RtlCfg -> RTL Stage " << s << " failed exact RTL pattern match for "
          << test_case.test_name << ": " << rtl_cmp.mismatch_detail;
    }
  }
  const auto& descriptors = InverseCompilerPipeline::AllStageDescriptors();
  EXPECT_TRUE(res.bit_exact) << "Exact Right-Inverse Stage 0 (" << descriptors[0].stage_name
                             << " / " << descriptors[0].gcc_stage_description
                             << ") output .o binary f_0(g_0(y_0)) == y_0 mismatch for "
                             << test_case.test_name;
  for (size_t i = 0; i < 12; ++i) {
    const auto& desc = descriptors[i + 1];
    EXPECT_TRUE(res.phase_right_inverse_matches[i])
        << "Exact Right-Inverse Stage " << desc.inverse_stage_index << " (" << desc.stage_name
        << " / " << desc.gcc_stage_description << ") output RTL f_k(g_k(y_k)) == y_k mismatch for "
        << test_case.test_name;
  }
  EXPECT_TRUE(res.stage13_rtl_matches)
      << "Exact Right-Inverse Stage 13 (" << descriptors[13].stage_name << " / "
      << descriptors[13].gcc_stage_description
      << ") output .00.rtl f_13(g_13(y_13)) == y_13 mismatch for " << test_case.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripFourteenStageSuite, GccStageInversionTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
