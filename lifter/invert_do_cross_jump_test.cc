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
  const RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, sched2_profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 8u);
  ASSERT_EQ(lifted.rpo_blocks[7].instructions.size(), 1u) << lifted.FormatSummary();
  EXPECT_EQ(lifted.rpo_blocks[7].instructions[0], "(use (reg/i:DF 32 $f0))");

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
  const RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, sched2_profile);
  ASSERT_EQ(lifted.rpo_blocks.size(), 3u);
  ASSERT_EQ(lifted.rpo_blocks[0].instructions.size(), 1u) << lifted.FormatSummary();

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

TEST_F(InvertDoCrossJumpTest, FixScratchWitnessLocalRmwBoundaryAndPrologueSaveMdStallRightInverse) {
  const CompilationProfile sched2_profile{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"},
  };

  // 1. `SelectSched2SafeSharedPrefixLen`: `B1` defines caller-saved `$2` (`v0`) via
  // `fix_truncdfsi2`
  //    `PARALLEL` with GPR scratch clobber `$8` (`t0`, `scr_ord > dead_ord`), `B2` defines `$2` via
  //    ALU, and shared `B3` starts with 1-cycle ALU killing `$2` (`REG_DEAD (reg:SI 2 v0)`).
  //    `InvertDoCrossJump` duplicates `shared_insns[0]` back into `B1` and `B2`.
  RtlControlFlowGraph fix_cfg;
  fix_cfg.pass = RtlPassKind::kJump2;
  fix_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(parallel[ (set (reg:SI 2 v0) (fix:SI (reg:DF 44 $f12))) "
                    "(clobber (reg:SI 8 t0)) (clobber (reg:DF 36 $f4)) ] ) "
                    "(expr_list:REG_DEAD (reg:DF 44 $f12) "
                    "(expr_list:REG_UNUSED (reg:SI 8 t0) "
                    "(expr_list:REG_UNUSED (reg:DF 36 $f4) (nil))))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3))) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 2 v0)) "
                    "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  fix_cfg.sideband.basic_block_live_at_start = {
      {0, {4, 5, 6, 44, 45}}, {1, {6, 44, 45}}, {2, {5, 6}}, {3, {2, 6}}};
  const RtlControlFlowGraph lifted_fix = InvertDoCrossJump(fix_cfg, sched2_profile);
  ASSERT_EQ(lifted_fix.rpo_blocks.size(), 4u);
  EXPECT_GE(lifted_fix.rpo_blocks[1].instructions.size(), 3u);
  EXPECT_GE(lifted_fix.rpo_blocks[2].instructions.size(), 2u);
  const RightInverseResult fix_res = verifier_.VerifyDoCrossJumpRightInverse(fix_cfg);
  EXPECT_TRUE(fix_res.matches) << fix_res.diff_detail;

  // 2. `SelectSched2SafeSharedPrefixLen`: `conflicts_with_local_rmw` trims shared prefix when the
  //    first remaining reference to written register `$6` (`a2`) is a non-dead RMW.
  RtlControlFlowGraph rmw_cfg;
  rmw_cfg.pass = RtlPassKind::kJump2;
  rmw_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 3 v1) (minus:SI (reg:SI 5 a1) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 6 a2) (plus:SI (reg:SI 3 v1) (const_int 2))) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                    "(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (reg:SI 7 a3))) "
                    "(expr_list:REG_DEAD (reg:SI 7 a3) (nil))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int 0)) "
                    "(label_ref L2) (pc)))",
                },
                {5, 4}),
      MakeBlock(4, false, {"(set (reg/i:SI 2 v0) (reg:SI 6 a2))"}, {5}),
      MakeBlock(5, true,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  rmw_cfg.sideband.basic_block_live_at_start = {{0, {2, 4, 5, 7}}, {1, {2, 5, 7}}, {2, {2, 5, 7}},
                                                {3, {2, 3, 7}},    {4, {6}},       {5, {2}}};
  const RtlControlFlowGraph lifted_rmw = InvertDoCrossJump(rmw_cfg, sched2_profile);
  ASSERT_EQ(lifted_rmw.rpo_blocks.size(), 6u);
  EXPECT_EQ(lifted_rmw.rpo_blocks[1].instructions.size(), 2u);
  const RightInverseResult rmw_res = verifier_.VerifyDoCrossJumpRightInverse(rmw_cfg);
  EXPECT_TRUE(rmw_res.matches) << rmw_res.diff_detail;

  // 3. `RestoreDeletedNoopMdOutputReloads`: entry-block prologue save store `[sp + 24] = ra`
  //    scheduled into the 3-cycle `imuldiv` stall window between `(set (reg:DI 6 a2) (reg:DI 64))`
  //    and `adddi3` restores `(set (reg:DI 64 hi) (reg:DI 6 a2))` and `REG_DEAD (reg:DI 64 hi)`.
  RtlControlFlowGraph pre_md_cfg;
  pre_md_cfg.pass = RtlPassKind::kSched2;
  pre_md_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))",
                    "(set (reg:SI 65 lo) (reg:SI 4 a0))",
                    "(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 17 s1))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0))",
                    "(set (reg:DI 16 s0) (reg:DI 6 a2)) (expr_list:REG_DEAD (reg:DI 6 a2) (nil))",
                    "(set (reg:SI 64 hi) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg:DI 6 a2) (reg:DI 64 hi)) (expr_list:REG_DEAD (reg:DI 64 hi) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 31 ra))",
                    "(set (reg:DI 64 hi) (reg:DI 6 a2))",
                    "(parallel[ (set (reg:DI 16 s0) (plus:DI (reg:DI 16 s0) (reg:DI 6 a2))) "
                    "(clobber (reg:SI 2 v0)) ] ) (expr_list:REG_DEAD (reg:DI 6 a2) "
                    "(expr_list:REG_DEAD (reg:DI 64 hi) "
                    "(expr_list:REG_UNUSED (reg:SI 2 v0) (nil))))",
                    "(set (reg/i:DI 2 v0) (reg:DI 16 s0)) "
                    "(expr_list:REG_DEAD (reg:DI 16 s0) (nil))",
                    "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))",
                    "(set (reg:SI 17 s1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
                    "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 32)))",
                    "(use (reg/i:DI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  pre_md_cfg.sideband.basic_block_live_at_start = {{0, {4, 6, 7, 16, 17, 29, 31}}};
  const auto md_snap =
      hooks_.ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, pre_md_cfg, {},
                                        sched2_profile.optimize_level, sched2_profile.c_flags);
  const RtlControlFlowGraph lifted_md = InvertDoCrossJump(md_snap.post_state, sched2_profile);
  ASSERT_EQ(lifted_md.rpo_blocks.size(), 1u);
  bool saw_restored_md_wb = false;
  bool saw_adddi_hi_dead = false;
  for (const std::string& insn : lifted_md.rpo_blocks[0].instructions) {
    if (insn.find("(set (reg:DI 64 hi) (reg:DI 6 a2))") != std::string::npos) {
      saw_restored_md_wb = true;
    }
    if (insn.find("(plus:DI (reg:DI 16 s0) (reg:DI 6 a2))") != std::string::npos &&
        insn.find("REG_DEAD (reg:DI 64 hi)") != std::string::npos) {
      saw_adddi_hi_dead = true;
    }
  }
  EXPECT_TRUE(saw_restored_md_wb) << lifted_md.FormatSummary();
  EXPECT_TRUE(saw_adddi_hi_dead) << lifted_md.FormatSummary();
  const RightInverseResult md_res = verifier_.VerifySubfunctionBoundaryRightInverse(md_snap);
  EXPECT_TRUE(md_res.matches) << md_res.diff_detail;
}

