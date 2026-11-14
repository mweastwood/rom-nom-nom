#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "core/compiler.h"
#include "core/elf_reader.h"
#include "core/process.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_sexpr_utils.h"
#include "core/toolchain.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::gcc_harness {
namespace {

void VerifyTraceCompletenessAndReplayability(GccContext* /*ctx*/, FunctionHooks* hooks,
                                             const FunctionCompilationTrace& trace,
                                             int optimize_level = 2) {
  // 1. All 13 RTL pass snapshots (`.00.rtl` through `.12.dbr`) must be present
  // and structurally valid.
  for (RtlPassKind pass : AllRtlPasses()) {
    auto it = trace.pass_snapshots.find(pass);
    ASSERT_NE(it, trace.pass_snapshots.end())
        << "Missing pass snapshot for " << RtlPassExtension(pass) << " in " << trace.function_name;
    const RtlControlFlowGraph& cfg = it->second;
    EXPECT_EQ(cfg.pass, pass);
    EXPECT_FALSE(cfg.rpo_blocks.empty())
        << "Empty RPO blocks for " << RtlPassExtension(pass) << " in " << trace.function_name;
    size_t total_insns = 0;
    for (const auto& blk : cfg.rpo_blocks) {
      total_insns += blk.instructions.size();
    }
    EXPECT_GT(total_insns, 0u) << "Zero instructions in pass " << RtlPassExtension(pass) << " for "
                               << trace.function_name;
  }

  // Flow snapshot must carry populated liveness sideband arrays when `optimize > 0`.
  const RtlControlFlowGraph& flow_cfg = trace.pass_snapshots.at(RtlPassKind::kFlow);
  if (optimize_level > 0) {
    EXPECT_FALSE(flow_cfg.sideband.reg_n_sets.empty());
    EXPECT_FALSE(flow_cfg.sideband.basic_block_live_at_start.empty());
  }

  // Post-reload `.09.greg` through `.12.dbr` must have no remaining pseudo
  // registers (`>= 68`) in executable instructions.
  const RtlControlFlowGraph& dbr_cfg = trace.pass_snapshots.at(RtlPassKind::kDbr);
  for (const auto& blk : dbr_cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      EXPECT_FALSE(absl::StrContains(pat, "virtual-stack-vars")) << pat;
      EXPECT_FALSE(absl::StrContains(pat, "virtual-outgoing-args")) << pat;
    }
  }

  // 2. All 35 subfunction boundary snapshots (covering all 28 unique Stage 0..12
  // + `instantiate_virtual_regs` subfunctions) must be present and replayable:
  // materializing `pre_state` into `GccContext` and running the corresponding
  // forward hook must reproduce `post_state`, `post_asm_lines`, and
  // `post_machine_words` identically.
  const auto all_ids = AllSubfunctionIds();
  ASSERT_EQ(all_ids.size(), 35u);
  ASSERT_EQ(trace.subfunction_snapshots.size(), all_ids.size());
  EXPECT_EQ(trace.optimize_level, optimize_level);

