#include "lifter/mips_reg_alloc_common.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

RtlExpr MustParsePattern(const std::string& pattern) {
  absl::StatusOr<RtlPatternAndNotes> pn = ParsePatternAndNotes(pattern);
  EXPECT_TRUE(pn.ok()) << "Failed to parse RTL pattern: " << pattern << " (" << pn.status() << ")";
  return pn.ok() ? std::move(pn->pattern) : RtlExpr{};
}

RtlPatternAndNotes MustParsePatternAndNotes(const std::string& text) {
  absl::StatusOr<RtlPatternAndNotes> pn = ParsePatternAndNotes(text);
  EXPECT_TRUE(pn.ok()) << "Failed to parse RTL insn: " << text << " (" << pn.status() << ")";
  return pn.ok() ? std::move(*pn) : RtlPatternAndNotes{};
}

// =============================================================================
// Unit Tests: `MipsTargetConfig`, `HardRegSet`, Reg Classes, Mode Geometry
// =============================================================================

TEST(MipsRegAllocCommonTest, ExtractMipsTargetConfigAndModeGeometry) {
  // Mirrors `config/mips/mips.c:3146-3270` (`override_options`),
  // `config/mips/mips.h:2928-2929` (`MEMORY_MOVE_COST`), and
  // `toplev.c:3028-3034` / `sched.c:4911-4966` (`flag_schedule_insns`).
  const MipsTargetConfig o2_32 =
      ExtractMipsTargetConfig(ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2"}}));
  EXPECT_FALSE(o2_32.gp_64bit);
  EXPECT_FALSE(o2_32.float_64bit);
  EXPECT_FALSE(o2_32.soft_float);
  EXPECT_FALSE(o2_32.single_float);
  EXPECT_TRUE(o2_32.runs_sched1);
  EXPECT_FALSE(o2_32.mem_move_cost_6);

  const MipsTargetConfig o1_mips1 = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 1, .c_flags = {"-O1", "-mips1"}}));
  EXPECT_FALSE(o1_mips1.runs_sched1);
  EXPECT_FALSE(o1_mips1.mem_move_cost_6);

  const MipsTargetConfig mips2_cfg = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips2"}}));
  EXPECT_TRUE(mips2_cfg.mem_move_cost_6);

  const MipsTargetConfig mips3_cfg = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips3"}}));
  EXPECT_TRUE(mips3_cfg.mem_move_cost_6);

  const MipsTargetConfig mips4_cfg = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips4"}}));
  EXPECT_FALSE(mips4_cfg.mem_move_cost_6);

  const MipsTargetConfig r4000_cfg = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips1", "-mcpu=r4000"}}));
  EXPECT_TRUE(r4000_cfg.mem_move_cost_6);

  const MipsTargetConfig r6000_cfg = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mcpu=r6000"}}));
  EXPECT_TRUE(r6000_cfg.mem_move_cost_6);

  const MipsTargetConfig r3000_cfg = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips3", "-mcpu=r3000"}}));
  EXPECT_FALSE(r3000_cfg.mem_move_cost_6);

  const MipsTargetConfig r4650_cfg = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips3", "-m4650"}}));
  EXPECT_FALSE(r4650_cfg.mem_move_cost_6);

  // Soft-float marks all FPRs (`32..63`) fixed and disallows FPR mode binding.
  const MipsTargetConfig soft_fp = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-msoft-float"}}));
  EXPECT_TRUE(soft_fp.soft_float);
  EXPECT_FALSE(HardRegnoModeOk(32, {RtlMachineMode::kSf, ""}, soft_fp));
  EXPECT_FALSE(HardRegnoModeOk(32, {RtlMachineMode::kDf, ""}, soft_fp));
  const HardRegSet soft_fixed = FixedRegSet(soft_fp);
  EXPECT_TRUE(soft_fixed.TestBit(kFirstFprRegnum));
  EXPECT_TRUE(soft_fixed.TestBit(kLastFprRegnum));

  // Single-float allows `SF` on even FPRs but rejects `DF`.
  const MipsTargetConfig single_fp = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-msingle-float"}}));
  EXPECT_TRUE(single_fp.single_float);
  EXPECT_TRUE(HardRegnoModeOk(32, {RtlMachineMode::kSf, ""}, single_fp));
  EXPECT_FALSE(HardRegnoModeOk(32, {RtlMachineMode::kDf, ""}, single_fp));

  // 32-bit vs 64-bit `HARD_REGNO_NREGS` and `HARD_REGNO_MODE_OK`
  // (`config/mips/mips.h:1255-1258, 1273-1274`, `config/mips/mips.c:3400-3465`).
  EXPECT_EQ(HardRegnoNregs(4, {RtlMachineMode::kDi, ""}, o2_32), 2);
  EXPECT_EQ(HardRegnoNregs(32, {RtlMachineMode::kDf, ""}, o2_32), 2);
  EXPECT_EQ(PseudoRegnoSize({RtlMachineMode::kDi, ""}, o2_32), 2);
  EXPECT_TRUE(HardRegnoModeOk(4, {RtlMachineMode::kDi, ""}, o2_32));
  EXPECT_FALSE(HardRegnoModeOk(5, {RtlMachineMode::kDi, ""}, o2_32));
  EXPECT_TRUE(HardRegnoModeOk(32, {RtlMachineMode::kDf, ""}, o2_32));
  EXPECT_FALSE(HardRegnoModeOk(33, {RtlMachineMode::kDf, ""}, o2_32));
  EXPECT_TRUE(HardRegnoModeOk(32, {RtlMachineMode::kSf, ""}, o2_32));
  EXPECT_FALSE(HardRegnoModeOk(33, {RtlMachineMode::kSf, ""}, o2_32));
  EXPECT_TRUE(HardRegnoModeOk(kFpswRegnum, {RtlMachineMode::kUnknown, "CC_FP"}, o2_32));
  EXPECT_FALSE(HardRegnoModeOk(4, {RtlMachineMode::kUnknown, "CC_FP"}, o2_32));
  EXPECT_TRUE(HardRegnoModeOk(kHiRegnum, {RtlMachineMode::kDi, ""}, o2_32));
  EXPECT_FALSE(HardRegnoModeOk(kLoRegnum, {RtlMachineMode::kDi, ""}, o2_32));
  EXPECT_FALSE(HardRegnoModeOk(kHiRegnum, {RtlMachineMode::kSf, ""}, o2_32));

  const MipsTargetConfig mips3_64 = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips3", "-mgp64", "-mfp64"}}));
  EXPECT_TRUE(mips3_64.gp_64bit);
  EXPECT_TRUE(mips3_64.float_64bit);
  EXPECT_EQ(HardRegnoNregs(4, {RtlMachineMode::kDi, ""}, mips3_64), 1);
  EXPECT_EQ(HardRegnoNregs(32, {RtlMachineMode::kDf, ""}, mips3_64), 1);
  EXPECT_EQ(PseudoRegnoSize({RtlMachineMode::kDi, ""}, mips3_64), 1);
  EXPECT_TRUE(HardRegnoModeOk(5, {RtlMachineMode::kDi, ""}, mips3_64));
  EXPECT_TRUE(HardRegnoModeOk(33, {RtlMachineMode::kDf, ""}, mips3_64));
}

