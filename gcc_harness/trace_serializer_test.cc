#include "gcc_harness/trace_serializer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "core/mips.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::gcc_harness {
namespace {

bool TraceHasLoopNotes(const FunctionCompilationTrace& trace) {
  for (const auto& [pass, cfg] : trace.pass_snapshots) {
    for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
      if (blk.HasLoopNotes()) {
        return true;
      }
    }
  }
  return false;
}

bool TraceHasCallFusage(const FunctionCompilationTrace& trace) {
  for (const auto& [pass, cfg] : trace.pass_snapshots) {
    for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
      for (const RtlInsn& insn : blk.rtl_insns) {
        if (insn.has_explicit_call_fusage && !insn.call_fusage.empty()) {
          return true;
        }
      }
    }
  }
  return false;
}

bool TraceHasPrologueMetadata(const FunctionCompilationTrace& trace) {
  for (const auto& [pass, cfg] : trace.pass_snapshots) {
    if (cfg.sideband.is_variadic || cfg.sideband.returns_aggregate_in_memory ||
        !cfg.sideband.prologue_args.empty()) {
      return true;
    }
  }
  return false;
}

// Strictly bijective alpha-renaming of GCC's process-monotonic local code-label
// numbers (`.L<digits>` and `$L<digits>`, never touching `$LC<digits>` or user
// symbols) in order of first appearance within a single trace.
std::string CanonicalizeLocalCodeLabelsInText(std::string_view text,
                                              absl::flat_hash_map<std::string, int>* label_to_id) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    if ((text[i] == '.' || text[i] == '$') && i + 2 < text.size() && text[i + 1] == 'L' &&
        text[i + 2] >= '0' && text[i + 2] <= '9') {
      size_t j = i + 2;
      while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
        ++j;
      }
      const std::string num_str(text.substr(i + 2, j - (i + 2)));
      auto [it, inserted] = label_to_id->emplace(num_str, static_cast<int>(label_to_id->size()));
      out.push_back(text[i]);
      out.push_back('L');
      out.append(std::to_string(it->second));
      i = j;
      continue;
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

FunctionCompilationTrace CanonicalizeLocalCodeLabelsForLiveComparison(
    const FunctionCompilationTrace& input) {
  FunctionCompilationTrace out = input;
  absl::flat_hash_map<std::string, int> label_to_id;
  out.final_asm_text = CanonicalizeLocalCodeLabelsInText(out.final_asm_text, &label_to_id);
  for (std::string& line : out.final_insn_asm_lines) {
    line = CanonicalizeLocalCodeLabelsInText(line, &label_to_id);
  }
  for (SubfunctionBoundarySnapshot& snap : out.subfunction_snapshots) {
    for (std::string& line : snap.pre_asm_lines) {
      line = CanonicalizeLocalCodeLabelsInText(line, &label_to_id);
    }
    for (std::string& line : snap.post_asm_lines) {
      line = CanonicalizeLocalCodeLabelsInText(line, &label_to_id);
    }
  }
  for (Stage0Relocation& rel : out.stage0_input.relocations) {
    rel.symbol_name = CanonicalizeLocalCodeLabelsInText(rel.symbol_name, &label_to_id);
  }
  for (Stage0JumpTable& jt : out.stage0_input.jump_tables) {
    jt.label_name = CanonicalizeLocalCodeLabelsInText(jt.label_name, &label_to_id);
  }
  return out;
}

