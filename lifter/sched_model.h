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

// MIPS post-reload stack-frame partition (`config/mips/mips.c:4459-4475`,
// `config/mips/mips.h:1649-1651, 1756-1764`) for pre-reload `$sp`/`$fp`/`argp`
// base recovery inside `local-alloc.c:540-665` (`validate_equiv_mem` /
// `memref_used_between_p`) and `reload1.c` spill-slot identification.
struct PostReloadStackLayout {
  bool valid = false;
  bool frame_pointer_needed = false;
  int64_t starting_frame_offset = 0;
  int64_t frame_size = 0;
  bool has_stack_slot_access_info = false;
  absl::btree_set<int64_t> stack_slot_load_offsets;
  absl::btree_set<int64_t> stack_slot_store_offsets;
  // Dedicated `caller-save.c:248-340, 575-630` save-area slot offsets
  // (`insert_save`/`insert_restore` around a `CALL_INSN`, never `reload1.c`
  // input/output reload contiguity pairs).
  absl::btree_set<int64_t> caller_save_slot_offsets;
  // Lowest stack offset in `[starting_frame_offset, frame_size)` eligible to
  // hold a `reload1.c:2352-2358` (`assign_stack_local(..., -1)`) 8-byte
  // big-endian spill slot across the CFG (above any pre-reload local array,
  // aggregate, `REG_EQUIV` stack slot, or address-escaped local slot).
  int64_t min_valid_reload_spill_offset = 0;
  // Lowest stack offset of the prologue/epilogue callee-saved GPR/FPR save area
  // (`config/mips/mips.c:compute_frame_size`, `save_restore_insns`), or `-1`
  // when unknown / no callee-saved registers are saved on the stack. Reload
  // spill slots sit strictly below this boundary.
  int64_t callee_saved_area_offset = -1;
  // Non-call-used hard registers live across a `CALL_INSN` in at least one
  // block of the CFG (`global.c:927`, `local-alloc.c:2106`: excluded from
  // `call_used_reg_set` copy-tie coalescing).
  absl::btree_set<int> cfg_call_crossing_regs;
};

