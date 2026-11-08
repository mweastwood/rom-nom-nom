#ifndef CORE_RTL_CFG_H_
#define CORE_RTL_CFG_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom {

// Frame parameters recovered by `InvertFunctionPrologueEpilogue` for
// `mips.c:function_prologue` and `mips.c:function_epilogue`.
struct PrologueEpilogueFrameSpec {
  int var_size = 0;
  uint32_t saved_gpr_mask = 0;

  bool operator==(const PrologueEpilogueFrameSpec& other) const {
    return var_size == other.var_size && saved_gpr_mask == other.saved_gpr_mask;
  }
  bool operator!=(const PrologueEpilogueFrameSpec& other) const { return !(*this == other); }
};

// Identifies each RTL dump pass emitted by KMC GCC 2.7.2 (`toplev.c:rest_of_compilation`)
// when invoked with `-da`, ordered chronologically along the forward compilation pipeline.
enum class RtlPassKind {
  kRtl,      // .rtl     - Initial RTL generation (stmt.c / expr.c)
  kJump,     // .jump    - First jump optimization pass (jump.c)
  kCse,      // .cse     - First common subexpression elimination pass (cse.c)
  kLoop,     // .loop    - Loop optimization pass (loop.c)
  kCse2,     // .cse2    - Second CSE pass (cse.c)
  kFlow,     // .flow    - Dataflow and life analysis pass (flow.c)
  kCombine,  // .combine - Instruction combination pass (combine.c)
  kSched,    // .sched   - Pre-allocation instruction scheduling pass (sched.c)
  kLreg,     // .lreg    - Local register allocation pass (local-alloc.c)
  kGreg,     // .greg    - Global register allocation & reload pass (global.c / reload1.c)
  kSched2,   // .sched2  - Post-reload instruction scheduling pass (sched.c)
  kJump2,    // .jump2   - Post-reload jump optimization pass (jump.c)
  kDbr,      // .dbr     - Delayed-branch scheduling pass (reorg.c)
};

// Returns the ordered list of all RTL passes in GCC 2.7.2's compilation pipeline.
absl::Span<const RtlPassKind> AllRtlPasses();

// Returns the file extension (including leading dot, e.g. `".rtl"`, `".dbr"`) for `pass`.
std::string_view RtlPassExtension(RtlPassKind pass);

// Identifies which GCC 2.7.2 `mips.md` pattern or `gas` macro expansion produced an `RtlInsn`.
enum class RtlFusedIdiomKind {
  kSingleInstruction,
  kMulSi3,
  kFloatSiSf2,
  kFloatSiDf2,
  kFixTruncSfSi2,
  kFixTruncDfSi2,
  kMove2Words,
  kMovstrsiBlockMove,
  kGasFloatImmediate,
  kGasMagicMultiplier,
  kGasAddressHiLo,
  kGasSymbolIndexedAccess,
  kGasSymbolDirectAccess,
  kDivisionWithTrap,
  kSplitLargeIntHiLo,
};

// Represents a single atomic RTL instruction in the RTL Control Flow Graph.
// Preserves both the canonical GCC 2.7.2 RTL pattern and the underlying machine instruction span
// (including symbol relocations, register operands, and immediate values).
struct RtlInsn {
  std::string canonical_pattern;
  std::string rtl_sexpr_pattern;
  RtlFusedIdiomKind idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  uint32_t vram = 0;
  bool is_prologue_insn = false;
  bool is_synthetic_rtl_expansion = false;
  std::vector<Instruction> machine_instructions;

  // Parses the core instruction pattern (stripping any trailing `REG_NOTES`)
  // into a structured `RtlExpr`.
  absl::StatusOr<RtlExpr> ParseExpr() const;

  // Parses both the core instruction pattern and any trailing `REG_NOTES`.
  absl::StatusOr<RtlPatternAndNotes> ParseExprAndNotes() const;

  // Updates `canonical_pattern` from a structured `RtlExpr` and optional
  // `REG_NOTES` list.
  void SetFromExpr(const RtlExpr& expr, absl::Span<const RtlRegNote> notes = {});
  void SetFromExprAndNotes(const RtlPatternAndNotes& expr_and_notes);
};

