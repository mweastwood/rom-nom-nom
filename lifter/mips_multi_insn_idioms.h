#ifndef LIFTER_MIPS_MULTI_INSN_IDIOMS_H_
#define LIFTER_MIPS_MULTI_INSN_IDIOMS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// 128-bit hard-register bitset covering MIPS GPRs (`$0..$31`), FPRs (`$f0..$f31` =
// `32..63`), and HI/LO/HILO/FCC (`64..67`).
struct MipsHardRegSet {
  uint64_t lo = 0;
  uint64_t hi = 0;

  bool Test(int r) const {
    if (r < 0 || r >= 128) {
      return false;
    }
    return r < 64 ? ((lo & (uint64_t{1} << r)) != 0) : ((hi & (uint64_t{1} << (r - 64))) != 0);
  }
  void Set(int r) {
    if (r >= 0 && r < 64) {
      lo |= (uint64_t{1} << r);
    } else if (r >= 64 && r < 128) {
      hi |= (uint64_t{1} << (r - 64));
    }
  }
  void Clear(int r) {
    if (r >= 0 && r < 64) {
      lo &= ~(uint64_t{1} << r);
    } else if (r >= 64 && r < 128) {
      hi &= ~(uint64_t{1} << (r - 64));
    }
  }
  void Or(const MipsHardRegSet& o) {
    lo |= o.lo;
    hi |= o.hi;
  }
  void AndNot(const MipsHardRegSet& o) {
    lo &= ~o.lo;
    hi &= ~o.hi;
  }
};

bool IsSavedGprOrRaRegno(int r);
bool IsValidGp32DiPair(int reg_hi);
bool IsPlainSiGpr(const RtlExpr& expr, int* out_reg = nullptr);
int DetectStackFrameSize(const RtlControlFlowGraph& cfg);
int ModeAccessByteSize(RtlMachineMode mode);

bool ExtractBasePlusConstAddr(const RtlExpr& addr, int* out_base, int64_t* out_off);
bool ExtractMovstrsiAddrsAndBytes(const RtlExpr& pat, int* out_dst_reg, int64_t* out_dst_off,
                                  int* out_src_reg, int64_t* out_src_off, int* out_bytes);

void CollectHardRegDefsAndUses(const RtlExpr& pat, bool gp64, bool fp64, MipsHardRegSet* defs,
                               MipsHardRegSet* uses);

void CollectCompareDiamondDiPairs(const std::vector<std::vector<RtlPatternAndNotes>>& blocks,
                                  absl::flat_hash_set<int>* out_di_even_regs);
void CollectExistingDiEvenRegs(const std::vector<std::vector<RtlPatternAndNotes>>& blocks,
                               absl::flat_hash_set<int>* out_di_even_regs, bool* out_has_any_di);

// Recombines adjacent `movstrsi_internal2` (`split_kind = 1`) +
// `movstrsi_internal3` (`split_kind = 2`) pairs back into unsplit
// `movstrsi_internal` (`split_kind = 0`, `mips.md:3609`).
bool TryRecombineAdjacentSplitMovstrsiInBlock(std::vector<RtlPatternAndNotes>* insns,
                                              bool mark_newly_folded_sentinel);

// Folds `mips.md` `!TARGET_64BIT` multi-instruction `DImode` arithmetic, shift,
// and widening multiply templates (`mulsidi3_internal`, `umulsidi3_internal`,
// `adddi3_internal_1`, `adddi3_internal_2`, `subdi3_internal`,
// `ashldi3_internal2/3`, `ashrdi3_internal2/3`, `lshrdi3_internal2/3`).
bool TryFoldGp32DiMultiInsnIdiomsInBlock(const RtlControlFlowGraph& cfg, size_t b,
                                         bool mark_newly_folded_sentinel,
                                         std::vector<RtlPatternAndNotes>* insns);

struct MipsWitnessedDiFoldOptions {
  // True in Stage 1 (`reorg.c:try_split` after delay-slot unpacking) where the
  // two halves of a `movdi` register/HILO move may be separated by non-conflicting
  // instructions in the same block; false in Stage 0 (`final_scan_insn`) where
  // `mips_move_2words` emits contiguous instruction pairs.
  bool allow_noncontiguous_reg_move = false;
  bool mark_newly_folded_sentinel = false;
};

// Folds `mips.md` `!TARGET_64BIT` witnessed `DImode` bitwise, memory, register/HILO
// move, 64-bit constant load, and `$2` `DImode` return `(use ...)` patterns.
bool TryFoldGp32DiWitnessedIdioms(const RtlControlFlowGraph& cfg, int frame_size,
                                  const absl::flat_hash_set<int>& compare_di_pairs,
                                  const absl::flat_hash_set<int>& existing_di_regs,
                                  const MipsWitnessedDiFoldOptions& options,
                                  std::vector<std::vector<RtlPatternAndNotes>>* blocks);

struct MipsMovstrsiFoldOptions {
  bool gp64 = false;
  bool fp64 = false;
  bool has_di_witness = false;
  bool skip_unfilled_dbr_jump_tail = false;
  bool allow_delay_slot_split_movstrsi = false;
  bool mark_newly_folded_sentinel = false;
};

// Folds `mips_expand_block_move` / `output_block_move` (`mips.md:3573,3609`)
// load-then-store bursts into `movstrsi_internal` (and, when enabled,
// `movstrsi_internal2` + `movstrsi_internal3` split across a trailing delay slot).
bool TryFoldMovstrsiBlockMovesInBlock(
    const RtlControlFlowGraph& cfg, size_t b, const MipsMovstrsiFoldOptions& options,
    std::vector<RtlPatternAndNotes>* insns,
    std::optional<RtlPatternAndNotes>* epilogue_delay_pn = nullptr);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_MIPS_MULTI_INSN_IDIOMS_H_