// Whole-function cross-block def-count context supplied by post-reload stages so
// `ProjectPostReloadBlockToSched1View` faithfully models `flow.c` /
// `sched.c:2469-2501` (`birthing_insn_p`) `reg_n_sets[r] == 1` across blocks.
struct Sched1ProjectionContext {
  bool has_cfg_context = false;
  // True when projecting block 0 (`sched.c:3191-3215`), where `schedule_block`
  // advances `head` past leading entry-parameter copies from hard regs before
  // running `sched_analyze` and list scheduling.
  bool is_entry_block = false;
  // Per hard-reg count of defs in other basic blocks (`b' != b`) that are
  // live-out of their defining block (reaching cross-block webs).
  absl::btree_map<int, int> cross_block_reaching_defs;
  // Mode-qualified reaching-def counts `(hard_regno, mode)` for renamed pseudos
  // (`rtl.h`, `flow.c:2065`: a single pseudo has one fixed `machine_mode`, so a
  // `DF` pseudo web never merges with an `SF` def on the same hard register).
  bool has_reaching_defs_by_mode = false;
  absl::btree_map<std::pair<int, RtlMachineMode>, int> cross_block_reaching_defs_by_mode;
  // Exact CFG def-use web other-block def counts (`flow.c:reg_n_sets`,
  // `sched.c:2469-2501`), separated by whether the hard reg is live-in (entry
  // web) or live-out (exit web) of this block so disjoint webs on the same hard
  // register in unrelated blocks never inflate `reg_n_sets`.
  bool has_per_endpoint_web_defs = false;
  absl::btree_map<std::pair<int, RtlMachineMode>, int> entry_web_other_defs;
  absl::btree_map<std::pair<int, RtlMachineMode>, int> exit_web_other_defs;
  // Hard registers whose entry use and exit def in this block belong to the
  // same CFG def-use web (`reach_in[b]` shares a union-find web root with `b`'s
  // exit def, e.g., loop-carried or join-shared webs). Disjoint DAG webs on the
  // same hard register are excluded so `FindCoalescedGlobalCarrierHardRegs`
  // never falsely merges them into a multi-set pseudo (`sched.c:2469-2501`).
  absl::btree_set<int> entry_exit_same_web_regs;
  // Per hard-reg total count of defs in other basic blocks (`b' != b`) for
  // unrenamed hard regs (e.g. `/i` return-value regs) whose `flow.c`
  // `reg_n_sets[r]` counts every explicit `SET` function-wide.
  absl::btree_map<int, int> other_block_total_defs;
  // Body-relative indices (into `post_reload_block`) of block-0 interleaved
  // prologue callee-save stores / `$fp` setup emitted by `mips_expand_prologue`
  // (`toplev.c:3103`, `function.c:5516`) after `reload` and `sched1`, which
  // `sched2` interleaved past leading body instructions.
  absl::btree_set<int> excluded_prologue_body_indices;
  // When true, models `local-alloc.c:1266-1300, 1851-1886` (`combine_regs`)
  // post-call dying-input reuse of a fresh `reg_n_calls_crossed == 0` pseudo
  // (`sched.c:1732`) even when `final.c` erased `(REG_DEAD r)` on the RMW.
  bool allow_post_call_combine_regs_split = true;
  // When true (Stage 0 discovery only), splits general block-local
  // `combine_regs` RMWs on call-used hard registers (`mm.CallUsed(r)`) without
  // splitting callee-saved multi-set accumulators (`local-alloc.c:1266-1300,
  // 1851-1886, 2106`, `sched.c:2490`).
  bool allow_call_used_general_combine_regs_split = false;
  // When true (Stage 0 discovery only), also splits general block-local
  // `combine_regs` RMWs (leaf, loop, or post-call) to model `sched.c:2490`
  // (`birthing_insn_p`, `reg_n_sets == 1`) during Stage 0 `REG_DEAD` attribution.
  bool allow_general_combine_regs_split = false;
  // Optional explicit set of body-relative indices (into `post_reload_block`)
  // forced to split as `combine_regs` RMWs (for Stage 0 greedy minimization).
  absl::btree_set<int> forced_combine_regs_split_body_indices;
  // Whole-function stack-frame spill-slot metadata (`reload1.c:2309-2400,
  // 5641-6160`) for folding cross-block output-reload spill stores and
  // input-reload spill loads out of the `Phase::kSched1` projection view.
  // Hard registers read by trailing `(use ...)` return-value markers at
  // `[bounds.epilogue_begin .. n)` (`sched.c:2325-2367, 2469-2501`): because
  // `ProjectPostReloadBlockToSched1View` projects only `[prologue_end ..
  // epilogue_begin)`, these return registers must be seeded into
  // `live_at_end_s1` so `birthing_insn_p` sees the return-value set as live.
  absl::btree_set<int> epilogue_use_regs;
  PostReloadStackLayout stack_layout;
  bool has_stack_addr_escape = false;
  // True when this block has a self-loop backedge and a unique non-self
  // fallthrough/conditional-jump preheader (`reload1.c:517-524, 566-573`),
  // matching `FoldSpilledConstDoubleReloads` in `invert_reload.cc`.
  bool has_self_loop_preheader = false;
  absl::btree_map<int64_t, int> other_block_spill_store_counts;
  absl::btree_set<int64_t> other_block_spill_load_offsets;
};

// Read-only helper: projects a post-reload candidate block onto a `Phase::kSched1`
// view via intra-block SSA live-range splitting (zero mutation, zero web-bundling,
// preserving ABI/call-interface defs/uses).
struct Sched1ProjectionView {
  std::vector<Insn> core_insns;
  std::vector<int> core_to_orig_index;
  // Body-relative indices (into `post_reload_block`) of block-0 leading
  // entry-arg copies pinned ahead of `core_insns` by `sched.c:3191-3215`.
  std::vector<int> pinned_prefix_orig_indices;
  // Body-relative indices (into `post_reload_block`) of `combine_regs` RMW
  // insns split without an explicit `(REG_DEAD r)` note.
  std::vector<int> implicit_combine_regs_split_orig_indices;
  // Excluded post-reload output-reload insns (`mfhi`/`mflo` output moves and
  // output spill stores) mapped from `post_reload_block` index to the owning
  // `core_insns` index they must immediately follow (`reload1.c:emit_reload_insns`).
  absl::btree_map<int, int> output_reload_after_core;
  // Excluded post-reload input-reload insns (cross-block spill loads) mapped
  // from `post_reload_block` index to the consuming `core_insns` index they
  // must immediately precede (`reload1.c:emit_reload_insns`).
  absl::btree_map<int, int> input_reload_before_core;
  RegFacts facts_s1;
  absl::btree_set<int> live_at_end_s1;
};