  for (size_t i = 0; i < trace.subfunction_snapshots.size(); ++i) {
    const SubfunctionBoundarySnapshot& snap = trace.subfunction_snapshots[i];
    EXPECT_EQ(snap.subfunction_id, all_ids[i]);
    EXPECT_EQ(snap.optimize_level, trace.optimize_level);
    EXPECT_EQ(snap.c_flags, trace.c_flags);

    const SubfunctionBoundarySnapshot replayed =
        hooks->ExecuteSubfunctionBoundary(snap, snap.pre_state, snap.pre_asm_lines);

    const RtlCfgPassComparison cmp = replayed.post_state.CompareAgainst(snap.post_state);
    EXPECT_TRUE(cmp.MatchesExactly())
        << "Subfunction replay structural mismatch at " << SubfunctionIdName(snap.subfunction_id)
        << " in " << trace.function_name << ": " << cmp.mismatch_detail;
    EXPECT_EQ(replayed.post_state.FormatSummary(), snap.post_state.FormatSummary())
        << "Subfunction replay CFG summary mismatch at " << SubfunctionIdName(snap.subfunction_id)
        << " in " << trace.function_name;
    EXPECT_EQ(replayed.post_state.sideband, snap.post_state.sideband)
        << "Subfunction replay exact sideband mismatch at "
        << SubfunctionIdName(snap.subfunction_id) << " in " << trace.function_name;
    EXPECT_EQ(replayed.post_asm_lines, snap.post_asm_lines)
        << "Subfunction replay asm mismatch at " << SubfunctionIdName(snap.subfunction_id) << " in "
        << trace.function_name;
    EXPECT_EQ(replayed.post_machine_words, snap.post_machine_words)
        << "Subfunction replay machine words mismatch at " << SubfunctionIdName(snap.subfunction_id)
        << " in " << trace.function_name;

    if (snap.subfunction_id == SubfunctionId::kGlobalAlloc &&
        MaxPseudoRegInCfg(snap.pre_state) >= 68) {
      EXPECT_GE(MaxPseudoRegInCfg(snap.post_state), 68)
          << "Expected pseudo-registers to remain in kGlobalAlloc post_state for kReload/kAlterReg "
             "in "
          << trace.function_name;
      if (optimize_level > 0) {
        EXPECT_FALSE(snap.post_state.sideband.reg_renumber.empty())
            << "Expected non-empty sideband.reg_renumber after kGlobalAlloc in "
            << trace.function_name;
      }
    }
    if (snap.subfunction_id == SubfunctionId::kAlterReg &&
        MaxPseudoRegInCfg(snap.pre_state) >= 68) {
      EXPECT_LT(MaxPseudoRegInCfg(snap.post_state), 68)
          << "Expected kAlterReg to rewrite all pseudo-registers (>= 68) into hard registers (< "
             "68) in "
          << trace.function_name;
    }
    if (snap.subfunction_id == SubfunctionId::kFinalScanInsn) {
      EXPECT_TRUE(snap.pre_asm_lines.empty())
          << "Expected empty pre_asm_lines entering kFinalScanInsn in " << trace.function_name;
      EXPECT_FALSE(snap.post_asm_lines.empty())
          << "Expected non-empty post_asm_lines produced by kFinalScanInsn in "
          << trace.function_name;
    }

    auto check_pass_match = [&](RtlPassKind pass_kind) {
      const RtlCfgPassComparison pass_cmp =
          snap.post_state.CompareAgainst(trace.pass_snapshots.at(pass_kind));
      EXPECT_TRUE(pass_cmp.MatchesExactly())
          << "Subfunction post_state mismatch against pass_snapshots["
          << RtlPassExtension(pass_kind) << "] at " << SubfunctionIdName(snap.subfunction_id)
          << " in " << trace.function_name << ": " << pass_cmp.mismatch_detail;
    };
    switch (snap.subfunction_id) {
      case SubfunctionId::kJump1Optimize:
        check_pass_match(RtlPassKind::kJump);
        break;
      case SubfunctionId::kJumpOptimizeCse1:
        check_pass_match(RtlPassKind::kCse);
        break;
      case SubfunctionId::kStrengthReduce:
        check_pass_match(RtlPassKind::kLoop);
        break;
      case SubfunctionId::kThreadJumpsCse2:
        check_pass_match(RtlPassKind::kCse2);
        break;
      case SubfunctionId::kPropagateBlock:
        check_pass_match(RtlPassKind::kFlow);
        break;
      case SubfunctionId::kTryCombine:
        check_pass_match(RtlPassKind::kCombine);
        break;
      case SubfunctionId::kScheduleBlockSched1:
        check_pass_match(RtlPassKind::kSched);
        break;
      case SubfunctionId::kBlockAlloc:
        check_pass_match(RtlPassKind::kLreg);
        break;
      case SubfunctionId::kAlterReg:
        check_pass_match(RtlPassKind::kGreg);
        break;
      case SubfunctionId::kScheduleBlockSched2:
        check_pass_match(RtlPassKind::kSched2);
        break;
      case SubfunctionId::kDoCrossJump:
        check_pass_match(RtlPassKind::kJump2);
        break;
      case SubfunctionId::kMakeReturnInsns:
        check_pass_match(RtlPassKind::kDbr);
        break;
      default:
        break;
    }
  }

  // 3. Stage 0 (`final` + `GAS`) outputs must be non-empty and contain a valid
  // MIPS return (`jr $31` == `0x03e00008`).
  EXPECT_FALSE(trace.final_asm_text.empty());
  EXPECT_FALSE(trace.final_insn_asm_lines.empty());
  ASSERT_GE(trace.final_machine_words.size(), 2u);
  bool has_jr_ra = false;
  for (uint32_t word : trace.final_machine_words) {
    if (word == 0x03e00008u) {
      has_jr_ra = true;
      break;
    }
  }
  EXPECT_TRUE(has_jr_ra) << "Expected jr $31 (0x03e00008) in final machine words for "
                         << trace.function_name << "; asm:\n"
                         << trace.final_asm_text;
}

TEST(CompilationTraceTest, TraceRtlPipelineOnHandCraftedRtl00Cfg) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  RtlControlFlowGraph rtl_00;
  rtl_00.pass = RtlPassKind::kRtl;
  RtlBasicBlock b0;
  b0.id = 0;
  b0.AddPatternOnly("(set (reg/v:SI 72) (reg:SI 4 a0))");
  b0.AddPatternOnly("(set (reg/v:SI 73) (reg:SI 5 a1))");
  b0.AddPatternOnly("(set (reg:SI 74) (plus:SI (reg/v:SI 72) (reg/v:SI 73)))");
  b0.AddPatternOnly("(set (reg:SI 75) (plus:SI (reg/v:SI 72) (reg/v:SI 73)))");
  b0.AddPatternOnly("(set (reg:SI 76) (plus:SI (reg:SI 74) (reg:SI 75)))");
  b0.AddPatternOnly("(set (reg:SI 2 v0) (reg:SI 76))");
  b0.AddPatternOnly("(use (reg/i:SI 2 v0))");
  rtl_00.rpo_blocks.push_back(b0);

  const FunctionCompilationTrace trace =
      hooks.TraceRtlPipeline(rtl_00, /*optimize_level=*/2, "handcrafted_add_cse", "handcrafted");

  VerifyTraceCompletenessAndReplayability(&ctx, &hooks, trace);
}