TEST_F(InvertDoCrossJumpTest, MinimumOneCrossJumpRequiresMatchingPredEquivForSingleSharedStore) {
  const CompilationProfile o1_profile = {.optimize_level = 1,
                                         .c_flags = {"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  hooks_.ApplyCompilationProfile(o1_profile);

  // 1. Positive case (`jump.c:1978` `minimum = 1` + `FunctionLocalStaticSequenceId` pattern):
  //    Join block `B3` has 1 shareable store `(set (mem [s_next]) v0)` before an active
  //    `REG_EQUIV`-stopped epilogue head, and BOTH predecessors' defs of `v0` already carry
  //    matching `REG_EQUIV (mem [s_next])`. `InvertDoCrossJump` duplicates the store into both
  //    predecessors and round-trips through `do_cross_jump`.
  RtlControlFlowGraph matching_equiv_cfg;
  matching_equiv_cfg.pass = RtlPassKind::kJump2;
  matching_equiv_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 2 v0) (const_int 1)) "
                    "(expr_list:REG_EQUIV (mem:SI (symbol_ref:SI (\"s_next\"))) (nil))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                    "(expr_list:REG_EQUIV (mem:SI (symbol_ref:SI (\"s_next\"))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil)))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (mem:SI (symbol_ref:SI (\"s_next\"))) (reg:SI 2 v0)) "
                    "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                    "(set (reg:SI 3 v1) (mem:SI (symbol_ref:SI (\"s_inv\")))) "
                    "(expr_list:REG_EQUIV (mem:SI (symbol_ref:SI (\"s_inv\"))) (nil))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  matching_equiv_cfg.sideband.basic_block_live_at_start = {
      {0, {4, 5}}, {1, {}}, {2, {5}}, {3, {2}}};
  const RtlControlFlowGraph lifted_match = InvertDoCrossJump(matching_equiv_cfg, o1_profile);
  ASSERT_EQ(lifted_match.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted_match.rpo_blocks[1].instructions.size(), 3u);
  EXPECT_EQ(lifted_match.rpo_blocks[2].instructions.size(), 2u);
  const RightInverseResult match_res = verifier_.VerifyDoCrossJumpRightInverse(matching_equiv_cfg);
  EXPECT_TRUE(match_res.matches) << match_res.diff_detail;

  // 2. Negative case (`GeneralizedCrossJump*` pattern): when fallthrough predecessor `B2` lacks a
  //    matching `REG_EQUIV (mem [s_next])` on its def of `v0`, duplicating the store into `B2`
  //    would synthesize a spurious `REG_EQUIV` in `update_equiv_regs`; leave the store in `B3`.
  RtlControlFlowGraph missing_equiv_cfg = matching_equiv_cfg;
  missing_equiv_cfg.rpo_blocks[2] =
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
                },
                {3});
  const RtlControlFlowGraph lifted_miss = InvertDoCrossJump(missing_equiv_cfg, o1_profile);
  ASSERT_EQ(lifted_miss.rpo_blocks.size(), 4u);
  EXPECT_EQ(lifted_miss.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_EQ(lifted_miss.rpo_blocks[2].instructions.size(), 1u);
  const RightInverseResult miss_res = verifier_.VerifyDoCrossJumpRightInverse(missing_equiv_cfg);
  EXPECT_TRUE(miss_res.matches) << miss_res.diff_detail;
}

TEST_F(InvertDoCrossJumpTest, UnCrossJumpChainedPureFallthroughStoreTailsPositiveAndNegative) {
  const CompilationProfile o2_profile = {.optimize_level = 2,
                                         .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-O2"}};
  hooks_.ApplyCompilationProfile(o2_profile);

  // Build exact 2-level chained pure fallthrough store tail with a labeled exit block:
  //   B0 -> {B8, B1} (keeps exit label alive during `jump_optimize`),
  //   B1 -> {B3, B2}; B3 -> {B5, B4};
  //   B2 (uncond jump -> B6), B4 (uncond jump -> B7), B5 (fallthrough -> B6),
  //   B6 (4-insn pure store tail, fallthrough -> B7),
  //   B7 (1-insn pure store tail, fallthrough -> B8),
  //   B8 (labeled `return_internal` `PARALLEL` exit, or uncond jump to B9 in negative case).
  auto build_cfg = [](bool uncond_jump_next, bool load_in_b2, bool tail_kills_v0) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 99)) "
                   "(label_ref L4) (pc)))"},
                  {8, 1}),
        MakeBlock(1, false,
                  {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                   "(label_ref L0) (pc)))"},
                  {3, 2}),
        MakeBlock(
            2, false,
            {
                load_in_b2 ? "(set (reg:QI 2 v0) (mem:QI (symbol_ref:SI (\"g_ext\"))))"
                           : "(set (reg:QI 2 v0) (const_int 19)) "
                             "(expr_list:REG_EQUIV (mem:QI (symbol_ref:SI (\"g_c3\"))) (nil))",
                "(set (pc) (label_ref L2))",
            },
            {6}),
        MakeBlock(3, true,
                  {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 1)) "
                   "(label_ref L1) (pc))) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))"},
                  {5, 4}),
        MakeBlock(4, false,
                  {
                      "(set (reg:QI 2 v0) (const_int 21)) "
                      "(expr_list:REG_EQUIV (mem:QI (symbol_ref:SI (\"g_c5\"))) (nil))",
                      "(set (pc) (label_ref L3))",
                  },
                  {7}),
        MakeBlock(5, true,
                  {
                      "(set (reg:QI 2 v0) (const_int 20)) "
                      "(expr_list:REG_EQUIV (mem:QI (symbol_ref:SI (\"g_c3\"))) (nil))",
                  },
                  {6}),
        MakeBlock(6, true,
                  {
                      "(set (mem:QI (symbol_ref:SI (\"g_c3\"))) (reg:QI 2 v0)) "
                      "(expr_list:REG_DEAD (reg:QI 2 v0) (nil))",
                      "(set (reg:QI 2 v0) (const_int 22))",
                      "(set (mem:QI (symbol_ref:SI (\"g_c4\"))) (reg:QI 2 v0)) "
                      "(expr_list:REG_DEAD (reg:QI 2 v0) (nil))",
                      "(set (reg:QI 2 v0) (const_int 23))",
                  },
                  {7}),
        MakeBlock(7, true,
                  {
                      tail_kills_v0 ? "(set (mem:QI (symbol_ref:SI (\"g_c5\"))) (reg:QI 2 v0)) "
                                      "(expr_list:REG_DEAD (reg:QI 2 v0) (nil))"
                                    : "(set (mem:QI (symbol_ref:SI (\"g_c5\"))) (reg:QI 2 v0))",
                  },
                  {8}),
        MakeBlock(8, true,
                  {
                      uncond_jump_next ? "(set (pc) (label_ref L5))"
                                       : "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  uncond_jump_next ? std::vector<size_t>{9} : std::vector<size_t>{}),
    };
    cfg.sideband.basic_block_live_at_start = {
        {0, {4}}, {1, {4}}, {2, {}}, {3, {4}}, {4, {}}, {5, {}}, {6, {2}}, {7, {2}}, {8, {}},
    };
    if (uncond_jump_next) {
      cfg.rpo_blocks.push_back(
          MakeBlock(9, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}));
      cfg.sideband.basic_block_live_at_start[9] = {};
    }
    return cfg;
  };

  // 1. Positive (`jump.c:1978, 2371-2560`, `sched.c:1488, 2428`, `mips.md:161`):
  //    both `B7` and `B6` unwind cleanly into `{B2, B4, B5}` and round-trip via `do_cross_jump`.
  const RtlControlFlowGraph pos_cfg =
      build_cfg(/*uncond_jump_next=*/false, /*load_in_b2=*/false, /*tail_kills_v0=*/true);
  const RtlControlFlowGraph lifted_pos = InvertDoCrossJump(pos_cfg, o2_profile);
  EXPECT_EQ(lifted_pos.rpo_blocks.size(), 7u) << lifted_pos.FormatSummary();
  const RightInverseResult pos_res = verifier_.VerifyDoCrossJumpRightInverse(pos_cfg);
  EXPECT_TRUE(pos_res.matches) << pos_res.diff_detail;

  // 2. Negative (unconditional jump head in `B8`): `B7` is NOT unwound because `next_b` (`B8`)
  //    starts with an unconditional jump that `jump_optimize` threads past.
  const RtlControlFlowGraph uncond_next_cfg =
      build_cfg(/*uncond_jump_next=*/true, /*load_in_b2=*/false, /*tail_kills_v0=*/true);
  const RtlControlFlowGraph lifted_uncond_next = InvertDoCrossJump(uncond_next_cfg, o2_profile);
  EXPECT_GE(lifted_uncond_next.rpo_blocks.size(), 9u) << lifted_uncond_next.FormatSummary();

  // 3. Negative (multi-cycle memory load in `B2` under `-O2` `sched2`): `B6` is NOT unwound
  //    because `B2` is not unit-latency ALU/store.
  const RtlControlFlowGraph load_pred_cfg =
      build_cfg(/*uncond_jump_next=*/false, /*load_in_b2=*/true, /*tail_kills_v0=*/true);
  const RtlControlFlowGraph lifted_load_pred = InvertDoCrossJump(load_pred_cfg, o2_profile);
  EXPECT_EQ(lifted_load_pred.rpo_blocks.size(), 8u) << lifted_load_pred.FormatSummary();

  // 4. Negative (missing `REG_DEAD` on `B7[0]`): `B7` is NOT unwound.
  const RtlControlFlowGraph no_dead_cfg =
      build_cfg(/*uncond_jump_next=*/false, /*load_in_b2=*/false, /*tail_kills_v0=*/false);
  const RtlControlFlowGraph lifted_no_dead = InvertDoCrossJump(no_dead_cfg, o2_profile);
  EXPECT_EQ(lifted_no_dead.rpo_blocks.size(), 8u) << lifted_no_dead.FormatSummary();
}