Sched1ProjectionView ProjectPostReloadBlockToSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context = {});

// Schedules `view.core_insns` under `Phase::kSched1`, expanding any unpaired
// `optabs.c:2485-2525` `REG_LIBCALL` no-conflict `DImode` block with its
// `sched.c:4803-4818`-deleted `REG_RETVAL` self-move before list scheduling
// and mapping the resulting order back to `view.core_insns` indices.
std::vector<int> ScheduleSched1ViewCoreOrder(const Sched1ProjectionView& view,
                                             const MachineModel& mm);

// Computes a minimal-edit `Im(F_sched1)` core order closest to `view.core_insns`
// by solving the maximal acyclic subset of `RunForcedReplay()` LUID tie-break
// constraints (`sched.c:2428` `rank_for_schedule`) via `SolveSupernodeKahn`
// before forward `Phase::kSched1` replay. Unlike raw `ScheduleSched1ViewCoreOrder`,
// this preserves already-achievable equal-priority prefixes/suffixes without
// scrambling them under identity LUIDs.
std::vector<int> ScheduleClosestSched1CoreOrder(const Sched1ProjectionView& view,
                                                const MachineModel& mm);

// Parsed schedulable region of an `RtlBasicBlock`, separating leading/trailing
// notes (and block-0 pre-alloc parameter copies under `Phase::kSched1`) from
// schedulable instructions and mid-block `LOOP_BEG`/`LOOP_END` barriers.
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

// Half-open slice `[prologue_end, epilogue_begin)` inside a post-reload
// `sched_block` excluding prologue saves (block 0) and epilogue restores that
// `thread_prologue_and_epilogue_insns` emits after `reload` and `sched1`.
struct PostReloadBodyBounds {
  int prologue_end = 0;
  int epilogue_begin = 0;
};

PostReloadBodyBounds FindPostReloadBodyBounds(absl::Span<const Insn> sched_block, size_t block_idx);

// Returns body-relative indices (`i - bounds.prologue_end`) within
// `[bounds.prologue_end, bounds.epilogue_begin)` of block-0 prologue
// callee-save stores (`$ra`, `$s0-$s7`, `$fp`, `$f20-$f30` saved to `$sp+off`
// before first body write or call) and `$fp <- $sp` setup that post-reload
// `sched2` interleaved past leading body instructions (`toplev.c:3103`,
// `function.c:5516`, `config/mips/mips.c:expand_prologue`).
absl::btree_set<int> FindInterleavedPrologueSaveBodyIndices(absl::Span<const Insn> sched_block,
                                                            const PostReloadBodyBounds& bounds,
                                                            size_t block_idx,
                                                            const MachineModel& mm);

// True iff `(first_insn, second_insn)` is an adjacent `reload1.c:emit_reload_insns`
// `hi`/`lo` (`64`/`65`) output-reload pair: `second_insn` reads `hi`/`lo` into a
// GPR AND `first_insn` either sets that `hi`/`lo` register or is an adjacent
// sibling `mflo`/`mfhi` output reload.
bool IsAdjacentMdOutputReloadPair(const Insn& first_insn, const Insn& second_insn);

// True iff `(first_insn, second_insn)` is an adjacent `reload1.c:emit_reload_insns`
// input-reload (`stack -> reg` or GPR->FPR `mtc1`) or output-spill (`def -> stack`)
// pair that must remain contiguous for `Pass04`/`Pass05` recovery.
bool IsAdjacentReloadContiguityPair(const Insn& first_insn, const Insn& second_insn,
                                    const MachineModel& mm,
                                    const PostReloadStackLayout& stack_layout = {});

