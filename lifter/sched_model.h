#ifndef ROM_NOM_NOM_LIFTER_SCHED_MODEL_H_
#define ROM_NOM_NOM_LIFTER_SCHED_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter::sched_model {

// Selects between GCC 2.7.2's pre-allocation (`reload_completed == 0`,
// `.06.combine` -> `.07.sched`) and post-reload (`reload_completed == 1`,
// `.09.greg` -> `.10.sched2`) invocations of `sched.c:schedule_insns`.
enum class Phase { kSched1, kSched2 };

// MIPS processor selection matching `config/mips/mips.h` (`enum processor_type`)
// and `config/mips/mips.c:3160-3277`.
enum class MipsCpu { kR3000, kR4000, kR4600, kR4650, kR6000, kR8000, kDefault };

// MIPS `define_attr "type"` classification (`config/mips/mips.md:68-70`).
enum class InsnType {
  kUnknown,
  kBranch,
  kJump,
  kCall,
  kLoad,
  kStore,
  kMove,
  kXfer,
  kHilo,
  kArith,
  kDarith,
  kImul,
  kIdiv,
  kIcmp,
  kFadd,
  kFmul,
  kFmadd,
  kFdiv,
  kFabs,
  kFneg,
  kFcmp,
  kFcvt,
  kFsqrt,
  kMulti,
  kNop,
};

// Functional-unit descriptor for an instruction (`config/mips/mips.md:153-307`,
// `genattrtab.c:1962-2043`, `insn-attrtab.c:1249-2055, 6297-6302`).
struct UnitUse {
  int unit = -1;         // 0=memory, 1=imuldiv, 2=adder, 3=mult, 4=divide, -1=none
  int ready = 1;         // `result_ready_cost`
  int issue = 0;         // `issue_delay` (0 except `imuldiv`)
  int simultaneity = 0;  // 0 (`memory`, `imuldiv`) or 1 (`adder`, `mult`, `divide`)
  int min_blockage = 1;  // `MIN_BLOCKAGE_COST(blockage_range(unit, C=this))`
  int max_blockage = 1;  // `MAX_BLOCKAGE_COST(blockage_range(unit, C=this))`
};

// Faithful static model of GCC 2.7.2's MIPS `insn-attrtab.c` and `mips.h`
// scheduling parameters.
class MachineModel {
 public:
  // Resolves `mips_isa`, `mips_cpu`, `TARGET_64BIT`, and `TARGET_FLOAT64`
  // from `profile` (`config/mips/elf64.h:26`, `config/mips/mips.c:3160-3277`).
  static MachineModel FromProfile(const CompilationProfile& profile);

  MipsCpu Cpu() const { return cpu_; }
  int MipsIsa() const { return mips_isa_; }
  bool Target64Bit() const { return target_64bit_; }
  bool TargetFloat64() const { return target_float64_; }

  // Mirrors `recog_memoized(insn) >= 0` (`sched.c:1111, 1375`): returns false
  // for `USE`, `CLOBBER`, `ASM_INPUT`, `ASM_OPERANDS`, and non-insn patterns.
  bool IsRecognized(const RtlPatternAndNotes& insn) const;

  // Classifies `insn` into `mips.md`'s `"type"` attribute accounting for
  // `constrain_operands(..., reload_completed)` (`insn-attrtab.c:5550`).
  InsnType TypeOf(const RtlPatternAndNotes& insn, Phase phase) const;

  // Classifies `insn` into `mips.md`'s `"mode"` attribute (primarily `SF`/`DF`
  // for FP `mult`/`divide` unit delays).
  RtlMachineMode AttrModeOf(const RtlPatternAndNotes& insn) const;

  // Returns functional unit, latencies, and `blockage_range` for `(type, mode)`.
  UnitUse UnitOf(InsnType type, RtlMachineMode mode) const;

  // Mirrors `result_ready_cost(insn)` (`sched.c:1382`, floored at 1).
  int ResultReadyCost(const RtlPatternAndNotes& insn, Phase phase) const;

  // Mirrors `<unit>_unit_blockage(executing, candidate)` (`genattrtab.c:1986-1996`).
  int Blockage(const UnitUse& executing, const UnitUse& candidate) const;

  // Mirrors `function_units[unit].max_blockage` (`insn-attrtab.c:6298-6302`).
  int MaxBlockage(int unit) const;

