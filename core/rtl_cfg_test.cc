#include "core/rtl_cfg.h"

#include <string>
#include <vector>

#include "core/mips.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(RtlCfgTest, AllRtlPassesAndExtensionsAreCompleteAndOrdered) {
  const auto passes = AllRtlPasses();
  ASSERT_EQ(passes.size(), 13u);
  EXPECT_EQ(passes.front(), RtlPassKind::kRtl);
  EXPECT_EQ(passes.back(), RtlPassKind::kDbr);

  EXPECT_EQ(RtlPassExtension(RtlPassKind::kRtl), ".rtl");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kJump), ".jump");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kCse), ".cse");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kLoop), ".loop");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kCse2), ".cse2");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kFlow), ".flow");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kCombine), ".combine");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kSched), ".sched");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kLreg), ".lreg");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kGreg), ".greg");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kSched2), ".sched2");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kJump2), ".jump2");
  EXPECT_EQ(RtlPassExtension(RtlPassKind::kDbr), ".dbr");
}

TEST(RtlCfgTest, CanonicalizeRtlPatternCollapsesWhitespace) {
  EXPECT_EQ(CanonicalizeRtlPattern("(set\n  (reg:SI 2 v0)\t\t(const_int 42))"),
            "(set (reg:SI 2 v0) (const_int 42))");
}

TEST(RtlCfgTest, SyncRtlInsnsPreservesMachineInstructionsAcrossPatternRewrites) {
  RtlBasicBlock blk;
  Instruction inst;
  inst.vram = 0x80001000u;
  inst.opcode = Opcode::kAddiu;
  inst.rt = Register::kV0;
  inst.rs = Register::kA0;
  inst.immediate = 5;

  RtlInsn rinsn;
  rinsn.canonical_pattern = "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 5)))";
  rinsn.vram = inst.vram;
  rinsn.machine_instructions = {inst};
  blk.AddInsn(rinsn);

  blk.instructions[0] = "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 5)))";
  blk.SyncRtlInsnsFromInstructions();

  ASSERT_EQ(blk.rtl_insns.size(), 1u);
  ASSERT_EQ(blk.rtl_insns[0].machine_instructions.size(), 1u);
  EXPECT_EQ(blk.rtl_insns[0].machine_instructions[0].vram, 0x80001000u);
  EXPECT_FALSE(blk.rtl_insns[0].is_synthetic_rtl_expansion);
}

TEST(RtlCfgTest, SyncAllBlocksPreservingMachineInsnsRecoversSplitBlocks) {
  RtlBasicBlock orig_blk;
  Instruction inst0;
  inst0.vram = 0x80001000u;
  Instruction inst1;
  inst1.vram = 0x80001004u;

  RtlInsn r0;
  r0.canonical_pattern = "(set (reg:SI 2 v0) (const_int 1))";
  r0.vram = inst0.vram;
  r0.machine_instructions = {inst0};
  orig_blk.AddInsn(r0);

  RtlInsn r1;
  r1.canonical_pattern = "(return)";
  r1.vram = inst1.vram;
  r1.machine_instructions = {inst1};
  orig_blk.AddInsn(r1);

  RtlControlFlowGraph split_cfg;
  split_cfg.rpo_blocks.resize(2);
  split_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 1))");
  split_cfg.rpo_blocks[1].AddPatternOnly("(return)");

  split_cfg.SyncAllBlocksPreservingMachineInsns({orig_blk});
  EXPECT_EQ(split_cfg.TotalMachineInsnCount(), 2u);
  EXPECT_EQ(split_cfg.rpo_blocks[0].id, 0u);
  EXPECT_EQ(split_cfg.rpo_blocks[1].id, 1u);
}