TEST(MipsRegAllocCommonTest, HardRegSetAndRegClassRelations) {
  HardRegSet s;
  EXPECT_TRUE(s.IsEmpty());
  s.SetBit(2);
  s.SetBit(63);
  s.SetBit(64);
  s.SetBit(67);
  s.SetBit(-1);  // Out of range: ignored.
  s.SetBit(68);  // Out of range: ignored.
  EXPECT_FALSE(s.IsEmpty());
  EXPECT_TRUE(s.TestBit(2));
  EXPECT_TRUE(s.TestBit(63));
  EXPECT_TRUE(s.TestBit(64));
  EXPECT_TRUE(s.TestBit(67));
  EXPECT_FALSE(s.TestBit(-1));
  EXPECT_FALSE(s.TestBit(68));
  s.ClearBit(64);
  EXPECT_FALSE(s.TestBit(64));

  HardRegSet other;
  other.SetBit(2);
  other.SetBit(65);
  s.Ior(other);
  EXPECT_TRUE(s.TestBit(65));
  s.AndCompl(other);
  EXPECT_FALSE(s.TestBit(2));
  EXPECT_FALSE(s.TestBit(65));
  EXPECT_TRUE(s.TestBit(63));
  EXPECT_TRUE(s.TestBit(67));

  HardRegSet comp;
  comp.IorCompl(RegClassContents(MipsRegClass::kGrRegs));
  EXPECT_FALSE(comp.TestBit(0));
  EXPECT_FALSE(comp.TestBit(31));
  EXPECT_TRUE(comp.TestBit(32));
  EXPECT_TRUE(comp.TestBit(67));
  EXPECT_FALSE(comp.TestBit(68));

  EXPECT_TRUE(RegClassContents(MipsRegClass::kNoRegs).IsEmpty());
  EXPECT_EQ(RegClassContents(MipsRegClass::kMdRegs),
            (HardRegSet{0, 0x3ULL}));  // `hi` (64) + `lo` (65).
  EXPECT_EQ(RegClassContents(MipsRegClass::kAllRegs), (HardRegSet{0xffffffffffffffffULL, 0xfULL}));

  EXPECT_EQ(RegNoRegClass(4), MipsRegClass::kGrRegs);
  EXPECT_EQ(RegNoRegClass(32), MipsRegClass::kFpRegs);
  EXPECT_EQ(RegNoRegClass(64), MipsRegClass::kHiReg);
  EXPECT_EQ(RegNoRegClass(65), MipsRegClass::kLoReg);
  EXPECT_EQ(RegNoRegClass(66), MipsRegClass::kHiloReg);
  EXPECT_EQ(RegNoRegClass(67), MipsRegClass::kStRegs);
  EXPECT_EQ(RegNoRegClass(-1), MipsRegClass::kNoRegs);
  EXPECT_EQ(RegNoRegClass(68), MipsRegClass::kNoRegs);

  EXPECT_TRUE(RegClassSubsetP(MipsRegClass::kHiReg, MipsRegClass::kMdRegs));
  EXPECT_TRUE(RegClassSubsetP(MipsRegClass::kLoReg, MipsRegClass::kMdRegs));
  EXPECT_TRUE(RegClassSubsetP(MipsRegClass::kGrRegs, MipsRegClass::kAllRegs));
  EXPECT_FALSE(RegClassSubsetP(MipsRegClass::kGrRegs, MipsRegClass::kFpRegs));
  EXPECT_TRUE(RegClassRelatedP(MipsRegClass::kGrRegs, MipsRegClass::kAllRegs));
  EXPECT_FALSE(RegClassRelatedP(MipsRegClass::kGrRegs, MipsRegClass::kFpRegs));

  const MipsTargetConfig o2_32 =
      ExtractMipsTargetConfig(ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2"}}));
  const HardRegSet fixed = FixedRegSet(o2_32);
  EXPECT_TRUE(fixed.TestBit(0));
  EXPECT_TRUE(fixed.TestBit(1));
  EXPECT_TRUE(fixed.TestBit(29));
  EXPECT_FALSE(fixed.TestBit(2));
  EXPECT_FALSE(fixed.TestBit(32));
  const HardRegSet call_used = CallUsedRegSet(o2_32);
  EXPECT_TRUE(call_used.TestBit(2));
  EXPECT_FALSE(call_used.TestBit(16));
  EXPECT_TRUE(call_used.TestBit(32));
  EXPECT_FALSE(call_used.TestBit(52));
  EXPECT_TRUE(call_used.TestBit(kHiRegnum));
  EXPECT_TRUE(call_used.TestBit(kLoRegnum));
  const HardRegSet elim = EliminableRegSet();
  EXPECT_TRUE(elim.TestBit(kArgPointerRegnum));
  EXPECT_TRUE(elim.TestBit(kFramePointerRegnum));
  EXPECT_FALSE(elim.TestBit(2));
}

// =============================================================================
// Unit Tests: `FloorLog2` and `ComputeAllocnoPriority` (`global.c:542-543, 589-608`)
// =============================================================================

TEST(MipsRegAllocCommonTest, FloorLog2AndComputeAllocnoPriority) {
  EXPECT_EQ(FloorLog2(-5), -1);
  EXPECT_EQ(FloorLog2(0), -1);
  EXPECT_EQ(FloorLog2(1), 0);
  EXPECT_EQ(FloorLog2(2), 1);
  EXPECT_EQ(FloorLog2(7), 2);
  EXPECT_EQ(FloorLog2(8), 3);

  // Mirrors `global.c:542-543, 589-608`: `live_length == 0` is clamped to `-1`,
  // producing negative priority in `allocno_compare` so unmeasured/dead allocnos sort last.
  EXPECT_LT(ComputeAllocnoPriority(10, 0, 1), 0);
  EXPECT_EQ(ComputeAllocnoPriority(10, 0, 1), ComputeAllocnoPriority(10, -1, 1));
  EXPECT_EQ(ComputeAllocnoPriority(10, 4, 0), ComputeAllocnoPriority(10, 4, 1));
  EXPECT_GT(ComputeAllocnoPriority(16, 4, 1), ComputeAllocnoPriority(4, 4, 1));
  EXPECT_GT(ComputeAllocnoPriority(8, 2, 1), ComputeAllocnoPriority(8, 10, 1));
  EXPECT_GT(ComputeAllocnoPriority(8, 4, 2), ComputeAllocnoPriority(8, 4, 1));
}

// =============================================================================
// Unit Tests: `IsMipsMatchScratchParallel` (`config/mips/mips.md`)
// =============================================================================

TEST(MipsRegAllocCommonTest, MatchScratchParallelPositiveCases) {
  // `mulsi3_internal` (`config/mips/mips.md:848-853`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI))])")));

  // `mulsidi3_internal` (`config/mips/mips.md:963-967`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:DI 72) "
                       "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1)))) "
                       "(clobber (scratch:SI))])")));

  // `smulsi3_highpart` (`config/mips/mips.md:986-993`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) "
                       "(truncate:SI (lshiftrt:DI "
                       "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1))) "
                       "(const_int 32)))) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI)) (clobber (scratch:SI))])")));

  // `madsi` (`config/mips/mips.md:1082-1088`) and `maddi` (`1095-1102`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) "
                       "(plus:SI (mult:SI (reg:SI 4 a0) (reg:SI 5 a1)) (reg:SI 6 a2))) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI))])")));

  // `divmodsi4` / `divsi3` / `modsi3` / `udivsi3` / `umodsi3`
  // (`config/mips/mips.md:1285-1482`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (div:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                       "(set (reg:SI 73) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                       "(clobber (scratch:SI))])")));
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI))])")));
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (udiv:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI))])")));
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (umod:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI))])")));

  // `ffssi2` / `ffsdi2` (`config/mips/mips.md:1596-1636`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (ffs:SI (reg:SI 4 a0))) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI))])")));

  // `fix_truncdfsi2` / `fix_truncsfsi2` (`config/mips/mips.md:2485-2574`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (fix:SI (reg:DF 44 $f12))) "
                       "(clobber (scratch:DF))])")));
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (fix:SI (reg:SF 44 $f12))) "
                       "(clobber (scratch:SF))])")));

  // `movstrsi_internal` (`config/mips/mips.md:3587-3668`).
  EXPECT_TRUE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
                       "(use (const_int 16)) (use (const_int 4)) "
                       "(clobber (scratch:SI)) (clobber (scratch:SI))])")));
}