TEST(TraceSerializerTest, CachedShardsMatchLiveTraceForwardCompilationFieldForField) {
  const std::vector<RoundTripTestCase>& all_cases = GetSharedRoundTripTestCases();
  const std::vector<RoundTripCompilerFlagProfile>& profiles = GetRoundTripCompilerFlagProfiles();
  ASSERT_FALSE(all_cases.empty());

  // 1. Verify every one of the 32 stride-sharded cache files loads and passes
  //    fingerprint verification.
  for (uint32_t shard = 0; shard < kDefaultSharedCorpusTraceShards; ++shard) {
    if (shard >= all_cases.size()) {
      break;
    }
    const absl::StatusOr<const FunctionCompilationTrace*> loaded =
        GetCachedSharedCorpusTraceByIndex(shard);
    ASSERT_TRUE(loaded.ok()) << "Shard " << shard << ": " << loaded.status();
    ASSERT_NE(*loaded, nullptr);
    EXPECT_EQ((*loaded)->test_name, all_cases[shard].test_name);
  }

  // 2. Select a feature-rich subset covering all 7 compiler flag profiles and
  //    every major RTL/Stage0 feature (jump tables, constant pools, $gp symbols,
  //    variadic/aggregate prologues, explicit call_fusage, loop notes, relocs).
  std::vector<size_t> selected_indices;
  absl::flat_hash_set<size_t> seen_indices;
  auto select_idx = [&](size_t idx) {
    if (seen_indices.insert(idx).second) {
      selected_indices.push_back(idx);
    }
  };

  absl::flat_hash_set<size_t> covered_profiles;
  bool saw_jump_table = false;
  bool saw_const_pool = false;
  bool saw_gp_sym = false;
  bool saw_prologue_meta = false;
  bool saw_call_fusage = false;
  bool saw_loop_notes = false;
  bool saw_relocs = false;

  // Scan stride-sampled indices across all shards until every profile & feature
  // is represented.
  for (size_t idx = 0; idx < all_cases.size(); ++idx) {
    const absl::StatusOr<const FunctionCompilationTrace*> cached =
        GetCachedSharedCorpusTraceByIndex(idx);
    ASSERT_TRUE(cached.ok()) << idx << ": " << cached.status();
    const FunctionCompilationTrace& t = **cached;

    bool wanted = false;
    for (size_t p = 0; p < profiles.size(); ++p) {
      if (!covered_profiles.contains(p) && all_cases[idx].options.c_flags == profiles[p].c_flags) {
        covered_profiles.insert(p);
        wanted = true;
      }
    }
    if (!saw_jump_table && !t.stage0_input.jump_tables.empty()) {
      saw_jump_table = true;
      wanted = true;
    }
    if (!saw_const_pool && !t.stage0_input.constant_pool.empty()) {
      saw_const_pool = true;
      wanted = true;
    }
    if (!saw_gp_sym && !t.gp_symbol_sizes.empty()) {
      saw_gp_sym = true;
      wanted = true;
    }
    if (!saw_prologue_meta && TraceHasPrologueMetadata(t)) {
      saw_prologue_meta = true;
      wanted = true;
    }
    if (!saw_call_fusage && TraceHasCallFusage(t)) {
      saw_call_fusage = true;
      wanted = true;
    }
    if (!saw_loop_notes && TraceHasLoopNotes(t)) {
      saw_loop_notes = true;
      wanted = true;
    }
    if (!saw_relocs && !t.stage0_input.relocations.empty()) {
      saw_relocs = true;
      wanted = true;
    }
    if (wanted) {
      select_idx(idx);
    }
    if (covered_profiles.size() == profiles.size() && saw_jump_table && saw_const_pool &&
        saw_gp_sym && saw_prologue_meta && saw_call_fusage && saw_loop_notes && saw_relocs) {
      break;
    }
  }

  EXPECT_EQ(covered_profiles.size(), profiles.size());
  EXPECT_TRUE(saw_jump_table);
  EXPECT_TRUE(saw_const_pool);
  EXPECT_TRUE(saw_gp_sym);
  EXPECT_TRUE(saw_prologue_meta);
  EXPECT_TRUE(saw_call_fusage);
  EXPECT_TRUE(saw_loop_notes);
  EXPECT_TRUE(saw_relocs);

  // 3. Compare every selected cached trace field-for-field against a fresh live
  //    `TraceForwardCompilation(tc)` invocation.
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  for (size_t idx : selected_indices) {
    const RoundTripTestCase& tc = all_cases[idx];
    const absl::StatusOr<const FunctionCompilationTrace*> by_name =
        GetCachedSharedCorpusTrace(tc.test_name);
    ASSERT_TRUE(by_name.ok()) << tc.test_name << ": " << by_name.status();
    const absl::StatusOr<const FunctionCompilationTrace*> by_index =
        GetCachedSharedCorpusTraceByIndex(idx);
    ASSERT_TRUE(by_index.ok()) << idx << ": " << by_index.status();
    EXPECT_EQ(*by_name, *by_index);

    ctx.BeginFunction("harness_init");
    hooks.ApplyCompilationProfile({2, tc.options.c_flags, {}});
    regclass_init();
    const FunctionCompilationTrace live = hooks.TraceForwardCompilation(tc);

    // 3a. Verbatim zero-renaming round-trip on live trace.
    const absl::StatusOr<FunctionCompilationTrace> round_tripped =
        DeserializeFunctionCompilationTrace(SerializeFunctionCompilationTrace(live));
    ASSERT_TRUE(round_tripped.ok()) << tc.test_name << ": " << round_tripped.status();
    std::string rt_diff;
    EXPECT_TRUE(FunctionCompilationTraceEqualsExact(*round_tripped, live, &rt_diff))
        << "Verbatim round-trip mismatch on " << tc.test_name << ": " << rt_diff;

    // 3b. Cached shard vs live: verbatim on all pass_snapshots, subfunction
    //     RTL/machine_words, final_machine_words, constant_pool, profile, and
    //     exact bijective .L/$L alpha-equivalence on assembly text/jump-table names.
    std::string diff;
    EXPECT_TRUE(FunctionCompilationTraceEqualsExact(
        CanonicalizeLocalCodeLabelsForLiveComparison(**by_name),
        CanonicalizeLocalCodeLabelsForLiveComparison(live), &diff))
        << "Mismatch on corpus case " << tc.test_name << " (idx=" << idx << "): " << diff;
  }
}

