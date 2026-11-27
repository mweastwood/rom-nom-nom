#ifndef ROM_NOM_NOM_LIFTER_MIPS_REG_ALLOC_COMMON_H_
#define ROM_NOM_NOM_LIFTER_MIPS_REG_ALLOC_COMMON_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/functional/function_ref.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {

// MIPS hard-register constants (`config/mips/mips.h:1179, 1217-1253`).
// GCC 2.7.2 MIPS does NOT define `REG_ALLOC_ORDER` (`config/mips/mips.h` omits
// it), so both `local-alloc.c:2158-2182` (`find_free_reg`) and
// `global.c:962-990, 1107-1118` (`find_reg`) scan hard registers in natural
// order `0 .. kFirstPseudoRegister - 1`.
inline constexpr int kArgPointerRegnum = 0;
inline constexpr int kFramePointerRegnum = 30;
inline constexpr int kFirstFprRegnum = 32;
inline constexpr int kLastFprRegnum = 63;
inline constexpr int kHiRegnum = 64;
inline constexpr int kLoRegnum = 65;
inline constexpr int kHiloRegnum = 66;
inline constexpr int kFpswRegnum = 67;
inline constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
inline constexpr int kMipsNaturalAllocOrderCount = kFirstPseudoRegister;

struct MipsTargetConfig {
  bool gp_64bit = false;
  bool float_64bit = false;
  bool soft_float = false;
  bool single_float = false;
  bool omit_frame_pointer = true;
  bool caller_saves = true;
  bool mem_move_cost_6 = false;
  bool runs_sched1 = false;
};

ParsedCompilerFlags ParseProfileFlags(const CompilationProfile& profile);

// Extracts GCC 2.7.2 MIPS target configuration mirroring
// `config/mips/mips.c:3146-3270` (`override_options`),
// `config/mips/mips.h:2928-2929` (`MEMORY_MOVE_COST`), and
// `toplev.c:3028-3034` / `sched.c:4911-4966` (`flag_schedule_insns`).
MipsTargetConfig ExtractMipsTargetConfig(const ParsedCompilerFlags& flags);

// MIPS register classes matching `enum reg_class` / `REG_CLASS_CONTENTS`
// (`config/mips/mips.h:1421-1432`).
enum class MipsRegClass : int {
  kNoRegs = 0,
  kGrRegs = 1,
  kFpRegs = 2,
  kHiReg = 3,
  kLoReg = 4,
  kHiloReg = 5,
  kMdRegs = 6,
  kStRegs = 7,
  kAllRegs = 8,
};

// Fixed-width bitset of the 68 MIPS hard registers (`0 .. 67`), matching
// GCC's `HARD_REG_SET` (`hard-reg-set.h`, `config/mips/mips.h:1179`).
struct HardRegSet {
  uint64_t lo = 0;
  uint64_t hi = 0;

  void Clear() {
    lo = 0;
    hi = 0;
  }
  void SetBit(int r) {
    if (r >= 0 && r < 64) {
      lo |= (uint64_t{1} << r);
    } else if (r >= 64 && r < kFirstPseudoRegister) {
      hi |= (uint64_t{1} << (r - 64));
    }
  }
  void ClearBit(int r) {
    if (r >= 0 && r < 64) {
      lo &= ~(uint64_t{1} << r);
    } else if (r >= 64 && r < kFirstPseudoRegister) {
      hi &= ~(uint64_t{1} << (r - 64));
    }
  }
  bool TestBit(int r) const {
    if (r >= 0 && r < 64) {
      return ((lo >> r) & 1) != 0;
    }
    if (r >= 64 && r < kFirstPseudoRegister) {
      return ((hi >> (r - 64)) & 1) != 0;
    }
    return false;
  }
  void Ior(const HardRegSet& other) {
    lo |= other.lo;
    hi |= other.hi;
  }
  void AndCompl(const HardRegSet& other) {
    lo &= ~other.lo;
    hi &= ~other.hi;
  }
  void IorCompl(const HardRegSet& other) {
    lo |= (~other.lo);
    hi |= ((~other.hi) & 0xfULL);
  }
  bool IsEmpty() const { return lo == 0 && (hi & 0xfULL) == 0; }

  bool operator==(const HardRegSet& other) const {
    return lo == other.lo && (hi & 0xfULL) == (other.hi & 0xfULL);
  }
  bool operator!=(const HardRegSet& other) const { return !(*this == other); }
};

