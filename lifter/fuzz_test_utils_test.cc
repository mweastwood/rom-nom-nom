#include "lifter/fuzz_test_utils.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr uint32_t kMipsNop = 0x00000000u;
constexpr uint32_t kMipsJrRa = 0x03e00008u;

uint32_t MakeAddiu(uint32_t rt, uint32_t rs, int16_t imm) {
  return (9u << 26) | ((rs & 0x1fu) << 21) | ((rt & 0x1fu) << 16) | static_cast<uint16_t>(imm);
}

uint32_t MakeBeq(uint32_t rs, uint32_t rt, int16_t offset) {
  return (4u << 26) | ((rs & 0x1fu) << 21) | ((rt & 0x1fu) << 16) | static_cast<uint16_t>(offset);
}

TEST(FuzzTestUtilsTest, StratifiedCorpusSelectorCoversAllSevenProfilesEvenly) {
  const auto& all_cases = GetSharedRoundTripTestCases();
  const auto& profiles = GetRoundTripCompilerFlagProfiles();
  ASSERT_EQ(profiles.size(), 7u);

  const std::vector<size_t> indices = GetStratifiedCorpusCaseIndices(70);
  ASSERT_EQ(indices.size(), 70u);

  absl::btree_map<size_t, int> profile_counts;
  for (size_t idx : indices) {
    ASSERT_LT(idx, all_cases.size());
    for (size_t p = 0; p < profiles.size(); ++p) {
      if (all_cases[idx].options.c_flags == profiles[p].c_flags) {
        ++profile_counts[p];
        break;
      }
    }
  }
  EXPECT_EQ(profile_counts.size(), profiles.size());
  for (size_t p = 0; p < profiles.size(); ++p) {
    EXPECT_EQ(profile_counts[p], 10) << "Profile " << profiles[p].suffix;
  }

  // Negative: non-positive count yields an empty index list.
  EXPECT_TRUE(GetStratifiedCorpusCaseIndices(0).empty());
  EXPECT_TRUE(GetStratifiedCorpusCaseIndices(-5).empty());
}