TEST_F(InvertDoCrossJumpTest, RestoreNoopMovesConsumedDeathsRestoresSingleDeadOnPointerLoadRmw) {
  auto make_cfg = [](bool b2_has_label) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.rpo_blocks = {
        MakeBlock(0, false, {"(set (reg:SI 3 v1) (const_int 0))"}, {1}),
        MakeBlock(1, true,
                  {
                      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                      "(set (reg:SI 2 v0) (ashift:SI (reg:SI 3 v1) (const_int 2)))",
                      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
                      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                      "(set (reg:SI 2 v0) (mem:SI (reg:SI 2 v0)))",
                      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (reg:SI 5 a1)) "
                      "(label_ref L1) (pc)))",
                  },
                  {3, 2}),
        MakeBlock(2, b2_has_label,
                  {
                      "(set (reg/i:SI 2 v0) (reg:SI 3 v1)) "
                      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                      "(set (pc) (label_ref L2))",
                  },
                  {5}),
        MakeBlock(3, true,
                  {
                      "(set (pc) (if_then_else (ge:SI (reg:SI 2 v0) (const_int 0)) "
                      "(label_ref L0) (pc))) (expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                  },
                  {1, 4}),
        MakeBlock(4, false, {"(set (reg/i:SI 2 v0) (const_int -1))"}, {5}),
        MakeBlock(5, true,
                  {
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 5}}, {1, {3, 4, 5}}, {2, {3}}, {3, {2, 3, 4, 5}}, {4, {}}, {5, {2}},
    };
    return cfg;
  };

  // Positive: label-less fallthrough `B2` setting `$v0 = $v1` restores ONE
  // `REG_DEAD (reg:SI 2 v0)` onto un-noted pointer-load self-RMW `B1[3]` and
  // does NOT duplicate `REG_DEAD 2 v0` onto `B1[2]` (`jump.c:452-459`).
  const RtlControlFlowGraph unlabelled_cfg = make_cfg(/*b2_has_label=*/false);
  const RtlControlFlowGraph lifted_pos = InvertDoCrossJump(unlabelled_cfg);
  ASSERT_EQ(lifted_pos.rpo_blocks.size(), 6u);
  EXPECT_EQ(lifted_pos.rpo_blocks[1].instructions[2],
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
            "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  EXPECT_EQ(lifted_pos.rpo_blocks[1].instructions[3],
            "(set (reg:SI 2 v0) (mem:SI (reg:SI 2 v0))) "
            "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  const RightInverseResult pos_res = verifier_.VerifyDoCrossJumpRightInverse(unlabelled_cfg);
  EXPECT_TRUE(pos_res.matches) << pos_res.diff_detail;

  // Negative: a `CODE_LABEL` before `B2` blocks `jump.c:453`, so `B1[3]` is
  // left untouched.
  const RtlControlFlowGraph labelled_cfg = make_cfg(/*b2_has_label=*/true);
  const RtlControlFlowGraph lifted_neg = InvertDoCrossJump(labelled_cfg);
  ASSERT_EQ(lifted_neg.rpo_blocks.size(), 6u);
  EXPECT_EQ(lifted_neg.rpo_blocks[1].instructions[3], "(set (reg:SI 2 v0) (mem:SI (reg:SI 2 v0)))");
}