HardRegSet RegClassContents(MipsRegClass cls);
MipsRegClass RegNoRegClass(int regno);
bool RegClassSubsetP(MipsRegClass c1, MipsRegClass c2);
bool RegClassRelatedP(MipsRegClass c1, MipsRegClass c2);

struct RegModeInfo {
  RtlMachineMode mode = RtlMachineMode::kSi;
  std::string raw_mode;
};

int ModeByteSize(const RegModeInfo& info);
bool IsFpCcMode(const RegModeInfo& info);
bool IsFloatOrComplexFloatMode(const RegModeInfo& info);
bool IsIntMode(const RegModeInfo& info);

// Mirrors `HARD_REGNO_NREGS` (`config/mips/mips.h:1255-1258`).
int HardRegnoNregs(int regno, const RegModeInfo& info, const MipsTargetConfig& cfg);
// Word count occupied by a pseudo register (`PSEUDO_REGNO_BYTES / UNITS_PER_WORD`).
int PseudoRegnoSize(const RegModeInfo& info, const MipsTargetConfig& cfg);
// Mirrors `HARD_REGNO_MODE_OK` (`config/mips/mips.h:1273-1274`,
// `config/mips/mips.c:3400-3465`).
bool HardRegnoModeOk(int regno, const RegModeInfo& info, const MipsTargetConfig& cfg);

// Mirrors `FIXED_REGISTERS` (`config/mips/mips.h:1186-1193`),
// `CALL_USED_REGISTERS` (`config/mips/mips.h:1203-1210`), and
// `ELIMINABLE_REGS` (`config/mips/mips.h:1734-1737`).
HardRegSet FixedRegSet(const MipsTargetConfig& cfg);
HardRegSet CallUsedRegSet(const MipsTargetConfig& cfg);
HardRegSet EliminableRegSet();

// Mirrors GCC `floor_log2` (`toplev.c`) and `global.c:589-610`
// (`allocno_compare` priority formula).
int FloorLog2(int x);
int ComputeAllocnoPriority(int n_refs, int live_length, int size);

// Single table-driven predicate returning true iff `parallel_expr` matches a
// GCC 2.7.2 `config/mips/mips.md` `define_insn` `PARALLEL` pattern whose
// `(clobber ...)` operands originate from `(match_scratch ...)`:
//   - `mul*` / `madd*` (`config/mips/mips.md:848-1146`)
//   - `div*` / `mod*` (`config/mips/mips.md:1285-1482`)
//   - `ffssi2` / `ffsdi2` (`config/mips/mips.md:1596-1636`)
//   - `fix_trunc*` (`config/mips/mips.md:2485-2574`)
//   - `movstrsi_internal*` (`config/mips/mips.md:3587-3668`)
// Returns false for non-scratch `PARALLEL` patterns whose clobbers use
// `(match_operand ... "register_operand")` (such as `adddi3`, `subdi3`,
// `negdi2`, `ashldi3`/`ashrdi3`/`lshrdi3`, `loadgp`, `get_fnaddr`, `call*`,
// `asm_operands`, `unspec_volatile`, and `casesi_internal` at
// `config/mips/mips.md:6001-6018` — whose `define_expand "casesi"` at
// `config/mips/mips.md:5946-5986` references `(match_scratch:SI 6 "")` at
// line 5963 but expands via `gen_casesi_internal(..., gen_reg_rtx(SImode))`
// to `match_operand:SI 2 "register_operand"` at line 6006).
bool IsMipsMatchScratchParallel(const RtlExpr& parallel_expr);

struct PseudoLivenessSummary {
  absl::btree_set<size_t> blocks_referenced;
  bool has_set_in_block = false;
  bool used_before_set = false;
  bool alive_after_redef = false;
  bool defined_by_di_mult = false;
  bool used_in_mem_address = false;
  MipsRegClass single_reg_md_class = MipsRegClass::kNoRegs;
  int def_count_in_block = 0;
  int total_def_count = 0;
  int total_read_count = 0;
  size_t def_insn_idx = 0;
  size_t death_insn_idx = 0;
  int note_deaths = 0;
};

struct MipsCfgRegAllocSummary {
  absl::btree_set<int> live_at_any_block_start;
  absl::btree_map<int, PseudoLivenessSummary> pseudo_summaries;
  absl::btree_map<int, RegModeInfo> pseudo_modes;
  absl::btree_set<int> fp_op_pseudos;
  absl::btree_set<int> gr_or_mem_op_pseudos;
  HardRegSet scratch_hard_regs;
  absl::btree_set<int> all_di_mult_pseudos;
  int num_hard_non_hi_di_mults = 0;
  std::vector<std::vector<bool>> block_insn_clobbers_md;
};