TEST(CompilationTraceTest, SubfunctionBoundariesExerciseFoldSimplifySubstReloadAndAlterReg) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  // 1. `kFoldRtx` folds constant arithmetic inside instruction patterns.
  ctx.BeginFunction("fold_boundary_fn");
  RtlControlFlowGraph fold_pre;
  fold_pre.pass = RtlPassKind::kJump;
  fold_pre.rpo_blocks.resize(1);
  fold_pre.rpo_blocks[0].id = 0;
  fold_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (const_int 10) (const_int 22)))");
  fold_pre.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 72))");
  fold_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  const SubfunctionBoundarySnapshot fold_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kFoldRtx, fold_pre, {}, /*optimize_level=*/2);
  ASSERT_EQ(fold_snap.post_state.BlockCount(), 1u);
  ASSERT_GE(fold_snap.post_state.rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(fold_snap.post_state.rpo_blocks[0].instructions[0], "(set (reg:SI 72) (const_int 32))");

  // 2. `kSimplifyRtx` simplifies compound algebraic/comparison RTL in instruction patterns.
  ctx.BeginFunction("simplify_boundary_fn");
  RtlControlFlowGraph simp_pre;
  simp_pre.pass = RtlPassKind::kFlow;
  simp_pre.rpo_blocks.resize(1);
  simp_pre.rpo_blocks[0].id = 0;
  simp_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (xor:SI (lt:SI (reg:SI 4 a0) (const_int 1)) (const_int 1)))");
  simp_pre.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (reg:SI 72))");
  simp_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  const SubfunctionBoundarySnapshot simp_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kSimplifyRtx, simp_pre, {}, /*optimize_level=*/2);
  ASSERT_EQ(simp_snap.post_state.BlockCount(), 1u);
  ASSERT_GE(simp_snap.post_state.rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(simp_snap.post_state.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (ge:SI (reg:SI 4 a0) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  // 3. `kSubst` substitutes and canonicalizes instruction patterns via `combine.c:subst`.
  ctx.BeginFunction("subst_boundary_fn");
  const SubfunctionBoundarySnapshot subst_snap =
      hooks.ExecuteSubfunctionBoundary(SubfunctionId::kSubst, simp_pre, {}, /*optimize_level=*/2);
  ASSERT_EQ(subst_snap.post_state.BlockCount(), 1u);
  ASSERT_GE(subst_snap.post_state.rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(subst_snap.post_state.rpo_blocks[0].instructions[0],
            "(set (reg:SI 72) (gt:SI (reg:SI 4 a0) (const_int 0))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  // 4. `kGlobalAlloc` -> `kReload` -> `kAlterReg` rewrites pseudo-registers (`>= 68`)
  // into hard registers (`< 68`).
  ctx.BeginFunction("alloc_reload_alter_boundary_fn");
  RtlControlFlowGraph alloc_pre;
  alloc_pre.pass = RtlPassKind::kLreg;
  alloc_pre.rpo_blocks.resize(1);
  alloc_pre.rpo_blocks[0].id = 0;
  alloc_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 7)))");
  alloc_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (ashift:SI (reg:SI 72) (const_int 2)))");
  alloc_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");
  EXPECT_GE(MaxPseudoRegInCfg(alloc_pre), 72);

  const SubfunctionBoundarySnapshot global_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kGlobalAlloc, alloc_pre, {}, /*optimize_level=*/2);
  EXPECT_GE(MaxPseudoRegInCfg(global_snap.post_state), 72);
  ASSERT_TRUE(global_snap.post_state.sideband.reg_renumber.contains(72));
  EXPECT_GE(global_snap.post_state.sideband.reg_renumber.at(72), 0);
  EXPECT_LT(global_snap.post_state.sideband.reg_renumber.at(72), 68);

  const SubfunctionBoundarySnapshot reload_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, global_snap.post_state, {}, /*optimize_level=*/2);
  EXPECT_GE(MaxPseudoRegInCfg(reload_snap.post_state), 72);

  const SubfunctionBoundarySnapshot alter_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kAlterReg, reload_snap.post_state, {}, /*optimize_level=*/2);
  EXPECT_GE(MaxPseudoRegInCfg(alter_snap.pre_state), 72);
  EXPECT_LT(MaxPseudoRegInCfg(alter_snap.post_state), 68);

  // 5. `kUpdateEquivRegs` -> `kBlockAlloc` -> `kGlobalAlloc` preserves `reg_live_length`
  // without redundant `update_equiv_regs()` doubling and preserves `kBlockAlloc` `reg_renumber`.
  ctx.BeginFunction("equiv_block_global_boundary_fn");
  RtlControlFlowGraph equiv_pre;
  equiv_pre.pass = RtlPassKind::kSched;
  equiv_pre.rpo_blocks.resize(1);
  equiv_pre.rpo_blocks[0].id = 0;
  equiv_pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (const_int 42)) (expr_list:REG_EQUAL (const_int 42) (nil))");
  equiv_pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg:SI 4 a0) (reg:SI 72)))");
  equiv_pre.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (reg:SI 72)))");
  equiv_pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const SubfunctionBoundarySnapshot equiv_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kUpdateEquivRegs, equiv_pre, {}, /*optimize_level=*/2);
  ASSERT_TRUE(equiv_snap.post_state.sideband.reg_live_length.contains(72));
  const int equiv_live_len = equiv_snap.post_state.sideband.reg_live_length.at(72);

  const SubfunctionBoundarySnapshot block_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kBlockAlloc, equiv_snap.post_state, {}, /*optimize_level=*/2);
  ASSERT_TRUE(block_snap.post_state.sideband.reg_live_length.contains(72));
  EXPECT_EQ(block_snap.post_state.sideband.reg_live_length.at(72), equiv_live_len);
  ASSERT_TRUE(block_snap.post_state.sideband.reg_renumber.contains(72));
  const int block_hard_reg = block_snap.post_state.sideband.reg_renumber.at(72);

  const SubfunctionBoundarySnapshot global_after_block_snap = hooks.ExecuteSubfunctionBoundary(
      SubfunctionId::kGlobalAlloc, block_snap.post_state, {}, /*optimize_level=*/2);
  ASSERT_TRUE(global_after_block_snap.post_state.sideband.reg_renumber.contains(72));
  EXPECT_EQ(global_after_block_snap.post_state.sideband.reg_renumber.at(72), block_hard_reg);
}

