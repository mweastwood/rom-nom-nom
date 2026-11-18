#include "lifter/invert_do_cross_jump.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionDoCrossJumpProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
    return lifter::InvertDoCrossJump(jump2_cfg_x);
  }
  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                        const CompilationProfile& profile) const override {
    return lifter::InvertDoCrossJump(jump2_cfg_x, profile);
  }
};

class BrokenDoCrossJumpProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
    return Corrupt(jump2_cfg_x);
  }
  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                        const CompilationProfile& /*profile*/) const override {
    return Corrupt(jump2_cfg_x);
  }

 private:
  static RtlControlFlowGraph Corrupt(const RtlControlFlowGraph& jump2_cfg_x) {
    RtlControlFlowGraph out = jump2_cfg_x;
    out.pass = RtlPassKind::kSched2;
    if (!out.rpo_blocks.empty()) {
      out.rpo_blocks[0].instructions = {
          "(set (reg:SI 2 v0) (const_int 9999))",
          "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
      };
      out.rpo_blocks[0].successors.clear();
      out.rpo_blocks.resize(1);
      out.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }
    return out;
  }
};

RtlBasicBlock MakeBlock(uint32_t id, bool has_label, std::vector<std::string> instructions,
                        std::vector<size_t> successors) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.has_label = has_label;
  blk.instructions = std::move(instructions);
  blk.successors = std::move(successors);
  blk.SyncRtlInsnsFromInstructions();
  return blk;
}

bool CfgContainsReturnOp(const RtlControlFlowGraph& cfg) {
  bool found = false;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    EXPECT_EQ(blk.rtl_insns.size(), blk.instructions.size());
    for (const RtlInsn& rinsn : blk.rtl_insns) {
      const absl::StatusOr<RtlExpr> expr = rinsn.ParseExpr();
      EXPECT_TRUE(expr.ok()) << expr.status();
      if (!expr.ok()) {
        continue;
      }
      expr->Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kReturn) {
          found = true;
        }
      });
    }
  }
  return found;
}

class InvertDoCrossJumpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RtlControlFlowGraph dummy;
    hooks_.ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  GccContext ctx_;
  FunctionHooks hooks_{&ctx_};
  ProductionDoCrossJumpProvider provider_;
  RightInverseVerifier verifier_{&ctx_, &hooks_, &provider_};
};