  // Mirrors `CALL_USED_REGISTERS` (`config/mips/mips.h:1203-1210`).
  bool CallUsed(int hard_regno) const;

  // Mirrors `HARD_REGNO_NREGS(hard_regno, mode)` (`config/mips/mips.h:1255-1258`).
  int HardRegNRegs(int hard_regno, RtlMachineMode mode) const;

 private:
  MipsCpu cpu_ = MipsCpu::kR4000;
  int mips_isa_ = 3;
  bool target_64bit_ = false;
  bool target_float64_ = false;
};

// Per-function register facts required by `sched.c`, derived purely and
// deterministically from the input `RtlControlFlowGraph` (zero B4 branching).
struct RegFacts {
  absl::btree_map<int, int> reg_n_sets;
  absl::btree_map<int, int> reg_n_calls_crossed;
  absl::btree_map<int, int> reg_live_length;
  absl::btree_map<int, RtlExpr> reg_known_value;  // `init_alias_analysis` (`sched.c:398-439`)
  absl::btree_set<int> reg_known_equiv_p;         // `REG_EQUIV` vs `REG_EQUAL`
  absl::btree_set<int> reg_block_global;          // `reg_basic_block[r] == REG_BLOCK_GLOBAL`
  bool frame_pointer_needed = false;
  int max_reg_num = kMipsFirstPseudoRegister;

  static RegFacts FromCfg(const RtlControlFlowGraph& cfg, const CompilationProfile& profile,
                          Phase phase);
};

// Block-level instruction descriptor consumed by `Analyze`, `Schedule`, and `UpdateLife`.
struct Insn {
  enum class Kind { kInsn, kJump, kCall };

  RtlPatternAndNotes pn;
  int luid = 0;
  Kind kind = Kind::kInsn;
  bool is_const_call = false;
  bool setjmp_follows = false;                      // `NOTE_INSN_SETJMP` immediately after call
  std::vector<RtlLoopNoteKind> leading_loop_notes;  // Mid-block `LOOP_BEG`/`LOOP_END` barriers
};

// Exact `enum reg_note` ordering (`rtl.h:347-351`): `kTrue < kAnti < kOutput`.
enum class DepKind : uint8_t { kTrue = 0, kAnti = 14, kOutput = 15 };

struct Link {
  int target = -1;  // Block index of the earlier insn depended upon.
  DepKind kind = DepKind::kTrue;
};

struct DepGraph {
  std::vector<std::vector<Link>> log_links;  // Exact GCC prepend order (`sched.c:948-953`).
  std::vector<bool> sched_group_p;           // `SCHED_GROUP_P` (`sched.c:2125-2157`).
};

// Alias analysis predicates exposed for fine-grained unit testing
// (`sched.c:370-882`, `rtlanal.c:76-183`).
RtlExpr CanonRtx(const RtlExpr& x, const RegFacts& facts);
bool RtxEqualForMemrefP(const RtlExpr& x, const RtlExpr& y, const RegFacts& facts);
bool RtxVariesP(const RtlExpr& x, const RegFacts& facts, Phase phase);
bool RtxAddrVariesP(const RtlExpr& x, const RegFacts& facts, Phase phase);
bool MemrefsConflictP(int xsize, const RtlExpr& x, int ysize, const RtlExpr& y, int64_t c,
                      const RegFacts& facts, Phase phase);
bool ReadDependence(const RtlExpr& r1_mem, const RtlExpr& r2_mem);
bool TrueDependence(const RtlExpr& write_mem, const RtlExpr& read_mem, const RegFacts& facts,
                    Phase phase);
bool AntiDependence(const RtlExpr& read_mem, const RtlExpr& write_mem, const RegFacts& facts,
                    Phase phase);
bool OutputDependence(const RtlExpr& w1_mem, const RtlExpr& w2_mem, const RegFacts& facts,
                      Phase phase);

// Builds exact `LOG_LINKS` and `SCHED_GROUP_P` flags for `block` (`sched.c:1658-2273`).
DepGraph Analyze(absl::Span<const Insn> block, const RegFacts& facts, const MachineModel& mm,
                 Phase phase);

struct StepTrace {
  int clock = 0;
  int chosen = -1;
  std::vector<int> ready_before_select;
  std::vector<int> priorities;
};

struct ScheduleResult {
  std::vector<int> order;  // Forward execution order (indices into `block`).
  std::vector<StepTrace> trace;
};

