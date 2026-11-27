#ifndef ROM_NOM_NOM_LIFTER_MIPS_IDIOM_UTILS_H_
#define ROM_NOM_NOM_LIFTER_MIPS_IDIOM_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// 128-bit hard-register bitset covering MIPS GPRs (`$0..$31`), FPRs
// (`$f0..$f31` = `32..63`), and HI/LO/HILO/FCC (`64..67`,
// `config/mips/mips.h:1179`).
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
  bool operator==(const MipsHardRegSet& o) const { return lo == o.lo && hi == o.hi; }
  bool operator!=(const MipsHardRegSet& o) const { return !(*this == o); }
};

// Shared low-level MIPS hard-register, mode-size, address, and block-liveness
// helpers (`config/mips/mips.h:1186-1210, 1255-1258`).
bool IsSavedGprOrRaRegno(int r);
bool IsValidGp32DiPair(int reg_hi);
bool IsPlainSiGpr(const RtlExpr& expr, int* out_reg = nullptr);
int DetectStackFrameSize(const RtlControlFlowGraph& cfg);
int ModeAccessByteSize(RtlMachineMode mode);
int HardRegModeSpan(int regno, RtlMachineMode mode, bool gp64, bool fp64);

bool ExtractBasePlusConstAddr(const RtlExpr& addr, int* out_base, int64_t* out_off);
bool ExtractMovstrsiAddrsAndBytes(const RtlExpr& pat, int* out_dst_reg, int64_t* out_dst_off,
                                  int* out_src_reg, int64_t* out_src_off, int* out_bytes);
struct ParsedMovstrsiInsn {
  int dst_base = -1;
  int64_t dst_off = 0;
  int src_base = -1;
  int64_t src_off = 0;
  int clobbers[4] = {-1, -1, -1, -1};
  int64_t bytes = 0;
  int64_t align = 0;
  int64_t kind = -1;
};

bool ParseMovstrsiInsn(const RtlExpr& pat, ParsedMovstrsiInsn* out);
bool MatchMovstrStore(const RtlExpr& pat, int* out_reg, int* out_base, int64_t* out_off,
                      int* out_width);
bool IsMovstrsiSplitPart(const RtlExpr& pat, int64_t expected_split_kind);

void CollectHardRegReads(const RtlExpr& expr, bool gp64, bool fp64, MipsHardRegSet* uses);
void CollectHardRegDefsAndUses(const RtlExpr& pat, bool gp64, bool fp64, MipsHardRegSet* defs,
                               MipsHardRegSet* uses);
void ApplyCallClobbersAndUses(const RtlPatternAndNotes& call_pn, bool gp64, bool fp64,
                              MipsHardRegSet* defs, MipsHardRegSet* uses,
                              bool include_call_arg_uses = true);

// Computes deterministic per-block live-in hard-register sets across
// `cfg.rpo_blocks` via backward fixed-point dataflow (`flow.c:1907, 2337`).
std::vector<MipsHardRegSet> ComputeHardRegLiveIn(const RtlControlFlowGraph& cfg, bool gp64,
                                                 bool fp64);

// Coalesces adjacent `movstrsi_internal` (`split_kind = 0`) batches emitted by
// `output_block_move` (`config/mips/mips.c:2217, 2361-2378`,
// `bytes_a == MAX_MOVE_BYTES`, `bytes_a + bytes_b <= 2 * MAX_MOVE_BYTES`) back
// into a single instruction (`config/mips/mips.md:3587-3601`).
bool TryCoalesceMultiBatchMovstrsiInBlock(std::vector<RtlPatternAndNotes>* insns, bool gp64,
                                          bool mark_newly_folded_sentinel);

// Recombines `movstrsi_internal2` (`split_kind = 1`, `config/mips/mips.md:3646`)
// + `movstrsi_internal3` (`split_kind = 2`, `mips.md:3662`, or unpacked
// delay-slot `swr`/`sb`) pairs back into unsplit `movstrsi_internal`
// (`split_kind = 0`, `mips.md:3587-3644`), and coalesces adjacent multi-batch
// `movstrsi_internal` chunks (`mips.c:2361`).
bool TryRecombineAdjacentSplitMovstrsiInBlock(std::vector<RtlPatternAndNotes>* insns,
                                              bool mark_newly_folded_sentinel, bool gp64 = false);

// Optional Stage-0 callback for checking whether a `DImode` pair `dh` is
// consumed after recombining a trailing `(sequence ...)` delay slot or
// `epilogue_delay_pattern` (`config/mips/mips.md` multi-insn idioms).
using TailDelaySlotDiConsumerFn = absl::FunctionRef<bool(
    const RtlControlFlowGraph& cfg, size_t b, absl::Span<const RtlPatternAndNotes> tail_insns,
    int dh, bool gp64, bool fp64)>;

// Restores `!TARGET_64BIT` `emit_no_conflict_block` (`optabs.c:2429-2526`)
// standalone `(clobber (reg:DI dh)) (insn_list:REG_LIBCALL (nil))` markers,
// `alter_subreg`-collapsed `(subreg:SI (reg:DI dh) 0/1)` destinations
// (`final.c:1928-1946, 2095-2107`), `REG_NO_CONFLICT` / `REG_DEAD` notes
// (`local-alloc.c:1317-1352`), and `muldi3` 3-multiply `SUBREG` operands.
// Idempotent when the `CLOBBER` marker is already present.
void RestoreNoConflictExtendClobbers(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                     bool runs_sched2 = false);

// Overload accepting a Stage-0 tail delay-slot `DImode` consumer check for
// `.12.dbr` CFGs that still contain `(sequence ...)` or `epilogue_delay_pattern`.
void RestoreNoConflictExtendClobbersWithTailCheck(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                                  bool runs_sched2,
                                                  TailDelaySlotDiConsumerFn tail_di_consumer_check);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_MIPS_IDIOM_UTILS_H_