void VerifyTraceMatchesActualToolchain(const Toolchain& toolchain, std::string_view c_source,
                                       std::string_view fn_name,
                                       const std::vector<std::string>& c_flags,
                                       const FunctionCompilationTrace& trace) {
  static int counter = 0;
  const std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() /
      absl::StrCat("compilation_trace_verify_", getpid(), "_", ++counter);
  std::error_code ec;
  std::filesystem::create_directories(temp_dir, ec);
  ASSERT_FALSE(ec) << ec.message();

  const std::filesystem::path src_file = temp_dir / "input.c";
  const std::filesystem::path s272_file = temp_dir / "input.s272";
  const std::filesystem::path obj_file = temp_dir / "input.o";

  {
    std::ofstream out(src_file);
    out << c_source << "\n";
  }

  std::vector<std::string> gcc_flags = c_flags;
  gcc_flags.push_back("-da");

  const std::vector<std::string> gcc_args =
      toolchain.BuildGcc272CompileArgs(gcc_flags, {}, src_file, s272_file);
  const auto gcc_res = RunProcess(ProcessOptions{
      .program = toolchain.Gcc272().string(),
      .args = gcc_args,
      .working_directory = temp_dir,
  });
  ASSERT_TRUE(gcc_res.ok()) << gcc_res.status();
  ASSERT_TRUE(gcc_res->Ok()) << gcc_res->stderr_output;

  const std::vector<std::string> as_args =
      toolchain.BuildKmcAsArgs(c_flags, {}, {}, s272_file, obj_file);
  const auto as_res = RunProcess(ProcessOptions{
      .program = toolchain.KmcAs().string(),
      .args = as_args,
      .working_directory = temp_dir,
  });
  ASSERT_TRUE(as_res.ok()) << as_res.status();
  ASSERT_TRUE(as_res->Ok()) << as_res->stderr_output;

  size_t emitted_pass_dumps = 0;
  for (RtlPassKind pass : AllRtlPasses()) {
    const std::filesystem::path dump_path =
        temp_dir / absl::StrCat("input.c", RtlPassExtension(pass));
    std::ifstream dump_in(dump_path);
    ASSERT_TRUE(dump_in.good()) << "Missing dump file: " << dump_path;
    std::ostringstream ss;
    ss << dump_in.rdbuf();
    std::string dump_text = ss.str();
    absl::StrReplaceAll({{"\"*$LC", "\"$LC"}, {"\"*.LC", "\".LC"}}, &dump_text);
    RtlPassDump pass_dump = ParseRtlPassDump(pass, dump_text);
    const RtlFunctionDump* fn_dump = pass_dump.FindFunction(fn_name);
    if (fn_dump == nullptr) {
      // At `-O0` and `-O1`, `cc1` leaves dump files empty for skipped passes.
      continue;
    }
    ++emitted_pass_dumps;
    RtlControlFlowGraph real_cfg = BuildCanonicalCfgFromRtlDump(*fn_dump);
    RtlControlFlowGraph trace_cfg = trace.pass_snapshots.at(pass);
    if (fn_name == "sum_scaled_array" || fn_name == "swap_and_accumulate") {
      EXPECT_FALSE(trace_cfg.sideband.pointer_regnos.empty())
          << "Expected non-empty sideband.pointer_regnos in " << fn_name << " pass "
          << RtlPassExtension(pass);
    }
    // External `cc1 -da` text dumps (`print-rtl.c`) do not print `regno_pointer_flag[]`,
    // whereas `RtlControlFlowGraph::CompareAgainst` strictly and unconditionally compares
    // `sideband.pointer_regnos`.
    real_cfg.sideband.pointer_regnos = trace_cfg.sideband.pointer_regnos;
    // Stock external `cc1` (`ARG_POINTER_REGNUM == 0`) prints `arg_pointer_rtx`
    // as `(reg:SI 0 $0)` in `-da` dumps (including inside `REG_EQUIV` notes on
    // post-reload dumps), whereas in-process `TraceForwardCompilation`
    // disambiguates `arg_pointer_rtx` as `(reg:SI 0 argp)`.
    for (auto& blk : trace_cfg.rpo_blocks) {
      for (std::string& insn : blk.instructions) {
        absl::StrReplaceAll({{" 0 argp)", " 0 $0)"}}, &insn);
      }
      blk.SyncRtlInsnsFromInstructions();
    }
    const RtlCfgPassComparison cmp = trace_cfg.CompareAgainst(real_cfg);
    EXPECT_TRUE(cmp.MatchesExactly())
        << "Pass " << RtlPassExtension(pass) << " mismatch against real cc1 in " << fn_name << ": "
        << cmp.mismatch_detail;
  }
  EXPECT_GE(emitted_pass_dumps, 4u);

  const auto elf_or = ElfReader::LoadFromFile(obj_file);
  ASSERT_TRUE(elf_or.ok()) << elf_or.status();
  const auto elf_fn_or = elf_or->ExtractFunction(fn_name);
  ASSERT_TRUE(elf_fn_or.ok()) << elf_fn_or.status();

  std::vector<uint32_t> trace_words = trace.final_machine_words;
  std::vector<uint32_t> elf_words = elf_fn_or->raw_words;
  // Normalize `.text` trailing 16-byte section-alignment `nop` (`0x00000000`)
  // padding when the full TU contains multiple symbols or section switches.
  while (trace_words.size() > elf_words.size() && trace_words.back() == 0u) {
    trace_words.pop_back();
  }
  while (elf_words.size() > trace_words.size() && elf_words.back() == 0u) {
    elf_words.pop_back();
  }
  // When the full TU defines multiple `.rdata`/`.data`/`.bss` objects before
  // `fn_name`, section-relative `R_MIPS_HI16` (`5`), `R_MIPS_LO16` (`6`), and
  // `R_MIPS_LITERAL` (`8`) relocations in `elf_words` embed the TU-wide section
  // offset in the low 16 bits, whereas single-function `trace_words` starts its
  // local constant pool at offset 0. Verify matching relocation opcodes and
  // mask the relocation immediate field on those section-relative words.
  for (const ElfRelocation& rel : elf_fn_or->relocations) {
    const size_t w = rel.function_offset / 4;
    if (w < trace_words.size() && w < elf_words.size() && !rel.symbol_name.empty() &&
        rel.symbol_name.front() == '.' && (rel.type == 5u || rel.type == 6u || rel.type == 8u)) {
      trace_words[w] &= 0xffff0000u;
      elf_words[w] &= 0xffff0000u;
    }
  }
  EXPECT_EQ(trace_words, elf_words)
      << "Machine words mismatch against real KMC AS in " << fn_name << "; trace asm:\n"
      << trace.final_asm_text;

  std::filesystem::remove_all(temp_dir, ec);
}