TEST(RtlCfgTest, CompareAgainstAndFormatSummaryVerifyTopologyAndPatterns) {
  RtlControlFlowGraph cfg_a;
  cfg_a.rpo_blocks.resize(2);
  cfg_a.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  cfg_a.rpo_blocks[0].successors = {1};
  cfg_a.rpo_blocks[1].AddPatternOnly("(return)");

  EXPECT_EQ(cfg_a.FormatSummary(),
            "B0 (-> [1]): (set (reg:SI 2 v0) (const_int 1))\n"
            "B1 (-> []): (return)");

  RtlControlFlowGraph cfg_b = cfg_a;
  const RtlCfgPassComparison exact_cmp = cfg_a.CompareAgainst(cfg_b);
  EXPECT_TRUE(exact_cmp.MatchesExactly());
  EXPECT_TRUE(exact_cmp.MatchesTopologyAndInsnCounts());

  cfg_b.rpo_blocks[0].instructions[0] = "(set (reg:SI 2 v0) (const_int 2))";
  const RtlCfgPassComparison pat_mismatch = cfg_a.CompareAgainst(cfg_b);
  EXPECT_TRUE(pat_mismatch.MatchesTopologyAndInsnCounts());
  EXPECT_FALSE(pat_mismatch.MatchesExactly());
  EXPECT_FALSE(pat_mismatch.mismatch_detail.empty());

  cfg_b.rpo_blocks[0].successors = {};
  const RtlCfgPassComparison edge_mismatch = cfg_a.CompareAgainst(cfg_b);
  EXPECT_FALSE(edge_mismatch.MatchesTopologyAndInsnCounts());
  EXPECT_FALSE(edge_mismatch.successors_match);
}

TEST(RtlCfgTest, RecomputeRtlCfgRpoReordersBlocksAndPrunesUnreachable) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(5);
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 8 t0) (const_int 0))");
  cfg.rpo_blocks[0].successors = {2, 1};

  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 2 v0) (const_int 1))");
  cfg.rpo_blocks[1].successors = {4};

  cfg.rpo_blocks[2].AddPatternOnly("(set (reg:SI 2 v0) (const_int 2))");
  cfg.rpo_blocks[2].successors = {4};

  cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 2 v0) (const_int 99))");
  cfg.rpo_blocks[3].successors = {4};

  cfg.rpo_blocks[4].AddPatternOnly("(return)");

  RecomputeRtlCfgRpo(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 4u);
  EXPECT_EQ(cfg.rpo_blocks[0].id, 0u);
  EXPECT_EQ(cfg.rpo_blocks[0].predecessors, std::vector<size_t>{});
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[1].id, 1u);
  EXPECT_EQ(cfg.rpo_blocks[1].predecessors, std::vector<size_t>{0});
  EXPECT_EQ(cfg.rpo_blocks[1].successors, std::vector<size_t>{3});
  EXPECT_EQ(cfg.rpo_blocks[2].id, 2u);
  EXPECT_EQ(cfg.rpo_blocks[2].predecessors, std::vector<size_t>{0});
  EXPECT_EQ(cfg.rpo_blocks[2].successors, std::vector<size_t>{3});
  EXPECT_EQ(cfg.rpo_blocks[3].id, 3u);
  EXPECT_EQ(cfg.rpo_blocks[3].predecessors, (std::vector<size_t>{1, 2}));
}

TEST(RtlCfgTest, RtlInsnStructuredExprHelpersRoundTripAndMutate) {
  RtlInsn insn;
  insn.canonical_pattern =
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 10))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))";

  auto expr_or = insn.ParseExpr();
  ASSERT_TRUE(expr_or.ok()) << expr_or.status().message();
  EXPECT_TRUE(expr_or->IsSet());
  EXPECT_EQ(expr_or->SetDest().RegNo(), 2);

  auto both_or = insn.ParseExprAndNotes();
  ASSERT_TRUE(both_or.ok()) << both_or.status().message();
  ASSERT_EQ(both_or->reg_notes.size(), 1u);
  EXPECT_EQ(both_or->reg_notes[0].kind, RtlMachineMode::kRegDead);

  both_or->ReplaceRegNo(2, 72);
  insn.SetFromExprAndNotes(*both_or);
  EXPECT_EQ(insn.canonical_pattern,
            "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 10))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
}