// Whole-function post-reload def-count and copy-pair summary shared by Stage 1
// (`InvertFillSimpleDelaySlots`) and Stage 3 (`InvertScheduleBlock`).
struct CfgSched1Context {
  std::vector<absl::btree_map<int, int>> block_total_defs;
  std::vector<absl::btree_set<int>> block_reaching_defs;
  absl::btree_map<int, int> cfg_total_defs;
  // Per-block and whole-CFG counts of genuine pre-reload unrenamed hard-reg
  // `SET`s (`flow.c:2065`: `CALL_INSN` return defs, `/i` return-value defs,
  // ABI `call_fusage` arg defs, `hi`/`lo` defs, and live-out defs feeding a
  // return `(use (reg/i ...))` or downstream `call_fusage`). Excludes
  // block-local pseudos that `local-alloc`/`global` happened to color to a
  // caller-saved hard reg like `$v0`.
  std::vector<absl::btree_map<int, int>> block_unrenamed_hard_defs;
  absl::btree_map<int, int> cfg_unrenamed_hard_defs;
  absl::btree_map<int, int> cfg_reaching_defs;
  std::vector<absl::btree_set<std::pair<int, RtlMachineMode>>> block_reaching_defs_by_mode;
  absl::btree_map<std::pair<int, RtlMachineMode>, int> cfg_reaching_defs_by_mode;
  // Exact per-block CFG def-use web other-block def counts (`flow.c:reg_n_sets`)
  // for live-in (`block_entry_web_other_defs`) and live-out (`block_exit_web_other_defs`).
  std::vector<absl::btree_map<std::pair<int, RtlMachineMode>, int>> block_entry_web_other_defs;
  std::vector<absl::btree_map<std::pair<int, RtlMachineMode>, int>> block_exit_web_other_defs;
  std::vector<absl::btree_set<int>> block_entry_exit_same_web_regs;
  absl::btree_set<std::pair<int, int>> cfg_copy_pairs;
  std::vector<absl::btree_set<int>> block_epilogue_use_regs;
  std::vector<bool> block_has_self_loop_preheader;
  PostReloadStackLayout stack_layout;
  bool has_stack_addr_escape = false;
  std::vector<absl::btree_map<int64_t, int>> block_spill_store_counts;
  absl::btree_map<int64_t, int> cfg_spill_store_counts;
  std::vector<absl::btree_set<int64_t>> block_spill_load_offsets;
  absl::btree_map<int64_t, int> cfg_spill_load_block_counts;
};

CfgSched1Context ComputeCfgSched1Context(const RtlControlFlowGraph& cfg,
                                         const RtlControlFlowGraph& live_cfg,
                                         const MachineModel& mm,
                                         absl::Span<const int> extra_b0_defs = {});

Sched1ProjectionContext BuildSched1ProjectionContextForBlock(size_t block_idx,
                                                             const CfgSched1Context& ctx);

// Post-reload reordering safety gate shared by Stage 0, Stage 1, and Stage 3:
// verifies that reordering `base_block` to `cand_in_base` preserves all
// hard-reg/memory/call dependences (`Analyze(kSched2)`), keeps prologue/epilogue
// slices pinned, preserves adjacent `hi`/`lo` output-reload and stack/mtc1
// reload contiguity (`reload1.c:emit_reload_insns`), preserves `REG_DEAD`
// reader order (`sched.c:3415-3611, 3764-3830`, allowing restoration of a
// `sched2`-stale pure-reader `REG_DEAD` back to the last reader in
// `cand_in_base`, or bidirectional pure-reader transposition when
// `allow_pure_reader_reg_dead_transfer` is set for Stage 0 discovery),
// preserves `live_at_entry`, `call_crossing_regs`
// (`caller-save.c:insert_save_restore`), exact `local-alloc.c:540-603, 990-1055`
// `validate_equiv_mem` status, and monotonic copy-tie / `hi`-`lo` allocation-order
// interferences (`global.c:962-985`, `local-alloc.c:1399, 2158-2181`).
bool PreservesPostReloadOrderSafety(absl::Span<const Insn> base_block,
                                    absl::Span<const int> cand_in_base, int prologue_end,
                                    int epilogue_begin, const absl::btree_set<int>& live_at_end_s2,
                                    const absl::btree_set<std::pair<int, int>>& cfg_copy_pairs,
                                    const RegFacts& facts_s2, const MachineModel& mm,
                                    bool allow_pure_reader_reg_dead_transfer = false,
                                    const PostReloadStackLayout& stack_layout = {},
                                    const absl::btree_set<int>& excluded_prologue_body_indices = {},
                                    bool allow_moved_store_load_equiv_invalidation = false);

// Returns body-relative indices (`0 .. body.size() - 1`) of block-0 entry-parameter
// copies (`(set dst src)` reading an unmodified incoming argument hard register
// `$a0-$a3`/`$f12-$f15` before the first call/jump) that post-reload `sched2`
// displaced past interleaved prologue saves or early body instructions
// (`function.c:3668-3742, 5129-5144`, `sched.c:3191-3215`, `toplev.c:3103`).
std::vector<int> CollectDisplacedBlock0EntryArgCopyBodyIndices(
    absl::Span<const Insn> post_reload_body, const Sched1ProjectionView& view,
    const absl::btree_set<int>& excluded_prologue_body_indices, const MachineModel& mm,
    bool frame_pointer_needed = true);