TEST(MipsRegAllocCommonTest, MatchScratchParallelNegativeCases) {
  // `casesi_internal` (`config/mips/mips.md:6001-6018`): uses `match_operand`,
  // NOT `match_scratch` (expanded from `define_expand "casesi"` at `5946-5986`).
  EXPECT_FALSE(IsMipsMatchScratchParallel(MustParsePattern(
      "(parallel [(set (pc) (mem:SI (plus:SI (mult:SI (reg:SI 4 a0) (const_int 4)) "
      "(label_ref 10)))) (clobber (reg:SI 72)) (clobber (reg:SI 31 ra))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (pc) "
                       "(if_then_else (leu:SI (reg:SI 4 a0) (const_int 4)) "
                       "(plus:SI (mem:SI (reg:SI 4 a0)) (label_ref 10)) (pc))) "
                       "(clobber (reg:SI 72))])")));

  // `adddi3` / `subdi3` / `negdi2` / `ashldi3` (`match_operand` clobber).
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:DI 72) (plus:DI (reg:DI 4 a0) (reg:DI 6 a2))) "
                       "(clobber (reg:SI 73))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:DI 72) (minus:DI (reg:DI 4 a0) (reg:DI 6 a2))) "
                       "(clobber (reg:SI 73))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:DI 72) (neg:DI (reg:DI 4 a0))) "
                       "(clobber (reg:SI 73))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:DI 72) (ashift:DI (reg:DI 4 a0) (const_int 3))) "
                       "(clobber (reg:SI 73))])")));

  // `call`, `asm_operands`, `unspec_volatile`.
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(call (mem:QI (symbol_ref:SI \"foo\")) (const_int 0)) "
                       "(clobber (reg:SI 31 ra))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(MustParsePattern(
      "(parallel [(set (reg:SI 72) (asm_operands:SI \"nop\" \"=r\" 0 [] [] (\"f.c\") 1)) "
      "(clobber (reg:SI 2 v0))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (unspec_volatile:SI [(reg:SI 4 a0)] 1)) "
                       "(clobber (reg:SI 73))])")));

  // Non-`parallel` plain `set`, `parallel` without `clobber`, `truncate`
  // without `mult`, and non-`BLK` `mem` copy.
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(set (reg:SI 72) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1)))")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1)))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (reg:SI 72) (truncate:SI (reg:DI 4 a0))) "
                       "(clobber (reg:SI 73))])")));
  EXPECT_FALSE(IsMipsMatchScratchParallel(
      MustParsePattern("(parallel [(set (mem:SI (reg:SI 4 a0)) (mem:SI (reg:SI 5 a1))) "
                       "(clobber (reg:SI 73))])")));
}