TEST(TraceSerializerTest, SyntheticAllFieldsRoundTripAndByteDeterminism) {
  auto build_synthetic_trace = [](bool reverse_map_insert_order) {
    FunctionCompilationTrace trace;
    trace.test_name = "synthetic_full_fields";
    trace.function_name = "synth_fn";
    trace.optimize_level = 2;
    trace.c_flags = {"-O2", "-G", "8", "-mips2", "-mcpu=r4000", "-Wa,-O1"};
    if (reverse_map_insert_order) {
      trace.gp_symbol_sizes["z_sym"] = 8u;
      trace.gp_symbol_sizes["a_sym"] = 4u;
    } else {
      trace.gp_symbol_sizes["a_sym"] = 4u;
      trace.gp_symbol_sizes["z_sym"] = 8u;
    }

    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.has_explicit_trailing_void_return = true;
    cfg.local_alloc_pseudo_reg_count = 11u;
    cfg.global_alloc_pseudo_reg_count = 7u;

    RtlBasicBlock bb0;
    bb0.id = 0;
    bb0.start_vram = 0x80020000u;
    bb0.end_vram = 0x80020010u;
    bb0.has_label = true;
    bb0.has_trailing_barrier = true;
    bb0.has_unreachable_entry_label = true;
    bb0.successors = {1, 2};
    bb0.predecessors = {0};
    bb0.addr_vec_targets = {1, 2};

    RtlInsn insn;
    insn.canonical_pattern = "(set (reg:SI 2 v0) (const_int 42))";
    insn.rtl_sexpr_pattern = "(insn 1 0 2 (set (reg:SI 2 v0) (const_int 42)))";
    insn.idiom_kind = RtlFusedIdiomKind::kGasAddressHiLo;
    insn.vram = 0x80020004u;
    insn.is_prologue_insn = true;
    insn.is_synthetic_rtl_expansion = true;
    insn.loop_note_kind = RtlLoopNoteKind::kLoopVtop;
    insn.has_explicit_call_fusage = true;

    RtlExpr expr;
    expr.op = RtlOp::kAsmOperands;
    expr.mode = RtlMachineMode::kDf;
    expr.flags = {.in_struct = true,
                  .volatil = true,
                  .unchanging = true,
                  .integrated = true,
                  .frame_related = true};
    expr.int_val = -1234567890123LL;
    expr.int_val_high = 9876543210987LL;
    expr.str_val = "sym_payload";
    expr.symbol_paren_wrapped = false;
    expr.bracketed_children = true;
    expr.raw_op = "custom_op";
    expr.raw_mode = "CC_FP";
    expr.extra_ints = {-1, 0, 42, 100000};
    expr.children.push_back(RtlExpr::MakeReg(RtlMachineMode::kSi, 72));
    expr.children.push_back(RtlExpr::MakeConstInt(7));
    expr.generic_items = {
        {RtlExpr::GenericItem::Kind::kChildExpr, 0, 1, 0, ""},
        {RtlExpr::GenericItem::Kind::kBracketedChildSlice, 0, 2, 0, ""},
        {RtlExpr::GenericItem::Kind::kInt, 0, 0, -99, ""},
        {RtlExpr::GenericItem::Kind::kQuotedString, 0, 0, 0, "quoted"},
        {RtlExpr::GenericItem::Kind::kParenQuotedString, 0, 0, 0, "pquoted"},
        {RtlExpr::GenericItem::Kind::kBareAtom, 0, 0, 0, "NOTE_INSN_PROLOGUE_END"},
        {RtlExpr::GenericItem::Kind::kBracketedRaw, 0, 0, 0, "[raw]"},
    };
    insn.call_fusage.push_back(std::move(expr));

    Instruction mi;
    mi.raw_word = 0x2482002au;
    mi.vram = 0x80020004u;
    mi.opcode = Opcode::kAddiu;
    mi.rd = Register::kV0;
    mi.rs = Register::kA0;
    mi.rt = Register::kV0;
    mi.unscheduled_clobber_reg = Register::kAt;
    mi.was_unscheduled_delay_slot = true;
    mi.fd = FpRegister::kF0;
    mi.fs = FpRegister::kF12;
    mi.ft = FpRegister::kF14;
    mi.immediate = -42;
    mi.shift_amount = 5;
    mi.target = 0x00123456u;
    insn.machine_instructions.push_back(mi);

    bb0.rtl_insns.push_back(insn);
    bb0.instructions.push_back(insn.canonical_pattern);
    cfg.rpo_blocks.push_back(std::move(bb0));

    cfg.sideband.max_regno = 85;
    if (reverse_map_insert_order) {
      cfg.sideband.reg_renumber[73] = 3;
      cfg.sideband.reg_renumber[72] = 2;
      cfg.sideband.basic_block_live_at_start[1] = {5, 4};
      cfg.sideband.basic_block_live_at_start[0] = {29, 4};
    } else {
      cfg.sideband.reg_renumber[72] = 2;
      cfg.sideband.reg_renumber[73] = 3;
      cfg.sideband.basic_block_live_at_start[0] = {4, 29};
      cfg.sideband.basic_block_live_at_start[1] = {4, 5};
    }
    cfg.sideband.reg_n_refs[72] = 4;
    cfg.sideband.reg_n_sets[72] = 1;
    cfg.sideband.reg_n_deaths[72] = 1;
    cfg.sideband.reg_live_length[72] = 6;
    cfg.sideband.reg_n_calls_crossed[72] = 2;
    cfg.sideband.frame_size_bytes = 32;
    cfg.sideband.outgoing_args_size = 24;
    cfg.sideband.is_leaf_function = false;
    cfg.sideband.frame_pointer_needed = true;
    cfg.sideband.saved_gpr_regnos = {16, 31};
    cfg.sideband.saved_fpr_regnos = {52, 54};
    cfg.sideband.epilogue_delay_pattern = "(set (reg:SI 2 v0) (reg:SI 16 s0))";
    cfg.sideband.is_variadic = true;
    cfg.sideband.returns_aggregate_in_memory = true;
    cfg.sideband.prologue_args = {{MipsPrologueArgKind::kStructOrUnion, 2, 16, 2}};
    cfg.sideband.spill_slots = {{-16, 8, 74, RtlMachineMode::kDf}};
    cfg.sideband.loops = {{10, 2, 0, {1, 2}, {72, 75}}};
    cfg.sideband.constant_pool = {{"$LC0", ".rdata", 8u, {0x400921fbu, 0x54442d18u}, 0x80030000u}};
    cfg.sideband.pointer_regnos = {72, 76};
    cfg.sideband.cse_jumps_altered = true;

    trace.pass_snapshots[RtlPassKind::kGreg] = cfg;
    trace.pass_snapshots[RtlPassKind::kDbr] = cfg;

    SubfunctionBoundarySnapshot snap;
    snap.subfunction_id = SubfunctionId::kAlterReg;
    snap.optimize_level = 2;
    snap.c_flags = trace.c_flags;
    snap.gp_symbol_sizes = trace.gp_symbol_sizes;
    snap.pre_state = cfg;
    snap.post_state = cfg;
    snap.pre_asm_lines = {"addu $2,$4,$5"};
    snap.post_asm_lines = {"j $31", "addu $2,$4,$5"};
    snap.post_machine_words = {0x03e00008u, 0x00851021u};
    trace.subfunction_snapshots.push_back(std::move(snap));

    trace.final_asm_text = "\tj\t$31\n\tnop\n";
    trace.final_insn_asm_lines = {"j $31", "nop"};
    trace.final_machine_words = {0x03e00008u, 0x00000000u, 0x00000000u, 0x00000000u};

    trace.stage0_input.machine_words = {0x03e00008u, 0x00000000u};
    trace.stage0_input.base_vram = 0x80020000u;
    trace.stage0_input.gp_vram = 0x80060000u;
    trace.stage0_input.relocations = {{0u, Stage0RelocKind::kMipsHi16, "g_global", 16}};
    trace.stage0_input.jump_tables = {{"$L5", {1u, 3u, 5u}, 0x80030020u}};
    trace.stage0_input.constant_pool = cfg.sideband.constant_pool;
    if (reverse_map_insert_order) {
      trace.stage0_input.vram_to_symbol[0x80040040u] = "sym_b";
      trace.stage0_input.vram_to_symbol[0x80040000u] = "sym_a";
      trace.stage0_input.vram_to_symbol_size[0x80040040u] = 8u;
      trace.stage0_input.vram_to_symbol_size[0x80040000u] = 4u;
    } else {
      trace.stage0_input.vram_to_symbol[0x80040000u] = "sym_a";
      trace.stage0_input.vram_to_symbol[0x80040040u] = "sym_b";
      trace.stage0_input.vram_to_symbol_size[0x80040000u] = 4u;
      trace.stage0_input.vram_to_symbol_size[0x80040040u] = 8u;
    }
    trace.stage0_input.profile.optimize_level = 2;
    trace.stage0_input.profile.c_flags = trace.c_flags;
    trace.stage0_input.profile.gp_symbol_sizes = trace.gp_symbol_sizes;
    return trace;
  };

  const FunctionCompilationTrace trace_a =
      build_synthetic_trace(/*reverse_map_insert_order=*/false);
  const FunctionCompilationTrace trace_b = build_synthetic_trace(/*reverse_map_insert_order=*/true);

  const std::string bytes_a = SerializeFunctionCompilationTrace(trace_a);
  const std::string bytes_b = SerializeFunctionCompilationTrace(trace_b);
  EXPECT_EQ(bytes_a, bytes_b) << "Serialization must be byte-for-byte deterministic";

  const absl::StatusOr<FunctionCompilationTrace> decoded =
      DeserializeFunctionCompilationTrace(bytes_a);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  std::string diff;
  EXPECT_TRUE(FunctionCompilationTraceEqualsExact(*decoded, trace_a, &diff)) << diff;

  SerializedTraceShardHeader header;
  header.shard_index = 3;
  header.num_shards = 32;
  header.total_corpus_cases = 100;
  header.corpus_fingerprint = 0x123456789abcdef0ull;
  std::vector<IndexedFunctionCompilationTrace> records = {{3u, trace_a}};
  const std::string shard_bytes = SerializeTraceShard(header, records);
  const absl::StatusOr<DeserializedTraceShard> decoded_shard =
      DeserializeTraceShard(shard_bytes, header.corpus_fingerprint);
  ASSERT_TRUE(decoded_shard.ok()) << decoded_shard.status();
  EXPECT_EQ(decoded_shard->header.shard_index, 3u);
  EXPECT_EQ(decoded_shard->header.num_shards, 32u);
  EXPECT_EQ(decoded_shard->header.total_corpus_cases, 100u);
  EXPECT_EQ(decoded_shard->header.corpus_fingerprint, header.corpus_fingerprint);
  ASSERT_EQ(decoded_shard->records.size(), 1u);
  EXPECT_EQ(decoded_shard->records[0].case_index, 3u);
  EXPECT_TRUE(FunctionCompilationTraceEqualsExact(decoded_shard->records[0].trace, trace_a, &diff))
      << diff;
}