// Represents a basic block in Reverse-Post-Order (RPO) in the `RtlControlFlowGraph`.
struct RtlBasicBlock {
  uint32_t id = 0;
  uint32_t start_vram = 0;
  uint32_t end_vram = 0;
  std::vector<RtlInsn> rtl_insns;
  std::vector<std::string> instructions;
  std::vector<size_t> successors;
  std::vector<size_t> predecessors;
  std::vector<size_t> addr_vec_targets;

  void AddInsn(RtlInsn insn) {
    instructions.push_back(insn.canonical_pattern);
    rtl_insns.push_back(std::move(insn));
  }

  void AddPatternOnly(std::string pattern) {
    RtlInsn insn;
    insn.canonical_pattern = pattern;
    insn.is_synthetic_rtl_expansion = true;
    rtl_insns.push_back(std::move(insn));
    instructions.push_back(std::move(pattern));
  }

  // Synchronizes `rtl_insns` with `instructions` while preserving existing `machine_instructions`
  // payloads and `RtlInsn` metadata.
  void SyncRtlInsnsFromInstructions();
};

// Result of comparing an actual `RtlControlFlowGraph` against an expected GCC 2.7.2 RTL pass dump.
struct RtlCfgPassComparison {
  bool pass_present = false;
  bool block_count_matches = false;
  bool successors_match = false;
  bool insn_counts_match = false;
  bool instructions_match = false;
  size_t actual_block_count = 0;
  size_t expected_block_count = 0;
  size_t actual_insn_count = 0;
  size_t expected_insn_count = 0;
  std::string mismatch_detail;

  // Returns true if basic block count, RPO successor edges, and per-block instruction counts match.
  bool MatchesTopologyAndInsnCounts() const {
    return pass_present && block_count_matches && successors_match && insn_counts_match;
  }

  // Returns true if topology, instruction counts, and every instruction pattern string match
  // verbatim.
  bool MatchesExactly() const {
    return pass_present && block_count_matches && successors_match && insn_counts_match &&
           instructions_match;
  }
};

// Per-loop sideband metadata reconstructed by InverseScanLoop.
struct RtlLoopInfo {
  int header_label_id = -1;
  int latch_block_id = -1;
  int preheader_block_id = -1;
  std::vector<int> body_block_ids;
  std::vector<int> hoisted_invariant_regnos;

  bool operator==(const RtlLoopInfo& other) const {
    return header_label_id == other.header_label_id && latch_block_id == other.latch_block_id &&
           preheader_block_id == other.preheader_block_id &&
           body_block_ids == other.body_block_ids &&
           hoisted_invariant_regnos == other.hoisted_invariant_regnos;
  }
  bool operator!=(const RtlLoopInfo& other) const { return !(*this == other); }
};

// Spill slot descriptor recovered by InverseReload.
struct RtlSpillSlotInfo {
  int frame_offset = 0;
  int byte_size = 4;
  int recovered_pseudo_regno = -1;
  RtlMachineMode mode = RtlMachineMode::kSI;

  bool operator==(const RtlSpillSlotInfo& other) const {
    return frame_offset == other.frame_offset && byte_size == other.byte_size &&
           recovered_pseudo_regno == other.recovered_pseudo_regno && mode == other.mode;
  }
  bool operator!=(const RtlSpillSlotInfo& other) const { return !(*this == other); }
};

// Explicit representation of the GCC 2.7.2 global side-band arrays that
// accompany an RTL instruction stream between optimization stages.
struct RtlSidebandState {
  // Next available pseudo-register number (`max_regno` in GCC).
  int max_regno = 67;

  // Pseudo-register -> hard-register assignment map (`reg_renumber` in GCC).
  // Unallocated pseudos map to -1 or are absent.
  absl::flat_hash_map<int, int> reg_renumber;

  // Per-register flow analysis statistics (`reg_n_refs`, `reg_n_sets`,
  // `reg_n_deaths`, `reg_live_length`, `reg_n_calls_crossed` in GCC).
  absl::flat_hash_map<int, int> reg_n_refs;
  absl::flat_hash_map<int, int> reg_n_sets;
  absl::flat_hash_map<int, int> reg_n_deaths;
  absl::flat_hash_map<int, int> reg_live_length;
  absl::flat_hash_map<int, int> reg_n_calls_crossed;

  // Per-basic-block live-in register sets (`basic_block_live_at_start`).
  // Keyed by basic block index (`0 .. rpo_blocks.size() - 1`).
  absl::flat_hash_map<int, absl::flat_hash_set<int>> basic_block_live_at_start;