TEST_F(InvertDoCrossJumpTest, RestoresDeletedNoopCallArgMoveAcrossFallthroughCondBranch) {
  auto make_cfg = [](bool b1_has_label, bool b2_reads_a0, bool b1_writes_a1) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.sideband.outgoing_args_size = 16;
    cfg.sideband.basic_block_live_at_start[0] = {4, 5, 6, 29, 31};
    std::vector<std::string> b1_insns;
    if (b1_writes_a1) {
      b1_insns.push_back(
          "(set (reg:SI 5 a1) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
    }
    b1_insns.push_back(
        "(parallel[ (call (mem:SI (symbol_ref:SI (\"left_fn\"))) (const_int 16)) "
        "(clobber (reg:SI 31 ra)) ] )");
    b1_insns.push_back("(set (pc) (label_ref L1))");

    std::vector<std::string> b2_insns;
    if (b2_reads_a0) {
      b2_insns.push_back(
          "(set (reg:SI 5 a1) (plus:SI (reg:SI 4 a0) (const_int 1))) "
          "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
    } else {
      b2_insns.push_back("(set (reg:SI 4 a0) (reg:SI 16 s0))");
      b2_insns.push_back(
          "(set (reg:SI 5 a1) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
    }
    b2_insns.push_back(
        "(parallel[ (call (mem:SI (symbol_ref:SI (\"right_fn\"))) (const_int 16)) "
        "(clobber (reg:SI 31 ra)) ] )");

    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 24)))",
                      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0))",
                      "(set (reg:SI 16 s0) (reg:SI 4 a0))",
                      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 31 ra))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                      "(label_ref L0) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, b1_has_label, std::move(b1_insns), {3}),
        MakeBlock(2, true, std::move(b2_insns), {3}),
        MakeBlock(3, true,
                  {
                      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (const_int 4))) "
                      "(expr_list:REG_DEAD (reg:SI 16 s0) (nil))",
                      "(use (reg/i:SI 2 v0))",
                      "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
                      "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                      "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 24)))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    return cfg;
  };

  // Positive: label-less fallthrough B1 writes `$a1` before the call while B2
  // overwrites `$a0`, so `jump.c:434-465` + `reload.c:find_equiv_reg:5413-5590`
  // deleted `(set (reg:SI 4 a0) (reg:SI 16 s0))` and stripped `REG_DEAD 4` from B0.
  const RtlControlFlowGraph pos_cfg =
      make_cfg(/*b1_has_label=*/false, /*b2_reads_a0=*/false, /*b1_writes_a1=*/true);
  const RtlControlFlowGraph pos_pre = InvertDoCrossJump(pos_cfg);
  ASSERT_EQ(pos_pre.rpo_blocks.size(), 4u);
  EXPECT_EQ(pos_pre.rpo_blocks[0].instructions[2],
            "(set (reg:SI 16 s0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  ASSERT_EQ(pos_pre.rpo_blocks[1].instructions.size(), 4u);
  EXPECT_EQ(pos_pre.rpo_blocks[1].instructions[0], "(set (reg:SI 4 a0) (reg:SI 16 s0))");
  EXPECT_EQ(pos_pre.rpo_blocks[1].instructions[1],
            "(set (reg:SI 5 a1) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))");
  const RightInverseResult pos_res = [&]() {
    RtlControlFlowGraph with_fusage = pos_cfg;
    with_fusage.rpo_blocks[1].rtl_insns[1].has_explicit_call_fusage = true;
    with_fusage.rpo_blocks[1].rtl_insns[1].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
    };
    with_fusage.rpo_blocks[2].rtl_insns[2].has_explicit_call_fusage = true;
    with_fusage.rpo_blocks[2].rtl_insns[2].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
    };
    return verifier_.VerifyDoCrossJumpRightInverse(with_fusage);
  }();
  EXPECT_TRUE(pos_res.matches) << pos_res.diff_detail;

  // Negative 1: a `CODE_LABEL` at B1 stops `reload.c:find_equiv_reg:5416`.
  const RtlControlFlowGraph neg_label =
      InvertDoCrossJump(make_cfg(/*b1_has_label=*/true, /*b2_reads_a0=*/false,
                                 /*b1_writes_a1=*/true));
  EXPECT_EQ(neg_label.rpo_blocks[0].instructions[2], "(set (reg:SI 16 s0) (reg:SI 4 a0))");
  EXPECT_EQ(neg_label.rpo_blocks[1].instructions.size(), 3u);

  // Negative 2: `$a0` live-in to taken branch B2 means B0's copy did not kill `$a0`.
  const RtlControlFlowGraph neg_live_taken =
      InvertDoCrossJump(make_cfg(/*b1_has_label=*/false, /*b2_reads_a0=*/true,
                                 /*b1_writes_a1=*/true));
  EXPECT_EQ(neg_live_taken.rpo_blocks[0].instructions[2], "(set (reg:SI 16 s0) (reg:SI 4 a0))");
  EXPECT_EQ(neg_live_taken.rpo_blocks[1].instructions.size(), 3u);

  // Negative 3: 0-arg call in B1 without explicit `call_fusage` or higher arg write.
  const RtlControlFlowGraph neg_zero_arg =
      InvertDoCrossJump(make_cfg(/*b1_has_label=*/false, /*b2_reads_a0=*/false,
                                 /*b1_writes_a1=*/false));
  EXPECT_EQ(neg_zero_arg.rpo_blocks[0].instructions[2], "(set (reg:SI 16 s0) (reg:SI 4 a0))");
  EXPECT_EQ(neg_zero_arg.rpo_blocks[1].instructions.size(), 2u);
}