TEST(RtlCfgTest, RtlSidebandStateStoresRegisterAndLoopMetadata) {
  RtlControlFlowGraph cfg;
  cfg.sideband.max_regno = 75;
  cfg.sideband.reg_renumber[72] = 2;
  cfg.sideband.reg_n_sets[72] = 1;
  cfg.sideband.reg_n_deaths[72] = 1;
  cfg.sideband.reg_live_length[72] = 3;
  cfg.sideband.reg_n_calls_crossed[72] = 0;
  cfg.sideband.basic_block_live_at_start[0].insert(4);
  cfg.sideband.frame_size_bytes = 24;
  cfg.sideband.is_leaf_function = true;
  cfg.sideband.saved_gpr_regnos = {16, 31};
  cfg.sideband.epilogue_delay_pattern = "(set (reg:SI 2 v0) (const_int 0))";
  cfg.sideband.spill_slots.push_back(RtlSpillSlotInfo{
      .frame_offset = 16,
      .byte_size = 4,
      .recovered_pseudo_regno = 74,
      .mode = RtlMachineMode::kSI,
  });
  cfg.sideband.loops.push_back(RtlLoopInfo{
      .header_label_id = 0,
      .latch_block_id = 1,
      .preheader_block_id = 0,
      .body_block_ids = {1},
      .hoisted_invariant_regnos = {73},
  });

  RtlControlFlowGraph copy = cfg;
  EXPECT_EQ(copy.sideband, cfg.sideband);
  copy.sideband.reg_renumber[72] = 3;
  EXPECT_NE(copy.sideband, cfg.sideband);
}

TEST(RtlCfgTest, MipsFirstPseudoRegisterBoundaryAndFcr31HardReg) {
  RtlControlFlowGraph default_cfg;
  EXPECT_EQ(default_cfg.sideband.max_regno, kMipsFirstPseudoRegister);
  EXPECT_EQ(kMipsStatusRegnum, 67);
  EXPECT_EQ(kMipsFirstPseudoRegister, 68);

  const RtlExpr fcr31 = RtlExpr::MakeReg(RtlMachineMode::kCc, 67);
  EXPECT_TRUE(fcr31.IsHardReg());
  EXPECT_FALSE(fcr31.IsPseudoReg());
  EXPECT_EQ(RtlExpr::HardRegName(67), "$fcr31");
  EXPECT_EQ(RtlExpr::ParseHardRegName("$fcr31"), 67);
  EXPECT_EQ(fcr31.ToSexprString(), "(reg:CC 67 $fcr31)");

  const RtlExpr virt_in = RtlExpr::MakeReg(RtlMachineMode::kSi, 68);
  EXPECT_FALSE(virt_in.IsHardReg());
  EXPECT_TRUE(virt_in.IsPseudoReg());
  EXPECT_EQ(virt_in.ToSexprString(), "(reg:SI 68)");
}

TEST(RtlCfgTest, Stage0FunctionInputEqualityAndMetadataFields) {
  Stage0FunctionInput input;
  input.machine_words = {0x0c000000u, 0x00000000u, 0x03e00008u, 0x00000000u};
  input.base_vram = 0x80246000u;
  input.relocations.push_back(Stage0Relocation{
      .word_index = 0,
      .kind = Stage0RelocKind::kMips26,
      .symbol_name = "callee_fn",
      .addend = 0,
  });
  input.jump_tables.push_back(Stage0JumpTable{
      .label_name = "$L8",
      .target_word_indices = {2, 3},
      .table_vram = 0x80250000u,
  });
  input.constant_pool.push_back(Stage0ConstantPoolEntry{
      .label_name = "$LC0",
      .section_name = ".lit4",
      .byte_size = 4,
      .words = {0x3fc00000u},
      .entry_vram = 0x80250020u,
  });
  input.vram_to_symbol[0x80246100u] = "helper_fn";
  input.vram_to_symbol[0x80260000u] = "g_actor_state";
  input.vram_to_symbol_size[0x80260000u] = 64u;
  input.gp_vram = 0x80268000u;
  input.profile.optimize_level = 2;
  input.profile.c_flags = {"-O2", "-G", "0"};

  Stage0FunctionInput copy = input;
  EXPECT_EQ(copy, input);
  copy.profile.optimize_level = 0;
  EXPECT_NE(copy, input);
  copy = input;
  copy.relocations[0].symbol_name = "other_fn";
  EXPECT_NE(copy, input);
  copy = input;
  copy.jump_tables[0].table_vram = 0x80250004u;
  EXPECT_NE(copy, input);
  copy = input;
  copy.constant_pool[0].entry_vram = 0x80250024u;
  EXPECT_NE(copy, input);
  copy = input;
  copy.vram_to_symbol_size[0x80260000u] = 128u;
  EXPECT_NE(copy, input);
  copy = input;
  copy.gp_vram = 0x80269000u;
  EXPECT_NE(copy, input);

  PrologueEpilogueFrameSpec frame_a{
      .var_size = 16, .saved_gpr_mask = 0x80010000u, .saved_fpr_mask = 0x00300000u};
  PrologueEpilogueFrameSpec frame_b = frame_a;
  EXPECT_EQ(frame_a, frame_b);
  frame_b.saved_fpr_mask = 0u;
  EXPECT_NE(frame_a, frame_b);
}

