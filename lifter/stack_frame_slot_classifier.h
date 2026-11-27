#ifndef ROM_NOM_NOM_LIFTER_STACK_FRAME_SLOT_CLASSIFIER_H_
#define ROM_NOM_NOM_LIFTER_STACK_FRAME_SLOT_CLASSIFIER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// GCC 2.7.2 MIPS `BIGGEST_ALIGNMENT` in bytes (`config/mips/mips.h:1080`,
// `BIGGEST_ALIGNMENT = 64` bits = 8 bytes). Both `reload1.c:2352, 2384`
// (`alter_reg` calling `assign_stack_local(..., -1)`, `function.c:682-686`) and
// `reload1.c:663` (`assign_stack_local(BLKmode, 0, 0)` before
// `setup_save_areas`, `function.c:679-680`) align spill-slot buckets to 8 bytes.
inline constexpr int64_t kMipsSpillSlotBucketBytes = 8;

// Byte interval `[offset, offset + byte_size)` for a stack-frame slot relative
// to the active frame elimination register (`$sp` = 29 or `$fp` = 30).
struct StackSlotBound {
  int64_t offset = 0;
  int byte_size = 0;

  bool operator==(const StackSlotBound& other) const {
    return offset == other.offset && byte_size == other.byte_size;
  }
  bool operator!=(const StackSlotBound& other) const { return !(*this == other); }
};

// High-level stack-frame region classification within a MIPS O32/N32 frame.
enum class StackFrameSlotKind {
  kUnknown,
  kOutgoingArg,
  kCallerSave,
  kSpillOrLocal,
};

// Returns the byte width of `mode` under GCC 2.7.2 MIPS (`machmode.def`).
int StackSlotModeByteSize(RtlMachineMode mode);

// Mirrors GCC 2.7.2 `HARD_REGNO_NREGS(regno, mode)`
// (`config/mips/mips.h:1255-1258`).
int StackSlotHardRegNregs(int regno, RtlMachineMode mode, bool gp64, bool fp64);

// Returns true for MIPS hard registers eligible for `caller-save.c`
// (`call_used_regs[hr] && !call_fixed_regs[hr]`, `config/mips/mips.h:1186-1210`,
// `caller-save.c:456`): `$2..$15`, `$24..$25`, and `$f0..$f19` (`32..51`).
bool IsMipsCallerSavableHardReg(int hr);

// Extracts the constant byte offset `off` from `base_reg` or
// `(plus base_reg (const_int off))` (`base_reg` must not be `argp`).
std::optional<int64_t> ExtractStackBaseOffset(const RtlExpr& addr, int base_regno);

// Rounds `offset` down to the nearest `kMipsSpillSlotBucketBytes` (8-byte)
// boundary relative to `frame_base` (`reload1.c:663`, `function.c:679-699`).
int64_t AlignDownToSpillBucket(int64_t offset, int64_t frame_base);

// Returns true if any `MEM` or address-taken expression inside `root` anchored
// at `base_regno` overlaps `[interval_lo, interval_hi)`.
bool ExprOverlapsStackInterval(const RtlExpr& root, int base_regno, int64_t interval_lo,
                               int64_t interval_hi);

// Verifies GCC 2.7.2 `caller-save.c:282-331` (`setup_save_areas`) monotonic
// allocation invariant: scanning hard registers in ascending order
// `0 .. FIRST_PSEUDO_REGISTER - 1` produces non-overlapping ascending stack
// offsets (`hr_a < hr_b => off_a + sz_a <= off_b`).
bool VerifyMonotonicCallerSaveLayout(const absl::btree_map<int, StackSlotBound>& hr_bounds);

// Classifies a stack offset relative to `$sp`/`$fp` given `outgoing_args_size`,
// `frame_size_bytes`, and any proven `caller_save_bounds`.
StackFrameSlotKind ClassifyStackFrameOffset(
    int64_t offset, int outgoing_args_size, int frame_size_bytes,
    const absl::btree_map<int, StackSlotBound>& caller_save_bounds);

// Configuration for `ClassifyCallerSavePairsAcrossCfg`.
struct CallerSaveMatchOptions {
  bool frame_pointer_needed = false;
  int frame_size_bytes = 0;
  int outgoing_args_size = 0;
  bool gp64 = false;
  bool fp64 = false;
  bool opt_caller_saves_enabled = true;
};

struct MatchedCallerSaveRestore {
  size_t restore_idx = 0;
  int restore_span = 0;
};

struct MatchedCallerSaveGroup {
  size_t save_idx = 0;
  int save_span = 0;
  std::vector<MatchedCallerSaveRestore> restores;
  std::vector<std::pair<int, StackSlotBound>> word_bounds;
};

struct CallerSaveCfgClassification {
  // Per-block matched caller-save groups in deterministic ascending `save_idx` order.
  std::vector<std::vector<MatchedCallerSaveGroup>> block_matches;
  // Deterministic hard-reg -> stack-slot interval map (`caller-save.c:282-331`).
  absl::btree_map<int, StackSlotBound> hr_slot_bounds;
  // Per-block, per-instruction save/restore hard-reg word span (`0` if not caller-save).
  std::vector<std::vector<int>> save_span;
  std::vector<std::vector<int>> restore_span;
  // 8-byte aligned pre-caller-save frame offset (`reload1.c:663`), or `0` if none matched.
  int64_t aligned_caller_save_base = 0;
};

// Deterministically identifies intra-block `caller-save` save/restore pairs
// (`caller-save.c:261-340` `setup_save_areas`, `caller-save.c:349-509`
// `save_call_clobbered_regs`, `caller-save.c:517-542` `set_reg_live`,
// `caller-save.c:646-770` `insert_save_restore`, `reload1.c:663`) across
// post-`alter_reg` hard-register `parsed_blocks`.
CallerSaveCfgClassification ClassifyCallerSavePairsAcrossCfg(
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const CallerSaveMatchOptions& options);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_STACK_FRAME_SLOT_CLASSIFIER_H_