// When post-reload `sched2` (`reload_completed == 1`, `sched.c:3415-3611`)
// moved a pure reader `j` after the pre-`sched2` last reader `i` carrying
// `(REG_DEAD r)` without updating `REG_DEAD`, returns a seed permutation of
// `0 .. base_block.size()-1` placing each such `j` at its earliest `kSched2`
// DAG-legal position before `i`. Returns `std::nullopt` if no inverted
// pure-reader `REG_DEAD` exists in `[prologue_end, epilogue_begin)`.
enum class InvertedRegDeadSeedMode {
  kHoistEarliest,
  kPullBeforeDead,
  kDeferAfterReaders,
};

std::optional<std::vector<int>> BuildInvertedRegDeadSeedOrderInBase(
    absl::Span<const Insn> base_block, int prologue_end, int epilogue_begin,
    const RegFacts& facts_s2, const MachineModel& mm,
    InvertedRegDeadSeedMode mode = InvertedRegDeadSeedMode::kHoistEarliest);

// Extracts direct and transitive `Phase::kSched2` dependence edges `(u, v)`
// (`0 <= u < v < view.core_insns.size()`, excluding pure `sched.c:1704`
// call-artifact edges) between core instructions in `cur_seq`
// (`toplev.c:3028-3087`: because register allocation and reload never reorder
// instructions, every such post-reload hard-register/memory/call edge held in
// `.07.sched`).
std::vector<std::pair<int, int>> ExtractCoreSched2Edges(absl::Span<const Insn> cur_seq,
                                                        int prologue_end, int epilogue_begin,
                                                        const Sched1ProjectionView& view,
                                                        const RegFacts& facts_s2,
                                                        const MachineModel& mm);

// Projects the acyclic closure of `target_sched_block`'s `Phase::kSched2` DAG
// and forced-replay LUID tie-break constraints (`sched.c:2428`
// `rank_for_schedule`, `InvertBlockOrder` Phase 2) onto core instruction
// indices `0 .. view.core_insns.size()-1`. Every returned edge `(u, v)` is a
// required ordering `LUID(u) < LUID(v)` for `Schedule(Q, kSched2) == target`.
std::vector<std::pair<int, int>> ExtractCoreSched2ReplayEdges(
    absl::Span<const Insn> target_sched_block, absl::Span<const int> cur_to_target,
    int prologue_end, int epilogue_begin, const Sched1ProjectionView& view,
    const RegFacts& facts_s2, const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2,
    bool preserve_reg_dead_reader_order = false);

// Explores alternative `Phase::kSched1` forward schedules reachable by flipping
// individual equal-priority LUID ties (`sched.c:2428` `rank_for_schedule`) in
// `view.core_insns`'s `Schedule(kSched1)` trace, prioritized closest to the
// first backwards divergence from `view.core_insns` (and, when `core_s2_edges`
// or `core_s2_replay_edges` is provided, via conflict-driven promotion of
// consumers blocking violated post-reload `kSched2` edges, `sched.c:2428,
// 2557-2609`). Each returned vector is a permutation of
// `0 .. view.core_insns.size()-1` guaranteed to lie in `Im(F_sched1)`.
std::vector<std::vector<int>> CollectSched1TieFlipCoreOrders(
    const Sched1ProjectionView& view, const MachineModel& mm, int max_orders = 16,
    absl::Span<const std::pair<int, int>> core_s2_edges = {},
    absl::Span<const std::pair<int, int>> core_s2_replay_edges = {});

// Identifies producer insns `p` pushed later in forward `Schedule(kSched1)`
// (`sched.c:3144-3942` backward list scheduling) relative to an insn `g > p`
// because `sched2` sunk `p`'s earliest consumer `c_p` past `g`'s earliest
// consumer `c_g`, and returns bounded candidate core-index permutations
// hoisting `c_p` (together with its in-window `kSched1` predecessors) ahead of
// `c_g` and ahead of `g`.
std::vector<std::vector<int>> CollectSched1SunkConsumerHoistCoreOrders(
    const Sched1ProjectionView& view, const MachineModel& mm, int max_orders = 12);