int OptimizeLevelFromFlags(absl::Span<const std::string> c_flags) {
  int opt = 2;
  for (std::string_view flag : c_flags) {
    if (flag == "-O0") {
      opt = 0;
    } else if (flag == "-O" || flag == "-O1") {
      opt = 1;
    } else if (flag == "-O2") {
      opt = 2;
    } else if (flag == "-O3") {
      opt = 3;
    }
  }
  return opt;
}

TEST(CompilationTraceTest, TraceForwardCompilationCategories) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const auto toolchain_or = Toolchain::Discover(ToolchainOptions{.mode = ToolchainMode::kOriginal});
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  struct CategoryCase {
    const char* name;
    const char* fn_name;
    const char* c_src;
  };

  const CategoryCase cases[] = {
      {
          "arithmetic",
          "arith_fold_and_combine",
          R"(
            int arith_fold_and_combine(int a, int b) {
              int x = (a + 3) + 5;
              int y = (b << 2) + x;
              return y ^ (a + 8);
            }
          )",
      },
      {
          "branches",
          "branch_diamond",
          R"(
            int branch_diamond(int a, int b, int c) {
              int res;
              if (a > b) {
                res = a + c;
              } else if (a == b) {
                res = b - c;
              } else {
                res = a + c;
              }
              return res * 2;
            }
          )",
      },
      {
          "loops",
          "sum_scaled_array",
          R"(
            int sum_scaled_array(int* p, int n, int scale) {
              int total = 0;
              int i;
              for (i = 0; i < n; i++) {
                total += p[i] + (scale * 4);
              }
              return total;
            }
          )",
      },
      {
          "memory_loads_stores",
          "swap_and_accumulate",
          R"(
            struct Pair {
              int x;
              int y;
              short tag;
            };
            int swap_and_accumulate(struct Pair* p, int delta) {
              int old_x = p->x;
              int old_y = p->y;
              p->x = old_y + delta;
              p->y = old_x - delta;
              p->tag = (short)(old_x + old_y);
              return p->x + p->y;
            }
          )",
      },
      {
          "function_calls",
          "caller_with_saved_regs",
          R"(
            int ext_op(int a, int b);
            int caller_with_saved_regs(int x, int y, int z) {
              int first = ext_op(x + 1, y);
              int second = ext_op(first, z + x);
              return first + second + y;
            }
          )",
      },
      {
          "multi_ret_void_leaf",
          "multi_ret_void_leaf",
          R"(
            void multi_ret_void_leaf(int x, int y, int* p, int* q) {
              if (x) {
                *p = 1;
                return;
              }
              if (y) {
                *q = 2;
                return;
              }
            }
          )",
      },
  };

  for (const auto& tc : cases) {
    SCOPED_TRACE(tc.name);
    const FunctionCompilationTrace trace =
        hooks.TraceForwardCompilation(tc.c_src, tc.fn_name, {"-O2"}, tc.name);
    VerifyTraceCompletenessAndReplayability(&ctx, &hooks, trace);
    VerifyTraceMatchesActualToolchain(*toolchain_or, tc.c_src, tc.fn_name, {"-O2"}, trace);
    if (std::string_view(tc.fn_name) == "sum_scaled_array") {
      bool found_scan_loop = false;
      bool found_strength_reduce = false;
      for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
        if (snap.subfunction_id == SubfunctionId::kScanLoop) {
          found_scan_loop = true;
          EXPECT_FALSE(snap.pre_state.CompareAgainst(snap.post_state).MatchesExactly())
              << "Expected kScanLoop to hoist loop-invariant scale*4 in sum_scaled_array";
        } else if (snap.subfunction_id == SubfunctionId::kStrengthReduce) {
          found_strength_reduce = true;
          EXPECT_FALSE(snap.pre_state.CompareAgainst(snap.post_state).MatchesExactly())
              << "Expected kStrengthReduce to perform induction-variable strength reduction in "
                 "sum_scaled_array";
        }
      }
      EXPECT_TRUE(found_scan_loop);
      EXPECT_TRUE(found_strength_reduce);
    }
  }
}