// Scans `out` (sideband `basic_block_live_at_start`) and `liveness_cfg`
// (instruction patterns + death notes) deterministically to populate per-pseudo
// summaries, modes, register-class hints, and MD clobber metadata shared by
// `local_alloc_model`, `global_alloc_model`, and Stage 4/5 lifters.
MipsCfgRegAllocSummary AnalyzeCfgForMipsRegAlloc(const RtlControlFlowGraph& out,
                                                 const RtlControlFlowGraph& liveness_cfg,
                                                 const MipsTargetConfig& mips_cfg);

void CollectUsedPseudoRegsInExpr(const RtlExpr& expr, bool in_mem_addr,
                                 absl::btree_set<int>* used_pseudos,
                                 absl::btree_map<int, int>* read_counts,
                                 absl::btree_set<int>* mem_addr_pseudos);

void CollectInsnElementUsesAndDefs(const RtlExpr& elem, absl::btree_set<int>* used_pseudos,
                                   absl::btree_set<int>* defined_pseudos,
                                   absl::btree_map<int, int>* read_counts,
                                   absl::btree_set<int>* mem_addr_pseudos);

// Identifies destinations of single-register MD output constraints (`"=h"`,
// `"=l"`, `"=a"`) where `CLASS_LIKELY_SPILLED_P(CLASS)` (`reg_class_size == 1`)
// holds (`local-alloc.c:76-77, 472-474`).
void CollectSingleRegMdOutputClasses(const RtlExpr& pat, const MipsTargetConfig& mips_cfg,
                                     absl::btree_map<int, MipsRegClass>* single_md_dests);

void CollectDiMultDestRegs(const RtlExpr& pat, absl::btree_set<int>* mult_di_dests,
                           bool* has_hard_non_hi_dest = nullptr);

bool ExprContainsOp(const RtlExpr& root, RtlOp target_op);

// Records MD hard registers (`64 hi`, `65 lo`, `66 hilo`) needed by `reload`
// in `basic_block_needs` for hard non-MD `SET_DEST`s or unallocated
// `(clobber (scratch:...))` operands on `mips.md` multiply/divide `PARALLEL`s.
void CollectHardAndScratchMdNeeds(const RtlExpr& pat, const MipsTargetConfig& mips_cfg,
                                  HardRegSet* needs);

// Mirrors `local-alloc.c:1381` (`note_stores` -> `reg_is_set`) and
// `local-alloc.c:2103-2106` (`find_free_reg`: `qty_n_calls_crossed[qty] > 0`
// copies `call_used_reg_set`, which includes `hi`/`lo` per
// `config/mips/mips.h:1203-1210`): `local_alloc` only marks `hi`/`lo`
// (`64`/`65`) live on explicit references to hard regs `64`/`65` or `CALL_INSN`,
// NOT on another pseudo-dest widening `mult:DI` whose sole clobber is
// `(clobber (reg:SI 66 accum))` (`config/mips/mips.md:963-1024`).
bool InsnHardClobbersHiLoForLocalAlloc(const RtlExpr& pat);

bool InsnTouchesMdRegsOrCall(const RtlExpr& pat);
bool HasSideEffects(const RtlExpr& expr);
bool HasRegUnusedNote(const std::vector<RtlRegNote>& notes, const RtlExpr& dest);
bool HasRegUnusedNoteForRegno(const std::vector<RtlRegNote>& notes, int regno);

// Mirrors `rtlanal.c:single_set`.
const RtlExpr* SingleSet(const RtlPatternAndNotes& parsed);
const RtlExpr* UnwrapNoteStoresDest(const RtlExpr& orig_dest);
// Mirrors `rtlanal.c:note_stores` (including reverse iteration over `PARALLEL`
// elements).
void NoteStores(const RtlExpr& pat,
                absl::FunctionRef<void(const RtlExpr& dest, const RtlExpr& setter)> fn);

bool FirstFormatCharIsExpr(RtlOp op);
bool ExprRefersToRegRange(int regno, int endregno, const RtlExpr& expr,
                          const MipsTargetConfig& mips_cfg);
// Mirrors `rtlanal.c:reg_overlap_mentioned_p`.
bool RegOverlapMentionedP(const RtlExpr& x, const RtlExpr& in, const MipsTargetConfig& mips_cfg);

}  // namespace rom_nom_nom::lifter

#endif  // ROM_NOM_NOM_LIFTER_MIPS_REG_ALLOC_COMMON_H_