TEST(TraceSerializerTest, RejectsCorruptedTruncatedAndMismatchedBuffers) {
  FunctionCompilationTrace trace;
  trace.test_name = "neg_case";
  trace.function_name = "neg_fn";
  trace.pass_snapshots[RtlPassKind::kDbr] = RtlControlFlowGraph{};
  const std::string valid_trace = SerializeFunctionCompilationTrace(trace);

  // 1. Truncated single-trace buffers.
  EXPECT_FALSE(DeserializeFunctionCompilationTrace("").ok());
  EXPECT_FALSE(
      DeserializeFunctionCompilationTrace(valid_trace.substr(0, valid_trace.size() / 2)).ok());
  EXPECT_FALSE(
      DeserializeFunctionCompilationTrace(valid_trace.substr(0, valid_trace.size() - 1)).ok());

  // 2. Trailing garbage byte.
  EXPECT_FALSE(DeserializeFunctionCompilationTrace(valid_trace + '\0').ok());

  // 3. Out-of-range string pool index (corrupt first StrId right after string_pool + cfg_pool).
  {
    std::string bad_str_id = valid_trace;
    // Empty trace with 2 strings ("neg_case", "neg_fn") and 1 CFG; test corrupting
    // the string pool count vs index by crafting a minimal trace buffer with 0 strings
    // and body reading StrId 0.
    std::string crafted;
    // string_pool count = 0 (4 bytes), cfg_pool count = 0 (4 bytes), test_name StrId = 0 (4 bytes)
    crafted.assign(12, '\0');
    EXPECT_FALSE(DeserializeFunctionCompilationTrace(crafted).ok());
  }

  // 4. Out-of-range CFG pool index: string_pool = {""}, cfg_pool = {}, pass_snapshots = {(kDbr, 9)}
  {
    std::string crafted;
    auto push_u32 = [&](uint32_t v) {
      crafted.push_back(static_cast<char>(v & 0xffu));
      crafted.push_back(static_cast<char>((v >> 8) & 0xffu));
      crafted.push_back(static_cast<char>((v >> 16) & 0xffu));
      crafted.push_back(static_cast<char>((v >> 24) & 0xffu));
    };
    push_u32(1u);  // string_pool count = 1
    push_u32(0u);  // string[0] len = 0
    push_u32(0u);  // cfg_pool count = 0
    push_u32(0u);  // test_name StrId = 0
    push_u32(0u);  // function_name StrId = 0
    push_u32(2u);  // optimize_level
    push_u32(0u);  // c_flags count
    push_u32(0u);  // gp_symbol_sizes count
    push_u32(1u);  // pass_snapshots count = 1
    push_u32(static_cast<uint32_t>(RtlPassKind::kDbr));
    push_u32(9u);  // out-of-range CfgId
    EXPECT_FALSE(DeserializeFunctionCompilationTrace(crafted).ok());
  }

  // 5. Shard header validation: bad magic, bad version, bad shard bounds, bad fingerprint.
  SerializedTraceShardHeader header;
  header.shard_index = 0;
  header.num_shards = 32;
  header.total_corpus_cases = 1;
  header.corpus_fingerprint = 0xabcdef0123456789ull;
  std::vector<IndexedFunctionCompilationTrace> records = {{0u, trace}};
  const std::string valid_shard = SerializeTraceShard(header, records);

  EXPECT_FALSE(DeserializeTraceShard(valid_shard.substr(0, 12)).ok());
  EXPECT_FALSE(DeserializeTraceShard(valid_shard + 'X').ok());
  EXPECT_FALSE(DeserializeTraceShard(valid_shard, header.corpus_fingerprint ^ 1ull).ok());

  {
    std::string bad_magic = valid_shard;
    bad_magic[0] ^= 0x55;
    EXPECT_FALSE(DeserializeTraceShard(bad_magic).ok());
  }
  {
    std::string bad_version = valid_shard;
    bad_version[4] = 99;
    EXPECT_FALSE(DeserializeTraceShard(bad_version).ok());
  }
  {
    SerializedTraceShardHeader bad_bounds = header;
    bad_bounds.shard_index = 32;
    bad_bounds.num_shards = 32;
    EXPECT_FALSE(DeserializeTraceShard(SerializeTraceShard(bad_bounds, records)).ok());
  }

  // 6. Loader error handling on unknown name / out-of-range index.
  EXPECT_FALSE(GetCachedSharedCorpusTrace("__nonexistent_test_case_name__").ok());
  EXPECT_FALSE(GetCachedSharedCorpusTraceByIndex(GetSharedRoundTripTestCases().size()).ok());
}