TEST(CompilationTraceTest, TraceForwardCompilationBaseRoundTripCorpusSubset) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const std::vector<RoundTripTestCase> all_cases = GetBaseRoundTripTestCases();
  ASSERT_GE(all_cases.size(), 10u);

  // Trace a representative subset spanning arithmetic, pointers, loops, structs,
  // and calls from `GetBaseRoundTripTestCases()`.
  const size_t indices_to_test[] = {0, 1, 2, 4, 6, 8, 10, 12};
  for (size_t idx : indices_to_test) {
    if (idx >= all_cases.size()) {
      continue;
    }
    const RoundTripTestCase& tc = all_cases[idx];
    SCOPED_TRACE(tc.test_name);
    const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(tc);
    VerifyTraceCompletenessAndReplayability(&ctx, &hooks, trace);
  }
}

TEST(CompilationTraceTest, MatchesActualToolchainDumpsAndMachineWords) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const auto toolchain_or = Toolchain::Discover(ToolchainOptions{.mode = ToolchainMode::kOriginal});
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  static constexpr std::string_view kBaseTypesPreamble = R"(
typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
typedef struct { u8 b[4]; } UnalignedWord;
typedef struct { u8 b[6]; } UnalignedBlock6;
typedef struct { u8 b[8]; } UnalignedBlock8;
typedef struct { u8 b[12]; } UnalignedBlock12;
typedef struct { u8 b[16]; } UnalignedBlock16;
)";

  const std::vector<RoundTripTestCase> all_cases = GetBaseRoundTripTestCases();
  ASSERT_GE(all_cases.size(), 10u);
  const size_t indices_to_test[] = {0, 1, 2, 4, 6, 8, 10, 12};
  for (size_t idx : indices_to_test) {
    if (idx >= all_cases.size()) {
      continue;
    }
    const RoundTripTestCase& tc = all_cases[idx];
    SCOPED_TRACE(tc.test_name);
    const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(tc);
    VerifyTraceCompletenessAndReplayability(&ctx, &hooks, trace);

    std::string full_source = absl::StrCat(kBaseTypesPreamble, "\n");
    if (!tc.options.extra_types_header.empty()) {
      absl::StrAppend(&full_source, tc.options.extra_types_header, "\n");
    }
    absl::StrAppend(&full_source, tc.original_c);
    VerifyTraceMatchesActualToolchain(*toolchain_or, full_source, tc.function_name,
                                      tc.options.c_flags, trace);
  }
}

TEST(CompilationTraceTest, SweepAllFiveCompilerFlagProfilesMatchesActualToolchain) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const auto toolchain_or = Toolchain::Discover(ToolchainOptions{.mode = ToolchainMode::kOriginal});
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  static constexpr std::string_view kBaseTypesPreamble = R"(
typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
typedef struct { u8 b[4]; } UnalignedWord;
typedef struct { u8 b[6]; } UnalignedBlock6;
typedef struct { u8 b[8]; } UnalignedBlock8;
typedef struct { u8 b[12]; } UnalignedBlock12;
typedef struct { u8 b[16]; } UnalignedBlock16;
)";

  const auto profiles = GetRoundTripCompilerFlagProfiles();
  ASSERT_EQ(profiles.size(), 7u);

  const std::vector<RoundTripTestCase> base_cases = GetBaseRoundTripTestCases();
  ASSERT_GE(base_cases.size(), 10u);
  const size_t indices_to_test[] = {0, 1, 2, 4, 6};

  for (const auto& profile : profiles) {
    SCOPED_TRACE(profile.suffix);
    const int opt_level = OptimizeLevelFromFlags(profile.c_flags);
    for (size_t idx : indices_to_test) {
      const RoundTripTestCase& base_tc = base_cases[idx];
      RoundTripTestCase tc = base_tc;
      tc.test_name = absl::StrCat(base_tc.test_name, "_", profile.suffix);
      tc.options.c_flags = profile.c_flags;
      SCOPED_TRACE(tc.test_name);

      const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(tc);
      VerifyTraceCompletenessAndReplayability(&ctx, &hooks, trace, opt_level);

      std::string full_source = absl::StrCat(kBaseTypesPreamble, "\n");
      if (!tc.options.extra_types_header.empty()) {
        absl::StrAppend(&full_source, tc.options.extra_types_header, "\n");
      }
      absl::StrAppend(&full_source, tc.original_c);
      VerifyTraceMatchesActualToolchain(*toolchain_or, full_source, tc.function_name,
                                        tc.options.c_flags, trace);
    }
  }
}

TEST(CompilationTraceTest, SubfunctionBoundaryRightInverseAndPersistedCFlagsAcrossProfiles) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RightInverseVerifier verifier(&ctx, &hooks);

  constexpr std::string_view kSource = "int add_shift(int a, int b) { return (a + b) << 2; }\n";
  const auto profiles = GetRoundTripCompilerFlagProfiles();
  ASSERT_EQ(profiles.size(), 7u);

  for (const auto& profile : profiles) {
    SCOPED_TRACE(profile.suffix);
    const FunctionCompilationTrace trace =
        hooks.TraceForwardCompilation(kSource, "add_shift", profile.c_flags, profile.suffix);
    EXPECT_EQ(trace.c_flags, profile.c_flags);
    ASSERT_EQ(trace.subfunction_snapshots.size(), AllSubfunctionIds().size());
    for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
      SCOPED_TRACE(SubfunctionIdName(snap.subfunction_id));
      EXPECT_EQ(snap.c_flags, profile.c_flags);
      const RightInverseResult res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(res.matches) << SubfunctionIdName(snap.subfunction_id) << " (" << profile.suffix
                               << "): " << res.diff_detail;
    }
  }
}