// =============================================================================
// Unit Tests: `CollectHardAndScratchMdNeeds` & `InsnHardClobbersHiLoForLocalAlloc`
// =============================================================================

TEST(MipsRegAllocCommonTest, CollectHardAndScratchMdNeedsAndHiLoClobber) {
  const MipsTargetConfig cfg32 =
      ExtractMipsTargetConfig(ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2"}}));
  const MipsTargetConfig cfg64 = ExtractMipsTargetConfig(
      ParseProfileFlags({.optimize_level = 2, .c_flags = {"-O2", "-mips3", "-mgp64"}}));

  // `smulsi3_highpart` (`config/mips/mips.md:986-1072`): hard non-HI dest +
  // scratch clobbers require `hi`, `lo`, `hilo`.
  {
    HardRegSet needs;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:SI 2 v0) "
                         "(truncate:SI (lshiftrt:DI "
                         "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1))) "
                         "(const_int 32)))) "
                         "(clobber (scratch:SI)) (clobber (scratch:SI))])"),
        cfg32, &needs);
    EXPECT_TRUE(needs.TestBit(kHiRegnum));
    EXPECT_TRUE(needs.TestBit(kLoRegnum));
    EXPECT_TRUE(needs.TestBit(kHiloRegnum));
  }
  // Contrast: HI dest + non-scratch reg clobbers require no reload MD regs.
  {
    HardRegSet needs;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:SI 64 hi) "
                         "(truncate:SI (lshiftrt:DI "
                         "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1))) "
                         "(const_int 32)))) "
                         "(clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum))])"),
        cfg32, &needs);
    EXPECT_TRUE(needs.IsEmpty());
  }

  // `mulsidi3` (`config/mips/mips.md:963-979, 1014-1030`): 32-bit vs 64-bit.
  {
    HardRegSet needs32;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:DI 2 v0) "
                         "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1)))) "
                         "(clobber (scratch:SI))])"),
        cfg32, &needs32);
    EXPECT_TRUE(needs32.TestBit(kHiRegnum));
    EXPECT_TRUE(needs32.TestBit(kLoRegnum));
    EXPECT_TRUE(needs32.TestBit(kHiloRegnum));

    HardRegSet needs64;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:DI 2 v0) "
                         "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1)))) "
                         "(clobber (scratch:DI)) (clobber (scratch:DI))])"),
        cfg64, &needs64);
    EXPECT_TRUE(needs64.TestBit(kHiloRegnum));
    EXPECT_TRUE(needs64.TestBit(kLoRegnum));
    EXPECT_TRUE(needs64.TestBit(kHiRegnum));
  }

  // `mulsi3_internal` (3-element, `config/mips/mips.md:848-853`) vs
  // `mulsi3_r4000` (4-element, `config/mips/mips.md:860-866`).
  {
    HardRegSet needs_internal;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:SI 2 v0) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                         "(clobber (scratch:SI)) (clobber (scratch:SI))])"),
        cfg32, &needs_internal);
    EXPECT_TRUE(needs_internal.TestBit(kLoRegnum));
    EXPECT_TRUE(needs_internal.TestBit(kHiRegnum));
    EXPECT_TRUE(needs_internal.TestBit(kHiloRegnum));

    HardRegSet needs_r4000;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:SI 2 v0) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                         "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) "
                         "(clobber (scratch:SI))])"),
        cfg32, &needs_r4000);
    EXPECT_FALSE(needs_r4000.TestBit(kHiRegnum));
    EXPECT_FALSE(needs_r4000.TestBit(kLoRegnum));
    EXPECT_TRUE(needs_r4000.TestBit(kHiloRegnum));
  }

  // `divmodsi4` (5-element, `config/mips/mips.md:1285-1369`).
  {
    HardRegSet needs_div;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:SI 72) (div:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                         "(set (reg:SI 73) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                         "(clobber (scratch:SI)) (clobber (scratch:SI)) (clobber (scratch:SI))])"),
        cfg32, &needs_div);
    EXPECT_TRUE(needs_div.TestBit(kLoRegnum));
    EXPECT_TRUE(needs_div.TestBit(kHiRegnum));
    EXPECT_TRUE(needs_div.TestBit(kHiloRegnum));
  }

  // Negatives: non-parallel `set`, `asm_operands`, and non-MD `adddi3`.
  {
    HardRegSet needs;
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(set (reg:SI 2 v0) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1)))"), cfg32,
        &needs);
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:DI 2 v0) (plus:DI (reg:DI 4 a0) (reg:DI 6 a2))) "
                         "(clobber (scratch:SI))])"),
        cfg32, &needs);
    CollectHardAndScratchMdNeeds(
        MustParsePattern("(parallel [(set (reg:SI 2 v0) (asm_operands:SI \"m\" \"=r\" 0 [] [] "
                         "(\"f.c\") 1)) (clobber (scratch:SI))])"),
        cfg32, &needs);
    EXPECT_TRUE(needs.IsEmpty());
  }

  // `InsnHardClobbersHiLoForLocalAlloc` (`local-alloc.c:1381, 2103-2106`,
  // `config/mips/mips.h:1203-1210`, `config/mips/mips.md:963-1024`):
  // positive on `call`, explicit `hi` (64), explicit `lo` (65); negative on
  // `mult:DI` clobbering only `accum` (66) and on plain GPR ops.
  EXPECT_TRUE(InsnHardClobbersHiLoForLocalAlloc(
      MustParsePattern("(parallel [(call (mem:QI (symbol_ref:SI \"foo\")) (const_int 0)) "
                       "(clobber (reg:SI 31 ra))])")));
  EXPECT_TRUE(
      InsnHardClobbersHiLoForLocalAlloc(MustParsePattern("(set (reg:SI 2 v0) (reg:SI 64 hi))")));
  EXPECT_TRUE(InsnHardClobbersHiLoForLocalAlloc(
      MustParsePattern("(parallel [(set (reg:SI 72) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                       "(clobber (reg:SI 65 lo))])")));
  EXPECT_FALSE(InsnHardClobbersHiLoForLocalAlloc(
      MustParsePattern("(parallel [(set (reg:DI 72) "
                       "(mult:DI (sign_extend:DI (reg:SI 4 a0)) (sign_extend:DI (reg:SI 5 a1)))) "
                       "(clobber (reg:SI 66 accum))])")));
  EXPECT_FALSE(InsnHardClobbersHiLoForLocalAlloc(
      MustParsePattern("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))")));
}

