#include "lifter/rtl_cfg_utils.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

RtlBasicBlock MakeBlock(uint32_t id, bool has_label, std::vector<std::string> insns,
                        std::vector<size_t> succs = {}, std::vector<size_t> addr_vec_targets = {}) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.has_label = has_label;
  blk.instructions = std::move(insns);
  blk.successors = std::move(succs);
  blk.addr_vec_targets = std::move(addr_vec_targets);
  return blk;
}

RtlExpr MustParse(std::string_view sexpr) {
  auto parsed = ParsePatternAndNotes(sexpr);
  EXPECT_TRUE(parsed.ok()) << sexpr;
  return parsed.ok() ? parsed->pattern : RtlExpr::MakeNil();
}

TEST(RtlCfgUtilsTest, GccMaxSavedConstIntMatchesEmitRtlConstant) {
  EXPECT_EQ(kGccMaxSavedConstInt, 64);
}

TEST(RtlCfgUtilsTest, ValidatesRtlExprSubtreeAndPatternSexpr) {
  EXPECT_TRUE(IsValidRtlPatternSexpr("(set (reg:SI 2 v0) (const_int 1))"));
  EXPECT_TRUE(IsValidRtlPatternSexpr("(parallel[ (return) (use (reg:SI 31 ra)) ] )"));
  EXPECT_TRUE(
      IsValidRtlPatternSexpr("(sequence [(jump_insn (set (pc) (label_ref L0))) "
                             "(insn (set (reg:SI 2 v0) (const_int 0)))])"));

  // Negative cases: bare (return), unknown opcode, single-element sequence,
  // non-insn sequence child, bad note subtree, and unparseable syntax.
  EXPECT_FALSE(IsValidRtlPatternSexpr("(return)"));
  EXPECT_FALSE(IsValidRtlPatternSexpr("(set (pc) (return))"));
  EXPECT_FALSE(IsValidRtlPatternSexpr("(bogus_opcode (reg:SI 2 v0))"));
  EXPECT_FALSE(IsValidRtlPatternSexpr("(sequence [(insn (set (reg:SI 2 v0) (const_int 0)))])"));
  EXPECT_FALSE(IsValidRtlPatternSexpr("(sequence [(reg:SI 2 v0) (reg:SI 3 v1)])"));
  EXPECT_FALSE(
      IsValidRtlPatternSexpr("(set (reg:SI 2 v0) (const_int 1)) "
                             "(expr_list:REG_DEAD (return) (nil))"));
  EXPECT_FALSE(IsValidRtlPatternSexpr("(set (reg:SI 2 v0)"));
}

TEST(RtlCfgUtilsTest, ValidatesCfgUnderAllEmptyBlockPolicies) {
  RtlControlFlowGraph empty_cfg;
  EXPECT_FALSE(IsValidCfg(empty_cfg));

  RtlControlFlowGraph trailing_empty_cfg;
  trailing_empty_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:SI 2 v0) (const_int 0))"}, {1}),
      MakeBlock(1, true, {}, {}),
  };
  EXPECT_TRUE(IsValidCfg(trailing_empty_cfg, CfgEmptyBlockPolicy::kAllowAnyEmptyBlock));
  EXPECT_TRUE(IsValidCfg(trailing_empty_cfg, CfgEmptyBlockPolicy::kAllowTrailingEmptyBlock));
  EXPECT_TRUE(
      IsValidCfg(trailing_empty_cfg, CfgEmptyBlockPolicy::kAllowTrailingTerminalEmptyBlockOnly));

  // Trailing empty block with an outgoing successor is rejected only under
  // `kAllowTrailingTerminalEmptyBlockOnly`.
  RtlControlFlowGraph trailing_empty_with_succ = trailing_empty_cfg;
  trailing_empty_with_succ.rpo_blocks[1].successors = {0};
  EXPECT_TRUE(IsValidCfg(trailing_empty_with_succ, CfgEmptyBlockPolicy::kAllowTrailingEmptyBlock));
  EXPECT_FALSE(IsValidCfg(trailing_empty_with_succ,
                          CfgEmptyBlockPolicy::kAllowTrailingTerminalEmptyBlockOnly));

  // Interior empty block is allowed only under `kAllowAnyEmptyBlock`.
  RtlControlFlowGraph interior_empty_cfg;
  interior_empty_cfg.rpo_blocks = {
      MakeBlock(0, false, {}, {1}),
      MakeBlock(1, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };
  EXPECT_TRUE(IsValidCfg(interior_empty_cfg, CfgEmptyBlockPolicy::kAllowAnyEmptyBlock));
  EXPECT_FALSE(IsValidCfg(interior_empty_cfg, CfgEmptyBlockPolicy::kAllowTrailingEmptyBlock));
  EXPECT_FALSE(
      IsValidCfg(interior_empty_cfg, CfgEmptyBlockPolicy::kAllowTrailingTerminalEmptyBlockOnly));

  // Out-of-range successor and out-of-range addr_vec target are rejected.
  RtlControlFlowGraph bad_succ = trailing_empty_cfg;
  bad_succ.rpo_blocks[0].successors = {5};
  EXPECT_FALSE(IsValidCfg(bad_succ));

  RtlControlFlowGraph bad_addr_vec = trailing_empty_cfg;
  bad_addr_vec.rpo_blocks[0].addr_vec_targets = {9};
  EXPECT_FALSE(IsValidCfg(bad_addr_vec));
}