TEST_F(InvertDoCrossJumpTest, UncrossJumpTailMoveRespectsFormerCodeLabelBarrierForRegDead) {
  // Mirrors `jump.c:434-465` (`if (noop_moves)` + line 453 `CODE_LABEL` barrier):
  // when `InvertDoCrossJump` duplicates a cross-jumped tail starting with
  // `(set (reg:SI 4 a0) (reg:SI 8 t0))` + `CALL` into `pred_b`, the copied tail
  // originally lived behind `join_b`'s `CODE_LABEL`. Scanning backward past that
  // copied-tail boundary must treat it as a `CODE_LABEL` shield and prepend a
  // compensating `(REG_DEAD a0)` onto the preceding consumer of `$a0` in `pred_b`
  // (so `jump2`'s `remove_death` consumes one note and preserves the original).
  auto count_substr = [](const std::string& haystack, std::string_view needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
      ++count;
      pos += needle.size();
    }
    return count;
  };
  RtlControlFlowGraph cross_cfg;
  cross_cfg.pass = RtlPassKind::kJump2;
  cross_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 8 t0) (plus:SI (reg:SI 6 a2) (const_int 2)))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 4 a0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 5)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  cross_cfg.rpo_blocks[3].rtl_insns[1].has_explicit_call_fusage = true;
  cross_cfg.rpo_blocks[3].rtl_insns[1].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  cross_cfg.sideband.basic_block_live_at_start = {
      {0, {4, 5, 6}},
      {1, {4}},
      {2, {6}},
      {3, {8}},
  };
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph lifted = InvertDoCrossJump(cross_cfg, o2_prof);
  ASSERT_GE(lifted.rpo_blocks.size(), 4u);
  // Positive: in `B1` (which received the un-cross-jumped tail), the producer
  // `(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1)))` preceding the
  // former `CODE_LABEL` boundary gets a compensating second `(REG_DEAD a0)`,
  // and replaying `jump2` (`do_cross_jump`) matches `cross_cfg` exactly.
  ASSERT_GE(lifted.rpo_blocks[1].instructions.size(), 3u) << lifted.FormatSummary();
  EXPECT_EQ(count_substr(lifted.rpo_blocks[1].instructions[0], "REG_DEAD (reg:SI 4 a0)"), 2)
      << lifted.FormatSummary();
  const RightInverseResult pos_res = verifier_.VerifyDoCrossJumpRightInverse(cross_cfg);
  EXPECT_TRUE(pos_res.matches) << pos_res.diff_detail;

  // Negative 1: without cross-jumping (a single straight-line block with no former
  // `CODE_LABEL` barrier between the read of `$a0` and `(set a0 t0)`), the
  // instruction does NOT receive a compensating second `REG_DEAD` note.
  RtlControlFlowGraph flat_cfg;
  flat_cfg.pass = RtlPassKind::kJump2;
  flat_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 8 t0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg:SI 4 a0) (reg:SI 8 t0)) (expr_list:REG_DEAD (reg:SI 8 t0) (nil))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 5)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  flat_cfg.rpo_blocks[0].rtl_insns[2].has_explicit_call_fusage = true;
  flat_cfg.rpo_blocks[0].rtl_insns[2].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  flat_cfg.sideband.basic_block_live_at_start = {{0, {4}}};
  const RtlControlFlowGraph lifted_flat = InvertDoCrossJump(flat_cfg, o2_prof);
  ASSERT_EQ(lifted_flat.rpo_blocks.size(), 1u);
  EXPECT_EQ(count_substr(lifted_flat.rpo_blocks[0].instructions[0], "REG_DEAD (reg:SI 4 a0)"), 1)
      << lifted_flat.FormatSummary();

  // Negative 2: an intervening `CALL_INSN` whose `call_fusage` already consumes
  // `$a0` (`flow.c:1303-1315`) absorbs `jump.c:455`'s `remove_death` scan so an
  // earlier `(REG_DEAD (reg:SI 4 a0))` in `B1` is NOT duplicated.
  RtlControlFlowGraph fusage_shield_cfg = cross_cfg;
  fusage_shield_cfg.rpo_blocks[1].instructions.insert(
      fusage_shield_cfg.rpo_blocks[1].instructions.begin() + 1,
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"pre_call\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )");
  fusage_shield_cfg.rpo_blocks[1].SyncRtlInsnsFromInstructions();
  fusage_shield_cfg.rpo_blocks[1].rtl_insns[1].has_explicit_call_fusage = true;
  fusage_shield_cfg.rpo_blocks[1].rtl_insns[1].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  const RtlControlFlowGraph lifted_fusage = InvertDoCrossJump(fusage_shield_cfg, o2_prof);
  ASSERT_GE(lifted_fusage.rpo_blocks.size(), 4u);
  EXPECT_EQ(count_substr(lifted_fusage.rpo_blocks[1].instructions[0], "REG_DEAD (reg:SI 4 a0)"), 1)
      << lifted_fusage.FormatSummary();
}

TEST_F(InvertDoCrossJumpTest, MultiCyclePreCallOpNotUnCrossJumpedIntoMultiCyclePred) {
  CompilationProfile o2_prof;
  o2_prof.optimize_level = 2;
  o2_prof.c_flags = {"-O2", "-mcpu=r4000", "-Wa,-g"};

  // Negative (Fix 42B): under `runs_sched2`, a join block starting with a
  // multi-cycle pre-call FP op (`add.s $f12`) independent of a multi-cycle
  // predecessor's definitions must NOT be un-cross-jumped into the predecessor.
  auto make_cfg = [](bool pred_is_multicycle) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                   "(label_ref L2) (pc)))"},
                  {2, 1}),
        MakeBlock(1, false,
                  {pred_is_multicycle
                       ? "(set (reg:SF 52 $f20) (mult:SF (reg:SF 54 $f22) (reg:SF 56 $f24)))"
                       : "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 1)))",
                   "(set (pc) (label_ref L3))"},
                  {3}),
        MakeBlock(2, true, {"(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 2)))"}, {3}),
        MakeBlock(3, true,
                  {"(set (reg:SF 44 $f12) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2)))",
                   "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                   "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                   "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0)))",
                   "(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                  {}),
    };
    cfg.rpo_blocks[3].rtl_insns[1].has_explicit_call_fusage = true;
    cfg.rpo_blocks[3].rtl_insns[1].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSf, 44)),
    };
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 16, 32, 34, 54, 56}},
        {1, {16, 32, 34, 54, 56}},
        {2, {16, 32, 34}},
        {3, {16, 32, 34}},
    };
    return cfg;
  };

  const RtlControlFlowGraph kept =
      InvertDoCrossJump(make_cfg(/*pred_is_multicycle=*/true), o2_prof);
  ASSERT_EQ(kept.rpo_blocks.size(), 4u);
  EXPECT_EQ(kept.rpo_blocks[1].instructions.size(), 2u) << kept.FormatSummary();
  EXPECT_EQ(kept.rpo_blocks[3].instructions.size(), 5u) << kept.FormatSummary();

  // Positive: when every predecessor is single-cycle ALU, un-cross-jumping proceeds.
  const RtlControlFlowGraph uncrossed =
      InvertDoCrossJump(make_cfg(/*pred_is_multicycle=*/false), o2_prof);
  ASSERT_EQ(uncrossed.rpo_blocks.size(), 4u);
  EXPECT_GT(uncrossed.rpo_blocks[1].instructions.size(), 2u) << uncrossed.FormatSummary();
}