TEST(CompilationTraceTest, Stage0InputCapturesRelocationsJumpTablesAndConstantPool) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  constexpr std::string_view kSource = R"(
extern int helper_a(int);
extern int helper_b(int, int, int, int, int, int);
extern int g_bias;
double compute_entry(int tag, int x) {
  switch (tag) {
    case 0: return (double)helper_a(x) + 1.25;
    case 1: return (double)helper_b(x, 1, 2, 3, 4, 5) + 3.141592653589793;
    case 2: return (double)(x + g_bias);
    case 3: return (double)(x - g_bias);
    case 4: return (double)(x * 5);
    default: return 0.0;
  }
}
)";
  const FunctionCompilationTrace trace =
      hooks.TraceForwardCompilation(kSource, "compute_entry", {"-O2"}, "stage0_trace_metadata");
  EXPECT_EQ(trace.function_name, "compute_entry");
  EXPECT_FALSE(trace.stage0_input.machine_words.empty());
  EXPECT_EQ(trace.stage0_input.jump_tables.size(), 1u);
  EXPECT_FALSE(trace.stage0_input.constant_pool.empty());
  EXPECT_EQ(trace.pass_snapshots.at(RtlPassKind::kDbr).sideband.outgoing_args_size, 24);

  bool saw_helper_a = false;
  bool saw_helper_b = false;
  bool saw_g_bias = false;
  for (const auto& r : trace.stage0_input.relocations) {
    if (r.kind == Stage0RelocKind::kMips26 && r.symbol_name == "helper_a") saw_helper_a = true;
    if (r.kind == Stage0RelocKind::kMips26 && r.symbol_name == "helper_b") saw_helper_b = true;
    if (r.symbol_name == "g_bias") saw_g_bias = true;
  }
  EXPECT_TRUE(saw_helper_a);
  EXPECT_TRUE(saw_helper_b);
  EXPECT_TRUE(saw_g_bias);
}

TEST(CompilationTraceTest, NewConstructCategoriesAndRetailFlagProfilesMatchActualToolchain) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const auto toolchain_or = Toolchain::Discover(ToolchainOptions{.mode = ToolchainMode::kOriginal});
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  static constexpr std::string_view kBaseTypesPreamble = R"(
typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
typedef struct { u8 b[4]; } UnalignedWord;
typedef struct { u8 b[6]; } UnalignedBlock6;
typedef struct { u8 b[8]; } UnalignedBlock8;
typedef struct { u8 b[12]; } UnalignedBlock12;
typedef struct { u8 b[16]; } UnalignedBlock16;
)";

  const std::vector<RoundTripTestCase>& base_cases = GetBaseRoundTripTestCases();
  EXPECT_GE(base_cases.size(), 592u);

  const std::string_view target_names[] = {
      "Int64AddSubAccum",
      "PlainCharLoadPromoteAndCompare",
      "BitfieldReadModifyWriteFlags",
      "UnionTaggedVariantEval",
      "StructByVal2ByteArg",
      "StructReturnSmall2ByteAndByValArg",
      "HighPressureGpr14LiveAcrossTwoCalls",
      "HighPressureSpillsPlus8OutgoingArgs",
      "VariadicSumCountedInts",
      "VolatileStructHardwareRegs",
      "StringLiteralPassToLogger",
      "StaticBssScalarCounterStep",
      "StaticDataInitializedScalars",
      "StaticRodataConstLookupTable",
      "SmallDataTentativeCommGlobals",
      "SmallDataExternScalarGlobals",
      "SmallDataSdataEightBytePair",
      "IndirectCallVtableStructOps",
      "TwoDenseSwitchesSequential",
      "BackwardGotoCrossedTwoLabelCycle",
  };

  const RoundTripCompilerFlagProfile retail_profiles[] = {
      {"O2_WaG", {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}},
      {"O2_G8_WaO1", {"-O2", "-G", "8", "-mips2", "-mcpu=r4000", "-Wa,-O1"}},
      {"O2_Mips1_UnsignedChar_WaO2", {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}},
  };

  for (std::string_view target_name : target_names) {
    const RoundTripTestCase* found = nullptr;
    for (const auto& tc : base_cases) {
      if (tc.test_name == target_name) {
        found = &tc;
        break;
      }
    }
    ASSERT_NE(found, nullptr) << "Missing base test case: " << target_name;

    for (const auto& prof : retail_profiles) {
      RoundTripTestCase tc = *found;
      tc.test_name = absl::StrCat(found->test_name, "_", prof.suffix);
      tc.options.c_flags = prof.c_flags;
      SCOPED_TRACE(tc.test_name);

      const FunctionCompilationTrace trace = hooks.TraceForwardCompilation(tc);
      VerifyTraceCompletenessAndReplayability(&ctx, &hooks, trace, /*optimize_level=*/2);

      std::string full_source = absl::StrCat(kBaseTypesPreamble, "\n");
      if (!tc.options.extra_types_header.empty()) {
        absl::StrAppend(&full_source, tc.options.extra_types_header, "\n");
      }
      absl::StrAppend(&full_source, tc.original_c);
      VerifyTraceMatchesActualToolchain(*toolchain_or, full_source, tc.function_name,
                                        tc.options.c_flags, trace);
    }
  }
}

TEST(CompilationTraceTest,
     FreshlyMaterializedLoopDiamondCallPointerStructMatchesToolchainAcrossAll35Subfunctions) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  const auto toolchain_or = Toolchain::Discover(ToolchainOptions{.mode = ToolchainMode::kOriginal});
  ASSERT_TRUE(toolchain_or.ok()) << toolchain_or.status();

  constexpr std::string_view kSource = R"(