  // Stack frame and prologue/epilogue metadata (`frame_offset`, leaf status,
  // saved general/FP registers, epilogue delay list).
  int frame_size_bytes = 0;
  int outgoing_args_size = 0;
  bool is_leaf_function = false;
  std::vector<int> saved_gpr_regnos;
  std::vector<int> saved_fpr_regnos;
  std::optional<std::string> epilogue_delay_pattern;

  // Spill slots recovered by InverseReload.
  std::vector<RtlSpillSlotInfo> spill_slots;

  // Natural loop metadata recovered by InverseScanLoop.
  std::vector<RtlLoopInfo> loops;

  bool operator==(const RtlSidebandState& other) const {
    return max_regno == other.max_regno && reg_renumber == other.reg_renumber &&
           reg_n_refs == other.reg_n_refs && reg_n_sets == other.reg_n_sets &&
           reg_n_deaths == other.reg_n_deaths && reg_live_length == other.reg_live_length &&
           reg_n_calls_crossed == other.reg_n_calls_crossed &&
           basic_block_live_at_start == other.basic_block_live_at_start &&
           frame_size_bytes == other.frame_size_bytes &&
           outgoing_args_size == other.outgoing_args_size &&
           is_leaf_function == other.is_leaf_function &&
           saved_gpr_regnos == other.saved_gpr_regnos &&
           saved_fpr_regnos == other.saved_fpr_regnos &&
           epilogue_delay_pattern == other.epilogue_delay_pattern &&
           spill_slots == other.spill_slots && loops == other.loops;
  }
  bool operator!=(const RtlSidebandState& other) const { return !(*this == other); }
};

// First-class Reverse-Post-Order (RPO) RTL Control Flow Graph representation shared across `core/`,
// `lifter/`, and `gcc_harness/`.
struct RtlControlFlowGraph {
  RtlPassKind pass = RtlPassKind::kDbr;
  std::vector<RtlBasicBlock> rpo_blocks;
  RtlSidebandState sideband;

  // Semantic recovery metadata populated across Track B (`Stages 0..12`) and consumed by Stage 13
  // (`InverseRtlToCAstStage`).
  bool has_explicit_trailing_void_return = false;
  size_t local_alloc_pseudo_reg_count = 0;
  size_t global_alloc_pseudo_reg_count = 0;

  size_t BlockCount() const { return rpo_blocks.size(); }

  size_t TotalInsnCount() const {
    size_t total = 0;
    for (const auto& blk : rpo_blocks) {
      total += blk.instructions.size();
    }
    return total;
  }

  // Returns the total number of preserved `Instruction` objects across all `RtlInsn`s in
  // `rpo_blocks`.
  size_t TotalMachineInsnCount() const {
    size_t total = 0;
    for (const auto& blk : rpo_blocks) {
      for (const auto& rinsn : blk.rtl_insns) {
        total += rinsn.machine_instructions.size();
      }
    }
    return total;
  }

  // Synchronizes `rtl_insns` with `instructions` across all `rpo_blocks` and preserves all real
  // `machine_instructions` from `prev_blocks` even when blocks were split or reconstructed.
  void SyncAllBlocksPreservingMachineInsns(const std::vector<RtlBasicBlock>& prev_blocks = {});

  // Compares `this` against `expected` for RPO block count, successor edges, and instruction count.
  RtlCfgPassComparison CompareAgainst(const RtlControlFlowGraph& expected) const;

  // Formats a human-readable multi-line summary of all RPO basic blocks, successor edges, and
  // instruction patterns (`"B0 (-> [1]): pat1 ; pat2\n..."`).
  std::string FormatSummary() const;
};

using RtlCfgBlock = RtlBasicBlock;
using RtlCanonicalCfg = RtlControlFlowGraph;

// Collapses whitespace in a raw RTL pattern string while preserving all tokens verbatim.
std::string CanonicalizeRtlPattern(std::string_view pattern);

// Recomputes Reverse Post-Order (RPO) from entry block 0, prunes unreachable blocks, and updates
// `id`, `successors`, and `predecessors` across `cfg->rpo_blocks`.
void RecomputeRtlCfgRpo(RtlControlFlowGraph* cfg);

}  // namespace rom_nom_nom

#endif  // CORE_RTL_CFG_H_
