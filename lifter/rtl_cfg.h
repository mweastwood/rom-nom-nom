#ifndef LIFTER_RTL_CFG_H_
#define LIFTER_RTL_CFG_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/mips.h"

namespace rom_nom_nom {

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

// Represents a single atomic RTL instruction in the lifter's RTL Control Flow Graph.
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

// First-class Reverse-Post-Order (RPO) RTL Control Flow Graph representation produced by
// `InverseFinalAndGasStage` (Stage 0) and transformed by downstream inverse compiler stages.
struct RtlControlFlowGraph {
  RtlPassKind pass = RtlPassKind::kDbr;
  std::vector<RtlBasicBlock> rpo_blocks;

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
};

using RtlCfgBlock = RtlBasicBlock;
using RtlCanonicalCfg = RtlControlFlowGraph;

// Collapses whitespace and canonicalizes `(label_ref ...)` targets in a raw RTL pattern string.
std::string CanonicalizeRtlPattern(std::string_view pattern);

}  // namespace rom_nom_nom

#endif  // LIFTER_RTL_CFG_H_