TEST_F(InvertDoCrossJumpTest, FpConsumerCrossJumpPrefixUnCrossJumpsAndSinksStage1GprAlu) {
  CompilationProfile o2_prof;
  o2_prof.optimize_level = 2;
  o2_prof.c_flags = {"-O2", "-mcpu=r4000", "-Wa,-g"};

  // Positive (Fix 42G): join block prefix `[addu v1, v0, 17 ; add.s $f2, $f0, $f2]`
  // kills `$f0` defined by multi-cycle `mul.s` in both predecessors, and `B2`
  // hoisted `mul.s` ahead of independent `xor v0`. Verify `InvertDoCrossJump`
  // duplicates both instructions into `B1`/`B2` and sinks `B2[0]` (`lui v0`)
  // past `[li.s $f0 ; mul.s $f0]` to right before `ori v0`.
  auto make_diamond_cfg = [](bool hoist_mul_in_b2) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    std::vector<std::string> b2_insns;
    if (hoist_mul_in_b2) {
      b2_insns = {
          "(set (reg:SI 2 v0) (const_int 324468736))",
          "(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1071644672))",
          "(set (reg:SF 32 $f0) (mult:SF (reg:SF 34 $f2) (reg:SF 32 $f0))) "
          "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil)))",
          "(set (reg:SI 2 v0) (ior:SI (reg:SI 2 v0) (const_int 39903)))",
          "(set (reg:SF 34 $f2) (const_double:SF (cc0) 0 1072693248))",
          "(set (reg:SI 2 v0) (xor:SI (reg:SI 4 a0) (reg:SI 2 v0))) "
          "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      };
    } else {
      b2_insns = {
          "(set (reg:SI 2 v0) (const_int 324468736))",
          "(set (reg:SI 2 v0) (ior:SI (reg:SI 2 v0) (const_int 39903)))",
          "(set (reg:SI 2 v0) (xor:SI (reg:SI 4 a0) (reg:SI 2 v0))) "
          "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
          "(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1071644672))",
          "(set (reg:SF 32 $f0) (mult:SF (reg:SF 34 $f2) (reg:SF 32 $f0))) "
          "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil)))",
          "(set (reg:SF 34 $f2) (const_double:SF (cc0) 0 1072693248))",
      };
    }
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {"(set (pc) (if_then_else (ge:SI (reg:SI 4 a0) (const_int 0)) "
                   "(label_ref L2) (pc)))"},
                  {2, 1}),
        MakeBlock(
            1, false,
            {"(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1071644672))",
             "(set (reg:SI 2 v0) (neg:SI (reg:SI 4 a0))) "
             "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
             "(set (reg:SF 32 $f0) (mult:SF (reg:SF 34 $f2) (reg:SF 32 $f0))) "
             "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SF 32 $f0) (nil)))",
             "(set (reg:SF 34 $f2) (const_double:SF (cc0) 0 1072693248))",
             "(set (pc) (label_ref L3))"},
            {3}),
        MakeBlock(2, true, std::move(b2_insns), {3}),
        MakeBlock(3, true,
                  {"(set (reg:SI 3 v1) (plus:SI (reg:SI 2 v0) (const_int 17))) "
                   "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                   "(set (reg:SF 34 $f2) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
                   "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil))",
                   "(set (reg/i:SF 32 $f0) (reg:SF 34 $f2))", "(use (reg/i:SF 32 $f0))",
                   "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                  {}),
    };
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 34}},
        {1, {4, 34}},
        {2, {4, 34}},
        {3, {2, 32, 34}},
    };
    return cfg;
  };

  const RtlControlFlowGraph pos =
      InvertDoCrossJump(make_diamond_cfg(/*hoist_mul_in_b2=*/true), o2_prof);
  ASSERT_EQ(pos.rpo_blocks.size(), 4u);
  ASSERT_EQ(pos.rpo_blocks[2].instructions.size(), 8u) << pos.FormatSummary();
  EXPECT_NE(pos.rpo_blocks[2].instructions[0].find("const_double:SF"), std::string::npos)
      << pos.FormatSummary();
  EXPECT_NE(pos.rpo_blocks[2].instructions[2].find("(const_int 324468736)"), std::string::npos)
      << pos.FormatSummary();
  EXPECT_NE(pos.rpo_blocks[2].instructions[7].find("(plus:SF (reg:SF 32 $f0)"), std::string::npos)
      << pos.FormatSummary();

  // Negative: when neither predecessor hoisted `mul.s` ahead of an independent
  // 1-cycle integer ALU op, the prefix is not un-cross-jumped.
  const RtlControlFlowGraph neg =
      InvertDoCrossJump(make_diamond_cfg(/*hoist_mul_in_b2=*/false), o2_prof);
  ASSERT_EQ(neg.rpo_blocks.size(), 4u);
  EXPECT_EQ(neg.rpo_blocks[2].instructions.size(), 6u) << neg.FormatSummary();
}

TEST_F(InvertDoCrossJumpTest, RestoresCalleeSavedFprArgCopyIntoPostRaXferStallAndGuardsO32Gap) {
  CompilationProfile mips1_o2;
  mips1_o2.optimize_level = 2;
  mips1_o2.c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"};

  // Positive (Fix 42C + 42H-5): when an FPR->GPR arg copy (`$a2 <- $f24`) and a
  // GPR arg copy (`$a0 <- $s1`) are restored in a block with an unfilled post-`$ra`
  // `const_double -> FP arith` stall slot, `$a0 <- $s1` sits ahead of `sw $ra`
  // while `$a2 <- $f24` fills the 1-cycle `xfer` stall slot between `li.s` and
  // `mul.s`. Negative: O32 `$a1` (`reg 5`) alignment gap is NOT restored when
  // `$a2` is an 8-byte `DFmode` arg.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kJump2;
  cfg.sideband.frame_size_bytes = 64;
  cfg.sideband.saved_gpr_regnos = {16, 17, 31};
  cfg.sideband.saved_fpr_regnos = {56};
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -64)))",
                    "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 48))) (reg:DF 56 $f24))",
                    "(set (reg:DF 56 $f24) (reg:DF 6 a2))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))) (reg:SI 17 s1))",
                    "(set (reg:SI 17 s1) (reg:SI 4 a0))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:SI 16 s0))",
                    "(set (reg:SI 16 s0) (reg:SI 5 a1))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 40))) (reg:SI 31 ra))",
                    "(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1071644672))",
                    "(set (reg:SF 32 $f0) (mult:SF (reg:SF 34 $f2) (reg:SF 32 $f0)))",
                    "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SF 32 $f0))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                    "(const_int 24))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:DF 32 $f0) (plus:DF (reg:DF 56 $f24) (reg:DF 56 $f24)))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 17 s1)))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  cfg.rpo_blocks[0].rtl_insns[11].has_explicit_call_fusage = true;
  cfg.rpo_blocks[0].rtl_insns[11].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kDf, 6)),
  };
  cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 6, 7, 34}}};

  const RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, mips1_o2);
  ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
  int idx_a0 = -1;
  int idx_ra = -1;
  int idx_lis = -1;
  int idx_a2 = -1;
  int idx_muls = -1;
  bool saw_a1 = false;
  for (size_t i = 0; i < lifted.rpo_blocks[0].instructions.size(); ++i) {
    const std::string& s = lifted.rpo_blocks[0].instructions[i];
    if (s.find("(set (reg:SI 4 a0) (reg:SI 17 s1))") != std::string::npos) idx_a0 = i;
    if (s.find("(reg:SI 31 ra))") != std::string::npos && s.find("mem:SI") != std::string::npos) {
      idx_ra = i;
    }
    if (s.find("const_double:SF") != std::string::npos) idx_lis = i;
    if (s.find("(set (reg:DF 6 a2) (reg:DF 56 $f24))") != std::string::npos) idx_a2 = i;
    if (s.find("mult:SF") != std::string::npos) idx_muls = i;
    if (s.find("(set (reg:SI 5 a1) (reg:SI 16 s0))") != std::string::npos) saw_a1 = true;
  }
  EXPECT_GE(idx_a0, 0) << lifted.FormatSummary();
  EXPECT_LT(idx_a0, idx_ra) << lifted.FormatSummary();
  EXPECT_EQ(idx_a2, idx_lis + 1) << lifted.FormatSummary();
  EXPECT_EQ(idx_muls, idx_a2 + 1) << lifted.FormatSummary();
  EXPECT_FALSE(saw_a1) << lifted.FormatSummary();
}