TEST(RtlCfgUtilsTest, SynchronizeCanonicalLabelsHandlesBranchesSequencesAndTablejumps) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks = {
      // B0: conditional branch to B2 with fallthrough B1 (successors reversed
      // to exercise `normalize_cond_branch_successor_order`).
      MakeBlock(
          0, false,
          {
              "(sequence [(jump_insn (set (pc) (if_then_else (eq (reg:SI 4 a0) (const_int 0)) "
              "(label_ref L99) (pc)))) (insn (set (reg:SI 2 v0) (const_int 1)))])",
          },
          {1, 2}),
      // B1: tablejump referencing old jump-table label `L42` into B2/B3.
      MakeBlock(1, false,
                {
                    "(set (reg:SI 3 v1) (high:SI (label_ref L42)))",
                    "(parallel[ (set (pc) (reg:SI 3 v1)) (use (label_ref L42)) ] )",
                },
                {2, 3}, {2, 3}),
      // B2: jumps unconditionally to B3.
      MakeBlock(2, false, {"(set (pc) (label_ref L77))"}, {3}),
      // B3: return.
      MakeBlock(3, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };

  SynchronizeCanonicalLabels(&cfg, CanonicalLabelSyncOptions{
                                       .normalize_cond_branch_successor_order = true,
                                   });

  // B1 has tablejump -> table label gets L0; B2 gets L1; B3 gets L2.
  EXPECT_FALSE(cfg.rpo_blocks[0].has_label);
  EXPECT_FALSE(cfg.rpo_blocks[1].has_label);
  EXPECT_TRUE(cfg.rpo_blocks[2].has_label);
  EXPECT_TRUE(cfg.rpo_blocks[3].has_label);

  // Successor order normalized so taken target B2 is first, fallthrough B1 second.
  ASSERT_EQ(cfg.rpo_blocks[0].successors.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors[0], 2u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors[1], 1u);

  // Sequence jump target rewritten from L99 to B2's canonical L1.
  EXPECT_NE(cfg.rpo_blocks[0].instructions[0].find("(label_ref L1)"), std::string::npos);
  // Tablejump `L42` remapped to canonical L0 in both address setup and jump.
  EXPECT_NE(cfg.rpo_blocks[1].instructions[0].find("(label_ref L0)"), std::string::npos);
  EXPECT_NE(cfg.rpo_blocks[1].instructions[1].find("(label_ref L0)"), std::string::npos);
  // B2 unconditional jump rewritten from L77 to B3's canonical L2.
  EXPECT_NE(cfg.rpo_blocks[2].instructions[0].find("(label_ref L2)"), std::string::npos);
}