// Executes GCC 2.7.2 `schedule_block` list scheduling (`sched.c:2373-2704, 3144-3940`)
// on `block` given its dependence graph `graph`.
ScheduleResult Schedule(absl::Span<const Insn> block, const DepGraph& graph, const RegFacts& facts,
                        const MachineModel& mm, Phase phase,
                        const absl::btree_set<int>& live_at_end);

struct LifeResult {
  std::vector<std::vector<RtlRegNote>> notes;  // Exact `attach_deaths` prepend order.
  absl::btree_map<int, int> sched_live_length;
  absl::btree_map<int, int> sched_calls_crossed;
};

// Reconstructs post-`sched1` `REG_DEAD` notes (`attach_deaths_insn`, `sched.c:2779-3018`)
// and per-block `sched_reg_live_length` / `sched_reg_n_calls_crossed` (`sched.c:3764-3942`).
LifeResult UpdateLife(absl::Span<const Insn> scheduled, const absl::btree_set<int>& live_at_start,
                      absl::Span<const Insn> block0_pinned_prefix, const RegFacts& facts,
                      const MachineModel& mm, Phase phase);

// Executes full forward `sched.c:schedule_insns` (`try_split` + `update_flow_info`
// when `kSched1`, mid-block no-op self-move deletion when `kSched2`, per-block
// `Analyze` + `Schedule` + `UpdateLife`, and `sched.c:4911-4966` sideband write-back).
RtlControlFlowGraph RunScheduleInsns(const RtlControlFlowGraph& in,
                                     const CompilationProfile& profile, Phase phase);

// -----------------------------------------------------------------------------
// Search-Based Block Inversion & Stage 3 Preimage-Selection Support
// -----------------------------------------------------------------------------

// LUID ordering constraint: `must_have_greater_luid` must be placed after `than`
// in the pre-schedule block so `rank_for_schedule` (`sched.c:2428`) prefers it.
struct Constraint {
  int must_have_greater_luid = -1;
  int than = -1;
};

// Optional soft-pair adjacency preference (e.g., split `large_int` `lui; ori` in
// Stage 6): `second` should immediately follow `first` (`LUID(second) == LUID(first) + 1`)
// whenever compatible with exact forward verification `Schedule(Q) == P`.
struct AdjacentPair {
  int first = -1;
  int second = -1;
};

// Finds a pre-schedule permutation `Q` (indices into `scheduled_block`) such that
// assigning `LUID(Q[pos]) = pos` and running `Analyze(Q)` + `Schedule(Q)` reproduces
// `scheduled_block` (`0..n-1`) bit-for-bit.
// When `preferred_order` is non-empty, tests/biases topological ties toward
// `preferred_order` (never short-circuiting to identity `P` before checking
// `preferred_order` or `preferred_adjacent`).
std::optional<std::vector<int>> InvertBlockOrder(
    absl::Span<const Insn> scheduled_block, const RegFacts& facts, const MachineModel& mm,
    Phase phase, const absl::btree_set<int>& live_at_end,
    absl::Span<const int> preferred_order = {},
    absl::Span<const AdjacentPair> preferred_adjacent = {});

// True iff there exists a valid pre-schedule permutation `Q` whose forward
// `Analyze(Q)` + `Schedule(Q)` reproduces `block` (`0..n-1`) bit-for-bit.
bool IsAchievable(absl::Span<const Insn> block, const RegFacts& facts, const MachineModel& mm,
                  Phase phase, const absl::btree_set<int>& live_at_end);

// Parsed schedulable region of an `RtlBasicBlock`, separating leading/trailing
// notes (and block-0 pre-alloc parameter copies under `Phase::kSched1`) from
// schedulable instructions and mid-block `LOOP_BEG`/`LOOP_END` barriers
// (`flow.c:468-472`, `sched.c:3191-3215`).
struct ParsedSchedRegion {
  std::vector<size_t> prefix_orig_indices;
  std::vector<RtlLoopNoteKind> region_note_list;
  std::vector<Insn> sched_block;
  std::vector<size_t> sched_orig_indices;
  std::vector<size_t> suffix_orig_indices;
  bool has_mid_block_loop_barrier = false;
  bool valid = true;
};

ParsedSchedRegion ParseBlockForSchedModel(const RtlBasicBlock& blk, size_t block_idx, Phase phase);