TEST(RtlCfgTest, LoopNotesAndPostBarrierLoopEndPreservedAcrossRecomputeRpo) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(5);
  cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 0))");
  cfg.rpo_blocks[0].AddLoopNote(RtlLoopNoteKind::kLoopBeg);
  cfg.rpo_blocks[0].successors = {1};

  cfg.rpo_blocks[1].has_label = true;
  cfg.rpo_blocks[1].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))");
  cfg.rpo_blocks[1].AddLoopNote(RtlLoopNoteKind::kLoopCont);
  cfg.rpo_blocks[1].AddLoopNote(RtlLoopNoteKind::kLoopVtop);
  cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (lt:SI (reg:SI 72) (const_int 10)) (label_ref L0) (pc)))");
  cfg.rpo_blocks[1].successors = {1, 2};

  cfg.rpo_blocks[2].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 72))");
  cfg.rpo_blocks[2].AddPatternOnly("(set (pc) (label_ref L1))");
  cfg.rpo_blocks[2].has_trailing_barrier = true;
  cfg.rpo_blocks[2].successors = {4};

  // Block 3 is post-barrier unreachable in the CFG, containing a dead instruction
  // plus `NOTE_INSN_LOOP_END`. `RecomputeRtlCfgRpo` must strip the dead non-note
  // instruction while keeping `(note NOTE_INSN_LOOP_END)` anchored after block 2.
  cfg.rpo_blocks[3].AddPatternOnly("(set (reg:SI 73) (const_int 999))");
  cfg.rpo_blocks[3].AddLoopNote(RtlLoopNoteKind::kLoopEnd);
  cfg.rpo_blocks[3].successors = {4};

  cfg.rpo_blocks[4].has_label = true;
  cfg.rpo_blocks[4].AddPatternOnly("(return)");

  EXPECT_TRUE(cfg.rpo_blocks[0].HasLoopNotes());
  EXPECT_EQ(cfg.rpo_blocks[1].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopCont, RtlLoopNoteKind::kLoopVtop}));

  RecomputeRtlCfgRpo(&cfg);

  ASSERT_EQ(cfg.rpo_blocks.size(), 5u);
  EXPECT_EQ(cfg.rpo_blocks[3].instructions,
            (std::vector<std::string>{"(note NOTE_INSN_LOOP_END)"}));
  EXPECT_EQ(cfg.rpo_blocks[3].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopEnd}));
  EXPECT_EQ(cfg.rpo_blocks[3].successors, (std::vector<size_t>{4}));
}

TEST(RtlCfgTest, ExplicitCallFusagePreservedAcrossSyncRtlInsnsSequenceUnwrap) {
  RtlBasicBlock blk;
  RtlInsn seq_call;
  seq_call.canonical_pattern =
      "(sequence [(call_insn (parallel[ (call (mem:SI (symbol_ref:SI (\"zero_arg_fn\"))) "
      "(const_int 16)) (clobber (reg:SI 31 ra)) ] )) "
      "(insn (set (reg:SI 16 s0) (reg:SI 4 a0)))])";
  seq_call.has_explicit_call_fusage = true;
  seq_call.call_fusage = {};
  blk.AddInsn(std::move(seq_call));

  blk.instructions = {
      "(set (reg:SI 16 s0) (reg:SI 4 a0))",
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"zero_arg_fn\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
  };
  blk.SyncRtlInsnsFromInstructions();

  ASSERT_EQ(blk.rtl_insns.size(), 2u);
  EXPECT_FALSE(blk.rtl_insns[0].has_explicit_call_fusage);
  EXPECT_TRUE(blk.rtl_insns[1].has_explicit_call_fusage);
  EXPECT_TRUE(blk.rtl_insns[1].call_fusage.empty());
}