// Identifies callee-saved hard-reg instructions sunk across an earlier
// `CALL_INSN` by post-reload `sched2` (`sched.c:1704` `call_used_regs[0]`
// anti-dep asymmetry + `sched.c:2235` pre-call `reg_n_calls_crossed == 0`
// lock) and returns bounded candidate permutations of `cur_seq` placing
// `sched1`-displaced (and then all independent) call-sunk callee-saved insns
// immediately before their preceding `CALL_INSN`.
std::vector<std::vector<int>> CollectSched1704CallHoistCandidatePrefs(
    absl::Span<const Insn> cur_seq, int prologue_end, int epilogue_begin,
    const Sched1ProjectionView& cur_view, const RegFacts& facts_s2, const MachineModel& mm);

// Returns true iff post-reload `cand_sched` (already verified to lie in
// `Im(F_sched2)`) either directly projects to a `kSched1`-achievable block or
// admits a `kSched2`-verified, `PreservesPostReloadOrderSafety`-safe preimage
// (including block-0 displaced entry-parameter copy hoisting, `sched.c:3191-3215`)
// whose `sched1` projection lies in `Im(F_sched1)`.
bool HasSched1AchievablePreimageForSched2Block(
    absl::Span<const Insn> cand_sched, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionView& cand_view, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, bool allow_pure_reader_reg_dead_transfer = true,
    bool allow_tie_flips = true);

// Stage 0 `sched1`-unsplit `SImode` `(const_double:VOID ...)` constant-load
// recovery (`varasm.c:1857-1895`, `config/mips/mips.md:3208-3221`,
// `config/mips/mips.c:1047-1057`): `large_int` (`mips.md:3208`) splits
// `CONST_INT` during `sched1` (`reg_n_sets == 2`) but never splits
// `CONST_DOUBLE` (`reg_n_sets == 1`, `birthing_insn_p == true`), while
// `mips_move_1word` emits `li` macros that GAS expands into `lui; ori`
// machine words identical to split `CONST_INT`s. For any `-O2+`
// (`RunsSched1 && RunsSched2 && !TARGET_64BIT`) block not already
// `kSched1`-achievable, greedily collapses the minimal subset of eligible
// bit-31-set `SI` (`lui; ori`) constant-load spans into `(const_double:VOID ...)`
// iff the collapsed block is both `kSched2`- and `kSched1`-achievable.
void CollapseSched1UnsplitConstDoubleLoads(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile);

// Stage 0 erased `MEM_UNCHANGING_P` (`/u`) and `MEM_IN_STRUCT_P` (`/s`) alias
// flag attribution (`varasm.c:601-604`, `expr.c:4534, 4571-4586, 4889`,
// `sched.c:817-882`): `final.c` erases `MEM` alias qualifiers from emitted
// assembly, which can reintroduce spurious `true_dependence`/`anti_dependence`
// edges in `sched2` and `sched1` when `sched2` hoisted read-only or aggregate
// loads across non-aliasing stores. For any `-O2+` (`RunsSched1 && RunsSched2`)
// block not already `kSched1`-achievable, recovers the minimal subset of
// GCC-valid semantic alias groups (read-only static symbols, read-only varying
// pointer bases, non-zero-offset varying pointer aggregates, and contiguous
// local stack-array aggregates) iff the attributed block remains in
// `Im(F_sched2)` and admits a `kSched1`-achievable preimage.
// Returns true if any instruction's `MEM` alias flags were updated.
bool AttributeSched2ErasedMemAliasFlags(RtlControlFlowGraph* cfg,
                                        const CompilationProfile& profile);

// Stage 0 post-`sched2` stale `REG_DEAD` / `REG_EQUIV` note attribution
// (`sched.c:3415-3611, 3764-3830`, `local-alloc.c:540-665, 990-1000, 1049-1055`):
// post-reload `sched2` (`reload_completed == 1`) never updates `REG_DEAD` or
// `REG_EQUIV` notes when reordering pure readers or moving independent memory
// operations across a `MEM`-equivalent register's live range, and `final.c`
// erases all notes. For any `-O2+` (`RunsSched1 && RunsSched2`) block whose
// post-`sched2` order is not `kSched1`-achievable, transfers pure-reader
// `REG_DEAD` notes and synthesizes `sched2`-obstructed `REG_EQUIV (mem ...)`
// notes iff the attributed block passes exact forward `kSched2` replay,
// `PreservesPostReloadOrderSafety`, and `IsAchievable(kSched1)`.
void AttributeSched2PureReaderRegDeadNotes(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile);

}  // namespace rom_nom_nom::lifter::sched_model

#endif  // ROM_NOM_NOM_LIFTER_SCHED_MODEL_H_