// Narrow core `sched.c` primitives shared with companion libraries
// (`sched_post_reload_projection` and `sched_stage0_attribution`).
namespace detail {

// MIPS hard-register numbers (`config/mips/mips.h:1186-1210`).
inline constexpr int kMipsArgPointerRegno = 0;
inline constexpr int kMipsAtRegno = 1;
inline constexpr int kMipsRetRegFirst = 2;
inline constexpr int kMipsRetRegLast = 3;
inline constexpr int kMipsArgRegFirst = 4;
inline constexpr int kMipsArgRegLast = 7;
inline constexpr int kMipsTemp1Regno = 12;
inline constexpr int kMipsCalleeSavedGprFirst = 16;
inline constexpr int kMipsCalleeSavedGprLast = 23;
inline constexpr int kMipsKernelRegFirst = 26;
inline constexpr int kMipsStackPointerRegno = 29;
inline constexpr int kMipsFramePointerRegno = 30;
inline constexpr int kMipsReturnAddrRegno = 31;
inline constexpr int kMipsFpRegFirst = 32;
inline constexpr int kMipsFpRetRegLast = 33;
inline constexpr int kMipsFpArgRegFirst = 44;
inline constexpr int kMipsFpArgRegLast = 47;
inline constexpr int kMipsCalleeSavedFprFirst = 52;
inline constexpr int kMipsFpRegLast = 63;
inline constexpr int kMipsMdRegFirst = 64;
inline constexpr int kMipsMdRegLast = 66;

// Mirrors `GET_MODE_SIZE` (`machmode.def`) for MIPS scheduling modes.
int ModeByteSize(RtlMachineMode mode);

// Mirrors `rtlanal.c:single_set` on an instruction pattern.
const RtlExpr* SingleSetOf(const RtlExpr& pat);

// Classifies an instruction pattern as `kCall`, `kJump` (including `kReturn`), or `kInsn`.
Insn::Kind ClassifyInsnKind(const RtlExpr& pat);

// Mirrors `sched.c:3197-3214` (block-0 leading hard-reg-source copy pinning).
bool IsBlock0PinnedParamCopy(const Insn& insn);

// Computes the semantic dependency graph without GCC 2.7.2's position-dependent
// `pending_lists_length > 32` cap (`sched.c:1740-1748`).
DepGraph AnalyzeWithoutPendingListFlushCap(absl::Span<const Insn> block, const RegFacts& facts,
                                           const MachineModel& mm, Phase phase);

// Result of Phase-2 forced reverse replay (`sched.c:2373-2378, 2616-2704`).
struct ForcedReplayResult {
  std::vector<Constraint> constraints;
  int last_tail = -1;
  bool had_conflict = false;
};

ForcedReplayResult RunForcedReplay(absl::Span<const Insn> block, const DepGraph& graph,
                                   const RegFacts& facts, const MachineModel& mm, Phase phase,
                                   const absl::btree_set<int>& live_at_end,
                                   bool continue_on_conflict = false);

// Checks whether a directed graph on `0..n-1` with edge set `edges` and optional
// contracted chains (`next_in_chain`) is acyclic, and if so runs preference-biased
// Kahn's topological sort keyed by `pref_rank[chain_head]`.
std::optional<std::vector<int>> SolveSupernodeKahn(
    int n, const absl::btree_set<std::pair<int, int>>& edges, const std::vector<int>& next_in_chain,
    const std::vector<int>& prev_in_chain, const std::vector<int>& pref_rank);

// Identifies `sched.c:1704-1707` (`call_used_regs[i]` word-offset bug hitting
// `call_used_regs[0] == 1`) post-call anti-dependence artifact edges.
bool IsPureSched1704CallArtifactEdge(absl::Span<const Insn> block, const DepGraph& graph,
                                     int call_idx, int insn_idx, const MachineModel& mm);

// Mirrors `optabs.c:2507-2523` (`emit_no_conflict_block`) + `sched.c:4803-4818`:
// expands unpaired `REG_LIBCALL` `DImode` blocks with their deleted `REG_RETVAL`
// self-move before `kSched1` scheduling.
bool TryExpandUnpairedNoConflictRetvalForSched1(absl::Span<const Insn> block, const RegFacts& facts,
                                                std::vector<Insn>* expanded_out,
                                                RegFacts* expanded_facts_out,
                                                std::vector<int>* expanded_to_orig_out = nullptr);

}  // namespace detail

}  // namespace rom_nom_nom::lifter::sched_model

#endif  // ROM_NOM_NOM_LIFTER_SCHED_MODEL_H_