TEST(RtlCfgTest, RtlSidebandPointerRegnosAndCseJumpsAlteredEquality) {
  RtlControlFlowGraph a;
  RtlControlFlowGraph b;
  EXPECT_EQ(a.sideband, b.sideband);

  a.sideband.pointer_regnos.push_back(72);
  EXPECT_NE(a.sideband, b.sideband);
  b.sideband.pointer_regnos.push_back(72);
  EXPECT_EQ(a.sideband, b.sideband);

  a.sideband.cse_jumps_altered = true;
  EXPECT_NE(a.sideband, b.sideband);
  b.sideband.cse_jumps_altered = true;
  EXPECT_EQ(a.sideband, b.sideband);
}

TEST(RtlCfgTest, SyncRtlInsnsPreservesDistinctCallFusagesInOrderAcrossIdenticalCalleeUnwrap) {
  RtlBasicBlock blk;

  // Call 0: wrapped inside a delay-slot `SEQUENCE`, 1 argument (`a0`).
  RtlInsn seq_call;
  seq_call.canonical_pattern =
      "(sequence [(call_insn (parallel[ (call (mem:SI (symbol_ref:SI (\"same_fn\"))) "
      "(const_int 16)) (clobber (reg:SI 31 ra)) ] )) "
      "(insn (set (reg:SI 4 a0) (const_int 1)))])";
  seq_call.has_explicit_call_fusage = true;
  seq_call.call_fusage = {RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4))};
  blk.AddInsn(std::move(seq_call));

  // Call 1: bare `CALL_INSN` to the exact same symbol `"same_fn"`, 2 arguments (`a0`, `a1`).
  RtlInsn bare_call;
  bare_call.canonical_pattern =
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"same_fn\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )";
  bare_call.has_explicit_call_fusage = true;
  bare_call.call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 5)),
  };
  blk.AddInsn(std::move(bare_call));

  // Unwrap Call 0's `SEQUENCE` while leaving Call 1 untouched; both call patterns are now
  // byte-for-byte identical strings, so `SyncRtlInsnsFromInstructions` must preserve their
  // distinct `call_fusage` payloads in sequential call order rather than swapping them.
  blk.instructions = {
      "(set (reg:SI 4 a0) (const_int 1))",
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"same_fn\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"same_fn\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
  };
  blk.SyncRtlInsnsFromInstructions();

  ASSERT_EQ(blk.rtl_insns.size(), 3u);
  EXPECT_FALSE(blk.rtl_insns[0].has_explicit_call_fusage);
  EXPECT_TRUE(blk.rtl_insns[1].has_explicit_call_fusage);
  ASSERT_EQ(blk.rtl_insns[1].call_fusage.size(), 1u);
  EXPECT_EQ(blk.rtl_insns[1].call_fusage[0].ToGccPrintRtlString(), "(use (reg:SI 4 a0))");
  EXPECT_TRUE(blk.rtl_insns[2].has_explicit_call_fusage);
  ASSERT_EQ(blk.rtl_insns[2].call_fusage.size(), 2u);
  EXPECT_EQ(blk.rtl_insns[2].call_fusage[0].ToGccPrintRtlString(), "(use (reg:SI 4 a0))");
  EXPECT_EQ(blk.rtl_insns[2].call_fusage[1].ToGccPrintRtlString(), "(use (reg:SI 5 a1))");

  // Also verify non-call string literals containing `(call ` are ignored by AST call detection.
  RtlBasicBlock str_blk;
  RtlInsn orphan_call;
  orphan_call.canonical_pattern =
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"fn\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )";
  orphan_call.has_explicit_call_fusage = true;
  orphan_call.call_fusage = {RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4))};
  str_blk.AddInsn(std::move(orphan_call));
  str_blk.instructions = {"(set (reg:SI 2 v0) (symbol_ref:SI (\"str_with_(call foo)\")))"};
  str_blk.SyncRtlInsnsFromInstructions();
  ASSERT_EQ(str_blk.rtl_insns.size(), 1u);
  EXPECT_FALSE(str_blk.rtl_insns[0].has_explicit_call_fusage);
}