TEST_F(InvertDoCrossJumpTest,
       RestoreNoopMovesConsumedDeathsPlacesSingleNoteOnDyingReadNotDuplicateOnEarlierInsn) {
  auto count_substr = [](const std::string& haystack, std::string_view needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
      ++count;
      pos += needle.size();
    }
    return count;
  };
  const CompilationProfile o2_prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // Positive (`flow.c:1670`, `jump.c:452-459`): when `$f6` dies at insn 1, is
  // reloaded from a constant at insn 2, read at insn 3, and overwritten by
  // `(set $f6 $s3)` at insn 4, `RestoreNoopMovesConsumedDeaths` attaches a
  // single `REG_DEAD $f6` to insn 3 rather than stacking duplicates on insn 1.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SF 38 $f6) (const_double:SF (cc0) 0 1071644672))",
                      "(set (reg:SF 34 $f2) (mult:SF (reg:SF 44 $f12) (reg:SF 38 $f6))) "
                      "(expr_list:REG_DEAD (reg:SF 38 $f6) (nil))",
                      "(set (reg:SF 38 $f6) (const_double:SF (cc0) 0 1070596096))",
                      "(set (reg:SF 32 $f0) (mult:SF (reg:SF 44 $f12) (reg:SF 38 $f6)))",
                      "(set (reg:SF 38 $f6) (reg:SF 19 s3))",
                      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 38 $f6)))",
                      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2)))",
                      "(use (reg/i:SF 32 $f0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    const RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_EQ(count_substr(lifted.rpo_blocks[0].instructions[1], "REG_DEAD (reg:SF 38 $f6)"), 1)
        << lifted.FormatSummary();
    EXPECT_EQ(count_substr(lifted.rpo_blocks[0].instructions[3], "REG_DEAD (reg:SF 38 $f6)"), 1)
        << lifted.FormatSummary();
  }

  // Negative: when the intervening consumer of `$f6` already carries
  // `REG_DEAD (mem:SF ...)` (a spill-slot reload consumer), it must NOT receive
  // an extra `REG_DEAD $f6` that `sched2` would duplicate as a second `MEM` death.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SF 38 $f6) (const_double:SF (cc0) 0 1071644672))",
                      "(set (reg:SF 34 $f2) (mult:SF (reg:SF 44 $f12) (reg:SF 38 $f6))) "
                      "(expr_list:REG_DEAD (reg:SF 38 $f6) (nil))",
                      "(set (reg:SF 38 $f6) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 36))))",
                      "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 38 $f6))) "
                      "(expr_list:REG_DEAD (mem:SF (plus:SI (reg:SI 29 sp) (const_int 36))) (nil))",
                      "(set (reg:SF 38 $f6) (reg:SF 19 s3))",
                      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 38 $f6)))",
                      "(use (reg/i:SF 32 $f0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    const RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, o2_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 1u);
    EXPECT_EQ(count_substr(lifted.rpo_blocks[0].instructions[3], "REG_DEAD (reg:SF 38 $f6)"), 0)
        << lifted.FormatSummary();
  }
}

TEST_F(InvertDoCrossJumpTest, CallArgGuardPreventsOverUncrossJumpingSharedCallPrefix) {
  // Mirrors `jump.c:1978` + `SelectSched2SafeSharedPrefixLen`: when a shared
  // join block starts with a call-bearing prefix (`last_call_idx > 0`) and
  // neither predecessor feeds any live MIPS ABI call-arg register surviving
  // into the call, duplicating the prefix requires every predecessor to feed
  // `shared_insns[0]` directly.
  auto make_cfg = [](bool b1_feeds_shared_first, bool b1_feeds_call_arg) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    std::vector<std::string> b1_insns;
    if (b1_feeds_call_arg) {
      b1_insns.push_back("(set (reg:SI 4 a0) (const_int 7))");
    }
    if (b1_feeds_shared_first) {
      b1_insns.push_back("(set (reg:SI 16 s0) (const_int 1))");
    } else {
      b1_insns.push_back("(set (reg:SI 17 s1) (const_int 1))");
    }
    b1_insns.push_back("(set (pc) (label_ref L1))");

    std::vector<std::string> b2_insns;
    if (b1_feeds_call_arg) {
      b2_insns.push_back("(set (reg:SI 4 a0) (const_int 8))");
    }
    b2_insns.push_back("(set (reg:SI 16 s0) (const_int 2))");
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                      "(label_ref L0) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false, std::move(b1_insns), {3}),
        MakeBlock(2, true, std::move(b2_insns), {3}),
        MakeBlock(3, true,
                  {
                      "(set (reg:SI 5 a1) (plus:SI (reg:SI 16 s0) (const_int 3)))",
                      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0)))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    cfg.rpo_blocks[3].rtl_insns[1].has_explicit_call_fusage = true;
    cfg.rpo_blocks[3].rtl_insns[1].call_fusage = {
        RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
    };
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 5, 16}},
        {1, {4, 16}},
        {2, {4}},
        {3, {4, 16}},
    };
    return cfg;
  };
  const CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips2"}};

  // Positive: neither predecessor supplies a call arg and `B1` does not write
  // `$s0` read by `shared_insns[0]`, so the call-bearing prefix in `B3` is NOT
  // duplicated into `B1`/`B2`.
  {
    const RtlControlFlowGraph lifted = InvertDoCrossJump(make_cfg(/*b1_feeds_shared_first=*/false,
                                                                  /*b1_feeds_call_arg=*/false),
                                                         o1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
    EXPECT_EQ(lifted.rpo_blocks[1].instructions.size(), 2u) << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[2].instructions.size(), 1u) << lifted.FormatSummary();
  }

  // Negative: when both predecessors supply `$s0` directly to `shared_insns[0]`,
  // duplicating the shared prefix is allowed.
  {
    const RtlControlFlowGraph lifted = InvertDoCrossJump(make_cfg(/*b1_feeds_shared_first=*/true,
                                                                  /*b1_feeds_call_arg=*/true),
                                                         o1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 4u);
    EXPECT_GT(lifted.rpo_blocks[1].instructions.size(), 3u) << lifted.FormatSummary();
  }
}