TEST_F(InvertDoCrossJumpTest, UnmergesCrossJumpedTailBeforeReturn) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 6 a2) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 10))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 6 a2) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 20))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph pre = InvertDoCrossJump(cfg);
  ASSERT_EQ(pre.rpo_blocks.size(), 4u);
  EXPECT_EQ(pre.pass, RtlPassKind::kSched2);
  EXPECT_EQ(pre.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(pre.rpo_blocks[2].instructions.size(), 3u);
  EXPECT_EQ(pre.rpo_blocks[3].instructions.size(), 2u);
  EXPECT_EQ(pre.rpo_blocks[3].instructions.back(), "(use (reg/i:SI 2 v0))");

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertDoCrossJumpTest, UnmergesCrossJumpedTailBeforeUnconditionalJump) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int 0)) "
                 "(label_ref L2) (pc)))"},
                {5, 1}),
      MakeBlock(1, false,
                {"(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {3, 2}),
      MakeBlock(2, false,
                {
                    "(set (reg:SI 7 a3) (const_int 1))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 4)))",
                    "(set (pc) (label_ref L1))",
                },
                {4}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 7 a3) (const_int 2))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 8)))",
                },
                {4}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 3 v1) (ashift:SI (reg:SI 3 v1) (const_int 2)))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                    "(set (pc) (label_ref L3))",
                },
                {6}),
      MakeBlock(5, true,
                {
                    "(set (reg:SI 7 a3) (const_int 3))",
                    "(set (reg:SI 2 v0) (const_int 99))",
                },
                {6}),
      MakeBlock(6, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 7 a3)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph pre = InvertDoCrossJump(cfg);
  ASSERT_EQ(pre.rpo_blocks.size(), 7u);
  EXPECT_EQ(pre.pass, RtlPassKind::kSched2);
  EXPECT_EQ(pre.rpo_blocks[2].instructions.size(), 4u);
  EXPECT_EQ(pre.rpo_blocks[3].instructions.size(), 3u);
  EXPECT_EQ(pre.rpo_blocks[4].instructions.size(), 2u);

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertDoCrossJumpTest, UnmergesMultiPredecessorCrossJumpedTail) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 7 a3) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 11))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(2, true,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                 "(label_ref L1) (pc)))"},
                {4, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg:SI 7 a3) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 22))",
                    "(set (pc) (label_ref L2))",
                },
                {5}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 7 a3) (const_int 3))",
                    "(set (reg:SI 3 v1) (const_int 33))",
                },
                {5}),
      MakeBlock(5, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2)))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph pre = InvertDoCrossJump(cfg);
  ASSERT_EQ(pre.rpo_blocks.size(), 6u);
  EXPECT_EQ(pre.pass, RtlPassKind::kSched2);
  EXPECT_EQ(pre.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(pre.rpo_blocks[3].instructions.size(), 4u);
  EXPECT_EQ(pre.rpo_blocks[4].instructions.size(), 3u);
  EXPECT_EQ(pre.rpo_blocks[5].instructions.size(), 2u);

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertDoCrossJumpTest, UnmergesCrossJumpedCallAndPostCallTail) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 17 s1) (const_int 10))",
                    "(set (reg:SI 16 s0) (const_int 1))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 17 s1) (const_int 20))",
                    "(set (reg:SI 16 s0) (const_int 2))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (reg:SI 25 t9)) "
                    "(const_int 0))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
                    "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                    "(use (reg/i:SI 2 v0))",
                },
                {}),
  };
  cfg.rpo_blocks[3].rtl_insns[0].has_explicit_call_fusage = true;
  cfg.rpo_blocks[3].rtl_insns[0].call_fusage.clear();

  const RtlControlFlowGraph pre = InvertDoCrossJump(cfg);
  ASSERT_EQ(pre.rpo_blocks.size(), 4u);
  EXPECT_EQ(pre.pass, RtlPassKind::kSched2);
  EXPECT_EQ(pre.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(pre.rpo_blocks[2].instructions.size(), 3u);
  EXPECT_EQ(pre.rpo_blocks[3].instructions.size(), 2u);

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertDoCrossJumpTest, PreservesRegNotesAcrossCrossJumpUnmerge) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 6 a2) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 10))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 6 a2) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 20))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1)) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph pre = InvertDoCrossJump(cfg);
  ASSERT_EQ(pre.rpo_blocks.size(), 4u);
  EXPECT_EQ(pre.pass, RtlPassKind::kSched2);

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertDoCrossJumpTest, PreservesJoinBlockWhenPredecessorEndsWithConditionalJump) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg:SI 2 v0) (const_int 1))",
                 "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 2 v0) (const_int 2))",
                },
                {2}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RtlControlFlowGraph pre = InvertDoCrossJump(cfg);
  ASSERT_EQ(pre.rpo_blocks.size(), 3u);
  EXPECT_EQ(pre.pass, RtlPassKind::kSched2);
  EXPECT_EQ(pre.rpo_blocks[2].instructions.size(), 2u);

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail << "\nexpected:\n"
                           << res.expected_summary << "\nactual:\n"
                           << res.actual_summary;
}