TEST(RtlCfgTest, CompareAgainstStrictlyEnforcesLoopNotesCallFusageAndPointerRegnos) {
  RtlControlFlowGraph expected;
  expected.rpo_blocks.resize(1);
  expected.rpo_blocks[0].AddLoopNote(RtlLoopNoteKind::kLoopBeg);
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  call_insn.call_fusage = {RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4))};
  expected.rpo_blocks[0].AddInsn(std::move(call_insn));
  expected.rpo_blocks[0].AddLoopNote(RtlLoopNoteKind::kLoopEnd);
  expected.rpo_blocks[0].AddPatternOnly("(return)");
  expected.sideband.pointer_regnos = {72, 74};

  RtlControlFlowGraph identical = expected;
  const RtlCfgPassComparison ok_cmp = identical.CompareAgainst(expected);
  EXPECT_TRUE(ok_cmp.MatchesExactly()) << ok_cmp.mismatch_detail;
  EXPECT_TRUE(ok_cmp.loop_notes_match);
  EXPECT_TRUE(ok_cmp.call_fusage_match);
  EXPECT_TRUE(ok_cmp.pointer_regnos_match);

  // 1. Corrupt `loop_note_kind` metadata (or drop loop note).
  RtlControlFlowGraph bad_loop_meta = expected;
  bad_loop_meta.rpo_blocks[0].rtl_insns[0].loop_note_kind = RtlLoopNoteKind::kLoopCont;
  const RtlCfgPassComparison loop_meta_cmp = bad_loop_meta.CompareAgainst(expected);
  EXPECT_FALSE(loop_meta_cmp.loop_notes_match);
  EXPECT_FALSE(loop_meta_cmp.MatchesExactly());

  RtlControlFlowGraph dropped_loop = expected;
  dropped_loop.rpo_blocks[0].instructions.erase(dropped_loop.rpo_blocks[0].instructions.begin());
  dropped_loop.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  const RtlCfgPassComparison dropped_loop_cmp = dropped_loop.CompareAgainst(expected);
  EXPECT_FALSE(dropped_loop_cmp.loop_notes_match);
  EXPECT_FALSE(dropped_loop_cmp.MatchesExactly());

  // 2. Corrupt or drop `call_fusage` / `has_explicit_call_fusage`.
  RtlControlFlowGraph bad_fusage_args = expected;
  bad_fusage_args.rpo_blocks[0].rtl_insns[1].call_fusage.clear();
  const RtlCfgPassComparison fusage_args_cmp = bad_fusage_args.CompareAgainst(expected);
  EXPECT_FALSE(fusage_args_cmp.call_fusage_match);
  EXPECT_FALSE(fusage_args_cmp.MatchesExactly());

  RtlControlFlowGraph dropped_fusage_flag = expected;
  dropped_fusage_flag.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage = false;
  dropped_fusage_flag.rpo_blocks[0].rtl_insns[1].call_fusage.clear();
  const RtlCfgPassComparison fusage_flag_cmp = dropped_fusage_flag.CompareAgainst(expected);
  EXPECT_FALSE(fusage_flag_cmp.call_fusage_match);
  EXPECT_FALSE(fusage_flag_cmp.MatchesExactly());

  // 3. Corrupt or drop `sideband.pointer_regnos` (and verify symmetry in both directions).
  RtlControlFlowGraph dropped_ptrs = expected;
  dropped_ptrs.sideband.pointer_regnos.pop_back();
  const RtlCfgPassComparison ptr_cmp_ab = dropped_ptrs.CompareAgainst(expected);
  const RtlCfgPassComparison ptr_cmp_ba = expected.CompareAgainst(dropped_ptrs);
  EXPECT_FALSE(ptr_cmp_ab.pointer_regnos_match);
  EXPECT_FALSE(ptr_cmp_ab.MatchesExactly());
  EXPECT_FALSE(ptr_cmp_ba.pointer_regnos_match);
  EXPECT_FALSE(ptr_cmp_ba.MatchesExactly());
}

}  // namespace
}  // namespace rom_nom_nom