TEST(FuzzTestUtilsTest, RtlRegisterPrimitivesAndAlphaRenaming) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  // FormatRegAtom prints MIPS reg 30 as `$fp`.
  EXPECT_EQ(FormatRegAtom(RtlMachineMode::kSi, 30), "(reg:SI 30 $fp)");
  EXPECT_EQ(FormatRegAtom(RtlMachineMode::kSi, 8), "(reg:SI 8 t0)");

  // Post-reload hard-reg discovery, free-reg pickers, InsertInsnsAt, and alpha-rename.
  RtlControlFlowGraph post_cfg;
  post_cfg.pass = RtlPassKind::kGreg;
  post_cfg.rpo_blocks.resize(1);
  post_cfg.rpo_blocks[0].id = 0;
  post_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  post_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 8 t0))");
  post_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RtlInsn extra = MakeRtlInsn("(set (reg:DF 36 $f4) (mem:DF (reg:SI 4 a0)))");
  InsertInsnsAt(&post_cfg.rpo_blocks[0], 1, absl::MakeConstSpan(&extra, 1));
  ASSERT_EQ(post_cfg.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(post_cfg.rpo_blocks[0].rtl_insns.size(), 4u);

  const absl::btree_set<int> used = CollectAllUsedHardRegsInCfg(post_cfg, prof);
  EXPECT_TRUE(used.contains(8));
  EXPECT_TRUE(used.contains(36));
  EXPECT_TRUE(used.contains(37));  // 2-register DFmode span on !fp64.
  EXPECT_FALSE(used.contains(9));

  const std::vector<int> free_gprs = PickFreeCallerSavedGprs(used, 2);
  EXPECT_EQ(free_gprs, (std::vector<int>{9, 10}));
  const std::vector<int> free_fprs = PickFreeCallerSavedFprs(used, 2);
  EXPECT_EQ(free_fprs, (std::vector<int>{38, 40}));
  EXPECT_EQ(PickFreeCalleeSavedGprs(used, 2), (std::vector<int>{16, 17}));
  EXPECT_EQ(PickFreeCalleeSavedFprs(used, 2), (std::vector<int>{52, 54}));

  std::mt19937_64 rng(42);
  EXPECT_TRUE(ApplyPostReloadHardRegAlphaRename(&post_cfg, prof, &rng));
  // `$8 t0` should be renamed to another caller-saved GPR while `/i` `$2 v0` stays pinned.
  EXPECT_EQ(post_cfg.rpo_blocks[0].instructions[0].find("(reg:SI 8 t0)"), std::string::npos);
  EXPECT_NE(post_cfg.rpo_blocks[0].instructions[2].find("(reg/i:SI 2 v0)"), std::string::npos);

  // Pre-reload pseudo discovery & alpha-renaming (including sideband-only
  // `reg_n_deaths`, `reg_renumber`, and `call_fusage` pseudos so shifting
  // cannot collide and overwrite entries).
  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kCombine;
  pre_cfg.sideband.max_regno = 88;
  pre_cfg.rpo_blocks.resize(1);
  pre_cfg.rpo_blocks[0].id = 0;
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 82) (plus:SI (reg:SI 80) (const_int 3)))");
  pre_cfg.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 82))");
  pre_cfg.rpo_blocks[0].rtl_insns[0].call_fusage.push_back(
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 83)));
  pre_cfg.sideband.reg_n_deaths[80] = 1;
  pre_cfg.sideband.reg_n_deaths[84] = 2;
  pre_cfg.sideband.reg_renumber[82] = 2;
  pre_cfg.sideband.reg_renumber[85] = 8;
  EXPECT_EQ(NextFreshPseudo(pre_cfg), 88);

  const std::string before_pre = pre_cfg.rpo_blocks[0].instructions[0];
  EXPECT_TRUE(ApplyPseudoAlphaRename(&pre_cfg, &rng));
  EXPECT_NE(pre_cfg.rpo_blocks[0].instructions[0], before_pre);
  // All distinct sideband/fusage pseudos must be bijectively preserved.
  EXPECT_EQ(pre_cfg.sideband.reg_n_deaths.size(), 2u);
  EXPECT_EQ(pre_cfg.sideband.reg_renumber.size(), 2u);
  ASSERT_EQ(pre_cfg.rpo_blocks[0].rtl_insns[0].call_fusage.size(), 1u);
  EXPECT_GE(pre_cfg.rpo_blocks[0].rtl_insns[0].call_fusage[0].children[0].RegNo(),
            kMipsFirstUserPseudoRegister);

  // Negative: CFG with no user pseudos does not rename.
  RtlControlFlowGraph no_pseudo_cfg;
  no_pseudo_cfg.pass = RtlPassKind::kCombine;
  no_pseudo_cfg.rpo_blocks.resize(1);
  no_pseudo_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  EXPECT_FALSE(ApplyPseudoAlphaRename(&no_pseudo_cfg, &rng));
}

TEST(FuzzTestUtilsTest, GreedyShrinkRtlCfgReducesAndRespectsNoShrinkNegative) {
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.resize(2);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].successors = {1};
  for (int i = 0; i < 8; ++i) {
    if (i == 3) {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 14 t6) (const_int 9999))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly(absl::StrCat(
          "(set ", FormatRegAtom(RtlMachineMode::kSi, 8 + (i % 4)), " (const_int ", i, "))"));
    }
  }
  cfg.rpo_blocks[0].AddPatternOnly("(set (pc) (label_ref L0))");
  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].has_label = true;
  cfg.rpo_blocks[1].predecessors = {0};
  cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");

  // Positive: collapses 2-block 10-insn CFG to <= 3 insns preserving the trigger.
  const RtlControlFlowGraph shrunk =
      GreedyShrinkRtlCfg(cfg, prof, /*reload_completed=*/true, [](const RtlControlFlowGraph& cand) {
        for (const RtlBasicBlock& blk : cand.rpo_blocks) {
          for (const std::string& insn : blk.instructions) {
            if (insn.find("9999") != std::string::npos) {
              return true;
            }
          }
        }
        return false;
      });
  EXPECT_EQ(shrunk.rpo_blocks.size(), 1u);
  EXPECT_LE(shrunk.TotalInsnCount(), 3u);

  // Negative: when every smaller candidate passes (`still_fails == false`), input is unchanged.
  const RtlControlFlowGraph unshrunk = GreedyShrinkRtlCfg(
      cfg, prof, /*reload_completed=*/true, [](const RtlControlFlowGraph&) { return false; });
  EXPECT_EQ(unshrunk.TotalInsnCount(), cfg.TotalInsnCount());
  EXPECT_EQ(unshrunk.rpo_blocks.size(), cfg.rpo_blocks.size());
}