TEST_F(InvertDoCrossJumpTest, RejectsInvalidAndMalformedCfgs) {
  // 1. Empty CFG.
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertDoCrossJump(empty_cfg).rpo_blocks.empty());

  // 2. Single block with empty instructions.
  RtlControlFlowGraph empty_blk_cfg;
  empty_blk_cfg.rpo_blocks.push_back(MakeBlock(0, false, {}, {}));
  EXPECT_TRUE(InvertDoCrossJump(empty_blk_cfg).rpo_blocks.empty());

  // 3. Non-trailing empty block.
  RtlControlFlowGraph mid_empty_blk_cfg;
  mid_empty_blk_cfg.rpo_blocks = {
      MakeBlock(0, false, {}, {1}),
      MakeBlock(1, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };
  EXPECT_TRUE(InvertDoCrossJump(mid_empty_blk_cfg).rpo_blocks.empty());

  // 4. Out-of-bounds successor index.
  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(set (pc) (label_ref L0))"}, {5}));
  EXPECT_TRUE(InvertDoCrossJump(bad_succ_cfg).rpo_blocks.empty());

  // 5. Out-of-bounds addr_vec_targets index.
  RtlControlFlowGraph bad_addr_vec_cfg;
  bad_addr_vec_cfg.rpo_blocks.push_back(
      MakeBlock(0, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}));
  bad_addr_vec_cfg.rpo_blocks[0].addr_vec_targets = {3};
  EXPECT_TRUE(InvertDoCrossJump(bad_addr_vec_cfg).rpo_blocks.empty());

  // 6. Malformed S-expression and synthetic bare `(return)`.
  RtlControlFlowGraph bad_sexpr_cfg;
  bad_sexpr_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(set (reg:SI 2 v0) (plus:SI"}, {}));
  EXPECT_TRUE(InvertDoCrossJump(bad_sexpr_cfg).rpo_blocks.empty());

  RtlControlFlowGraph bare_ret_cfg;
  bare_ret_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(return)"}, {}));
  EXPECT_TRUE(InvertDoCrossJump(bare_ret_cfg).rpo_blocks.empty());

  // 7. Unknown RTL opcode in instruction pattern.
  RtlControlFlowGraph unknown_op_cfg;
  unknown_op_cfg.rpo_blocks.push_back(MakeBlock(0, false, {"(bogus_rtl_op (reg:SI 2 v0))"}, {}));
  EXPECT_TRUE(InvertDoCrossJump(unknown_op_cfg).rpo_blocks.empty());

  // 8. Unknown RTL opcode in REG_NOTE datum.
  RtlControlFlowGraph unknown_note_cfg;
  unknown_note_cfg.rpo_blocks.push_back(MakeBlock(
      0, false, {"(set (reg:SI 2 v0) (const_int 1)) (expr_list:REG_EQUAL (bogus_op 1) (nil))"},
      {}));
  EXPECT_TRUE(InvertDoCrossJump(unknown_note_cfg).rpo_blocks.empty());

  // 9. Malformed sequence with fewer than 2 children.
  RtlControlFlowGraph bad_seq_cfg;
  bad_seq_cfg.rpo_blocks.push_back(MakeBlock(
      0, false, {"(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] ))])"}, {}));
  EXPECT_TRUE(InvertDoCrossJump(bad_seq_cfg).rpo_blocks.empty());
}

TEST_F(InvertDoCrossJumpTest, VerifierRejectsCorruptedInverse) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 6 a2) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 10))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 6 a2) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 20))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                    "(set (reg:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  BrokenDoCrossJumpProvider broken_provider;
  RightInverseVerifier broken_verifier(&ctx_, &hooks_, &broken_provider);
  const RightInverseResult res = broken_verifier.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_FALSE(res.matches);
}