TEST_F(InvertDoCrossJumpTest, UnCrossJumpsChainedPureFallthroughAluTail) {
  // Mirrors `jump.c:1978, 2371-2470` (`UnCrossJumpChainedPureFallthroughStoreTails`):
  // a single-instruction pure-fallthrough non-copy ALU tail (`s1 = s1 + v0`)
  // shared by a jump predecessor (`B1`) and a fallthrough predecessor (`B2`)
  // is un-cross-jumped into `B1` at `-O1` (positive), whereas a tail block
  // containing a `CALL_INSN` is NOT treated as a pure-fallthrough ALU tail (negative).
  const CompilationProfile o1_prof{.optimize_level = 1, .c_flags = {"-O1", "-mips2", "-Wa,-g"}};
  auto make_cfg = [](bool tail_has_call) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 3)))",
                      "(set (pc) (label_ref L2))",
                  },
                  {3}),
        MakeBlock(2, true,
                  {
                      "(set (reg:SI 2 v0) (xor:SI (reg:SI 5 a1) (const_int 7)))",
                  },
                  {3}),
        MakeBlock(3, true,
                  tail_has_call
                      ? std::vector<std::string>{
                            "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI "
                            "(\"helper\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                        }
                      : std::vector<std::string>{
                            "(set (reg:SI 17 s1) (plus:SI (reg:SI 17 s1) (reg:SI 2 v0))) "
                            "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))",
                        },
                  {4}),
        MakeBlock(4, true,
                  {
                      "(set (reg/i:SI 2 v0) (reg:SI 17 s1))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    cfg.sideband.basic_block_live_at_start = {
        {0, {4, 5, 17}}, {1, {5, 17}}, {2, {5, 17}}, {3, {2, 17}}, {4, {17}},
    };
    return cfg;
  };

  // Positive: pure-fallthrough ALU tail `s1 = s1 + v0` is duplicated into `B1`
  // (and `B3` is merged into its sole remaining fallthrough predecessor `B2`).
  {
    const RtlControlFlowGraph cfg = make_cfg(/*tail_has_call=*/false);
    const RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, o1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 4u) << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[1].instructions.size(), 3u) << lifted.FormatSummary();
    EXPECT_NE(lifted.rpo_blocks[1].instructions[1].find("(set (reg:SI 17 s1) (plus:SI"),
              std::string::npos)
        << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[1].successors, (std::vector<size_t>{3}));
  }

  // Negative: a tail containing a `CALL_INSN` is not un-cross-jumped by the
  // pure-fallthrough store/ALU tail pass.
  {
    const RtlControlFlowGraph cfg = make_cfg(/*tail_has_call=*/true);
    const RtlControlFlowGraph lifted = InvertDoCrossJump(cfg, o1_prof);
    ASSERT_EQ(lifted.rpo_blocks.size(), 5u);
    EXPECT_EQ(lifted.rpo_blocks[1].instructions.size(), 2u) << lifted.FormatSummary();
    EXPECT_EQ(lifted.rpo_blocks[1].successors, (std::vector<size_t>{3}));
  }
}

TEST_F(InvertDoCrossJumpTest,
       RestoreJump2ConsumedRegDeadNotesTargetsEarlyclobberDiProdInOperandOrder) {
  // Mirrors `reload1.c:5599-5665` (`emit_reload_insns`) + `jump.c:452-459` +
  // `sched.c:2872-2906` (`attach_deaths_insn`): when `sched2` separates an
  // earlyclobber `adddi3_internal_1` (`t8 = t2 + v0`) from its output-reload
  // move `(set (reg/i:DI 2 v0) (reg:DI 24 t8))` with an intervening pure reader
  // of `v0` (`[a2+8] = v0`), `jump2` consumed `(REG_DEAD v0)` from `adddi3` (NOT
  // the intervening store), and restoring it must place `(REG_DEAD v0)` AFTER
  // `(REG_DEAD t2)` and BEFORE `(REG_UNUSED a0)`.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}};
  auto make_cfg = [](bool is_earlyclobber) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kJump2;
    const std::string prod_insn =
        is_earlyclobber ? "(parallel[ (set (reg:DI 24 t8) (plus:DI (reg:DI 10 t2) (reg:DI 2 v0))) "
                          "(clobber (reg:SI 4 a0)) ] ) (expr_list:REG_DEAD (reg:DI 10 t2) "
                          "(expr_list:REG_UNUSED (reg:SI 4 a0) (nil)))"
                        : "(set (reg:DI 24 t8) (xor:DI (reg:DI 10 t2) (reg:DI 2 v0))) "
                          "(expr_list:REG_DEAD (reg:DI 10 t2) (nil))";
    cfg.rpo_blocks = {
        MakeBlock(
            0, /*has_label=*/false,
            {
                "(set (reg:DI 2 v0) (ior:DI (reg:DI 2 v0) (reg:DI 12 t4))) "
                "(expr_list:REG_DEAD (reg:DI 2 v0) (expr_list:REG_DEAD (reg:DI 12 t4) (nil)))",
                prod_insn,
                "(set (mem:DI (plus:SI (reg:SI 6 a2) (const_int 8))) (reg:DI 2 v0))",
                "(set (reg/i:DI 2 v0) (reg:DI 24 t8))",
                "(use (reg/i:DI 2 v0))",
            },
            {}),
    };
    cfg.sideband.basic_block_live_at_start = {{0, {2, 3, 6, 10, 11, 12, 13}}};
    return cfg;
  };

  // Positive: earlyclobber `adddi3_internal_1` receives `(REG_DEAD (reg:DI 2 v0))`
  // after `(REG_DEAD (reg:DI 10 t2))` and before `(REG_UNUSED (reg:SI 4 a0))`;
  // the intervening store receives NO `(REG_DEAD v0)`.
  {
    const RtlControlFlowGraph lifted =
        InvertDoCrossJump(make_cfg(/*is_earlyclobber=*/true), o2_prof);
    const std::string& prod_str = lifted.rpo_blocks[0].instructions[1];
    const std::string& store_str = lifted.rpo_blocks[0].instructions[2];
    const size_t t2_pos = prod_str.find("REG_DEAD (reg:DI 10 t2)");
    const size_t v0_pos = prod_str.find("REG_DEAD (reg:DI 2 v0)");
    const size_t unused_pos = prod_str.find("REG_UNUSED (reg:SI 4 a0)");
    ASSERT_NE(t2_pos, std::string::npos) << prod_str;
    ASSERT_NE(v0_pos, std::string::npos) << prod_str;
    ASSERT_NE(unused_pos, std::string::npos) << prod_str;
    EXPECT_LT(t2_pos, v0_pos) << prod_str;
    EXPECT_LT(v0_pos, unused_pos) << prod_str;
    EXPECT_EQ(store_str.find("REG_DEAD (reg:DI 2 v0)"), std::string::npos) << store_str;
  }

  // Negative: plain non-earlyclobber `xor:DI` restores `(REG_DEAD v0)` onto the
  // intervening store instead.
  {
    const RtlControlFlowGraph lifted =
        InvertDoCrossJump(make_cfg(/*is_earlyclobber=*/false), o2_prof);
    EXPECT_EQ(lifted.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:DI 2 v0)"),
              std::string::npos)
        << lifted.rpo_blocks[0].instructions[1];
    EXPECT_NE(lifted.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:DI 2 v0)"),
              std::string::npos)
        << lifted.rpo_blocks[0].instructions[2];
  }
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