TEST(FuzzTestUtilsTest, GreedyShrinkStage0InputUpdatesBranchesRelocsAndJumpTables) {
  // Build a 10-word Stage-0 function:
  //   [0] beq $a0, $0, +5  (branches to word 6 = `addiu $v0, $0, 777`)
  //   [1] nop              (branch delay slot — must stay attached to [0])
  //   [2..5] 4 removable addiu filler words (word 4 carries a removable reloc)
  //   [6] addiu $v0, $0, 777  (trigger word, also targeted by branch [0] and jump table!)
  //   [7] addiu $t0, $0, 1    (carries a surviving relocation)
  //   [8] jr $ra
  //   [9] nop
  const uint32_t trigger_word = MakeAddiu(2, 0, 777);
  const uint32_t reloc_word = MakeAddiu(8, 0, 1);

  Stage0FunctionInput input;
  input.machine_words = {
      MakeBeq(4, 0, 5),      // [0] -> target = 0 + 1 + 5 = 6
      kMipsNop,              // [1] delay slot
      MakeAddiu(9, 0, 10),   // [2] filler
      MakeAddiu(10, 0, 20),  // [3] filler
      MakeAddiu(11, 0, 30),  // [4] filler + reloc
      MakeAddiu(12, 0, 40),  // [5] filler
      trigger_word,          // [6] branch & jump-table target
      reloc_word,            // [7] surviving reloc
      kMipsJrRa,             // [8] return
      kMipsNop,              // [9] return delay slot
  };
  input.relocations = {
      Stage0Relocation{.word_index = 4, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "drop"},
      Stage0Relocation{.word_index = 7, .kind = Stage0RelocKind::kMipsLo16, .symbol_name = "keep"},
  };
  input.jump_tables = {
      Stage0JumpTable{.label_name = "$L_jt", .target_word_indices = {6}},
  };

  // Positive: require trigger word AND surviving relocation on `reloc_word` AND the branch.
  const Stage0FunctionInput shrunk =
      GreedyShrinkStage0Input(input, [&](const Stage0FunctionInput& cand) {
        bool has_branch = false;
        bool has_trigger = false;
        bool has_keep_reloc = false;
        for (uint32_t w : cand.machine_words) {
          if (((w >> 26) & 0x3fu) == 4u) {
            has_branch = true;
          }
          if (w == trigger_word) {
            has_trigger = true;
          }
        }
        for (const Stage0Relocation& r : cand.relocations) {
          if (r.symbol_name == "keep" && r.word_index < cand.machine_words.size() &&
              cand.machine_words[r.word_index] == reloc_word) {
            has_keep_reloc = true;
          }
        }
        return has_branch && has_trigger && has_keep_reloc;
      });

  // Filler words [2..5] should be deleted: 10 -> 6 words:
  //   [0] beq $a0, $0, +1  (0 + 1 + 1 == 2 == new trigger index!)
  //   [1] nop
  //   [2] trigger_word
  //   [3] reloc_word
  //   [4] jr $ra
  //   [5] nop
  ASSERT_EQ(shrunk.machine_words.size(), 6u);
  EXPECT_EQ(shrunk.machine_words[0], MakeBeq(4, 0, 1));
  EXPECT_EQ(shrunk.machine_words[1], kMipsNop);
  EXPECT_EQ(shrunk.machine_words[2], trigger_word);
  EXPECT_EQ(shrunk.machine_words[3], reloc_word);
  EXPECT_EQ(shrunk.machine_words[4], kMipsJrRa);
  EXPECT_EQ(shrunk.machine_words[5], kMipsNop);

  ASSERT_EQ(shrunk.relocations.size(), 1u);
  EXPECT_EQ(shrunk.relocations[0].symbol_name, "keep");
  EXPECT_EQ(shrunk.relocations[0].word_index, 3u);

  ASSERT_EQ(shrunk.jump_tables.size(), 1u);
  EXPECT_EQ(shrunk.jump_tables[0].target_word_indices, (std::vector<size_t>{2}));

  // Negative: when predicate never holds for reduced candidates, input is untouched.
  const Stage0FunctionInput unshrunk =
      GreedyShrinkStage0Input(input, [](const Stage0FunctionInput&) { return false; });
  EXPECT_EQ(unshrunk.machine_words, input.machine_words);
  EXPECT_EQ(unshrunk.relocations, input.relocations);
  EXPECT_EQ(unshrunk.jump_tables, input.jump_tables);
}

}  // namespace
}  // namespace rom_nom_nom::lifter