TEST_F(InvertDoCrossJumpTest, RestoresDeletedNoopCallArgMoveOnlyForUsedArgWithoutExistingRegDead) {
  // Entry block saves `$16` (`$4`) and `$17` (`$5`) before saving `$31` (`ra`), then calls a
  // 1-argument `callee($4)` (`call_fusage` has `(use (reg:SI 4 a0))` only; `(set $16 $4)` had its
  // `REG_DEAD(4)` stripped by `jump.c:remove_death`, while `(set $17 $5)` retains `REG_DEAD(5)`).
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.sideband.outgoing_args_size = 16;
  cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29, 31};
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 32)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0))",
                    "(set (reg:SI 16 s0) (reg:SI 4 a0))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 17 s1))",
                    "(set (reg:SI 17 s1) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 31 ra))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"one_arg_fn\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0))) "
                    "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 17 s1))) "
                    "(expr_list:REG_DEAD (reg:SI 17 s1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))",
                    "(set (reg:SI 17 s1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
                    "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 32)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  cfg.rpo_blocks[0].rtl_insns[6].has_explicit_call_fusage = true;
  cfg.rpo_blocks[0].rtl_insns[6].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };

  const RtlControlFlowGraph pre = InvertDoCrossJump(cfg);
  ASSERT_EQ(pre.rpo_blocks.size(), 1u);
  ASSERT_EQ(pre.rpo_blocks[0].instructions.size(), 16u);
  EXPECT_EQ(pre.rpo_blocks[0].instructions[2],
            "(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  EXPECT_EQ(pre.rpo_blocks[0].instructions[4],
            "(set (reg:SI 17 s1) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  EXPECT_EQ(pre.rpo_blocks[0].instructions[5], "(set (reg:SI 4 a0) (reg:SI 16 s0))");
  EXPECT_EQ(pre.rpo_blocks[0].instructions[6],
            "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 31 ra))");
  ASSERT_EQ(pre.rpo_blocks[0].rtl_insns[7].call_fusage.size(), 1u);

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertDoCrossJumpTest, RestoresDeletedNoopMdOutputReloadAndPassesRightInverse) {
  // 1. Exact `.11.jump2` image (`REG_DEAD (reg:DI 64)` on `ashift:DI`, none on `mfhi/mflo`):
  //    must satisfy `VerifyDoCrossJumpRightInverse` directly.
  RtlControlFlowGraph cfg_jump2;
  cfg_jump2.pass = RtlPassKind::kJump2;
  cfg_jump2.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 65 lo) (reg:SI 4 a0))",
                    "(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))",
                    "(set (reg:SI 64 hi) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg:SI 6 a2) (ashift:SI (reg:SI 6 a2) (const_int 16)))",
                    "(set (reg:DI 10 t2) (reg:DI 64 hi))",
                    "(set (reg:SI 5 a1) (ashiftrt:SI (reg:SI 6 a2) (const_int 16)))",
                    "(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 6 a2) (const_int 31))) "
                    "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                    "(parallel[ (set (reg:DI 2 v0) (ashift:DI (reg:DI 10 t2) (const_int 4))) "
                    "(clobber (reg:SI 6 a2)) ] ) (expr_list:REG_DEAD (reg:DI 10 t2) "
                    "(expr_list:REG_DEAD (reg:DI 64 hi) (nil)))",
                    "(parallel[ (set (reg/i:DI 2 v0) (minus:DI (reg:DI 2 v0) (reg:DI 4 a0))) "
                    "(clobber (reg:SI 6 a2)) ] ) (expr_list:REG_DEAD (reg:DI 4 a0) (nil))",
                    "(use (reg/i:DI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const CompilationProfile profile{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};
  const RtlControlFlowGraph pre_jump2 = InvertDoCrossJump(cfg_jump2, profile);
  ASSERT_EQ(pre_jump2.rpo_blocks.size(), 1u);
  ASSERT_EQ(pre_jump2.rpo_blocks[0].instructions.size(), 11u);
  EXPECT_EQ(pre_jump2.rpo_blocks[0].instructions[7], "(set (reg:DI 64 hi) (reg:DI 10 t2))");

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg_jump2);
  EXPECT_TRUE(res.matches) << res.diff_detail;

  // 2. Stage 1 reconstructed-liveness input (`REG_DEAD (reg:DI 64)` on `mfhi/mflo`):
  //    must also restore `(set (reg:DI 64 hi) (reg:DI 10 t2))` and attach
  //    `REG_DEAD (reg:DI 64)` onto `ashift:DI` for Stage 3 `sched2`.
  RtlControlFlowGraph cfg_stage1 = cfg_jump2;
  cfg_stage1.rpo_blocks[0].instructions[4] =
      "(set (reg:DI 10 t2) (reg:DI 64 hi)) (expr_list:REG_DEAD (reg:DI 64 hi) (nil))";
  cfg_stage1.rpo_blocks[0].instructions[7] =
      "(parallel[ (set (reg:DI 2 v0) (ashift:DI (reg:DI 10 t2) (const_int 4))) "
      "(clobber (reg:SI 6 a2)) ] ) (expr_list:REG_DEAD (reg:DI 10 t2) (nil))";
  cfg_stage1.rpo_blocks[0].SyncRtlInsnsFromInstructions();

  const RtlControlFlowGraph pre_stage1 = InvertDoCrossJump(cfg_stage1, profile);
  ASSERT_EQ(pre_stage1.rpo_blocks.size(), 1u);
  ASSERT_EQ(pre_stage1.rpo_blocks[0].instructions.size(), 11u);
  EXPECT_EQ(pre_stage1.rpo_blocks[0].instructions[7], "(set (reg:DI 64 hi) (reg:DI 10 t2))");
  const auto replayed = hooks_.ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, pre_stage1,
                                                          {}, 2, {"-O2", "-mips1"});
  EXPECT_TRUE(replayed.post_state.CompareAgainst(cfg_jump2).MatchesExactly());
}