// =============================================================================
// Unit Tests: `SingleSet`, `UnwrapNoteStoresDest`, `HasSideEffects`, `HasRegUnusedNote`
// =============================================================================

TEST(MipsRegAllocCommonTest, SingleSetUnwrapNoteStoresSideEffectsAndRegUnused) {
  // `SingleSet` (`rtlanal.c:single_set`): plain `set`, `parallel` with 1 live
  // `set` + `clobber`, and `parallel` with 1 live `set` + 1 `REG_UNUSED` `set`.
  {
    const RtlPatternAndNotes plain =
        MustParsePatternAndNotes("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))");
    ASSERT_NE(SingleSet(plain), nullptr);
    EXPECT_EQ(*SingleSet(plain), plain.pattern);

    const RtlPatternAndNotes with_clobber = MustParsePatternAndNotes(
        "(parallel [(set (reg:SI 72) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
        "(clobber (reg:SI 64 hi))])");
    ASSERT_NE(SingleSet(with_clobber), nullptr);
    EXPECT_EQ(*SingleSet(with_clobber), with_clobber.pattern.children[0]);

    const RtlPatternAndNotes one_unused = MustParsePatternAndNotes(
        "(parallel [(set (reg:SI 72) (div:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
        "(set (reg:SI 73) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1)))]) "
        "(expr_list:REG_UNUSED (reg:SI 73) (nil))");
    ASSERT_NE(SingleSet(one_unused), nullptr);
    EXPECT_EQ(*SingleSet(one_unused), one_unused.pattern.children[0]);

    // Negative: two live `set`s, `REG_UNUSED` volatile `set` (side effects),
    // and non-`set` `(use ...)`.
    const RtlPatternAndNotes two_live = MustParsePatternAndNotes(
        "(parallel [(set (reg:SI 72) (div:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
        "(set (reg:SI 73) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1)))])");
    EXPECT_EQ(SingleSet(two_live), nullptr);

    const RtlPatternAndNotes unused_volatile = MustParsePatternAndNotes(
        "(parallel [(set (reg:SI 72) (reg:SI 4 a0)) "
        "(set (reg:SI 73) (mem/v:SI (reg:SI 5 a1)))]) "
        "(expr_list:REG_UNUSED (reg:SI 73) (nil))");
    EXPECT_EQ(SingleSet(unused_volatile), nullptr);

    const RtlPatternAndNotes use_only = MustParsePatternAndNotes("(use (reg:SI 2 v0))");
    EXPECT_EQ(SingleSet(use_only), nullptr);
  }

  // `UnwrapNoteStoresDest` & `NoteStores` (`rtlanal.c:note_stores`):
  // unwraps `subreg` of pseudo reg + `strict_low_part`/`zero_extract`/`sign_extract`,
  // preserves `subreg` of hard reg, and visits `parallel` elements in reverse order.
  {
    const RtlExpr pseudo_subreg = MustParsePattern("(subreg:SI (reg:DI 72) 0)");
    const RtlExpr* unwrapped_pseudo = UnwrapNoteStoresDest(pseudo_subreg);
    ASSERT_NE(unwrapped_pseudo, nullptr);
    EXPECT_EQ(unwrapped_pseudo->op, RtlOp::kReg);
    EXPECT_EQ(unwrapped_pseudo->RegNo(), 72);

    const RtlExpr hard_subreg = MustParsePattern("(subreg:SI (reg:DI 4 a0) 0)");
    const RtlExpr* unwrapped_hard = UnwrapNoteStoresDest(hard_subreg);
    ASSERT_NE(unwrapped_hard, nullptr);
    EXPECT_EQ(unwrapped_hard->op, RtlOp::kSubreg);

    const RtlExpr nested_extract = MustParsePattern(
        "(strict_low_part (zero_extract:SI (reg:SI 74) (const_int 8) "
        "(const_int 0)))");
    const RtlExpr* unwrapped_nested = UnwrapNoteStoresDest(nested_extract);
    ASSERT_NE(unwrapped_nested, nullptr);
    EXPECT_EQ(unwrapped_nested->op, RtlOp::kReg);
    EXPECT_EQ(unwrapped_nested->RegNo(), 74);

    const RtlExpr par = MustParsePattern(
        "(parallel [(set (reg:SI 72) (reg:SI 4 a0)) (clobber (reg:SI 64 hi)) "
        "(set (subreg:SI (reg:DI 73) 0) (reg:SI 5 a1))])");
    std::vector<int> visited_dest_regnos;
    NoteStores(par, [&](const RtlExpr& dest, const RtlExpr& /*setter*/) {
      if (dest.op == RtlOp::kReg) {
        visited_dest_regnos.push_back(dest.RegNo());
      }
    });
    EXPECT_EQ(visited_dest_regnos, (std::vector<int>{73, 64, 72}));
  }

  // `HasSideEffects`: positive on volatile `mem/v`, `call`, `unspec_volatile`,
  // `asm_operands`; negative on plain `mem` and arithmetic.
  EXPECT_TRUE(HasSideEffects(MustParsePattern("(set (reg:SI 72) (mem/v:SI (reg:SI 4 a0)))")));
  EXPECT_TRUE(
      HasSideEffects(MustParsePattern("(call (mem:QI (symbol_ref:SI \"foo\")) (const_int 0))")));
  EXPECT_TRUE(
      HasSideEffects(MustParsePattern("(set (reg:SI 72) (unspec_volatile:SI [(reg:SI 4 a0)] 1))")));
  EXPECT_TRUE(HasSideEffects(
      MustParsePattern("(set (reg:SI 72) (asm_operands:SI \"nop\" \"=r\" 0 [] [] (\"f.c\") 1))")));
  EXPECT_FALSE(HasSideEffects(MustParsePattern("(set (reg:SI 72) (mem:SI (reg:SI 4 a0)))")));
  EXPECT_FALSE(
      HasSideEffects(MustParsePattern("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))")));

  // `HasRegUnusedNote` & `HasRegUnusedNoteForRegno`.
  {
    const RtlPatternAndNotes pn = MustParsePatternAndNotes(
        "(set (reg:SI 72) (reg:SI 4 a0)) "
        "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_UNUSED (reg:SI 72) (nil)))");
    EXPECT_TRUE(HasRegUnusedNote(pn.reg_notes, MustParsePattern("(reg:SI 72)")));
    EXPECT_FALSE(HasRegUnusedNote(pn.reg_notes, MustParsePattern("(reg:SI 4 a0)")));
    EXPECT_FALSE(HasRegUnusedNote(pn.reg_notes, MustParsePattern("(reg:SI 73)")));
    EXPECT_TRUE(HasRegUnusedNoteForRegno(pn.reg_notes, 72));
    EXPECT_FALSE(HasRegUnusedNoteForRegno(pn.reg_notes, 4));
    EXPECT_FALSE(HasRegUnusedNoteForRegno(pn.reg_notes, 73));
  }
}

}  // namespace
}  // namespace rom_nom_nom::lifter