TEST(RtlCfgUtilsTest, BuildPredecessorsAndInsertBlockAtMaintainTopology) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (pc) (label_ref L0))"}, {1}, {1, 1}),
      MakeBlock(1, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };

  const auto preds = BuildPredecessors(cfg);
  ASSERT_EQ(preds.size(), 2u);
  EXPECT_TRUE(preds[0].empty());
  // Duplicate `addr_vec_targets` entry does not duplicate predecessor 0.
  EXPECT_EQ(preds[1], std::vector<size_t>({0}));

  InsertBlockAt(&cfg, 1, MakeBlock(1, false, {"(set (reg:SI 2 v0) (const_int 0))"}, {2}));
  ASSERT_EQ(cfg.rpo_blocks.size(), 3u);
  EXPECT_EQ(cfg.rpo_blocks[0].successors, std::vector<size_t>({2}));
  EXPECT_EQ(cfg.rpo_blocks[0].addr_vec_targets, std::vector<size_t>({2, 2}));
  EXPECT_EQ(cfg.rpo_blocks[1].successors, std::vector<size_t>({2}));
}

TEST(RtlCfgUtilsTest, ClassifiesAndInvertsControlFlowPatterns) {
  const RtlExpr ret = MustParse("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
  const RtlExpr uncond = MustParse("(set (pc) (label_ref L0))");
  const RtlExpr tbl = MustParse("(parallel[ (set (pc) (reg:SI 2 v0)) (use (label_ref L1)) ] )");
  RtlExpr cond = MustParse(
      "(set (pc) (if_then_else (lt (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L2) (pc)))");
  const RtlExpr alu = MustParse("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");

  EXPECT_TRUE(IsReturnExpr(ret));
  EXPECT_FALSE(IsReturnExpr(alu));
  EXPECT_TRUE(IsSimpleUncondJumpExpr(uncond));
  EXPECT_TRUE(IsSimpleUncondJumpInsn("(set (pc) (label_ref L0))"));
  EXPECT_FALSE(IsSimpleUncondJumpInsn("(set (reg:SI 2 v0) (const_int 0))"));
  EXPECT_TRUE(IsTableJumpExpr(tbl));
  EXPECT_FALSE(IsTableJumpExpr(ret));
  EXPECT_TRUE(IsCondJumpExpr(cond));
  EXPECT_FALSE(IsCondJumpExpr(uncond));

  EXPECT_TRUE(IsAnyJumpOrReturnTopExpr(ret));
  EXPECT_TRUE(IsAnyJumpOrReturnTopExpr(uncond));
  EXPECT_TRUE(IsAnyJumpOrReturnTopExpr(tbl));
  EXPECT_TRUE(IsAnyJumpOrReturnTopExpr(cond));
  EXPECT_FALSE(IsAnyJumpOrReturnTopExpr(alu));

  EXPECT_TRUE(IsUncondJumpOrReturnOrTableJumpTopExpr(ret));
  EXPECT_TRUE(IsUncondJumpOrReturnOrTableJumpTopExpr(uncond));
  EXPECT_TRUE(IsUncondJumpOrReturnOrTableJumpTopExpr(tbl));
  EXPECT_FALSE(IsUncondJumpOrReturnOrTableJumpTopExpr(cond));

  EXPECT_TRUE(IsEpilogueOrControlTopExpr(MustParse("(use (reg/i:SI 2 v0))")));
  EXPECT_TRUE(IsEpilogueOrControlTopExpr(MustParse("(clobber (reg:SI 2 v0))")));
  EXPECT_FALSE(IsEpilogueOrControlTopExpr(alu));

  EXPECT_EQ(ReverseComparisonOp(RtlOp::kEq), RtlOp::kNe);
  EXPECT_EQ(ReverseComparisonOp(RtlOp::kLt), RtlOp::kGe);
  EXPECT_EQ(ReverseComparisonOp(RtlOp::kLeu), RtlOp::kGtu);
  EXPECT_EQ(ReverseComparisonOp(RtlOp::kPlus), RtlOp::kUnknown);

  EXPECT_TRUE(TryInvertCondJumpExpr(&cond));
  const RtlExpr* ite = GetCondJumpIfThenElse(cond);
  ASSERT_NE(ite, nullptr);
  EXPECT_EQ(ite->children[0].op, RtlOp::kGe);

  RtlExpr non_cond = alu;
  EXPECT_FALSE(TryInvertCondJumpExpr(&non_cond));

  EXPECT_TRUE(IsLoopHeaderNote(RtlExpr::MakeNote("NOTE_INSN_LOOP_CONT")));
  EXPECT_TRUE(IsLoopHeaderNote(RtlExpr::MakeNote("NOTE_INSN_LOOP_VTOP")));
  EXPECT_FALSE(IsLoopHeaderNote(RtlExpr::MakeNote("NOTE_INSN_LOOP_BEG")));
  EXPECT_FALSE(IsLoopHeaderNote(alu));
}

TEST(RtlCfgUtilsTest, RegisterConflictAndPrologueHelpers) {
  const RtlExpr fp = MakeGccHardReg(RtlMachineMode::kSi, 30);
  EXPECT_EQ(fp.RegNo(), 30);
  EXPECT_EQ(fp.str_val, "$fp");

  const RtlExpr sub = MustParse("(subreg:SI (reg:DI 4 a0) 1)");
  EXPECT_EQ(TrueRegnum(sub), 5);
  EXPECT_EQ(TrueRegnum(MustParse("(const_int 0)")), -1);

  EXPECT_TRUE(
      ContainsCallInExpr(MustParse("(call (mem:QI (symbol_ref:SI (\"foo\"))) (const_int 16))")));
  EXPECT_FALSE(ContainsCallInExpr(MustParse("(set (reg:SI 2 v0) (const_int 0))")));

  EXPECT_TRUE(IsMipsO32CallArgReg(4));
  EXPECT_TRUE(IsMipsO32CallArgReg(7));
  EXPECT_TRUE(IsMipsO32CallArgReg(44));
  EXPECT_TRUE(IsMipsO32CallArgReg(47));
  EXPECT_FALSE(IsMipsO32CallArgReg(2));
  EXPECT_FALSE(IsMipsO32CallArgReg(8));
  EXPECT_FALSE(IsMipsO32CallArgReg(48));

  const RtlExpr df_set = MustParse("(set (reg:DF 32 $f0) (reg:DF 44 $f12))");
  EXPECT_TRUE(SubtreeReferencesHardReg(df_set, 33));
  EXPECT_TRUE(SubtreeReferencesHardReg(df_set, 45));
  EXPECT_FALSE(SubtreeReferencesHardReg(df_set, 34));
  EXPECT_FALSE(ExprReferencesRegInSrcOrMem(df_set, 32));
  EXPECT_TRUE(ExprReferencesRegInSrcOrMem(df_set, 45));

  const RtlExpr store_sp =
      MustParse("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 31 ra))");
  EXPECT_TRUE(ExprHasMem(store_sp));
  EXPECT_TRUE(ExprReferencesRegInSrcOrMem(store_sp, 29));
  EXPECT_TRUE(ExprReferencesRegInSrcOrMem(store_sp, 31));
  int saved_reg = -1;
  int64_t sp_off = -1;
  EXPECT_TRUE(IsPrologueStackRegSave(store_sp, &saved_reg, &sp_off));
  EXPECT_EQ(saved_reg, 31);
  EXPECT_EQ(sp_off, 20);

  const RtlExpr non_sp_store = MustParse("(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0))");
  EXPECT_FALSE(IsPrologueStackRegSave(non_sp_store, &saved_reg, &sp_off));

  const RtlExpr def_v0 = MustParse("(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
  const RtlExpr use_v0 = MustParse("(set (reg:SI 3 v1) (reg:SI 2 v0))");
  const RtlExpr indep = MustParse("(set (reg:SI 8 t0) (const_int 7))");
  EXPECT_TRUE(ExpressionsConflict(def_v0, use_v0));
  EXPECT_FALSE(ExpressionsConflict(def_v0, indep));

  auto dead_pn = ParsePatternAndNotes(
      "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  ASSERT_TRUE(dead_pn.ok());
  EXPECT_TRUE(HasRegDeadForRegno(*dead_pn, 4));
  EXPECT_FALSE(HasRegDeadForRegno(*dead_pn, 5));
}

}  // namespace
}  // namespace rom_nom_nom::lifter