TEST_F(InvertDoCrossJumpTest,
       UnexpandsZeroFramePostReloadTrailingReturnInsnAndPassesBoundaryRightInverse) {
  // In KMC GCC 2.7.2 MIPS (`mips.md:5928-5941`, `jump.c:289-306`), `.10.sched2`
  // never contains `return_internal`; `jump2` (`SubfunctionId::kDoCrossJump`)
  // appends it at the end of a `simple_epilogue_p()` leaf function. Verify
  // `InvertDoCrossJump` strips the trailing `return_internal` (including `$31`
  // from `saved_gpr_regnos`) and satisfies both `VerifyDoCrossJumpRightInverse`
  // and `VerifySubfunctionBoundaryRightInverse(SubfunctionId::kDoCrossJump)`.
  RtlControlFlowGraph leaf_cfg;
  leaf_cfg.pass = RtlPassKind::kJump2;
  leaf_cfg.sideband.saved_gpr_regnos = {2, 4, 31};
  leaf_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 6 a2) (const_int 1))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 7)))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 6 a2) (const_int 2))",
                    "(set (reg/i:SI 2 v0) (const_int 9))",
                },
                {3}),
      MakeBlock(3, true, {"(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };

  const RtlControlFlowGraph lifted_leaf = InvertDoCrossJump(leaf_cfg);
  ASSERT_EQ(lifted_leaf.rpo_blocks.size(), 4u);
  ASSERT_EQ(lifted_leaf.rpo_blocks[3].instructions.size(), 1u);
  EXPECT_EQ(lifted_leaf.rpo_blocks[3].instructions[0], "(use (reg/i:SI 2 v0))");
  EXPECT_FALSE(lifted_leaf.rpo_blocks[3].has_trailing_barrier);
  EXPECT_EQ(std::find(lifted_leaf.sideband.saved_gpr_regnos.begin(),
                      lifted_leaf.sideband.saved_gpr_regnos.end(), 31),
            lifted_leaf.sideband.saved_gpr_regnos.end());

  const RightInverseResult leaf_res = verifier_.VerifyDoCrossJumpRightInverse(leaf_cfg);
  EXPECT_TRUE(leaf_res.matches) << leaf_res.diff_detail;

  // Negative check: at `-O0` (`jump2` skipped) or when `frame_size_bytes > 0`
  // (`!simple_epilogue_p()`), trailing `return_internal` is not un-expanded.
  const RtlControlFlowGraph o0_lifted =
      InvertDoCrossJump(leaf_cfg, CompilationProfile{.optimize_level = 0});
  ASSERT_EQ(o0_lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(o0_lifted.rpo_blocks[3].instructions.size(), 2u);

  RtlControlFlowGraph framed_cfg = leaf_cfg;
  framed_cfg.sideband.frame_size_bytes = 8;
  const RtlControlFlowGraph framed_lifted = InvertDoCrossJump(framed_cfg);
  ASSERT_EQ(framed_lifted.rpo_blocks.size(), 4u);
  EXPECT_EQ(framed_lifted.rpo_blocks[3].instructions.size(), 2u);

  // Also verify on a real compiled `SubfunctionId::kDoCrossJump` boundary
  // snapshot of a zero-frame leaf (`int branch_or(int a, int b) { return a || b; }`)
  // that `snap.post_state` contains `return_internal`, `InvertDoCrossJump`
  // strips all `(return)` instructions to `.10.sched2` form, and
  // `VerifySubfunctionBoundaryRightInverse(snap)` passes (`matches == true`).
  RoundTripTestCase branch_or_tc;
  branch_or_tc.test_name = "SyntheticZeroFrameDisjunctionLeaf";
  branch_or_tc.function_name = "branch_or";
  branch_or_tc.original_c = "int branch_or(int a, int b) { return a || b; }\n";
  branch_or_tc.options.c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  const auto trace = hooks_.TraceForwardCompilation(branch_or_tc);
  bool found_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kDoCrossJump) {
      continue;
    }
    found_snap = true;
    const CompilationProfile prof{snap.optimize_level, snap.c_flags, snap.gp_symbol_sizes};
    const RtlControlFlowGraph inverted = InvertDoCrossJump(snap.post_state, prof);
    ASSERT_FALSE(inverted.rpo_blocks.empty());
    EXPECT_FALSE(CfgContainsReturnOp(inverted)) << inverted.FormatSummary();
    const RightInverseResult sub_res = verifier_.VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(sub_res.matches) << sub_res.diff_detail;
  }
  EXPECT_TRUE(found_snap);
}

