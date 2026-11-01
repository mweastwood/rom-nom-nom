#include "lifter/rtl_def_use_analysis.h"

#include <string>
#include <vector>

#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gtest/gtest.h"
#include "lifter/rtl_cfg.h"
#include "lifter/rtl_cfg_builder.h"

namespace rom_nom_nom {
namespace {

Instruction MakeMtc1(Register rt, FpRegister fs) {
  Instruction inst;
  inst.raw_word = 0x44800000u;
  inst.opcode = Opcode::kMtc1;
  inst.rt = rt;
  inst.fs = fs;
  return inst;
}

Instruction MakeAddiu(Register rt, Register rs, int16_t imm) {
  Instruction inst;
  inst.raw_word = 0x24000000u;
  inst.opcode = Opcode::kAddiu;
  inst.rt = rt;
  inst.rs = rs;
  inst.immediate = imm;
  return inst;
}

Instruction MakeJal(uint32_t target) {
  Instruction inst;
  inst.raw_word = 0x0C000000u;
  inst.opcode = Opcode::kJal;
  inst.target = (target >> 2) & 0x03FFFFFF;
  inst.vram = 0x80010000;
  return inst;
}

RtlInsn MakeSingleRtlInsn(const Instruction& inst) {
  RtlInsn out;
  out.canonical_pattern = inst.Disassemble();
  out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  out.machine_instructions = {inst};
  return out;
}

TEST(RtlDefUseAnalysisTest, ExtractsDefsUsesAndCalleeAcrossSingleAndFusedIdioms) {
  // 1. Fused `mips_move_2words` (`mtc1 $a3, $f12; mtc1 $a2, $f13` -> uses `a2`, defines `$f12`).
  RtlInsn move_2words;
  move_2words.idiom_kind = RtlFusedIdiomKind::kMove2Words;
  move_2words.machine_instructions = {
      MakeMtc1(Register::kA3, FpRegister::kF12),
      MakeMtc1(Register::kA2, FpRegister::kF13),
  };
  move_2words.canonical_pattern = move_2words.machine_instructions[0].Disassemble();
  const RtlInsnDefUse du_m2w = AnalyzeRtlInsnDefUse(move_2words);
  EXPECT_TRUE(du_m2w.is_reg_to_reg_move);
  EXPECT_TRUE(du_m2w.is_df_di_a2);
  EXPECT_EQ(du_m2w.uses, std::vector<std::string>{"a2"});
  EXPECT_EQ(du_m2w.defs, std::vector<std::string>{"$f12"});

  // 2. Call instruction with symbol resolution.
  auto symbols_or = SymbolIndex::ParseFromTextproto(
      "symbols { address: 0x80040000 name: \"helper_fn\" type: SYMBOL_FUNC }");
  ASSERT_TRUE(symbols_or.ok());
  const RtlInsn call_insn = MakeSingleRtlInsn(MakeJal(0x80040000));
  const RtlInsnDefUse du_call = AnalyzeRtlInsnDefUse(call_insn, &(*symbols_or));
  EXPECT_TRUE(du_call.is_call);
  EXPECT_EQ(du_call.callee, "helper_fn");
  EXPECT_TRUE(HasRegItem(du_call.defs, "v0"));
  EXPECT_TRUE(HasRegItem(du_call.defs, "$f0"));
}

TEST(RtlDefUseAnalysisTest, ComputesSuccessorLivenessRespectingKillsAndCalls) {
  // Block 0 -> {1, 2}, Block 1 -> {3} (overwrites a1 before reading), Block 2 -> {3} (no touch),
  // Block 3 reads a1 and kills a2 via call.
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(4);
  cfg.rpo_blocks[0].successors = {1, 2};
  cfg.rpo_blocks[1].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kA1, Register::kZero, 5)));
  cfg.rpo_blocks[1].successors = {3};
  cfg.rpo_blocks[2].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kT0, Register::kZero, 1)));
  cfg.rpo_blocks[2].successors = {3};
  cfg.rpo_blocks[3].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kV0, Register::kA1, 10)));
  cfg.rpo_blocks[3].AddInsn(MakeSingleRtlInsn(MakeJal(0x80040000)));
  cfg.rpo_blocks[3].AddInsn(MakeSingleRtlInsn(MakeAddiu(Register::kV0, Register::kA2, 1)));

  std::vector<std::vector<RtlInsnDefUse>> parsed_map(4);
  for (size_t b = 0; b < 4; ++b) {
    for (const auto& insn : cfg.rpo_blocks[b].rtl_insns) {
      parsed_map[b].push_back(AnalyzeRtlInsnDefUse(insn));
    }
  }

  // From Block 0, `a1` is live along path 0 -> 2 -> 3, but from Block 1 (after `a1` is overwritten)
  // `a1` is also read in Block 3. `a2` is killed by the call in Block 3 before being read.
  EXPECT_TRUE(IsRegLiveInSuccessors(cfg, parsed_map, 0, "a1"));
  EXPECT_FALSE(IsRegLiveInSuccessors(cfg, parsed_map, 0, "a2"));
}

}  // namespace
}  // namespace rom_nom_nom