struct Elem {
  int val;
  int weight;
};
extern int combine_pair(int a, int b);
extern void tick_barrier(void);

int step31_end_to_end_probe(struct Elem* items, int n, int bias, int mode_flag) {
  int acc = 0;
  int i;
  for (i = 0; i < n; i++) {
    int inv = bias * 3 + 7;
    int arg0;
    if (mode_flag > 0) {
      arg0 = items[i].val + inv;
    } else {
      arg0 = items[i].weight - inv;
    }
    acc += combine_pair(arg0, i);
  }
  if (acc == 0) {
    tick_barrier();
  }
  return acc;
}
)";

  const std::vector<std::string> flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  const FunctionCompilationTrace forward_trace = hooks.TraceForwardCompilation(
      kSource, "step31_end_to_end_probe", flags, "step31_end_to_end_probe_O2");
  ASSERT_EQ(forward_trace.subfunction_snapshots.size(), 35u);
  VerifyTraceCompletenessAndReplayability(&ctx, &hooks, forward_trace, /*optimize_level=*/2);
  VerifyTraceMatchesActualToolchain(*toolchain_or, kSource, "step31_end_to_end_probe", flags,
                                    forward_trace);

  // Also round-trip `.00.rtl` through `MaterializeFromCfg` -> `DematerializeToCfg` in a fresh
  // function context and replay `TraceRtlPipeline` from scratch, verifying every pass `.00.rtl` ..
  // `.12.dbr` and assembled machine words match `forward_trace` identically.
  RtlControlFlowGraph rtl00 = forward_trace.pass_snapshots.at(RtlPassKind::kRtl);
  ctx.BeginFunction("step31_end_to_end_probe");
  Rtx mat_head = ctx.MaterializeFromCfg(rtl00);
  ASSERT_NE(mat_head, nullptr);
  RtlControlFlowGraph demat_rtl00 = ctx.DematerializeToCfg(mat_head, RtlPassKind::kRtl);
  demat_rtl00.sideband = rtl00.sideband;

  const FunctionCompilationTrace replayed_trace = hooks.TraceRtlPipeline(
      demat_rtl00, /*optimize_level=*/2, "step31_end_to_end_probe",
      "step31_end_to_end_probe_replayed", flags, forward_trace.gp_symbol_sizes);
  for (RtlPassKind pass : AllRtlPasses()) {
    const RtlCfgPassComparison cmp = replayed_trace.pass_snapshots.at(pass).CompareAgainst(
        forward_trace.pass_snapshots.at(pass));
    EXPECT_TRUE(cmp.MatchesExactly())
        << "Pass " << RtlPassExtension(pass) << " mismatch: " << cmp.mismatch_detail;
  }
  EXPECT_EQ(replayed_trace.final_machine_words, forward_trace.final_machine_words);
}

TEST(CompilationTraceTest,
     BoundedLoopSignificantLatchRegReadBeforeOverwriteMatchesMonolithicFlowAnalysis) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  constexpr std::string_view kSource = R"(
int bounded_copy_probe(char* dst, int capacity, int select_alt) {
  const char* src = select_alt ? "alpha" : "beta_string";
  int i = 0;
  if (capacity <= 0) {
    return 0;
  }
  while (i + 1 < capacity && src[i] != 0) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
  return i;
}
)";

  const std::vector<std::string> flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  const FunctionCompilationTrace forward_trace =
      hooks.TraceForwardCompilation(kSource, "bounded_copy_probe", flags, "bounded_copy_probe_O2");
  ASSERT_EQ(forward_trace.subfunction_snapshots.size(), 35u);
  VerifyTraceCompletenessAndReplayability(&ctx, &hooks, forward_trace, /*optimize_level=*/2);

  const RtlControlFlowGraph& cse2_cfg = forward_trace.pass_snapshots.at(RtlPassKind::kCse2);
  const RtlControlFlowGraph& granular_flow = forward_trace.pass_snapshots.at(RtlPassKind::kFlow);
  ctx.BeginFunction("bounded_copy_probe");
  hooks.ApplyCompilationProfile(forward_trace.stage0_input.profile);
  Rtx cse2_head = ctx.MaterializeFromCfg(cse2_cfg);
  ASSERT_NE(cse2_head, nullptr);
  hooks.RunFlowAnalysis(cse2_head, 0);
  const RtlControlFlowGraph monolithic_flow = ctx.DematerializeToCfg(cse2_head, RtlPassKind::kFlow);

  const RtlCfgPassComparison cmp = granular_flow.CompareAgainst(monolithic_flow);
  EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
  EXPECT_EQ(granular_flow.sideband.basic_block_live_at_start,
            monolithic_flow.sideband.basic_block_live_at_start);
  EXPECT_EQ(granular_flow.sideband.reg_n_refs, monolithic_flow.sideband.reg_n_refs);
  EXPECT_EQ(granular_flow.sideband.reg_n_sets, monolithic_flow.sideband.reg_n_sets);
  EXPECT_EQ(granular_flow.sideband.reg_n_deaths, monolithic_flow.sideband.reg_n_deaths);
  EXPECT_EQ(granular_flow.sideband.reg_live_length, monolithic_flow.sideband.reg_live_length);
  EXPECT_EQ(granular_flow.sideband.reg_n_calls_crossed,
            monolithic_flow.sideband.reg_n_calls_crossed);
}

}  // namespace
}  // namespace rom_nom_nom::gcc_harness