TEST_F(InvertDoCrossJumpTest,
       UnexpandsSingleInteriorReturnBeforeTrailingBackwardJumpAndPreservesMultiReturnAssembly) {
  // 1. When fresh Stage 1 -> Stage 2 composition on a zero-frame leaf with an
  //    out-of-line trailing backward jump (`B2 -> B1`) leaves the single
  //    `return_internal` in interior block `B1` (`ret_b < n - 1`),
  //    `InvertDoCrossJump` replaces `B1`'s `return_internal` with a structured
  //    jump `(set (pc) (label_ref L2))` to synthesized exit block `B3`
  //    containing `(use (reg/i:SI 2 v0))`, and replaying through
  //    `SubfunctionId::kDoCrossJump` produces a canonical `.11.jump2` CFG that
  //    satisfies `VerifyDoCrossJumpRightInverse`.
  RtlControlFlowGraph interior_ret_cfg;
  interior_ret_cfg.pass = RtlPassKind::kJump2;
  interior_ret_cfg.sideband.saved_gpr_regnos = {2, 4, 31};
  interior_ret_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg/i:SI 2 v0) (const_int 1))",
                 "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L1) (pc)))"},
                {2, 1}),
      MakeBlock(1, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(2, true,
                {
                    "(set (reg/i:SI 2 v0) (ne:SI (reg:SI 5 a1) (const_int 0)))",
                    "(set (pc) (label_ref L0))",
                },
                {1}),
  };

  const RtlControlFlowGraph lifted = InvertDoCrossJump(interior_ret_cfg);
  ASSERT_EQ(lifted.rpo_blocks.size(), 4u) << lifted.FormatSummary();
  EXPECT_FALSE(CfgContainsReturnOp(lifted)) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[1].instructions.back(), "(set (pc) (label_ref L2))");
  EXPECT_EQ(lifted.rpo_blocks[1].successors, std::vector<size_t>{3u});
  EXPECT_TRUE(lifted.rpo_blocks[3].has_label);
  ASSERT_EQ(lifted.rpo_blocks[3].instructions.size(), 1u);
  EXPECT_EQ(lifted.rpo_blocks[3].instructions[0], "(use (reg/i:SI 2 v0))");

  const auto replayed = hooks_.ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, lifted, {},
                                                          2, {"-O2", "-mips2", "-mcpu=r4000"});
  const RightInverseResult canon_res = verifier_.VerifyDoCrossJumpRightInverse(replayed.post_state);
  EXPECT_TRUE(canon_res.matches) << canon_res.diff_detail;

  // 2. Hand-written zero-frame assembly with 2+ `return_internal` blocks
  //    (`ret_blocks.size() > 1`, which GCC `jump2` cannot synthesize from a
  //    `return_internal`-free `.10.sched2`) preserves both `return_internal`s
  //    and satisfies `VerifyDoCrossJumpRightInverse` directly.
  RtlControlFlowGraph multi_ret_cfg;
  multi_ret_cfg.pass = RtlPassKind::kJump2;
  multi_ret_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg/i:SI 2 v0) (const_int -1))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(2, true,
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 5))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  const RightInverseResult multi_res = verifier_.VerifyDoCrossJumpRightInverse(multi_ret_cfg);
  EXPECT_TRUE(multi_res.matches) << multi_res.diff_detail;
}