TEST(TraceSerializerTest, V2ShardIndexLazySingleRecordDecodeAndNegativeBoundsChecks) {
  auto write_u32_le = [](std::string* buf, size_t pos, uint32_t value) {
    (*buf)[pos + 0] = static_cast<char>(value & 0xffu);
    (*buf)[pos + 1] = static_cast<char>((value >> 8) & 0xffu);
    (*buf)[pos + 2] = static_cast<char>((value >> 16) & 0xffu);
    (*buf)[pos + 3] = static_cast<char>((value >> 24) & 0xffu);
  };

  auto make_trace = [](const std::string& name, uint32_t word) {
    FunctionCompilationTrace t;
    t.test_name = name;
    t.function_name = name + "_fn";
    t.optimize_level = 2;
    t.c_flags = {"-O2", "-mips2", "-Wa,-g"};
    t.final_machine_words = {word, 0x03e00008u, 0x00000000u};
    t.stage0_input.machine_words = t.final_machine_words;
    t.stage0_input.profile.optimize_level = 2;
    t.stage0_input.profile.c_flags = t.c_flags;
    t.pass_snapshots[RtlPassKind::kDbr] = RtlControlFlowGraph{};
    return t;
  };

  const FunctionCompilationTrace t0 = make_trace("case_a", 0x24020001u);
  const FunctionCompilationTrace t1 = make_trace("case_b", 0x24020002u);
  const FunctionCompilationTrace t2 = make_trace("case_c", 0x24020003u);

  SerializedTraceShardHeader header;
  header.shard_index = 2;
  header.num_shards = 32;
  header.total_corpus_cases = 100;
  header.corpus_fingerprint = 0xfeedfacecafebeefull;
  std::vector<IndexedFunctionCompilationTrace> records = {{2u, t0}, {34u, t1}, {66u, t2}};
  const std::string shard_bytes = SerializeTraceShard(header, records);

  // Positive 1: `ParseTraceShardIndex` validates v2 header + tiling and allows
  // lazy single-record decoding matching full `DeserializeTraceShard`.
  const absl::StatusOr<ParsedTraceShardIndex> parsed_index =
      ParseTraceShardIndex(shard_bytes, header.corpus_fingerprint);
  ASSERT_TRUE(parsed_index.ok()) << parsed_index.status();
  EXPECT_EQ(parsed_index->header.format_version, 2u);
  ASSERT_EQ(parsed_index->entries.size(), 3u);
  EXPECT_EQ(parsed_index->entries[0].case_index, 2u);
  EXPECT_EQ(parsed_index->entries[1].case_index, 34u);
  EXPECT_EQ(parsed_index->entries[2].case_index, 66u);

  const absl::StatusOr<DeserializedTraceShard> full_decode =
      DeserializeTraceShard(shard_bytes, header.corpus_fingerprint);
  ASSERT_TRUE(full_decode.ok()) << full_decode.status();
  ASSERT_EQ(full_decode->records.size(), 3u);

  const TraceShardIndexEntry& mid = parsed_index->entries[1];
  const std::string_view mid_slice =
      std::string_view(shard_bytes)
          .substr(parsed_index->payload_base_offset + mid.byte_offset, mid.byte_length);
  const absl::StatusOr<FunctionCompilationTrace> lazy_mid =
      DeserializeFunctionCompilationTrace(mid_slice);
  ASSERT_TRUE(lazy_mid.ok()) << lazy_mid.status();
  std::string diff;
  EXPECT_TRUE(FunctionCompilationTraceEqualsExact(*lazy_mid, t1, &diff)) << diff;
  EXPECT_TRUE(FunctionCompilationTraceEqualsExact(*lazy_mid, full_decode->records[1].trace, &diff))
      << diff;

  // Positive 2: Pointer stability in `SharedCorpusTraceStore` across multiple
  // lazy decodes within the same shard (`case 0` and `case 32` both live in shard 0).
  const auto first_ptr = GetCachedSharedCorpusTraceByIndex(0);
  ASSERT_TRUE(first_ptr.ok()) << first_ptr.status();
  if (GetSharedRoundTripTestCases().size() > kDefaultSharedCorpusTraceShards) {
    const auto second_ptr = GetCachedSharedCorpusTraceByIndex(kDefaultSharedCorpusTraceShards);
    ASSERT_TRUE(second_ptr.ok()) << second_ptr.status();
    EXPECT_NE(*first_ptr, *second_ptr);
    const auto first_ptr_again = GetCachedSharedCorpusTraceByIndex(0);
    ASSERT_TRUE(first_ptr_again.ok()) << first_ptr_again.status();
    EXPECT_EQ(*first_ptr, *first_ptr_again);
    EXPECT_EQ((*first_ptr)->test_name, GetSharedRoundTripTestCases()[0].test_name);
  }

  // Header layout: magic(0), format_version(4), shard_index(8), num_shards(12),
  // total_corpus_cases(16), corpus_fingerprint(20..27), record_count(28),
  // entry[i] at 32 + 12*i: case_index(+0), byte_offset(+4), byte_length(+8).
  constexpr size_t kEntry0Pos = 32u;
  constexpr size_t kEntry1Pos = 32u + 12u;

  // Negative 1: Version mismatch (`1u` legacy v1 and `99u` unknown).
  {
    std::string v1_buf = shard_bytes;
    write_u32_le(&v1_buf, 4u, 1u);
    EXPECT_FALSE(ParseTraceShardIndex(v1_buf).ok());
    EXPECT_FALSE(DeserializeTraceShard(v1_buf).ok());

    std::string v99_buf = shard_bytes;
    write_u32_le(&v99_buf, 4u, 99u);
    EXPECT_FALSE(ParseTraceShardIndex(v99_buf).ok());
    EXPECT_FALSE(DeserializeTraceShard(v99_buf).ok());
  }

  // Negative 2: Non-ascending / duplicate `case_index`.
  {
    std::string dup_case = shard_bytes;
    write_u32_le(&dup_case, kEntry1Pos + 0u, 2u);
    EXPECT_FALSE(ParseTraceShardIndex(dup_case).ok());
  }

  // Negative 3: Out-of-bounds `byte_offset` (`offset > payload_size`).
  {
    std::string oob_offset = shard_bytes;
    write_u32_le(&oob_offset, kEntry0Pos + 4u, static_cast<uint32_t>(shard_bytes.size() + 16u));
    EXPECT_FALSE(ParseTraceShardIndex(oob_offset).ok());
  }

  // Negative 4: Out-of-bounds `byte_length` (`length > payload_size - offset`).
  {
    std::string oob_len = shard_bytes;
    write_u32_le(&oob_len, kEntry1Pos + 8u, static_cast<uint32_t>(shard_bytes.size()));
    EXPECT_FALSE(ParseTraceShardIndex(oob_len).ok());
  }

  // Negative 5: `uint32_t` `offset + length` wraparound overflow
  // (`0xfffffff0u + 0x20u` wraps to `0x10u` in 32-bit arithmetic).
  {
    std::string overflow_buf = shard_bytes;
    write_u32_le(&overflow_buf, kEntry0Pos + 4u, 0xfffffff0u);
    write_u32_le(&overflow_buf, kEntry0Pos + 8u, 0x00000020u);
    EXPECT_FALSE(ParseTraceShardIndex(overflow_buf).ok());
  }

  // Negative 6: Tiling gap / uncovered trailing byte.
  {
    std::string gap_buf = shard_bytes;
    ASSERT_GT(parsed_index->entries[2].byte_length, 1u);
    write_u32_le(&gap_buf, kEntry0Pos + 24u + 8u, parsed_index->entries[2].byte_length - 1u);
    EXPECT_FALSE(ParseTraceShardIndex(gap_buf).ok());
  }
}

}  // namespace
}  // namespace rom_nom_nom::gcc_harness