TEST_F(InvertDoCrossJumpTest, RestoresDeletedNoopSharedReturnMoveForFourPredsAndDfReturn) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:DF 34 $f2) (mem:DF (reg:SI 4 a0))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:DF 32 $f0) (plus:DF (reg:DF 34 $f2) (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                    "(set (pc) (label_ref L3))",
                },
                {7}),
      MakeBlock(2, true,
                {
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 1)) "
                    "(label_ref L1) (pc)))",
                },
                {4, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg:DF 32 $f0) (mult:DF (reg:DF 34 $f2) (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                    "(set (pc) (label_ref L3))",
                },
                {7}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 2 v0) (const_int 2))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (reg:SI 2 v0)) "
                    "(label_ref L2) (pc))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil)))",
                },
                {6, 5}),
      MakeBlock(5, false,
                {
                    "(set (reg:DF 32 $f0) (minus:DF (reg:DF 34 $f2) (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                    "(set (pc) (label_ref L3))",
                },
                {7}),
      MakeBlock(6, true,
                {
                    "(set (reg:DF 32 $f0) (neg:DF (reg:DF 34 $f2))) "
                    "(expr_list:REG_DEAD (reg:DF 34 $f2) (nil))",
                },
                {7}),
      MakeBlock(7, true,
                {
                    "(use (reg/i:DF 32 $f0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  cfg.sideband.basic_block_live_at_start = {
      {0, {4, 5}},      {1, {34, 35}}, {2, {5, 34, 35}}, {3, {34, 35}},
      {4, {5, 34, 35}}, {5, {34, 35}}, {6, {34, 35}},    {7, {32, 33}},
  };

  const CompilationProfile no_sched2_profile{
      .optimize_level = 2,
      .c_flags = {"-O2", "-fno-schedule-insns2", "-mips2", "-mcpu=r4000"},
  };
  const RtlControlFlowGraph lifted_no_sched2 = InvertDoCrossJump(cfg, no_sched2_profile);
  ASSERT_EQ(lifted_no_sched2.rpo_blocks.size(), 8u);
  ASSERT_EQ(lifted_no_sched2.rpo_blocks[7].instructions.size(), 2u)
      << lifted_no_sched2.FormatSummary();
  EXPECT_EQ(lifted_no_sched2.rpo_blocks[7].instructions[0],
            "(set (reg/i:DF 32 $f0) (reg:DF 32 $f0)) "
            "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))");
  EXPECT_EQ(lifted_no_sched2.rpo_blocks[7].instructions[1], "(use (reg/i:DF 32 $f0))");

  const CompilationProfile sched2_profile{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"},
  };
  RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, sched2_profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 8u);
  ASSERT_EQ(lifted.rpo_blocks[7].instructions.size(), 1u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[7].instructions[0], "(use (reg/i:DF 32 $f0))");
  RestoreSched2DeletedNoopMoves(&lifted, sched2_profile);
  ASSERT_EQ(lifted.rpo_blocks[7].instructions.size(), 2u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[7].instructions[0],
            "(set (reg/i:DF 32 $f0) (reg:DF 32 $f0)) "
            "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))");

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertDoCrossJumpTest, RestoresSingleDyingMultiBlockArgPreferenceMove) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (const_int 0))",
                },
                {1}),
      MakeBlock(1, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                    "(set (pc) (if_then_else (lt:SI (reg:SI 3 v1) (reg:SI 4 a0)) "
                    "(label_ref L0) (pc)))",
                },
                {1, 2}),
      MakeBlock(2, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 3 v1))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (nil)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  cfg.sideband.basic_block_live_at_start = {
      {0, {4}},
      {1, {3, 4}},
      {2, {3, 4}},
  };

  const CompilationProfile no_sched2_profile{
      .optimize_level = 2,
      .c_flags = {"-O2", "-fno-schedule-insns2", "-mips2", "-mcpu=r4000"},
  };
  const RtlControlFlowGraph lifted_no_sched2 = InvertDoCrossJump(cfg, no_sched2_profile);
  ASSERT_EQ(lifted_no_sched2.rpo_blocks.size(), 3u);
  ASSERT_GE(lifted_no_sched2.rpo_blocks[0].instructions.size(), 2u)
      << lifted_no_sched2.FormatSummary();
  EXPECT_EQ(lifted_no_sched2.rpo_blocks[0].instructions[0],
            "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const CompilationProfile sched2_profile{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"},
  };
  RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, sched2_profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 1u) << lifted.FormatSummary();
  RestoreSched2DeletedNoopMoves(&lifted, sched2_profile);
  ASSERT_GE(lifted.rpo_blocks[0].instructions.size(), 2u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[0].instructions[0],
            "(set (reg:SI 4 a0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  const RightInverseResult res = verifier_.VerifyDoCrossJumpRightInverse(cfg);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

TEST_F(InvertDoCrossJumpTest, RestoreNoopMovesConsumedDeathsRequiresInterveningShield) {
  // 1. Unshielded register-to-register overwrite `(set (reg:SI 5 a1) (reg:SI 4 a0))`
  //    immediately after an earlier write `(set (reg:SI 5 a1) (const_int 0))` with
  //    no intervening read of `$5` and no `NOTE_INSN_LOOP_END` must NOT duplicate
  //    `REG_DEAD (reg:SI 5 a1)` onto an earlier consumer of `$5` (which `jump2`
  //    only strips when `find_reg_note(p, REG_DEAD, dreg)` did not cross a write
  //    without an intervening read or `NOTE_INSN_LOOP_END` shield).
  RtlControlFlowGraph unshielded_cfg;
  unshielded_cfg.pass = RtlPassKind::kJump2;
  unshielded_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(set (reg:SI 5 a1) (const_int 0))",
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 5 a1))",
                    "(set (reg:SI 5 a1) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  unshielded_cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 6}}};

  const RtlControlFlowGraph lifted_shielded = InvertDoCrossJump(unshielded_cfg);
  ASSERT_EQ(lifted_shielded.rpo_blocks.size(), 1u);
  // Because `(set (mem:SI (reg:SI 6 a2)) (reg:SI 5 a1))` reads `$5` between the
  // `REG_DEAD (reg:SI 5 a1)` on `insn[0]` and `(set (reg:SI 5 a1) (reg:SI 4 a0))`,
  // `insn[0]`'s `REG_DEAD` IS shielded and preserved cleanly, while an unshielded
  // overwrite without any intervening read does not duplicate `REG_DEAD`.
  const RightInverseResult ok_res = verifier_.VerifyDoCrossJumpRightInverse(unshielded_cfg);
  EXPECT_TRUE(ok_res.matches) << ok_res.diff_detail;

  RtlControlFlowGraph no_read_cfg;
  no_read_cfg.pass = RtlPassKind::kJump2;
  no_read_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1)))",
                    "(set (reg:SI 5 a1) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  no_read_cfg.sideband.basic_block_live_at_start = {{0, {4, 5}}};
  const RtlControlFlowGraph lifted_no_read = InvertDoCrossJump(no_read_cfg);
  ASSERT_EQ(lifted_no_read.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted_no_read.rpo_blocks[0].instructions[0],
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1)))");
  const RightInverseResult no_read_res = verifier_.VerifyDoCrossJumpRightInverse(no_read_cfg);
  EXPECT_TRUE(no_read_res.matches) << no_read_res.diff_detail;

  // `NOTE_INSN_LOOP_END` between a `REG_DEAD (reg:SI 5 a1)` instruction and a subsequent
  // `(set (reg:SI 5 a1) (reg:SI 4 a0))` move also shields `insn[0]` in GCC `jump2` (`noop_moves=1`
  // strips one `REG_DEAD` note without deleting across the loop boundary). Verify structured AST
  // `RtlLoopNoteKind::kLoopEnd` detection both with synced `rtl_insns` and with cleared
  // `rtl_insns`, plus `VerifyDoCrossJumpRightInverse`.
  RtlControlFlowGraph loop_end_shield_cfg;
  loop_end_shield_cfg.pass = RtlPassKind::kJump2;
  loop_end_shield_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(note NOTE_INSN_LOOP_END)",
                    "(set (reg:SI 5 a1) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  loop_end_shield_cfg.sideband.basic_block_live_at_start = {{0, {4, 5}}};
  const RtlControlFlowGraph lifted_loop_end = InvertDoCrossJump(loop_end_shield_cfg);
  ASSERT_EQ(lifted_loop_end.rpo_blocks.size(), 1u);
  RtlControlFlowGraph loop_end_unsynced = loop_end_shield_cfg;
  loop_end_unsynced.rpo_blocks[0].rtl_insns.clear();
  const RtlControlFlowGraph lifted_loop_end_unsynced = InvertDoCrossJump(loop_end_unsynced);
  ASSERT_EQ(lifted_loop_end_unsynced.rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted_loop_end_unsynced.rpo_blocks[0].instructions,
            lifted_loop_end.rpo_blocks[0].instructions);
  const RightInverseResult loop_end_res =
      verifier_.VerifyDoCrossJumpRightInverse(loop_end_shield_cfg);
  EXPECT_TRUE(loop_end_res.matches) << loop_end_res.diff_detail;

  BrokenDoCrossJumpProvider bad_provider;
  RightInverseVerifier bad_verifier(&ctx_, &hooks_, &bad_provider);
  const RightInverseResult bad_res = bad_verifier.VerifyDoCrossJumpRightInverse(no_read_cfg);
  EXPECT_FALSE(bad_res.matches);
}

class DoCrossJumpCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionDoCrossJumpProvider();
    verifier = new RightInverseVerifier(ctx, hooks, provider);

    RtlControlFlowGraph dummy;
    hooks->ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
  static ProductionDoCrossJumpProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* DoCrossJumpCorpusTest::ctx = nullptr;
FunctionHooks* DoCrossJumpCorpusTest::hooks = nullptr;
ProductionDoCrossJumpProvider* DoCrossJumpCorpusTest::provider = nullptr;
RightInverseVerifier* DoCrossJumpCorpusTest::verifier = nullptr;

TEST_P(DoCrossJumpCorpusTest, RoundTrip2480Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto trace = hooks->TraceForwardCompilation(tc);

  bool found_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kDoCrossJump) {
      continue;
    }
    found_snap = true;
    const RtlControlFlowGraph inverted = provider->InvertDoCrossJump(snap.post_state);
    ASSERT_FALSE(inverted.rpo_blocks.empty()) << "Empty InvertDoCrossJump for " << tc.test_name;
    EXPECT_EQ(inverted.pass, RtlPassKind::kSched2);

    const RightInverseResult res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(res.matches) << "kDoCrossJump right-inverse mismatch for " << tc.test_name << ": "
                             << res.diff_detail << "\nexpected:\n"
                             << res.expected_summary << "\nactual:\n"
                             << res.actual_summary;
  }
  EXPECT_TRUE(found_snap) << "Missing kDoCrossJump snapshot for " << tc.test_name;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, DoCrossJumpCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
