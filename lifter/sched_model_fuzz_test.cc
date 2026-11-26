#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_schedule_block.h"
#include "lifter/pass06_sched1_pre_alloc.h"
#include "lifter/rtl_liveness.h"
#include "lifter/sched_model.h"

namespace rom_nom_nom::lifter::sched_model {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

constexpr int kNumFuzzSeeds = 1000;
constexpr int kNumMicroKernels = 25;
constexpr int kNumStratifiedCorpusSeeds = 200;
constexpr uint64_t kSeedSalt = 0x5CAED000F4ULL;

// Subclasses `SubfunctionInverseProvider` directly (never `ToySubfunctionInverses`)
// to supply production Stage 6 (`InvertPass06Sched1PreAlloc`) and Stage 3
// (`InvertScheduleBlock`) right-inverses to `RightInverseVerifier`.
class FuzzSchedInverseProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertScheduleBlock(
      const RtlControlFlowGraph& scheduled_cfg_x) const override {
    if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
      return lifter::InvertPass06Sched1PreAlloc(scheduled_cfg_x);
    }
    return lifter::InvertScheduleBlock(scheduled_cfg_x);
  }

  RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                          const CompilationProfile& profile) const override {
    if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
      return lifter::InvertPass06Sched1PreAlloc(scheduled_cfg_x, profile);
    }
    return lifter::InvertScheduleBlock(scheduled_cfg_x, profile);
  }
};

enum class MutationFamily : int {
  kM1TopoPerm = 0,
  kM2AlphaRename = 1,
  kM3ConstLargeInt = 2,
  kM4UnitAndCpu = 3,
  kM5MemAliasFlush = 4,
  kM6CallBoundary = 5,
};

constexpr int kNumMutationFamilies = 6;

std::string_view MutationFamilyName(MutationFamily fam) {
  switch (fam) {
    case MutationFamily::kM1TopoPerm:
      return "M1_TopoPerm";
    case MutationFamily::kM2AlphaRename:
      return "M2_AlphaRename";
    case MutationFamily::kM3ConstLargeInt:
      return "M3_ConstLargeInt";
    case MutationFamily::kM4UnitAndCpu:
      return "M4_UnitAndCpu";
    case MutationFamily::kM5MemAliasFlush:
      return "M5_MemAliasFlush";
    case MutationFamily::kM6CallBoundary:
      return "M6_CallBoundary";
  }
  return "Unknown";
}

ParsedCompilerFlags ParseProfileFlags(const CompilationProfile& profile) {
  std::vector<std::string> eff_flags;
  eff_flags.reserve(profile.c_flags.size() + 1);
  eff_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  eff_flags.insert(eff_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  return ParsedCompilerFlags::Parse(eff_flags);
}

// =============================================================================
// 1. Seed Pool: 25 Synthetic Micro-Kernels + 200 Stratified Corpus Snapshots
// =============================================================================

const std::vector<RoundTripTestCase>& GetSyntheticMicroKernels() {
  static const auto* const k_kernels = new std::vector<RoundTripTestCase>{
      // 01: Memory load/store unit (`lw`/`sw`/`lb`/`sb`/`lhu`/`sh`).
      {
          .test_name = "Mk01MemoryLoadStore",
          .function_name = "Mk01MemoryLoadStore",
          .original_c = R"(
s32 Mk01MemoryLoadStore(s32* a, s16* b, u8* c) {
    s32 x = a[0] + a[1];
    s32 y = b[0] + b[1];
    s32 z = c[0] + c[1];
    a[2] = x ^ y;
    b[2] = (s16)(y - z);
    c[2] = (u8)(x + z);
    return x + y + z;
}
)",
      },
      // 02: `imul` & `hilo` (`mult`, high-part multiply, `mflo`/`mfhi`).
      {
          .test_name = "Mk02ImulHilo",
          .function_name = "Mk02ImulHilo",
          .original_c = R"(
s32 Mk02ImulHilo(s32 a, s32 b, s32 c, s32 d) {
    s32 p0 = a * b;
    s32 p1 = c * d;
    s32 q0 = (a / 7) + (b / 11);
    return p0 + p1 + q0;
}
)",
      },
      // 03: `idiv` unit (`div`/`divu` + `%`, long `imuldiv` blockage).
      {
          .test_name = "Mk03IdivMod",
          .function_name = "Mk03IdivMod",
          .original_c = R"(
s32 Mk03IdivMod(s32 a, s32 b, u32 c, u32 d) {
    s32 q = a / b;
    s32 r = a % b;
    u32 uq = c / d;
    u32 ur = c % d;
    return q + r + (s32)(uq ^ ur);
}
)",
      },
      // 04: `fadd` + `fcmp` + `$fcr31` (`add.s`, `sub.s`, `c.lt.s`).
      {
          .test_name = "Mk04FaddFcmpFcr31",
          .function_name = "Mk04FaddFcmpFcr31",
          .original_c = R"(
f32 Mk04FaddFcmpFcr31(f32 a, f32 b, f32 c, f32 d) {
    f32 s0 = a + b;
    f32 s1 = c - d;
    f32 s2 = s0 + d;
    if (s0 < s1) {
        return s2 + s1;
    }
    return s2 - s1;
}
)",
      },
      // 05: `fmul` unit (`SF` and `DF` multiply chains).
      {
          .test_name = "Mk05FmulSfDf",
          .function_name = "Mk05FmulSfDf",
          .original_c = R"(
f64 Mk05FmulSfDf(f32 a, f32 b, f64 x, f64 y) {
    f32 p = (a * b) * (a + b);
    f64 q = (x * y) + (x * x) - (y * y);
    return q + (f64)p;
}
)",
      },
      // 06: `fdiv` & `fsqrt` (`divide` unit, `SF`/`DF` divisions).
      {
          .test_name = "Mk06FdivFsqrt",
          .function_name = "Mk06FdivFsqrt",
          .original_c = R"(
extern f64 sqrt(f64);
f64 Mk06FdivFsqrt(f32 a, f32 b, f64 x, f64 y) {
    f32 qf = a / b;
    f64 qd = x / y;
    f64 sq = sqrt(x + y);
    return qd + sq + (f64)qf;
}
)",
      },
      // 07: `xfer` & `fcvt` (`int <-> float <-> double` conversions).
      {
          .test_name = "Mk07XferFcvt",
          .function_name = "Mk07XferFcvt",
          .original_c = R"(
s32 Mk07XferFcvt(s32 a, s32 b, f32 x, f64 y) {
    f32 fa = (f32)a;
    f64 fb = (f64)b;
    f32 sum = fa + x;
    f64 dsum = fb + y + (f64)sum;
    return (s32)dsum + (s32)sum;
}
)",
      },
      // 08: `sched.c:1704` (`call_used_regs[0]` bug: callee-saved GPRs + FPRs across call).
      {
          .test_name = "Mk08Sched1704CalleeSavedCall",
          .function_name = "Mk08Sched1704CalleeSavedCall",
          .original_c = R"(
extern s32 ExtCallee08(s32 a, s32 b);
f32 Mk08Sched1704CalleeSavedCall(s32 a, s32 b, f32 x, f32 y) {
    s32 t0 = a + 3;
    s32 t1 = b ^ 5;
    f32 f0 = x + y;
    f32 f1 = x * y;
    s32 r = ExtCallee08(t0, t1);
    s32 u = t0 + t1 + r;
    f32 g = f0 + f1 + (f32)u;
    return g;
}
)",
      },
      // 09: `pending_lists_length > 32` (`sched.c:1740` flush: 36 disjoint loads before store).
      {
          .test_name = "Mk09PendingListsOver32",
          .function_name = "Mk09PendingListsOver32",
          .original_c = R"(
s32 Mk09PendingListsOver32(const s32* p, s32* out) {
    s32 s = p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7]
          + p[8] + p[9] + p[10] + p[11] + p[12] + p[13] + p[14] + p[15]
          + p[16] + p[17] + p[18] + p[19] + p[20] + p[21] + p[22] + p[23]
          + p[24] + p[25] + p[26] + p[27] + p[28] + p[29] + p[30] + p[31]
          + p[32] + p[33] + p[34] + p[35];
    *out = s;
    return s;
}
)",
      },
      // 10: `birthing_insn_p` (single-set dying pseudos vs multi-set accumulator).
      {
          .test_name = "Mk10BirthingInsnP",
          .function_name = "Mk10BirthingInsnP",
          .original_c = R"(
s32 Mk10BirthingInsnP(s32* a, s32 n) {
    s32 acc = n;
    s32 x0 = a[0] + 1;
    s32 x1 = a[1] + 2;
    s32 x2 = a[2] + 3;
    acc += x0;
    acc ^= x1;
    acc += x2;
    acc += a[3];
    return acc;
}
)",
      },
      // 11: `swap_sort` staleness (multiple independent tails + 1 dependency chain).
      {
          .test_name = "Mk11SwapSortStaleness",
          .function_name = "Mk11SwapSortStaleness",
          .original_c = R"(
void Mk11SwapSortStaleness(s32 a, s32 b, s32 c, s32 d, s32* out) {
    s32 t0 = a + 1;
    s32 t1 = b + 2;
    s32 t2 = c + 3;
    s32 t3 = d + 4;
    s32 t4 = t2 + 5;
    out[0] = t0;
    out[1] = t1;
    out[2] = t3;
    out[3] = t4;
}
)",
      },
      // 12: `large_int` splitting (`mips.md:3208` `try_split` + `update_flow_info`).
      {
          .test_name = "Mk12LargeIntSplit",
          .function_name = "Mk12LargeIntSplit",
          .original_c = R"(
s32 Mk12LargeIntSplit(s32 a, s32 b, s32* out) {
    s32 c0 = 0x12345678;
    s32 c1 = 0x76543210;
    s32 c2 = (s32)0x89abcdef;
    s32 r = (a ^ c0) + (b & c1);
    *out = r | c2;
    return r + c0;
}
)",
      },
      // 13: `large_int` splitting including `(subreg:SI ...)` via 64-bit `s64`/`u64`.
      {
          .test_name = "Mk13LargeIntSubregSplit",
          .function_name = "Mk13LargeIntSubregSplit",
          .original_c = R"(
s64 Mk13LargeIntSubregSplit(s64 a, s64 b) {
    s64 k0 = 0x123456789abcdef0LL;
    s64 k1 = 0x0fedcba987654321LL;
    return (a ^ k0) + (b & k1);
}
)",
      },
      // 14: `SCHED_GROUP_P` double `INSN_REF_COUNT` (`USE` before call/return, `DImode`).
      {
          .test_name = "Mk14SchedGroupDoubleRefCount",
          .function_name = "Mk14SchedGroupDoubleRefCount",
          .original_c = R"(
extern s64 ExtCallee14(s64 x, s32 y);
s64 Mk14SchedGroupDoubleRefCount(s64 a, s32 b, s32 c) {
    s64 t = a + (s64)b;
    s64 r = ExtCallee14(t, c + 1);
    return r + (s64)(b ^ c);
}
)",
      },
      // 15: `CALL_INSN_FUNCTION_USAGE` `USE` upgrade (4 int + float args).
      {
          .test_name = "Mk15CallFusageUseUpgrade",
          .function_name = "Mk15CallFusageUseUpgrade",
          .original_c = R"(
extern s32 ExtCallee15(s32 a, s32 b, s32 c, s32 d);
extern f32 ExtCallee15F(f32 x, f32 y);
s32 Mk15CallFusageUseUpgrade(s32* p, f32* q) {
    s32 r = ExtCallee15(p[0], p[1] + 1, p[2] ^ 3, p[3] - 2);
    f32 f = ExtCallee15F(q[0] + 1.0f, q[1] * 2.0f);
    return r + (s32)f;
}
)",
      },
      // 16: `NOTE_INSN_LOOP_BEG`/`CONT`/`END` (`for` loop with `continue` and `break`).
      {
          .test_name = "Mk16LoopNotesBegContEnd",
          .function_name = "Mk16LoopNotesBegContEnd",
          .original_c = R"(
s32 Mk16LoopNotesBegContEnd(const s32* a, s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; ++i) {
        s32 v = a[i];
        if (v == 0) {
            continue;
        }
        if (v < 0) {
            break;
        }
        sum += v * 3 + 1;
    }
    return sum;
}
)",
      },
      // 17: `MEM_VOLATILE_P` (`volatile` loads/stores interleaved with regular accesses).
      {
          .test_name = "Mk17MemVolatile",
          .function_name = "Mk17MemVolatile",
          .original_c = R"(
s32 Mk17MemVolatile(volatile s32* hw, s32* buf) {
    s32 s0;
    s32 b0;
    s32 b1;
    s32 s1;
    s0 = *hw;
    b0 = buf[0];
    *hw = s0 + 1;
    b1 = buf[1];
    s1 = *hw;
    buf[2] = b0 + b1;
    return s0 + s1 + b0;
}
)",
      },
      // 18: `RTX_UNCHANGING_P` (`const` static table + float literals `$LC*` across stores).
      {
          .test_name = "Mk18RtxUnchanging",
          .function_name = "Mk18RtxUnchanging",
          .original_c = R"(
static const s32 kMk18Tbl[4] = { 10, 20, 30, 40 };
f32 Mk18RtxUnchanging(s32 idx, s32* out, f32 x) {
    s32 v;
    f32 y;
    v = kMk18Tbl[idx & 3];
    *out = v + 5;
    y = x * 3.14159f + 2.71828f;
    return y + (f32)kMk18Tbl[(idx + 1) & 3];
}
)",
      },
      // 19: `MEM_IN_STRUCT_P` (struct field loads/stores interleaved with scalar pointer).
      {
          .test_name = "Mk19MemInStruct",
          .function_name = "Mk19MemInStruct",
          .original_c = R"(
struct Mk19Pair { s32 x; s32 y; s32 z; };
s32 Mk19MemInStruct(struct Mk19Pair* s, s32* scalar) {
    s32 a;
    s32 b;
    a = s->x;
    *scalar = a + 7;
    b = s->y;
    s->z = a + b;
    return a + b + *scalar;
}
)",
      },
      // 20: `MULT` address scaling (`sched.c:727-750`).
      {
          .test_name = "Mk20MultScaledAlias",
          .function_name = "Mk20MultScaledAlias",
          .original_c = R"(
s32 Mk20MultScaledAlias(s32* a, s32 i, s32 val) {
    s32 x;
    s32 y;
    x = a[i];
    a[i + 1] = val;
    y = a[i + 2];
    a[i] = x + y;
    return x + y;
}
)",
      },
      // 21: `CONST` address unwrapping (`sched.c:753-774` on static global array).
      {
          .test_name = "Mk21ConstUnwrapAlias",
          .function_name = "Mk21ConstUnwrapAlias",
          .original_c = R"(
static s32 g_mk21_arr[8];
s32 Mk21ConstUnwrapAlias(s32 v) {
    s32 a;
    s32 b;
    a = g_mk21_arr[0];
    g_mk21_arr[1] = v;
    b = g_mk21_arr[2];
    g_mk21_arr[3] = a + b;
    return a + b + g_mk21_arr[4];
}
)",
      },
      // 22: `movstrsi_internal3` (fixed-size 24-byte struct copy -> `mem:BLK` `PARALLEL`).
      {
          .test_name = "Mk22MovstrsiInternal3",
          .function_name = "Mk22MovstrsiInternal3",
          .original_c = R"(
struct Mk22Blk { s32 w[6]; };
s32 Mk22MovstrsiInternal3(struct Mk22Blk* dst, const struct Mk22Blk* src, s32 k) {
    s32 t = src->w[0] + k;
    *dst = *src;
    return t + dst->w[5];
}
)",
      },
      // 23: `$fcr31` + FP compare/branch + conditional select (`SF` & `DF`).
      {
          .test_name = "Mk23FpCompareSelectFcr31",
          .function_name = "Mk23FpCompareSelectFcr31",
          .original_c = R"(
f64 Mk23FpCompareSelectFcr31(f32 a, f32 b, f64 x, f64 y) {
    f64 r = (a <= b) ? (x + y) : (x - y);
    if (x > y) {
        r = r * 1.5;
    }
    return r;
}
)",
      },
      // 24: Multi-block diamond with call-crossing vs local pseudos (`reg_n_calls_crossed`).
      {
          .test_name = "Mk24MultiBlockCallCrossing",
          .function_name = "Mk24MultiBlockCallCrossing",
          .original_c = R"(
extern s32 ExtCallee24(s32 x);
s32 Mk24MultiBlockCallCrossing(s32 a, s32 b, s32 flag) {
    s32 keep = a * 3 + b;
    s32 res;
    if (flag != 0) {
        s32 local_tmp = a + 7;
        res = ExtCallee24(local_tmp) + keep;
    } else {
        res = keep ^ (b - 4);
    }
    return res + keep;
}
)",
      },
      // 25: `-mips1 -funsigned-char` R3000 load-delay + unsigned byte loads.
      {
          .test_name = "Mk25Mips1R3000UnsignedChar",
          .function_name = "Mk25Mips1R3000UnsignedChar",
          .original_c = R"(
s32 Mk25Mips1R3000UnsignedChar(char* p, s32* q) {
    s32 a = p[0];
    s32 b = p[1];
    s32 c = q[0];
    s32 d = q[1];
    q[2] = a + b + c;
    return (a ^ b) + (c - d);
}
)",
          .options = {.c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"}},
      },
  };
  return *k_kernels;
}

// Selects exactly 200 stratified corpus case indices from
// `GetSharedRoundTripTestCases()`, evenly covering all 7 compilation profiles
// in `GetRoundTripCompilerFlagProfiles()`.
const std::vector<size_t>& GetStratifiedCorpusCaseIndices() {
  static const auto* const k_indices = []() {
    const auto& all_cases = GetSharedRoundTripTestCases();
    const auto& profiles = GetRoundTripCompilerFlagProfiles();
    const size_t num_profiles = profiles.size();
    std::vector<std::vector<size_t>> by_profile(num_profiles);

    for (size_t i = 0; i < all_cases.size(); ++i) {
      size_t matched = 0;
      for (size_t p = 0; p < num_profiles; ++p) {
        if (all_cases[i].options.c_flags == profiles[p].c_flags) {
          matched = p;
          break;
        }
      }
      by_profile[matched].push_back(i);
    }

    auto* result = new std::vector<size_t>();
    result->reserve(kNumStratifiedCorpusSeeds);
    const size_t base_quota = static_cast<size_t>(kNumStratifiedCorpusSeeds) / num_profiles;
    const size_t rem = static_cast<size_t>(kNumStratifiedCorpusSeeds) % num_profiles;

    for (size_t p = 0; p < num_profiles; ++p) {
      const size_t quota = base_quota + (p < rem ? 1 : 0);
      const auto& bucket = by_profile[p];
      if (bucket.empty()) {
        continue;
      }
      for (size_t k = 0; k < quota; ++k) {
        const size_t idx = (k * bucket.size()) / quota;
        result->push_back(bucket[idx]);
      }
    }
    return result;
  }();
  return *k_indices;
}

struct CachedSeedSnapshot {
  std::string name;
  CompilationProfile base_profile;
  RtlControlFlowGraph sched1_pre;
  RtlControlFlowGraph sched2_pre;
  bool valid = false;
};

// Lazily compiles and caches ONLY the seed snapshot(s) touched by the current
// Bazel shard (`pool_idx` in `[0, 25)` = synthetic micro-kernels, `[25, 225)`
// = stratified corpus snapshots).
const CachedSeedSnapshot& GetCachedSeedSnapshot(int pool_idx, GccContext* trace_ctx,
                                                FunctionHooks* trace_hooks) {
  static auto* const k_cache = new absl::btree_map<int, CachedSeedSnapshot>();
  if (auto it = k_cache->find(pool_idx); it != k_cache->end()) {
    return it->second;
  }

  FunctionCompilationTrace live_trace;
  const FunctionCompilationTrace* trace_ptr = nullptr;
  std::string seed_name;
  if (pool_idx < kNumMicroKernels) {
    const RoundTripTestCase& tc = GetSyntheticMicroKernels()[static_cast<size_t>(pool_idx)];
    seed_name = tc.test_name;
    trace_ctx->BeginFunction("fuzz_seed_trace");
    trace_hooks->ApplyCompilationProfile({2, tc.options.c_flags, {}});
    regclass_init();
    live_trace = trace_hooks->TraceForwardCompilation(tc);
    trace_ptr = &live_trace;
  } else {
    const auto& corpus_indices = GetStratifiedCorpusCaseIndices();
    const size_t c_idx = corpus_indices[static_cast<size_t>(pool_idx - kNumMicroKernels)];
    seed_name = GetSharedRoundTripTestCases()[c_idx].test_name;
    const absl::StatusOr<const FunctionCompilationTrace*> cached_trace =
        gcc_harness::GetCachedSharedCorpusTraceByIndex(c_idx);
    EXPECT_TRUE(cached_trace.ok()) << seed_name << ": " << cached_trace.status();
    if (!cached_trace.ok()) {
      CachedSeedSnapshot invalid;
      invalid.name = std::move(seed_name);
      invalid.valid = false;
      auto [it, _] = k_cache->emplace(pool_idx, std::move(invalid));
      return it->second;
    }
    trace_ptr = *cached_trace;
  }
  const FunctionCompilationTrace& trace = *trace_ptr;

  CachedSeedSnapshot snap;
  snap.name = std::move(seed_name);
  snap.base_profile = trace.stage0_input.profile;

  for (const SubfunctionBoundarySnapshot& s : trace.subfunction_snapshots) {
    if (s.subfunction_id == SubfunctionId::kScheduleBlockSched1) {
      snap.sched1_pre = s.pre_state;
    } else if (s.subfunction_id == SubfunctionId::kScheduleBlockSched2) {
      snap.sched2_pre = s.pre_state;
    }
  }
  if (snap.sched1_pre.rpo_blocks.empty()) {
    if (auto it = trace.pass_snapshots.find(RtlPassKind::kCombine);
        it != trace.pass_snapshots.end()) {
      snap.sched1_pre = it->second;
    }
  }
  if (snap.sched2_pre.rpo_blocks.empty()) {
    if (auto it = trace.pass_snapshots.find(RtlPassKind::kGreg); it != trace.pass_snapshots.end()) {
      snap.sched2_pre = it->second;
    }
  }

  // For `-O1` corpus snapshots (`optimize_level == 1`), `flow` and `combine`
  // already produced valid flow liveness; enabling `-fschedule-insns` and
  // `-fschedule-insns2` exercises real scheduling on genuine `-O1`-shaped RTL.
  // `-O0` corpus snapshots stay at `-O0` to cover `!RunsSched*()` passthrough.
  if (snap.base_profile.optimize_level == 1) {
    snap.base_profile.c_flags.push_back("-fschedule-insns");
    snap.base_profile.c_flags.push_back("-fschedule-insns2");
  }

  snap.valid = !snap.sched1_pre.rpo_blocks.empty() && !snap.sched2_pre.rpo_blocks.empty();
  auto [it, _] = k_cache->emplace(pool_idx, std::move(snap));
  return it->second;
}

// =============================================================================
// 2. Deterministic Seed Plan (`seed = 0..999`, `std::mt19937_64`)
// =============================================================================

struct SeedPlan {
  int seed = 0;
  int pool_idx = 0;  // `[0, 25)` micro-kernel, `[25, 225)` stratified corpus
  Phase phase = Phase::kSched1;
  std::vector<MutationFamily> families;
};

SeedPlan BuildSeedPlan(int seed) {
  SeedPlan plan;
  plan.seed = seed;
  std::mt19937_64 rng(kSeedSalt + static_cast<uint64_t>(seed));

  // 250 seeds from the 25 synthetic micro-kernels (10 per kernel: 5 kSched1 + 5 kSched2),
  // 750 seeds from the 200 stratified corpus snapshots (3-4 per snapshot across both phases).
  if (seed % 4 == 0) {
    const int k_slot = seed / 4;
    plan.pool_idx = k_slot % kNumMicroKernels;
    plan.phase = ((k_slot / kNumMicroKernels) % 2 == 0) ? Phase::kSched1 : Phase::kSched2;
  } else {
    const int c_slot = seed - (seed / 4) - 1;
    plan.pool_idx = kNumMicroKernels + (c_slot % kNumStratifiedCorpusSeeds);
    plan.phase = ((c_slot / kNumStratifiedCorpusSeeds) % 2 == 0) ? Phase::kSched1 : Phase::kSched2;
  }

  // Guarantee uniform primary coverage across all 6 families plus 0-2 composed
  // secondary families, emitted in canonical composition order.
  absl::btree_set<int> selected;
  selected.insert(seed % kNumMutationFamilies);
  const int extra_count = static_cast<int>(rng() % 3);  // 0, 1, or 2 extra families
  for (int i = 0; i < extra_count; ++i) {
    selected.insert(static_cast<int>(rng() % kNumMutationFamilies));
  }

  // Canonical composition order: structural injections/perturbations (M4, M3,
  // M6, M5) -> alpha-renaming (M2) -> DAG-preserving topo permutation (M1).
  static constexpr MutationFamily kCompositionOrder[kNumMutationFamilies] = {
      MutationFamily::kM4UnitAndCpu,   MutationFamily::kM3ConstLargeInt,
      MutationFamily::kM6CallBoundary, MutationFamily::kM5MemAliasFlush,
      MutationFamily::kM2AlphaRename,  MutationFamily::kM1TopoPerm,
  };
  for (MutationFamily fam : kCompositionOrder) {
    if (selected.contains(static_cast<int>(fam))) {
      plan.families.push_back(fam);
    }
  }
  return plan;
}

// =============================================================================
// 3. Common RTL Inspection, Free-Register Discovery & Liveness Helpers
// =============================================================================

int NextFreshPseudo(const RtlControlFlowGraph& cfg) {
  int next_reg = std::max(cfg.sideband.max_regno, kMipsFirstUserPseudoRegister);
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& raw : blk.instructions) {
      auto parsed = ParsePatternAndNotes(raw);
      if (!parsed.ok()) continue;
      parsed->pattern.Visit([&](const RtlExpr& n) {
        if (n.IsReg() && n.RegNo() >= next_reg) {
          next_reg = n.RegNo() + 1;
        }
      });
      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit([&](const RtlExpr& n) {
          if (n.IsReg() && n.RegNo() >= next_reg) {
            next_reg = n.RegNo() + 1;
          }
        });
      }
    }
  }
  return next_reg;
}

std::string FormatPn(const RtlPatternAndNotes& pn) {
  return FormatGccPrintRtlPatternAndNotes(pn.pattern, pn.reg_notes);
}

bool BlockHasReg(const RtlBasicBlock& blk, int regno) {
  for (const std::string& raw : blk.instructions) {
    auto parsed = ParsePatternAndNotes(raw);
    if (!parsed.ok()) continue;
    if (parsed->pattern.ContainsReg(regno)) return true;
  }
  return false;
}

absl::btree_set<int> CollectAllUsedHardRegsInCfg(const RtlControlFlowGraph& cfg,
                                                 const MachineModel& mm) {
  absl::btree_set<int> used;
  auto mark_expr = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& n) {
      if (n.IsHardReg()) {
        const int r = n.RegNo();
        const int nregs = std::max(1, mm.HardRegNRegs(r, n.mode));
        for (int k = 0; k < nregs; ++k) {
          used.insert(r + k);
        }
      }
    });
  };
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) continue;
      mark_expr(parsed->pattern);
      for (const RtlRegNote& note : parsed->reg_notes) {
        mark_expr(note.datum);
      }
      if (i < blk.rtl_insns.size()) {
        for (const RtlExpr& fu : blk.rtl_insns[i].call_fusage) {
          mark_expr(fu);
        }
      }
    }
  }
  for (const auto& [b, live_set] : cfg.sideband.basic_block_live_at_start) {
    for (int r : live_set) {
      if (r >= 0 && r < kMipsFirstPseudoRegister) {
        used.insert(r);
      }
    }
  }
  for (int r : cfg.sideband.saved_gpr_regnos) used.insert(r);
  for (int r : cfg.sideband.saved_fpr_regnos) {
    used.insert(r);
    used.insert(r + 1);
  }
  if (cfg.sideband.epilogue_delay_pattern.has_value()) {
    auto parsed = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
    if (parsed.ok()) mark_expr(parsed->pattern);
  }
  return used;
}

// Finds a safe insertion index inside `blk` (strictly inside the schedulable
// region, after block-0 pinned param copies/prologue, before trailing jump/return
// or `(use (reg/i ...))`, and never splitting a `SCHED_GROUP_P` or `REG_LIBCALL`
// sequence).
std::optional<size_t> FindSafeInsertionIndex(const RtlBasicBlock& blk, size_t block_idx,
                                             Phase phase, std::mt19937_64* rng) {
  const ParsedSchedRegion region = ParseBlockForSchedModel(blk, block_idx, phase);
  if (!region.valid || region.sched_orig_indices.empty()) {
    return std::nullopt;
  }
  int lo = 0;
  int hi = static_cast<int>(region.sched_block.size());
  if (phase == Phase::kSched2) {
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, block_idx);
    lo = bounds.prologue_end;
    hi = bounds.epilogue_begin;
  }
  // Never insert after a trailing jump, return, or `(use ...)` return marker.
  while (hi > lo) {
    const Insn& last = region.sched_block[static_cast<size_t>(hi - 1)];
    const RtlExpr& pat = last.pn.pattern;
    if (last.kind == Insn::Kind::kJump || pat.op == RtlOp::kUse || pat.op == RtlOp::kReturn) {
      --hi;
    } else {
      break;
    }
  }

  // Track `REG_LIBCALL .. REG_RETVAL` nesting so we never insert inside a libcall.
  int libcall_depth = 0;
  std::vector<size_t> candidates;
  for (int idx = lo; idx <= hi; ++idx) {
    bool inside_libcall = (libcall_depth > 0);
    bool splits_use_group = false;
    if (idx > lo && idx < static_cast<int>(region.sched_block.size())) {
      const RtlExpr& prev_pat = region.sched_block[static_cast<size_t>(idx - 1)].pn.pattern;
      if (prev_pat.op == RtlOp::kUse || prev_pat.op == RtlOp::kClobber) {
        splits_use_group = true;
      }
    }
    if (!inside_libcall && !splits_use_group) {
      size_t orig_pos = (idx < static_cast<int>(region.sched_orig_indices.size()))
                            ? region.sched_orig_indices[static_cast<size_t>(idx)]
                            : (region.sched_orig_indices.back() + 1);
      // Avoid inserting immediately before a mid-block loop note if possible.
      candidates.push_back(orig_pos);
    }
    if (idx < hi) {
      for (const RtlRegNote& note : region.sched_block[static_cast<size_t>(idx)].pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegLibcall) ++libcall_depth;
        if (note.kind == RtlMachineMode::kRegRetval && libcall_depth > 0) --libcall_depth;
      }
    }
  }
  if (candidates.empty()) {
    return std::nullopt;
  }
  return candidates[(*rng)() % candidates.size()];
}

void InsertInsnsAt(RtlBasicBlock* blk, size_t pos, absl::Span<const RtlInsn> to_insert) {
  const size_t clamped = std::min(pos, blk->instructions.size());
  std::vector<std::string> texts;
  texts.reserve(to_insert.size());
  for (const RtlInsn& insn : to_insert) {
    texts.push_back(insn.canonical_pattern);
  }
  blk->instructions.insert(blk->instructions.begin() + static_cast<std::ptrdiff_t>(clamped),
                           texts.begin(), texts.end());
  if (blk->rtl_insns.size() + to_insert.size() == blk->instructions.size()) {
    blk->rtl_insns.insert(blk->rtl_insns.begin() + static_cast<std::ptrdiff_t>(clamped),
                          to_insert.begin(), to_insert.end());
  } else {
    blk->SyncRtlInsnsFromInstructions();
  }
}

RtlInsn MakeRtlInsn(std::string pattern, std::vector<RtlExpr> call_fusage = {},
                    bool has_explicit_fusage = false) {
  RtlInsn insn;
  insn.canonical_pattern = std::move(pattern);
  insn.is_synthetic_rtl_expansion = true;
  insn.loop_note_kind = ParseRtlLoopNoteKind(insn.canonical_pattern);
  insn.has_explicit_call_fusage = has_explicit_fusage;
  insn.call_fusage = std::move(call_fusage);
  return insn;
}

std::string FormatRegAtom(RtlMachineMode mode, int regno) {
  std::string s = RtlExpr::MakeReg(mode, regno).ToGccPrintRtlString();
  // GCC 2.7.2 `config/mips/mips.h:3146` (`DEBUG_REGISTER_NAMES`) prints reg 30
  // as `$fp` (whereas `core/rtl_expr.cc` uses `fp`).
  if (regno == 30) {
    const size_t pos = s.find(" 30 fp)");
    if (pos != std::string::npos) {
      s.replace(pos, 7, " 30 $fp)");
    }
  }
  return s;
}

// Recomputes consistent `flow.c` liveness / `REG_DEAD` / `sideband` after
// structural mutations while preserving any post-reload `REG_DEAD (mem ...)`
// notes attached by M5.
void RefreshLivenessPreservingMemDeadNotes(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile, Phase phase,
                                           bool update_reg_notes) {
  // Snapshot any `REG_DEAD (mem ...)` notes keyed by `(block, insn_idx)`.
  absl::btree_map<std::pair<size_t, size_t>, std::vector<RtlRegNote>> saved_mem_dead;
  if (phase == Phase::kSched2 && update_reg_notes) {
    for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
      const RtlBasicBlock& blk = cfg->rpo_blocks[b];
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok()) continue;
        for (const RtlRegNote& note : parsed->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.IsMem()) {
            saved_mem_dead[{b, i}].push_back(note);
          }
        }
      }
    }
  }

  const bool do_notes = update_reg_notes && (profile.optimize_level > 0);
  ReconstructRtlLiveness(cfg, profile,
                         {.reload_completed = (phase == Phase::kSched2),
                          .update_reg_notes = do_notes,
                          .preserve_phantom_pseudos = (phase == Phase::kSched1)});

  // Restore saved post-reload `REG_DEAD (mem ...)` notes.
  for (const auto& [key, mem_notes] : saved_mem_dead) {
    auto& blk = cfg->rpo_blocks[key.first];
    if (key.second >= blk.instructions.size()) continue;
    auto parsed = ParsePatternAndNotes(blk.instructions[key.second]);
    if (!parsed.ok()) continue;
    for (const RtlRegNote& mn : mem_notes) {
      parsed->reg_notes.push_back(mn);
    }
    blk.instructions[key.second] = FormatPn(*parsed);
    blk.SyncRtlInsnsFromInstructions();
  }
  cfg->sideband.max_regno = std::max(cfg->sideband.max_regno, NextFreshPseudo(*cfg));
}

// Allocates `count` free caller-saved GPRs in `kSched2` (not used anywhere in `cfg`).
std::vector<int> PickFreeCallerSavedGprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCallerGprs[] = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25};
  std::vector<int> out;
  for (int r : kCallerGprs) {
    if (!used.contains(r)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) break;
    }
  }
  return out;
}

// Allocates `count` free caller-saved even FPR pairs `(r, r+1)` in `kSched2`.
std::vector<int> PickFreeCallerSavedFprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCallerFprs[] = {36, 38, 40, 42, 48, 50};
  std::vector<int> out;
  for (int r : kCallerFprs) {
    if (!used.contains(r) && !used.contains(r + 1)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) break;
    }
  }
  return out;
}

// Allocates `count` free callee-saved GPRs (`$s0..$s7`) in `kSched2`.
std::vector<int> PickFreeCalleeSavedGprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCalleeGprs[] = {16, 17, 18, 19, 20, 21, 22, 23};
  std::vector<int> out;
  for (int r : kCalleeGprs) {
    if (!used.contains(r)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) break;
    }
  }
  return out;
}

// Allocates `count` free callee-saved even FPR pairs (`$f20..$f30`) in `kSched2`.
std::vector<int> PickFreeCalleeSavedFprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCalleeFprs[] = {52, 54, 56, 58, 60, 62};
  std::vector<int> out;
  for (int r : kCalleeFprs) {
    if (!used.contains(r) && !used.contains(r + 1)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) break;
    }
  }
  return out;
}

// =============================================================================
// 4. Composable Legal Mutation Families (M1-M6)
// =============================================================================

// M1: Intra-block DAG-preserving topological permutation (`Analyze` + seeded
// Kahn, keeping `SCHED_GROUP_P`, `REG_LIBCALL`, prologue/epilogue, loop
// barriers, and `REG_DEAD` consistent).
bool ApplyMutationM1TopoPerm(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                             Phase phase, std::mt19937_64* rng) {
  const MachineModel mm = MachineModel::FromProfile(profile);
  const RegFacts facts = RegFacts::FromCfg(*cfg, profile, phase);
  bool any_permuted = false;

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    const ParsedSchedRegion region = ParseBlockForSchedModel(blk, b, phase);
    if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() < 2) {
      continue;
    }
    const int n = static_cast<int>(region.sched_block.size());
    const DepGraph g = Analyze(region.sched_block, facts, mm, phase);

    // Group atomic spans: `SCHED_GROUP_P` chains, `REG_LIBCALL..REG_RETVAL`
    // spans, and post-reload prologue/epilogue prefixes/suffixes.
    int prologue_end = 0;
    int epilogue_begin = n;
    if (phase == Phase::kSched2) {
      const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
      prologue_end = bounds.prologue_end;
      epilogue_begin = bounds.epilogue_begin;
    }

    std::vector<std::vector<int>> groups;
    std::vector<int> insn_to_group(static_cast<size_t>(n), -1);
    int libcall_depth = 0;
    for (int i = 0; i < n; ++i) {
      const bool attach_prev =
          (i > 0) &&
          (g.sched_group_p[static_cast<size_t>(i)] || libcall_depth > 0 || i < prologue_end ||
           i > epilogue_begin ||
           region.sched_block[static_cast<size_t>(i - 1)].pn.pattern.op == RtlOp::kUse ||
           region.sched_block[static_cast<size_t>(i - 1)].pn.pattern.op == RtlOp::kClobber);
      if (attach_prev && !groups.empty()) {
        groups.back().push_back(i);
      } else {
        groups.push_back({i});
      }
      insn_to_group[static_cast<size_t>(i)] = static_cast<int>(groups.size()) - 1;
      for (const RtlRegNote& note : region.sched_block[static_cast<size_t>(i)].pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegLibcall) ++libcall_depth;
        if (note.kind == RtlMachineMode::kRegRetval && libcall_depth > 0) --libcall_depth;
      }
    }

    const int num_g = static_cast<int>(groups.size());
    if (num_g < 2) continue;

    std::vector<absl::btree_set<int>> adj(static_cast<size_t>(num_g));
    std::vector<int> indeg(static_cast<size_t>(num_g), 0);
    auto add_edge = [&](int u, int v) {
      if (u == v) return;
      if (adj[static_cast<size_t>(u)].insert(v).second) {
        ++indeg[static_cast<size_t>(v)];
      }
    };

    // Preserve all `Analyze` DAG edges (`l.target -> i`).
    for (int i = 0; i < n; ++i) {
      const int gi = insn_to_group[static_cast<size_t>(i)];
      for (const Link& l : g.log_links[static_cast<size_t>(i)]) {
        if (l.target >= 0 && l.target < n) {
          add_edge(insn_to_group[static_cast<size_t>(l.target)], gi);
        }
      }
      // Pin prologue groups before body, and body before epilogue / jump / return.
      if (i < prologue_end) {
        for (int j = prologue_end; j < n; ++j) {
          add_edge(gi, insn_to_group[static_cast<size_t>(j)]);
        }
      }
      if (i >= epilogue_begin ||
          region.sched_block[static_cast<size_t>(i)].kind == Insn::Kind::kJump ||
          region.sched_block[static_cast<size_t>(i)].pn.pattern.op == RtlOp::kReturn) {
        for (int j = 0; j < i; ++j) {
          add_edge(insn_to_group[static_cast<size_t>(j)], gi);
        }
      }
    }

    // Also preserve relative order of any `REG_EQUIV (mem ...)` producer vs
    // memory writes/calls so `local-alloc` equivalence invariants stay valid.
    // (Already covered by `Analyze` memory/call edges for aliasing stores.)

    // Seeded Kahn's algorithm with anti-identity tie bias when >= 2 choices exist.
    std::vector<int> ready;
    for (int gi = 0; gi < num_g; ++gi) {
      if (indeg[static_cast<size_t>(gi)] == 0) ready.push_back(gi);
    }

    std::vector<int> new_group_order;
    new_group_order.reserve(static_cast<size_t>(num_g));
    bool diverged = false;
    while (!ready.empty()) {
      size_t pick_idx = static_cast<size_t>((*rng)() % ready.size());
      if (!diverged && ready.size() > 1 &&
          ready[pick_idx] == static_cast<int>(new_group_order.size())) {
        pick_idx = (pick_idx + 1) % ready.size();
      }
      const int chosen = ready[pick_idx];
      ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(pick_idx));
      if (chosen != static_cast<int>(new_group_order.size())) {
        diverged = true;
      }
      new_group_order.push_back(chosen);
      for (int nxt : adj[static_cast<size_t>(chosen)]) {
        if (--indeg[static_cast<size_t>(nxt)] == 0) {
          ready.push_back(nxt);
        }
      }
    }

    if (static_cast<int>(new_group_order.size()) != num_g || !diverged) {
      continue;
    }

    std::vector<int> new_sched_order;
    new_sched_order.reserve(static_cast<size_t>(n));
    for (int gi : new_group_order) {
      for (int idx : groups[static_cast<size_t>(gi)]) {
        new_sched_order.push_back(idx);
      }
    }

    // Splice permuted schedulable insns back into `blk` at their original slots.
    std::vector<std::string> orig_texts = blk.instructions;
    std::vector<RtlInsn> orig_rtl = blk.rtl_insns;
    for (int pos = 0; pos < n; ++pos) {
      const size_t dst_slot = region.sched_orig_indices[static_cast<size_t>(pos)];
      const size_t src_slot =
          region.sched_orig_indices[static_cast<size_t>(new_sched_order[static_cast<size_t>(pos)])];
      blk.instructions[dst_slot] = orig_texts[src_slot];
      if (dst_slot < blk.rtl_insns.size() && src_slot < orig_rtl.size()) {
        blk.rtl_insns[dst_slot] = orig_rtl[src_slot];
      }
    }
    any_permuted = true;
  }

  if (any_permuted) {
    if (phase == Phase::kSched1) {
      RefreshLivenessPreservingMemDeadNotes(cfg, profile, phase, /*update_reg_notes=*/true);
    } else if (profile.optimize_level > 0) {
      // In `kSched2`, `reload1.c` omits `REG_DEAD` on certain reload-emitted copies
      // (such as earlyclobber `DI` output-reload moves). Preserve exact existing
      // `REG_DEAD` ownership and only relocate an existing `(REG_DEAD r)` to the
      // true last reader of `r` within its live range if permutation swapped readers.
      for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
        RtlBasicBlock& blk = cfg->rpo_blocks[b];
        const size_t n = blk.instructions.size();
        std::vector<std::optional<RtlPatternAndNotes>> parsed_vec(n);
        for (size_t i = 0; i < n; ++i) {
          if (ParseRtlLoopNoteKind(blk.instructions[i]).has_value()) continue;
          auto p = ParsePatternAndNotes(blk.instructions[i]);
          if (p.ok()) parsed_vec[i] = std::move(*p);
        }
        auto insn_reads_hard = [&](size_t idx, int regno) -> bool {
          if (!parsed_vec[idx].has_value()) return false;
          const RtlExpr& pat = parsed_vec[idx]->pattern;
          bool reads = false;
          auto scan = [&](auto& self, const RtlExpr& e, bool in_dst) -> void {
            if (e.IsHardReg()) {
              if (!in_dst) {
                const int nr = std::max(1, mm.HardRegNRegs(e.RegNo(), e.mode));
                if (regno >= e.RegNo() && regno < e.RegNo() + nr) reads = true;
              }
              return;
            }
            if (e.IsSet()) {
              self(self, e.SetDest(), /*in_dst=*/e.SetDest().IsReg() || e.SetDest().IsSubreg());
              self(self, e.SetSrc(), false);
              return;
            }
            if (e.op == RtlOp::kClobber) return;
            for (const RtlExpr& ch : e.children) self(self, ch, false);
          };
          scan(scan, pat, false);
          if (idx < blk.rtl_insns.size()) {
            for (const RtlExpr& fu : blk.rtl_insns[idx].call_fusage) {
              if (fu.op == RtlOp::kUse && fu.ContainsReg(regno)) reads = true;
            }
          }
          return reads;
        };
        auto insn_writes_hard = [&](size_t idx, int regno) -> bool {
          if (!parsed_vec[idx].has_value()) return false;
          const RtlExpr& pat = parsed_vec[idx]->pattern;
          bool writes = false;
          auto check_dst = [&](const RtlExpr& dst) {
            const RtlExpr* r = &dst;
            while (r->IsSubreg() && !r->children.empty()) r = &r->children[0];
            if (r->IsHardReg()) {
              const int nr = std::max(1, mm.HardRegNRegs(r->RegNo(), r->mode));
              if (regno >= r->RegNo() && regno < r->RegNo() + nr) writes = true;
            }
          };
          if (pat.IsSet()) {
            check_dst(pat.SetDest());
          } else if (pat.IsParallel()) {
            for (const RtlExpr& ch : pat.children) {
              if (ch.IsSet()) check_dst(ch.SetDest());
              if (ch.op == RtlOp::kClobber && !ch.children.empty()) check_dst(ch.children[0]);
            }
          } else if (pat.op == RtlOp::kClobber && !pat.children.empty()) {
            check_dst(pat.children[0]);
          }
          if (pat.IsCallPattern() && mm.CallUsed(regno)) writes = true;
          return writes;
        };
        bool blk_dirty = false;
        for (size_t i = 0; i < n; ++i) {
          if (!parsed_vec[i].has_value()) continue;
          auto& notes = parsed_vec[i]->reg_notes;
          for (size_t ni = 0; ni < notes.size();) {
            if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.IsHardReg()) {
              const int r = notes[ni].datum.RegNo();
              // If `i` writes `r` (read-modify-write such as `a2 = a2 + c`),
              // `REG_DEAD r` marks the OLD input dying at `i`; any later reader
              // consumes the NEW value of `r` and must not inherit `REG_DEAD`.
              if (insn_writes_hard(i, r)) {
                ++ni;
                continue;
              }
              size_t last_reader = i;
              for (size_t k = i + 1; k < n; ++k) {
                if (insn_reads_hard(k, r)) last_reader = k;
                if (insn_writes_hard(k, r)) break;
              }
              if (last_reader != i && parsed_vec[last_reader].has_value() &&
                  !parsed_vec[last_reader]->pattern.IsCallPattern()) {
                RtlRegNote moved = notes[ni];
                notes.erase(notes.begin() + static_cast<std::ptrdiff_t>(ni));
                parsed_vec[last_reader]->reg_notes.push_back(std::move(moved));
                blk_dirty = true;
                continue;
              }
            }
            ++ni;
          }
        }
        if (blk_dirty) {
          for (size_t i = 0; i < n; ++i) {
            if (parsed_vec[i].has_value()) {
              blk.instructions[i] = FormatPn(*parsed_vec[i]);
              if (i < blk.rtl_insns.size()) {
                blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
              }
            }
          }
        }
      }
    }
  }
  return any_permuted;
}

// M2: Register alpha-renaming (pseudos `>= 72` in `kSched1`, free caller/callee
// hard regs in `kSched2` respecting even pairs).
bool ApplyMutationM2AlphaRename(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                                Phase phase, std::mt19937_64* rng) {
  const MachineModel mm = MachineModel::FromProfile(profile);
  absl::btree_map<int, int> rename_map;

  if (phase == Phase::kSched1) {
    absl::btree_set<int> pseudos;
    auto collect_pseudos = [&](const RtlExpr& expr) {
      expr.Visit([&](const RtlExpr& n) {
        if (n.IsReg() && n.RegNo() >= kMipsFirstUserPseudoRegister) {
          pseudos.insert(n.RegNo());
        }
      });
    };
    for (const RtlBasicBlock& blk : cfg->rpo_blocks) {
      for (const std::string& raw : blk.instructions) {
        auto parsed = ParsePatternAndNotes(raw);
        if (!parsed.ok()) continue;
        collect_pseudos(parsed->pattern);
        for (const RtlRegNote& note : parsed->reg_notes) collect_pseudos(note.datum);
      }
    }
    for (const auto& [b, live] : cfg->sideband.basic_block_live_at_start) {
      for (int r : live) {
        if (r >= kMipsFirstUserPseudoRegister) pseudos.insert(r);
      }
    }
    for (const auto& [r, _] : cfg->sideband.reg_n_refs) {
      if (r >= kMipsFirstUserPseudoRegister) pseudos.insert(r);
    }
    for (const auto& [r, _] : cfg->sideband.reg_n_sets) {
      if (r >= kMipsFirstUserPseudoRegister) pseudos.insert(r);
    }
    for (const auto& [r, _] : cfg->sideband.reg_live_length) {
      if (r >= kMipsFirstUserPseudoRegister) pseudos.insert(r);
    }
    for (const auto& [r, _] : cfg->sideband.reg_n_calls_crossed) {
      if (r >= kMipsFirstUserPseudoRegister) pseudos.insert(r);
    }
    for (int r : cfg->sideband.pointer_regnos) {
      if (r >= kMipsFirstUserPseudoRegister) pseudos.insert(r);
    }
    if (pseudos.empty()) return false;

    std::vector<int> orig_list(pseudos.begin(), pseudos.end());
    std::vector<int> targets = orig_list;
    const int shift = 1 + static_cast<int>((*rng)() % 8);
    if (targets.size() >= 2) {
      std::shuffle(targets.begin(), targets.end(), *rng);
    }
    // Guarantee non-trivial alpha-renaming by shifting at least when shuffle is identity.
    bool changed = false;
    for (size_t i = 0; i < orig_list.size(); ++i) {
      if (targets[i] != orig_list[i]) changed = true;
    }
    if (!changed || ((*rng)() % 2 == 0)) {
      for (int& t : targets) t += shift;
    }
    for (size_t i = 0; i < orig_list.size(); ++i) {
      rename_map[orig_list[i]] = targets[i];
    }
  } else {
    // Post-reload (`kSched2`): rename free-target eligible caller/callee hard regs,
    // strictly respecting even pairs for `DI`/`DF` and FPRs and never touching
    // ABI-fixed regs, `call_fusage`, `/i`, or `epilogue_delay_pattern`.
    const absl::btree_set<int> used = CollectAllUsedHardRegsInCfg(*cfg, mm);
    absl::btree_set<int> pinned;
    for (int r : {0,  1,  2,  3,  4,  5,  6,  7,  26, 27, 28, 29, 31,
                  32, 33, 34, 35, 44, 45, 46, 47, 64, 65, 66, 67}) {
      pinned.insert(r);
    }
    if (cfg->sideband.frame_pointer_needed) pinned.insert(30);
    // Only pin `saved_gpr_regnos`/`saved_fpr_regnos` if prologue/epilogue stack
    // save/restore insns have already been threaded into block 0. `.09.greg` is
    // pre-prologue-threading, so callee-saved hard regs can be alpha-renamed
    // cleanly along with `saved_*_regnos`.
    bool has_prologue_sp_adjust = false;
    if (!cfg->rpo_blocks.empty()) {
      const ParsedSchedRegion r0 = ParseBlockForSchedModel(cfg->rpo_blocks[0], 0, Phase::kSched2);
      if (r0.valid) {
        has_prologue_sp_adjust = (FindPostReloadBodyBounds(r0.sched_block, 0).prologue_end > 0);
      }
    }
    if (has_prologue_sp_adjust) {
      for (int r : cfg->sideband.saved_gpr_regnos) pinned.insert(r);
      for (int r : cfg->sideband.saved_fpr_regnos) {
        pinned.insert(r);
        pinned.insert(r + 1);
      }
    }

    absl::btree_set<int> wide_regs;  // Regs appearing in 2-reg mode (`DI`/`DF`) or FPRs.
    absl::btree_set<int> body_regs;
    for (const RtlBasicBlock& blk : cfg->rpo_blocks) {
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok()) continue;
        parsed->pattern.Visit([&](const RtlExpr& n) {
          if (!n.IsHardReg()) return;
          const int r = n.RegNo();
          body_regs.insert(r);
          if (n.flags.integrated) pinned.insert(r);
          if (mm.HardRegNRegs(r, n.mode) > 1 || (r >= 32 && r < 64)) {
            wide_regs.insert(r);
            if (r % 2 == 1) {
              pinned.insert(r);
              pinned.insert(r - 1);
            } else if (mm.HardRegNRegs(r, n.mode) > 1) {
              wide_regs.insert(r + 1);
            }
          }
        });
        if (i < blk.rtl_insns.size()) {
          for (const RtlExpr& fu : blk.rtl_insns[i].call_fusage) {
            fu.Visit([&](const RtlExpr& n) {
              if (n.IsHardReg()) pinned.insert(n.RegNo());
            });
          }
        }
      }
    }
    if (cfg->sideband.epilogue_delay_pattern.has_value()) {
      auto parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
      if (parsed.ok()) {
        parsed->pattern.Visit([&](const RtlExpr& n) {
          if (n.IsHardReg()) pinned.insert(n.RegNo());
        });
      }
    }

    absl::btree_set<int> occupied = used;
    auto try_rename_from_pool = [&](const std::vector<int>& pool, bool is_pair) {
      std::vector<int> free_targets;
      for (int cand : pool) {
        if (!occupied.contains(cand) && (!is_pair || !occupied.contains(cand + 1))) {
          free_targets.push_back(cand);
        }
      }
      std::shuffle(free_targets.begin(), free_targets.end(), *rng);
      size_t free_cursor = 0;
      for (int r : pool) {
        if (free_cursor >= free_targets.size()) break;
        if (!body_regs.contains(r) || pinned.contains(r) || rename_map.contains(r)) continue;
        if (is_pair) {
          if (r % 2 != 0 || pinned.contains(r + 1)) continue;
          const int dst = free_targets[free_cursor++];
          rename_map[r] = dst;
          rename_map[r + 1] = dst + 1;
          occupied.insert(dst);
          occupied.insert(dst + 1);
        } else {
          if (wide_regs.contains(r)) continue;
          const int dst = free_targets[free_cursor++];
          rename_map[r] = dst;
          occupied.insert(dst);
        }
      }
    };

    const std::vector<int> caller_gprs = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25};
    const std::vector<int> callee_gprs = {16, 17, 18, 19, 20, 21, 22, 23};
    const std::vector<int> caller_fprs = {36, 38, 40, 42, 48, 50};
    const std::vector<int> callee_fprs = {52, 54, 56, 58, 60, 62};
    try_rename_from_pool(caller_gprs, /*is_pair=*/false);
    try_rename_from_pool(callee_gprs, /*is_pair=*/false);
    try_rename_from_pool(caller_fprs, /*is_pair=*/true);
    try_rename_from_pool(callee_fprs, /*is_pair=*/true);
    // Also handle even `DI` GPR pairs (`$t0/$t1`, `$t2/$t3`, `$t4/$t5`, `$t6/$t7`, `$s0/$s1`, ...).
    try_rename_from_pool({8, 10, 12, 14, 24}, /*is_pair=*/true);
    try_rename_from_pool({16, 18, 20, 22}, /*is_pair=*/true);

    if (rename_map.empty()) return false;
  }

  auto rewrite_expr = [&](RtlExpr* expr) {
    expr->RewriteRegs([&](RtlExpr& n) {
      if (n.IsArgPointerReg()) return;
      if (auto it = rename_map.find(n.RegNo()); it != rename_map.end()) {
        n.SetRegNo(it->second);
      }
    });
  };

  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (ParseRtlLoopNoteKind(blk.instructions[i]).has_value()) continue;
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) continue;
      rewrite_expr(&parsed->pattern);
      for (RtlRegNote& note : parsed->reg_notes) {
        rewrite_expr(&note.datum);
      }
      blk.instructions[i] = FormatPn(*parsed);
      if (i < blk.rtl_insns.size()) {
        blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
        for (RtlExpr& fu : blk.rtl_insns[i].call_fusage) {
          rewrite_expr(&fu);
        }
      }
    }
  }

  auto remap_int_map = [&](absl::flat_hash_map<int, int>* m) {
    absl::flat_hash_map<int, int> out;
    for (const auto& [k, v] : *m) {
      auto it = rename_map.find(k);
      out[it != rename_map.end() ? it->second : k] = v;
    }
    *m = std::move(out);
  };
  remap_int_map(&cfg->sideband.reg_n_refs);
  remap_int_map(&cfg->sideband.reg_n_sets);
  remap_int_map(&cfg->sideband.reg_n_deaths);
  remap_int_map(&cfg->sideband.reg_live_length);
  remap_int_map(&cfg->sideband.reg_n_calls_crossed);
  remap_int_map(&cfg->sideband.reg_renumber);

  for (auto& [b, live] : cfg->sideband.basic_block_live_at_start) {
    absl::flat_hash_set<int> next_live;
    for (int r : live) {
      auto it = rename_map.find(r);
      next_live.insert(it != rename_map.end() ? it->second : r);
    }
    live = std::move(next_live);
  }
  for (int& r : cfg->sideband.pointer_regnos) {
    if (auto it = rename_map.find(r); it != rename_map.end()) r = it->second;
  }
  std::sort(cfg->sideband.pointer_regnos.begin(), cfg->sideband.pointer_regnos.end());
  for (int& r : cfg->sideband.saved_gpr_regnos) {
    if (auto it = rename_map.find(r); it != rename_map.end()) r = it->second;
  }
  std::sort(cfg->sideband.saved_gpr_regnos.begin(), cfg->sideband.saved_gpr_regnos.end());
  for (int& r : cfg->sideband.saved_fpr_regnos) {
    if (auto it = rename_map.find(r); it != rename_map.end()) r = it->second;
  }
  std::sort(cfg->sideband.saved_fpr_regnos.begin(), cfg->sideband.saved_fpr_regnos.end());
  cfg->sideband.max_regno = std::max(cfg->sideband.max_regno, NextFreshPseudo(*cfg));
  return true;
}

bool IsLargeIntConst(int64_t val) {
  if (val >= -32768 && val <= 32767) return false;
  if (val >= 0 && val <= 65535) return false;
  if ((val & 0xffffLL) == 0) return false;
  return true;
}

// M3: Constant & `large_int` perturbation (`small_int` <-> `large_int`
// triggering `mips.md:3208` `try_split` + `update_flow_info`, including
// `(subreg:SI ...)`).
bool ApplyMutationM3ConstLargeInt(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                                  Phase phase, std::mt19937_64* rng) {
  static constexpr int64_t kLargeInts[] = {0x12345678LL, 0x7fff0001LL, 0x00010001LL,
                                           0x55aa1234LL, -305419897LL, -2130706431LL};
  static constexpr int64_t kSmallInts[] = {0LL, 7LL, -13LL, 42LL, 1024LL, 65536LL};

  bool mutated = false;
  // 1. Perturb existing `(set (reg:SI ...) (const_int ...))` or
  //    `(set (subreg:SI (reg:DI ...) ...) (const_int ...))` instructions.
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok() || !parsed->pattern.IsSet()) continue;
      const RtlExpr& dst = parsed->pattern.SetDest();
      const RtlExpr& src = parsed->pattern.SetSrc();
      if (!src.IsConstInt()) continue;

      const bool is_si_reg = (dst.IsReg() && dst.mode == RtlMachineMode::kSi &&
                              (phase == Phase::kSched1 ? dst.RegNo() >= kMipsFirstUserPseudoRegister
                                                       : (dst.RegNo() >= 2 && dst.RegNo() <= 25)));
      const bool is_si_subreg = (dst.IsSubreg() && dst.mode == RtlMachineMode::kSi &&
                                 !dst.children.empty() && dst.children[0].IsReg());
      if (!is_si_reg && !is_si_subreg) continue;

      bool skip = false;
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegRetval ||
            note.kind == RtlMachineMode::kRegLibcall) {
          skip = true;
          break;
        }
      }
      if (skip) continue;

      const int64_t old_val = src.IntVal();
      const int64_t new_val = IsLargeIntConst(old_val)
                                  ? kSmallInts[(*rng)() % std::size(kSmallInts)]
                                  : kLargeInts[(*rng)() % std::size(kLargeInts)];
      parsed->pattern.MutableSetSrc() = RtlExpr::MakeConstInt(new_val);
      // Drop any stale `REG_EQUAL` note whose constant no longer matches.
      parsed->reg_notes.erase(
          std::remove_if(parsed->reg_notes.begin(), parsed->reg_notes.end(),
                         [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegEqual; }),
          parsed->reg_notes.end());
      blk.instructions[i] = FormatPn(*parsed);
      if (i < blk.rtl_insns.size()) {
        blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
      }
      mutated = true;
    }
  }

  // 2. If no existing constant-load was flipped (or 50% of the time in `kSched1`
  //    to guarantee `large_int` + `(subreg:SI ...)` split coverage), inject a
  //    live `large_int` load + store snippet into a safe block position.
  if (!mutated || (phase == Phase::kSched1 && ((*rng)() % 2 == 0))) {
    const MachineModel mm = MachineModel::FromProfile(profile);
    const absl::btree_set<int> used = CollectAllUsedHardRegsInCfg(*cfg, mm);
    const size_t b = (*rng)() % cfg->rpo_blocks.size();
    if (auto ins_pos = FindSafeInsertionIndex(cfg->rpo_blocks[b], b, phase, rng);
        ins_pos.has_value()) {
      const int64_t lv0 = kLargeInts[(*rng)() % std::size(kLargeInts)];
      const int64_t lv1 = kLargeInts[((*rng)() + 1) % std::size(kLargeInts)];
      std::vector<RtlInsn> snippet;
      if (phase == Phase::kSched1) {
        const int p = NextFreshPseudo(*cfg);
        if ((*rng)() % 2 == 0) {
          // Direct `SImode` `large_int` triggering `mips.md:3208` `try_split`.
          const std::string reg_p = FormatRegAtom(RtlMachineMode::kSi, p);
          snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", reg_p, " (const_int ", lv0, "))")));
          snippet.push_back(
              MakeRtlInsn(absl::StrCat("(set (mem:SI (symbol_ref:SI (\"__fuzz_m3_sink\"))) ", reg_p,
                                       ") (expr_list:REG_DEAD ", reg_p, " (nil))")));
        } else {
          // `(subreg:SI (reg:DI p) 0/1)` `large_int` triggering `try_split` + `update_flow_info`
          // on a genuinely live `DImode` pseudo initialized from memory.
          const std::string reg_di = FormatRegAtom(RtlMachineMode::kDi, p);
          snippet.push_back(MakeRtlInsn(
              absl::StrCat("(set ", reg_di, " (mem:DI (symbol_ref:SI (\"__fuzz_m3_src64\"))))")));
          snippet.push_back(
              MakeRtlInsn(absl::StrCat("(set (subreg:SI ", reg_di, " 0) (const_int ", lv0, "))")));
          snippet.push_back(
              MakeRtlInsn(absl::StrCat("(set (subreg:SI ", reg_di, " 1) (const_int ", lv1, "))")));
          snippet.push_back(
              MakeRtlInsn(absl::StrCat("(set (mem:DI (symbol_ref:SI (\"__fuzz_m3_sink64\"))) ",
                                       reg_di, ") (expr_list:REG_DEAD ", reg_di, " (nil))")));
        }
      } else {
        const std::vector<int> free_gprs = PickFreeCallerSavedGprs(used, 1);
        if (!free_gprs.empty()) {
          const std::string reg_g = FormatRegAtom(RtlMachineMode::kSi, free_gprs[0]);
          snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", reg_g, " (const_int ", lv0, "))")));
          snippet.push_back(MakeRtlInsn(absl::StrCat(
              "(set (mem:SI (symbol_ref:SI (\"__fuzz_m3_sink\"))) ", reg_g,
              (profile.optimize_level > 0 ? absl::StrCat(") (expr_list:REG_DEAD ", reg_g, " (nil))")
                                          : ")"))));
        }
      }
      if (!snippet.empty()) {
        InsertInsnsAt(&cfg->rpo_blocks[b], *ins_pos, snippet);
        if (phase == Phase::kSched1) {
          RefreshLivenessPreservingMemDeadNotes(cfg, profile, phase, /*update_reg_notes=*/true);
        }
        mutated = true;
      }
    }
  }
  return mutated;
}

// Helper: checks if `cfg` contains `mulsi3_r4000` (3 clobbers) or `mulsi3_internal` (2 clobbers).
void DetectCpuSensitivePatterns(const RtlControlFlowGraph& cfg, bool* has_r4000_mul,
                                bool* has_non_r4000_mul, bool* has_sqrt) {
  *has_r4000_mul = false;
  *has_non_r4000_mul = false;
  *has_sqrt = false;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& raw : blk.instructions) {
      auto parsed = ParsePatternAndNotes(raw);
      if (!parsed.ok()) continue;
      const RtlExpr& pat = parsed->pattern;
      pat.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kSqrt) *has_sqrt = true;
      });
      if (pat.IsParallel() && !pat.children.empty() && pat.children[0].IsSet() &&
          pat.children[0].SetSrc().op == RtlOp::kMult &&
          pat.children[0].SetSrc().mode == RtlMachineMode::kSi) {
        int clobbers = 0;
        for (size_t k = 1; k < pat.children.size(); ++k) {
          if (pat.children[k].op == RtlOp::kClobber) ++clobbers;
        }
        if (clobbers >= 3) {
          *has_r4000_mul = true;
        } else if (clobbers == 2) {
          *has_non_r4000_mul = true;
        }
      }
    }
  }
}

// M4: Functional-unit & CPU perturbation (injecting independent `load`/`store`/
// `xfer`/`hilo`/`imul`/`idiv`/`fadd`/`fmul`/`fdiv`/`fsqrt`/`fcmp` ops and
// sweeping `r3000`/`r4000`/`r4600`/`r4650`/`r6000`).
bool ApplyMutationM4UnitAndCpu(RtlControlFlowGraph* cfg, CompilationProfile* profile, Phase phase,
                               std::mt19937_64* rng) {
  bool mutated = false;

  // 1. CPU sweep across `r3000`, `r4000`, `r4600`, `r4650`, `r6000` respecting
  //    `mips.md` pattern constraints (`mulsi3_r4000`, `mulsi3_internal`, `sqrt`).
  bool has_r4000_mul = false;
  bool has_non_r4000_mul = false;
  bool has_sqrt = false;
  DetectCpuSensitivePatterns(*cfg, &has_r4000_mul, &has_non_r4000_mul, &has_sqrt);

  struct CpuChoice {
    const char* isa_flag;
    const char* cpu_flag;
    int isa_num;
    bool is_r4000;
  };
  static constexpr CpuChoice kCpuChoices[] = {
      {"-mips1", "-mcpu=r3000", 1, false}, {"-mips2", "-mcpu=r4000", 2, true},
      {"-mips2", "-mcpu=r4600", 2, false}, {"-mips2", "-mcpu=r4650", 2, false},
      {"-mips2", "-mcpu=r6000", 2, false},
  };

  std::vector<CpuChoice> legal_cpus;
  for (const CpuChoice& c : kCpuChoices) {
    if (has_r4000_mul && !c.is_r4000) continue;
    if (has_non_r4000_mul && c.is_r4000) continue;
    if (has_sqrt && c.isa_num < 2) continue;
    legal_cpus.push_back(c);
  }
  if (!legal_cpus.empty()) {
    const CpuChoice chosen = legal_cpus[(*rng)() % legal_cpus.size()];
    std::vector<std::string> new_flags;
    for (const std::string& f : profile->c_flags) {
      if (f == "-mips1" || f == "-mips2" || f == "-mips3" || f == "-mips4" ||
          f.rfind("-mcpu=", 0) == 0 || f == "-m4650") {
        continue;
      }
      new_flags.push_back(f);
    }
    new_flags.push_back(chosen.isa_flag);
    new_flags.push_back(chosen.cpu_flag);
    if (new_flags != profile->c_flags) {
      profile->c_flags = std::move(new_flags);
      mutated = true;
    }
  }

  // 2. Functional-unit op injection: pick 1-2 units from the 11 MIPS units.
  const MachineModel mm = MachineModel::FromProfile(*profile);
  const absl::btree_set<int> used = CollectAllUsedHardRegsInCfg(*cfg, mm);
  const size_t b = (*rng)() % cfg->rpo_blocks.size();
  auto ins_pos = FindSafeInsertionIndex(cfg->rpo_blocks[b], b, phase, rng);
  if (!ins_pos.has_value()) {
    return mutated;
  }

  // Unit kinds: 0=load, 1=store, 2=xfer, 3=hilo, 4=imul, 5=idiv, 6=fadd,
  //             7=fmul, 8=fdiv, 9=fsqrt, 10=fcmp.
  const int unit_kind = static_cast<int>((*rng)() % 11);
  const bool notes_on = (profile->optimize_level > 0);
  auto dead_suffix = [&](absl::Span<const std::string> dead_regs) -> std::string {
    if (!notes_on || dead_regs.empty()) return "";
    std::string tail = "(nil)";
    for (auto it = dead_regs.rbegin(); it != dead_regs.rend(); ++it) {
      tail = absl::StrCat("(expr_list:REG_DEAD ", *it, " ", tail, ")");
    }
    return absl::StrCat(" ", tail);
  };

  std::vector<RtlInsn> snippet;
  int p = NextFreshPseudo(*cfg);
  const std::vector<int> fg = PickFreeCallerSavedGprs(used, 4);
  const std::vector<int> ff = PickFreeCallerSavedFprs(used, 3);
  const bool hi_lo_free = !used.contains(64) && !used.contains(65) && !used.contains(66);
  const bool fcr_free = !used.contains(67) && !BlockHasReg(cfg->rpo_blocks[b], 67);

  auto reg_s = [&](int idx, RtlMachineMode m) -> std::string {
    if (phase == Phase::kSched1) return FormatRegAtom(m, p + idx);
    if (m == RtlMachineMode::kSf || m == RtlMachineMode::kDf) {
      return FormatRegAtom(m, ff[static_cast<size_t>(idx)]);
    }
    return FormatRegAtom(m, fg[static_cast<size_t>(idx)]);
  };

  const bool have_g2 = (phase == Phase::kSched1 || fg.size() >= 2);
  const bool have_g4 = (phase == Phase::kSched1 || fg.size() >= 4);
  const bool have_f3 = (phase == Phase::kSched1 || ff.size() >= 3);
  const bool have_g1f1 = (phase == Phase::kSched1 || (!fg.empty() && !ff.empty()));

  int eff_unit = unit_kind;
  if (eff_unit == 9 && mm.MipsIsa() < 2) eff_unit = 8;  // `fsqrt` requires `mips_isa >= 2`
  if (eff_unit == 5 && profile->optimize_level == 0) {
    eff_unit = 4;  // `divmodsi4` requires `optimize`
  }
  if ((eff_unit == 3 || eff_unit == 4) && phase == Phase::kSched2 && !hi_lo_free) eff_unit = 0;
  if (eff_unit == 5 && phase == Phase::kSched2 && (!hi_lo_free || !have_g4)) eff_unit = 0;
  if (eff_unit == 10 && !fcr_free) eff_unit = 6;
  if ((eff_unit >= 6 && eff_unit <= 10) && !have_f3) eff_unit = 0;
  if (eff_unit == 2 && !have_g1f1) eff_unit = 0;

  switch (eff_unit) {
    case 0: {  // `load`
      if (!have_g2) break;
      const std::string r0 = reg_s(0, RtlMachineMode::kSi);
      const std::string r1 = reg_s(1, RtlMachineMode::kSi);
      snippet.push_back(MakeRtlInsn(
          absl::StrCat("(set ", r0, " (mem:SI (symbol_ref:SI (\"__fuzz_m4_ld_src\"))))")));
      snippet.push_back(MakeRtlInsn(
          absl::StrCat("(set ", r1, " (plus:SI ", r0, " (const_int 5)))", dead_suffix({r0}))));
      snippet.push_back(MakeRtlInsn(absl::StrCat(
          "(set (mem:SI (symbol_ref:SI (\"__fuzz_m4_ld_dst\"))) ", r1, ")", dead_suffix({r1}))));
      break;
    }
    case 1: {  // `store`
      if (phase == Phase::kSched2 && fg.empty()) break;
      const std::string r0 = reg_s(0, RtlMachineMode::kSi);
      snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", r0, " (const_int 19))")));
      snippet.push_back(MakeRtlInsn(absl::StrCat(
          "(set (mem:SI (symbol_ref:SI (\"__fuzz_m4_st_dst\"))) ", r0, ")", dead_suffix({r0}))));
      break;
    }
    case 2: {  // `xfer` (GPR <-> FPR `mtc1` move via `movsf_internal1`)
      if (fg.empty() || (phase == Phase::kSched2 && ff.empty())) break;
      const std::string g0_si = FormatRegAtom(RtlMachineMode::kSi, fg[0]);
      const std::string g0_sf = FormatRegAtom(RtlMachineMode::kSf, fg[0]);
      const std::string f0 = (phase == Phase::kSched1) ? FormatRegAtom(RtlMachineMode::kSf, p)
                                                       : FormatRegAtom(RtlMachineMode::kSf, ff[0]);
      snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", g0_si, " (const_int 0))")));
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", f0, " ", g0_sf, ")", dead_suffix({g0_sf}))));
      snippet.push_back(MakeRtlInsn(absl::StrCat(
          "(set (mem:SF (symbol_ref:SI (\"__fuzz_m4_xf_dst\"))) ", f0, ")", dead_suffix({f0}))));
      break;
    }
    case 3:    // `hilo` (`smulsi3_highpart` + `mfhi`)
    case 4: {  // `imul` (`smulsi3_highpart`)
      if (phase == Phase::kSched2 && fg.size() < 3) break;
      const std::string a = reg_s(0, RtlMachineMode::kSi);
      const std::string b_r = reg_s(1, RtlMachineMode::kSi);
      const std::string out = reg_s(2, RtlMachineMode::kSi);
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", a, " (mem:SI (symbol_ref:SI (\"__fuzz_m4_ma\"))))")));
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", b_r, " (mem:SI (symbol_ref:SI (\"__fuzz_m4_mb\"))))")));
      if (phase == Phase::kSched1) {
        const std::string dst = (eff_unit == 3) ? "(reg:SI 64 hi)" : reg_s(3, RtlMachineMode::kSi);
        snippet.push_back(MakeRtlInsn(absl::StrCat(
            "(parallel[ (set ", dst, " (truncate:SI (lshiftrt:DI (mult:DI (sign_extend:DI ", a,
            ") (sign_extend:DI ", b_r,
            ")) (const_int 32)))) (clobber (scratch:SI)) (clobber (scratch:SI)) ] )",
            dead_suffix({a, b_r}))));
        snippet.push_back(
            MakeRtlInsn(absl::StrCat("(set ", out, " ", dst, ")", dead_suffix({dst}))));
      } else {
        snippet.push_back(MakeRtlInsn(absl::StrCat(
            "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI "
            "(sign_extend:DI ",
            a, ") (sign_extend:DI ", b_r,
            ")) (const_int 32)))) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) "
            "] )",
            dead_suffix({a, b_r}))));
        snippet.push_back(MakeRtlInsn(
            absl::StrCat("(set ", out, " (reg:SI 64 hi))", dead_suffix({"(reg:SI 64 hi)"}))));
      }
      snippet.push_back(MakeRtlInsn(absl::StrCat(
          "(set (mem:SI (symbol_ref:SI (\"__fuzz_m4_mul_dst\"))) ", out, ")", dead_suffix({out}))));
      break;
    }
    case 5: {  // `idiv` (`divmodsi4`)
      if (!have_g4) break;
      const std::string a = reg_s(0, RtlMachineMode::kSi);
      const std::string b_r = reg_s(1, RtlMachineMode::kSi);
      const std::string q = reg_s(2, RtlMachineMode::kSi);
      const std::string rem = reg_s(3, RtlMachineMode::kSi);
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", a, " (mem:SI (symbol_ref:SI (\"__fuzz_m4_da\"))))")));
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", b_r, " (mem:SI (symbol_ref:SI (\"__fuzz_m4_db\"))))")));
      const std::string clobbers =
          (phase == Phase::kSched1)
              ? "(clobber (scratch:SI)) (clobber (scratch:SI)) (clobber (scratch:SI))"
              : "(clobber (reg:SI 65 lo)) (clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum))";
      const std::string unused_note =
          notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", a, " (expr_list:REG_DEAD ", b_r,
                                  " (expr_list:REG_UNUSED ", rem, " (nil))))")
                   : "";
      snippet.push_back(MakeRtlInsn(absl::StrCat("(parallel[ (set ", q, " (div:SI ", a, " ", b_r,
                                                 ")) (set ", rem, " (mod:SI ", a, " ", b_r, ")) ",
                                                 clobbers, " ] )", unused_note)));
      snippet.push_back(MakeRtlInsn(absl::StrCat(
          "(set (mem:SI (symbol_ref:SI (\"__fuzz_m4_div_dst\"))) ", q, ")", dead_suffix({q}))));
      break;
    }
    case 6:    // `fadd`
    case 7:    // `fmul`
    case 8: {  // `fdiv`
      if (!have_f3) break;
      const bool use_df = ((*rng)() % 2 == 0);
      const RtlMachineMode fm = use_df ? RtlMachineMode::kDf : RtlMachineMode::kSf;
      const std::string m_str = use_df ? "DF" : "SF";
      const std::string op_str = (eff_unit == 6) ? "plus" : (eff_unit == 7 ? "mult" : "div");
      const std::string fa = reg_s(0, fm);
      const std::string fb = reg_s(1, fm);
      const std::string fc = reg_s(2, fm);
      snippet.push_back(MakeRtlInsn(
          absl::StrCat("(set ", fa, " (mem:", m_str, " (symbol_ref:SI (\"__fuzz_m4_fa\"))))")));
      snippet.push_back(MakeRtlInsn(
          absl::StrCat("(set ", fb, " (mem:", m_str, " (symbol_ref:SI (\"__fuzz_m4_fb\"))))")));
      snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", fc, " (", op_str, ":", m_str, " ", fa,
                                                 " ", fb, "))", dead_suffix({fa, fb}))));
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set (mem:", m_str, " (symbol_ref:SI (\"__fuzz_m4_fp_dst\"))) ",
                                   fc, ")", dead_suffix({fc}))));
      break;
    }
    case 9: {  // `fsqrt`
      if (!have_f3) break;
      const bool use_df = ((*rng)() % 2 == 0);
      const RtlMachineMode fm = use_df ? RtlMachineMode::kDf : RtlMachineMode::kSf;
      const std::string m_str = use_df ? "DF" : "SF";
      const std::string fa = reg_s(0, fm);
      const std::string fc = reg_s(1, fm);
      snippet.push_back(MakeRtlInsn(
          absl::StrCat("(set ", fa, " (mem:", m_str, " (symbol_ref:SI (\"__fuzz_m4_sqa\"))))")));
      snippet.push_back(MakeRtlInsn(
          absl::StrCat("(set ", fc, " (sqrt:", m_str, " ", fa, "))", dead_suffix({fa}))));
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set (mem:", m_str, " (symbol_ref:SI (\"__fuzz_m4_sq_dst\"))) ",
                                   fc, ")", dead_suffix({fc}))));
      break;
    }
    case 10: {  // `fcmp` (`$fcr31`, `mips.md:5718` `slt_sf`)
      if (!have_f3) break;
      // Place `slt_sf` + trailing `(use (reg:CC_FP 67 $fcr31))` in an exit
      // block so `flow.c` (`ReconstructRtlLiveness`) treats `$fcr31`, `fa`,
      // and `fb` as 100% live (`insn_dead_p == 0` for `USE`).
      size_t exit_b = cfg->rpo_blocks.size();
      for (size_t bi = cfg->rpo_blocks.size(); bi > 0; --bi) {
        if (cfg->rpo_blocks[bi - 1].successors.empty()) {
          exit_b = bi - 1;
          break;
        }
      }
      if (exit_b >= cfg->rpo_blocks.size()) break;
      RtlBasicBlock& eblk = cfg->rpo_blocks[exit_b];
      const ParsedSchedRegion ereg = ParseBlockForSchedModel(eblk, exit_b, phase);
      if (!ereg.valid || ereg.sched_orig_indices.empty()) break;
      size_t use_pos = ereg.sched_orig_indices.back() + 1;
      if (ereg.sched_block.back().kind == Insn::Kind::kJump) {
        use_pos = ereg.sched_orig_indices.back();
      }
      if (use_pos < ereg.prefix_orig_indices.size()) break;
      const std::string fa = reg_s(0, RtlMachineMode::kSf);
      const std::string fb = reg_s(1, RtlMachineMode::kSf);
      std::vector<RtlInsn> fcmp_snippet;
      fcmp_snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", fa, " (mem:SF (symbol_ref:SI (\"__fuzz_m4_ca\"))))")));
      fcmp_snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", fb, " (mem:SF (symbol_ref:SI (\"__fuzz_m4_cb\"))))")));
      fcmp_snippet.push_back(MakeRtlInsn(absl::StrCat("(set (reg:CC_FP 67 $fcr31) (lt:CC_FP ", fa,
                                                      " ", fb, "))", dead_suffix({fa, fb}))));
      fcmp_snippet.push_back(MakeRtlInsn(
          absl::StrCat("(use (reg:CC_FP 67 $fcr31))",
                       (notes_on ? " (expr_list:REG_DEAD (reg:CC_FP 67 $fcr31) (nil))" : ""))));
      InsertInsnsAt(&eblk, use_pos, fcmp_snippet);
      if (phase == Phase::kSched1) {
        RefreshLivenessPreservingMemDeadNotes(cfg, *profile, phase, /*update_reg_notes=*/true);
      }
      mutated = true;
      break;
    }
  }

  if (!snippet.empty()) {
    InsertInsnsAt(&cfg->rpo_blocks[b], *ins_pos, snippet);
    if (phase == Phase::kSched1) {
      RefreshLivenessPreservingMemDeadNotes(cfg, *profile, phase, /*update_reg_notes=*/true);
    }
    mutated = true;
  }
  return mutated;
}

// M5: Memory alias & `> 32` flush perturbation (`$sp`/`$fp`/`argp`, `/u`/`/v`/`/s`,
// 33-40 independent memory ops ahead of a store, `REG_DEAD (mem ...)` post-reload).
bool ApplyMutationM5MemAliasFlush(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                                  Phase phase, std::mt19937_64* rng) {
  const MachineModel mm = MachineModel::FromProfile(profile);
  const absl::btree_set<int> used = CollectAllUsedHardRegsInCfg(*cfg, mm);
  const bool notes_on = (profile.optimize_level > 0);
  bool mutated = false;

  // Pick 1-2 sub-mutations from `{0=33..40 flush, 1=qualifier /u/v/s, 2=sp/fp/argp, 3=REG_DEAD
  // mem}`.
  const int primary_sub = static_cast<int>((*rng)() % (phase == Phase::kSched2 ? 4 : 3));
  absl::btree_set<int> subs = {primary_sub};
  if ((*rng)() % 2 == 0) {
    subs.insert(static_cast<int>((*rng)() % (phase == Phase::kSched2 ? 4 : 3)));
  }

  // Sub 1: Toggle `/u`, `/v`, `/s` qualifiers on eligible `MEM` operands.
  if (subs.contains(1)) {
    for (RtlBasicBlock& blk : cfg->rpo_blocks) {
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok() || !parsed->pattern.IsSet()) continue;
        // Skip insns with `REG_EQUIV (mem ...)` so we never desync note flags.
        bool has_equiv = false;
        for (const RtlRegNote& n : parsed->reg_notes) {
          if (n.kind == RtlMachineMode::kRegEquiv) has_equiv = true;
        }
        if (has_equiv) continue;

        bool changed_insn = false;
        const bool is_load = parsed->pattern.SetSrc().IsMem();
        const bool is_store = parsed->pattern.SetDest().IsMem();
        if (is_load) {
          RtlExpr& m = parsed->pattern.MutableSetSrc();
          const int bit = static_cast<int>((*rng)() % 3);
          if (bit == 0) m.flags.in_struct = !m.flags.in_struct;
          if (bit == 1) m.flags.unchanging = !m.flags.unchanging;
          if (bit == 2 && !m.flags.unchanging) m.flags.volatil = !m.flags.volatil;
          changed_insn = true;
        } else if (is_store) {
          RtlExpr& m = parsed->pattern.MutableSetDest();
          if ((*rng)() % 2 == 0) {
            m.flags.in_struct = !m.flags.in_struct;
          } else {
            m.flags.volatil = !m.flags.volatil;
          }
          changed_insn = true;
        }
        if (changed_insn) {
          blk.instructions[i] = FormatPn(*parsed);
          if (i < blk.rtl_insns.size()) blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
          mutated = true;
        }
      }
    }
  }

  // Sub 0: Inject 33..40 independent memory loads ahead of a disjoint store
  // (`sched.c:1740` `pending_lists_length > 32` flush!).
  if (subs.contains(0)) {
    const size_t b = (*rng)() % cfg->rpo_blocks.size();
    if (auto ins_pos = FindSafeInsertionIndex(cfg->rpo_blocks[b], b, phase, rng);
        ins_pos.has_value()) {
      const int num_loads = 33 + static_cast<int>((*rng)() % 8);  // 33..40
      const std::vector<int> fg = PickFreeCallerSavedGprs(used, 2);
      if (phase == Phase::kSched1 || fg.size() >= 2) {
        int p = NextFreshPseudo(*cfg);
        const std::string acc = (phase == Phase::kSched1)
                                    ? FormatRegAtom(RtlMachineMode::kSi, p)
                                    : FormatRegAtom(RtlMachineMode::kSi, fg[0]);
        const std::string tmp = (phase == Phase::kSched1)
                                    ? FormatRegAtom(RtlMachineMode::kSi, p + 1)
                                    : FormatRegAtom(RtlMachineMode::kSi, fg[1]);
        std::vector<RtlInsn> snippet;
        snippet.reserve(static_cast<size_t>(num_loads * 2 + 2));
        for (int k = 0; k < num_loads; ++k) {
          const std::string addr =
              (k == 0) ? "(symbol_ref:SI (\"__fuzz_m5_arr\"))"
                       : absl::StrCat(
                             "(const:SI (plus:SI (symbol_ref:SI (\"__fuzz_m5_arr\")) "
                             "(const_int ",
                             k * 4, ")))");
          if (k == 0) {
            snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", acc, " (mem/u:SI ", addr, "))")));
          } else {
            snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", tmp, " (mem/u:SI ", addr, "))")));
            snippet.push_back(MakeRtlInsn(absl::StrCat(
                "(set ", acc, " (plus:SI ", acc, " ", tmp, "))",
                (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", tmp, " (nil))") : ""))));
          }
        }
        // Disjoint store after >32 pending reads triggers `flush_pending_lists`.
        snippet.push_back(MakeRtlInsn(
            absl::StrCat("(set (mem:SI (symbol_ref:SI (\"__fuzz_m5_sink\"))) ", acc, ")",
                         (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", acc, " (nil))") : ""))));
        InsertInsnsAt(&cfg->rpo_blocks[b], *ins_pos, snippet);
        if (phase == Phase::kSched1) {
          RefreshLivenessPreservingMemDeadNotes(cfg, profile, phase, /*update_reg_notes=*/true);
        }
        mutated = true;
      }
    }
  }

  // Sub 2: `$sp`/`$fp`/`argp` base loads interleaved with static symbol store.
  if (subs.contains(2)) {
    const size_t b = (*rng)() % cfg->rpo_blocks.size();
    if (auto ins_pos = FindSafeInsertionIndex(cfg->rpo_blocks[b], b, phase, rng);
        ins_pos.has_value()) {
      const std::vector<int> fg = PickFreeCallerSavedGprs(used, 2);
      if (phase == Phase::kSched1 || fg.size() >= 2) {
        int p = NextFreshPseudo(*cfg);
        const std::string r0 = (phase == Phase::kSched1)
                                   ? FormatRegAtom(RtlMachineMode::kSi, p)
                                   : FormatRegAtom(RtlMachineMode::kSi, fg[0]);
        const std::string r1 = (phase == Phase::kSched1)
                                   ? FormatRegAtom(RtlMachineMode::kSi, p + 1)
                                   : FormatRegAtom(RtlMachineMode::kSi, fg[1]);
        const int base_pick = static_cast<int>((*rng)() % (phase == Phase::kSched1 ? 3 : 2));
        const std::string base_reg =
            (base_pick == 0)
                ? FormatRegAtom(RtlMachineMode::kSi, 29)
                : (base_pick == 1 && (phase == Phase::kSched1 || cfg->sideband.frame_pointer_needed)
                       ? FormatRegAtom(RtlMachineMode::kSi, 30)
                       : (phase == Phase::kSched1 ? "(reg:SI 0 argp)"
                                                  : FormatRegAtom(RtlMachineMode::kSi, 29)));
        std::vector<RtlInsn> snippet;
        snippet.push_back(MakeRtlInsn(
            absl::StrCat("(set ", r0, " (mem:SI (plus:SI ", base_reg, " (const_int 4))))")));
        snippet.push_back(MakeRtlInsn(
            absl::StrCat("(set (mem/s:SI (symbol_ref:SI (\"__fuzz_m5_sym\"))) ", r0, ")",
                         (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", r0, " (nil))") : ""))));
        snippet.push_back(MakeRtlInsn(
            absl::StrCat("(set ", r1, " (mem/s:SI (plus:SI ", base_reg, " (const_int 8))))")));
        snippet.push_back(MakeRtlInsn(
            absl::StrCat("(set (mem:SI (symbol_ref:SI (\"__fuzz_m5_sym2\"))) ", r1, ")",
                         (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", r1, " (nil))") : ""))));
        InsertInsnsAt(&cfg->rpo_blocks[b], *ins_pos, snippet);
        if (phase == Phase::kSched1) {
          RefreshLivenessPreservingMemDeadNotes(cfg, profile, phase, /*update_reg_notes=*/true);
        }
        mutated = true;
      }
    }
  }

  // Sub 3: Post-reload `REG_DEAD (mem ...)` (`sched.c:2095-2102`).
  if (phase == Phase::kSched2 && subs.contains(3)) {
    for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
      RtlBasicBlock& blk = cfg->rpo_blocks[b];
      const ParsedSchedRegion region = ParseBlockForSchedModel(blk, b, phase);
      if (!region.valid || region.sched_orig_indices.empty()) continue;
      const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
      for (int idx = bounds.prologue_end; idx < bounds.epilogue_begin; ++idx) {
        const Insn& insn = region.sched_block[static_cast<size_t>(idx)];
        if (insn.kind != Insn::Kind::kInsn || !insn.pn.pattern.IsSet()) continue;
        const size_t orig_i = region.sched_orig_indices[static_cast<size_t>(idx)];
        auto parsed = ParsePatternAndNotes(blk.instructions[orig_i]);
        if (!parsed.ok()) continue;
        RtlRegNote mem_dead;
        mem_dead.kind = RtlMachineMode::kRegDead;
        mem_dead.list_op = RtlOp::kExprList;
        mem_dead.datum = RtlExpr::MakeMem(
            RtlMachineMode::kSi, RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi,
                                                     RtlExpr::MakeReg(RtlMachineMode::kSi, 29),
                                                     RtlExpr::MakeConstInt(4)));
        parsed->reg_notes.push_back(std::move(mem_dead));
        blk.instructions[orig_i] = FormatPn(*parsed);
        if (orig_i < blk.rtl_insns.size()) {
          blk.rtl_insns[orig_i].canonical_pattern = blk.instructions[orig_i];
        }
        mutated = true;
        break;
      }
      if (mutated) break;
    }
  }
  return mutated;
}

// M6: Call-boundary & `reg_n_calls_crossed` perturbation (`call_fusage`,
// pre/post-call callee-saved ops, `$sp` stores, `REG_EQUIV` vs `REG_DEAD`
// ordering, `SCHED_GROUP_P` `USE` before call/return).
bool ApplyMutationM6CallBoundary(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                                 Phase phase, std::mt19937_64* rng) {
  const MachineModel mm = MachineModel::FromProfile(profile);
  const bool notes_on = (profile.optimize_level > 0);
  bool mutated = false;

  // Sub A: Perturb `REG_EQUIV` / `REG_EQUAL` vs `REG_DEAD` ordering on existing insns.
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok() || parsed->reg_notes.size() < 2) continue;
      bool has_dead = false;
      bool has_eq = false;
      for (const RtlRegNote& n : parsed->reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead) has_dead = true;
        if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual) {
          has_eq = true;
        }
      }
      if (has_dead && has_eq) {
        std::reverse(parsed->reg_notes.begin(), parsed->reg_notes.end());
        blk.instructions[i] = FormatPn(*parsed);
        if (i < blk.rtl_insns.size()) blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
        mutated = true;
      }
    }
  }

  // Sub B: Inject a legal call boundary with `$a0` arg setup, optional `$sp`
  // outgoing store, optional `SCHED_GROUP_P` `(use (reg:SI 4 a0))`, explicit
  // `call_fusage`, AND pre/post-call callee-saved / call-crossing ops.
  const absl::btree_set<int> used = CollectAllUsedHardRegsInCfg(*cfg, mm);
  const size_t b = (*rng)() % cfg->rpo_blocks.size();
  auto ins_pos = FindSafeInsertionIndex(cfg->rpo_blocks[b], b, phase, rng);
  if (!ins_pos.has_value()) {
    return mutated;
  }

  // In `kSched2`, only inject a `CALL_INSN` if `$a0` (`4`) and `$ra` (`31`) and
  // caller-saved regs aren't live across `*ins_pos` — easiest guaranteed-safe
  // condition: only inject a fresh call in `kSched1` (where `flow_analysis`
  // tracks pseudos across calls cleanly!) OR in `kSched2` around an EXISTING
  // `CALL_INSN` (or if the block already has a call)!
  if (phase == Phase::kSched1) {
    // Only inject a `CALL_INSN` at a position where no caller-saved hard reg
    // (incoming args `$a0-$a3`/`$f12-$f15`, existing call args, or return value
    // `$v0/$v1/$f0/$f2`) is live across the call. If the block already has a
    // `CALL_INSN`, inject pre/post-call pseudos around the existing call
    // (guaranteed safe for all caller-saved hard regs!).
    const RtlBasicBlock& target_blk = cfg->rpo_blocks[b];
    std::optional<size_t> existing_call;
    for (size_t i = 0; i < target_blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(target_blk.instructions[i]);
      if (parsed.ok() && parsed->pattern.IsCallPattern()) {
        existing_call = i;
        break;
      }
    }
    const int p_cross = NextFreshPseudo(*cfg);
    const int p_local = p_cross + 1;
    const std::string rc = FormatRegAtom(RtlMachineMode::kSi, p_cross);
    const std::string rl = FormatRegAtom(RtlMachineMode::kSi, p_local);
    if (existing_call.has_value()) {
      size_t ci = *existing_call;
      RtlBasicBlock& mut_blk = cfg->rpo_blocks[b];
      // Insert pre-call `p_cross` + `p_local` (`reg_n_calls_crossed == 0`) +
      // `$sp` store before any argument setup leading into `ci`, and post-call
      // store of `p_cross` (`reg_n_calls_crossed >= 1`) immediately after `ci`.
      std::vector<RtlInsn> pre_snip;
      pre_snip.push_back(
          MakeRtlInsn(absl::StrCat("(set ", rc, " (mem:SI (symbol_ref:SI (\"__fuzz_m6_in\"))))")));
      pre_snip.push_back(
          MakeRtlInsn(absl::StrCat("(set ", rl, " (plus:SI ", rc, " (const_int 9)))")));
      pre_snip.push_back(MakeRtlInsn(
          absl::StrCat("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 0))) ", rl, ")",
                       (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", rl, " (nil))") : ""))));
      RtlInsn post_insn = MakeRtlInsn(
          absl::StrCat("(set (mem:SI (symbol_ref:SI (\"__fuzz_m6_out\"))) ", rc, ")",
                       (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", rc, " (nil))") : "")));
      InsertInsnsAt(&mut_blk, ci + 1, absl::MakeConstSpan(&post_insn, 1));
      InsertInsnsAt(&mut_blk, ci, pre_snip);
      RefreshLivenessPreservingMemDeadNotes(cfg, profile, phase, /*update_reg_notes=*/true);
      return true;
    }

    size_t call_pos = *ins_pos;
    size_t earliest_ok = 0;
    size_t latest_ok = target_blk.instructions.size();
    for (size_t i = 0; i < target_blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(target_blk.instructions[i]);
      if (!parsed.ok()) continue;
      bool reads_caller_hard = false;
      bool sets_ret_or_hard = false;
      parsed->pattern.Visit([&](const RtlExpr& n) {
        if (n.IsHardReg() && mm.CallUsed(n.RegNo()) && n.RegNo() != 29 && n.RegNo() != 28 &&
            n.RegNo() != 0) {
          if (n.flags.integrated || n.RegNo() == 2 || n.RegNo() == 3 || n.RegNo() == 32 ||
              n.RegNo() == 34 || n.RegNo() == 67) {
            sets_ret_or_hard = true;
          } else {
            reads_caller_hard = true;
          }
        }
      });
      if (reads_caller_hard && i + 1 > earliest_ok) earliest_ok = i + 1;
      if (sets_ret_or_hard && i < latest_ok) latest_ok = i;
    }
    if (earliest_ok > latest_ok) {
      return mutated;
    }
    call_pos = std::clamp(call_pos, earliest_ok, latest_ok);
    const bool a0_safe = !BlockHasReg(target_blk, 4);
    const bool add_sched_group_use = ((*rng)() % 2 == 0);

    std::vector<RtlInsn> snippet;
    snippet.push_back(
        MakeRtlInsn(absl::StrCat("(set ", rc, " (mem:SI (symbol_ref:SI (\"__fuzz_m6_in\"))))")));
    snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", rl, " (plus:SI ", rc, " (const_int 9)))")));
    snippet.push_back(MakeRtlInsn(
        absl::StrCat("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 0))) ", rl, ")",
                     (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", rl, " (nil))") : ""))));
    std::vector<RtlExpr> fusage;
    if (a0_safe) {
      snippet.push_back(MakeRtlInsn(absl::StrCat("(set (reg:SI 4 a0) ", rc, ")")));
      if (add_sched_group_use) {
        snippet.push_back(MakeRtlInsn("(use (reg:SI 4 a0))"));
      }
      fusage.push_back(RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)));
    }
    snippet.push_back(MakeRtlInsn(
        "(parallel[ (call (mem:SI (symbol_ref:SI (\"__fuzz_m6_callee\"))) (const_int 16)) "
        "(clobber (reg:SI 31 ra)) ] )",
        std::move(fusage), /*has_explicit_fusage=*/true));
    snippet.push_back(MakeRtlInsn(
        absl::StrCat("(set (mem:SI (symbol_ref:SI (\"__fuzz_m6_out\"))) ", rc, ")",
                     (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", rc, " (nil))") : ""))));

    InsertInsnsAt(&cfg->rpo_blocks[b], call_pos, snippet);
    cfg->sideband.is_leaf_function = false;
    RefreshLivenessPreservingMemDeadNotes(cfg, profile, phase, /*update_reg_notes=*/true);
    mutated = true;
  } else {
    // In `kSched2`: locate an existing `CALL_INSN` to inject pre/post-call
    // callee-saved ops (`sched.c:1704`), `$sp` store, `call_fusage`, or
    // `SCHED_GROUP_P` `(use ...)` before the call; if no call exists, inject
    // independent callee-saved GPR/FPR + `$sp` ops or a `REG_EQUAL` + `REG_DEAD` pair.
    std::optional<std::pair<size_t, size_t>> call_loc;
    for (size_t bi = 0; bi < cfg->rpo_blocks.size() && !call_loc.has_value(); ++bi) {
      const RtlBasicBlock& blk = cfg->rpo_blocks[bi];
      for (size_t ii = 0; ii < blk.instructions.size(); ++ii) {
        auto parsed = ParsePatternAndNotes(blk.instructions[ii]);
        if (parsed.ok() && parsed->pattern.IsCallPattern()) {
          call_loc = {bi, ii};
          break;
        }
      }
    }
    const std::vector<int> free_s = PickFreeCalleeSavedGprs(used, 1);
    const std::vector<int> free_fs = PickFreeCalleeSavedFprs(used, 1);
    const std::vector<int> free_t = PickFreeCallerSavedGprs(used, 1);

    if (call_loc.has_value()) {
      auto [cb, ci] = *call_loc;
      RtlBasicBlock& cblk = cfg->rpo_blocks[cb];
      // Optionally add `(use (reg:SI 4 a0))` before the call if `call_fusage`
      // uses `$a0` (exercises `SCHED_GROUP_P` double `INSN_REF_COUNT`!).
      bool has_a0_fusage = false;
      if (ci < cblk.rtl_insns.size()) {
        for (const RtlExpr& fu : cblk.rtl_insns[ci].call_fusage) {
          if (fu.ContainsReg(4)) has_a0_fusage = true;
        }
      }
      if (has_a0_fusage && ((*rng)() % 2 == 0)) {
        RtlInsn use_a0 = MakeRtlInsn("(use (reg:SI 4 a0))");
        InsertInsnsAt(&cblk, ci, absl::MakeConstSpan(&use_a0, 1));
        ++ci;
        mutated = true;
      }
      // Inject pre-call + post-call callee-saved GPR/FPR ops exercising `sched.c:1704`!
      if (!free_s.empty()) {
        const std::string rs = FormatRegAtom(RtlMachineMode::kSi, free_s[0]);
        RtlInsn pre =
            MakeRtlInsn(absl::StrCat("(set ", rs, " (mem:SI (symbol_ref:SI (\"__fuzz_m6_s0\"))))"));
        RtlInsn post = MakeRtlInsn(
            absl::StrCat("(set (mem:SI (symbol_ref:SI (\"__fuzz_m6_s1\"))) ", rs, ")",
                         (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", rs, " (nil))") : "")));
        InsertInsnsAt(&cblk, ci + 1, absl::MakeConstSpan(&post, 1));
        InsertInsnsAt(&cblk, ci, absl::MakeConstSpan(&pre, 1));
        mutated = true;
      } else if (!free_fs.empty()) {
        const std::string rfs = FormatRegAtom(RtlMachineMode::kSf, free_fs[0]);
        RtlInsn pre = MakeRtlInsn(
            absl::StrCat("(set ", rfs, " (mem:SF (symbol_ref:SI (\"__fuzz_m6_fs0\"))))"));
        RtlInsn post = MakeRtlInsn(
            absl::StrCat("(set (mem:SF (symbol_ref:SI (\"__fuzz_m6_fs1\"))) ", rfs, ")",
                         (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", rfs, " (nil))") : "")));
        InsertInsnsAt(&cblk, ci + 1, absl::MakeConstSpan(&post, 1));
        InsertInsnsAt(&cblk, ci, absl::MakeConstSpan(&pre, 1));
        mutated = true;
      }
    } else if (!free_t.empty()) {
      // No call in this `kSched2` function: inject a `REG_EQUAL` + `REG_DEAD`
      // ordered snippet (`REG_EQUAL` before vs after `REG_DEAD`) with `$sp` load.
      const std::string rt = FormatRegAtom(RtlMachineMode::kSi, free_t[0]);
      std::vector<RtlInsn> snippet;
      snippet.push_back(MakeRtlInsn(absl::StrCat("(set ", rt, " (const_int 123))")));
      const std::string note_str =
          notes_on ? (((*rng)() % 2 == 0)
                          ? absl::StrCat(" (expr_list:REG_EQUAL (const_int 124) "
                                         "(expr_list:REG_DEAD ",
                                         rt, " (nil)))")
                          : absl::StrCat(" (expr_list:REG_DEAD ", rt,
                                         " (expr_list:REG_EQUAL (const_int 124) (nil)))"))
                   : "";
      snippet.push_back(
          MakeRtlInsn(absl::StrCat("(set ", rt, " (plus:SI ", rt, " (const_int 1)))", note_str)));
      snippet.push_back(MakeRtlInsn(
          absl::StrCat("(set (mem:SI (symbol_ref:SI (\"__fuzz_m6_eq\"))) ", rt, ")",
                       (notes_on ? absl::StrCat(" (expr_list:REG_DEAD ", rt, " (nil))") : ""))));
      InsertInsnsAt(&cfg->rpo_blocks[b], *ins_pos, snippet);
      mutated = true;
    }
  }
  return mutated;
}

struct MutationOutcome {
  RtlControlFlowGraph mutated_cfg;
  CompilationProfile mutated_profile;
  std::vector<MutationFamily> applied_families;
};

MutationOutcome ApplyMutationPlan(const RtlControlFlowGraph& base_cfg,
                                  const CompilationProfile& base_profile, const SeedPlan& plan) {
  MutationOutcome out;
  out.mutated_cfg = base_cfg;
  out.mutated_profile = base_profile;
  std::mt19937_64 rng(kSeedSalt ^ (static_cast<uint64_t>(plan.seed) * 0x9E3779B97F4A7C15ULL));

  for (MutationFamily fam : plan.families) {
    bool did = false;
    switch (fam) {
      case MutationFamily::kM1TopoPerm:
        did = ApplyMutationM1TopoPerm(&out.mutated_cfg, out.mutated_profile, plan.phase, &rng);
        break;
      case MutationFamily::kM2AlphaRename:
        did = ApplyMutationM2AlphaRename(&out.mutated_cfg, out.mutated_profile, plan.phase, &rng);
        break;
      case MutationFamily::kM3ConstLargeInt:
        did = ApplyMutationM3ConstLargeInt(&out.mutated_cfg, out.mutated_profile, plan.phase, &rng);
        break;
      case MutationFamily::kM4UnitAndCpu:
        did = ApplyMutationM4UnitAndCpu(&out.mutated_cfg, &out.mutated_profile, plan.phase, &rng);
        break;
      case MutationFamily::kM5MemAliasFlush:
        did = ApplyMutationM5MemAliasFlush(&out.mutated_cfg, out.mutated_profile, plan.phase, &rng);
        break;
      case MutationFamily::kM6CallBoundary:
        did = ApplyMutationM6CallBoundary(&out.mutated_cfg, out.mutated_profile, plan.phase, &rng);
        break;
    }
    if (did) {
      out.applied_families.push_back(fam);
    }
  }
  return out;
}

// =============================================================================
// 5. Oracles F4-A (Forward Bit-Exactness), F4-B (Right-Inverse Law),
//    and F4-C (Greedy Line-Deletion Shrinker)
// =============================================================================

struct OracleFailure {
  bool failed = false;
  std::string oracle_name;
  std::string detail;
};

absl::btree_map<int, int> FilterPositivePseudos(const absl::flat_hash_map<int, int>& m) {
  absl::btree_map<int, int> out;
  for (const auto& [r, v] : m) {
    if (r >= kMipsFirstPseudoRegister && v > 0) {
      out[r] = v;
    }
  }
  return out;
}

RtlControlFlowGraph RunGccForwardSchedule(const RtlControlFlowGraph& pre_cfg,
                                          const CompilationProfile& profile, Phase phase,
                                          GccContext* ctx, FunctionHooks* hooks,
                                          const FuzzSchedInverseProvider& provider) {
  const ParsedCompilerFlags flags = ParseProfileFlags(profile);
  const bool active = (phase == Phase::kSched1) ? flags.RunsSched1() : flags.RunsSched2();
  const RtlPassKind target_pass =
      (phase == Phase::kSched1) ? RtlPassKind::kSched : RtlPassKind::kSched2;

  ctx->BeginFunction("fuzz_forward");
  hooks->ApplyCompilationProfile(profile);
  const RightInverseVerifier verifier(ctx, hooks, &provider);
  RtlControlFlowGraph gcc_out;
  // Delegate through `VerifyRtlPassRightInverse` with identity pre-image so
  // `rtx_equal_function_value_matters = 0` (`toplev.c:2779`,
  // `right_inverse_verifier.cc:341`) and materialization match GCC exactly.
  (void)verifier.VerifyRtlPassRightInverse(
      pre_cfg, [](const RtlControlFlowGraph& x) { return x; },
      [active, target_pass, &gcc_out](GccContext* c, FunctionHooks* h) {
        if (active) {
          h->RunScheduleInsns();
        }
        gcc_out = c->DematerializeToCfg(c->GetInsns(), target_pass);
      },
      target_pass);
  return gcc_out;
}

OracleFailure EvaluateOracles(const RtlControlFlowGraph& y_prime, const CompilationProfile& profile,
                              Phase phase, GccContext* ctx, FunctionHooks* hooks,
                              const FuzzSchedInverseProvider& provider) {
  const ParsedCompilerFlags flags = ParseProfileFlags(profile);
  const bool active = (phase == Phase::kSched1) ? flags.RunsSched1() : flags.RunsSched2();

  // Oracle F4-A: `sched_model::RunScheduleInsns(y', profile, phase)` equals
  // real GCC `FunctionHooks::RunScheduleInsns(y')`.
  const RtlControlFlowGraph gcc_out =
      RunGccForwardSchedule(y_prime, profile, phase, ctx, hooks, provider);
  const RtlControlFlowGraph model_out = RunScheduleInsns(y_prime, profile, phase);
  const RtlCfgPassComparison cmp = model_out.CompareAgainst(gcc_out);
  if (!cmp.MatchesExactly()) {
    return {.failed = true,
            .oracle_name = "Oracle F4-A (Forward Bit-Exactness)",
            .detail = absl::StrCat(cmp.mismatch_detail, "\nmodel:\n", model_out.FormatSummary(),
                                   "\ngcc:\n", gcc_out.FormatSummary())};
  }
  if (phase == Phase::kSched1 && active) {
    if (FilterPositivePseudos(model_out.sideband.reg_n_sets) !=
        FilterPositivePseudos(gcc_out.sideband.reg_n_sets)) {
      return {.failed = true,
              .oracle_name = "Oracle F4-A (kSched1 reg_n_sets Sideband)",
              .detail = absl::StrCat("reg_n_sets mismatch\ngcc:\n", gcc_out.FormatSummary())};
    }
    if (FilterPositivePseudos(model_out.sideband.reg_live_length) !=
        FilterPositivePseudos(gcc_out.sideband.reg_live_length)) {
      return {.failed = true,
              .oracle_name = "Oracle F4-A (kSched1 reg_live_length Sideband)",
              .detail = absl::StrCat("reg_live_length mismatch\ngcc:\n", gcc_out.FormatSummary())};
    }
    if (FilterPositivePseudos(model_out.sideband.reg_n_calls_crossed) !=
        FilterPositivePseudos(gcc_out.sideband.reg_n_calls_crossed)) {
      return {
          .failed = true,
          .oracle_name = "Oracle F4-A (kSched1 reg_n_calls_crossed Sideband)",
          .detail = absl::StrCat("reg_n_calls_crossed mismatch\ngcc:\n", gcc_out.FormatSummary())};
    }
  }

  // Oracle F4-B: `RightInverseVerifier` passes bit-for-bit on `x' = F_GCC(y')`.
  ctx->BeginFunction("fuzz_right_inverse");
  hooks->ApplyCompilationProfile(profile);
  const RightInverseVerifier verifier(ctx, hooks, &provider);
  if (phase == Phase::kSched1) {
    const RightInverseResult res = verifier.VerifySched1StageRightInverse(gcc_out);
    if (!res.matches) {
      return {.failed = true,
              .oracle_name = "Oracle F4-B (VerifySched1StageRightInverse)",
              .detail = res.diff_detail};
    }
  } else {
    const RightInverseResult res = verifier.VerifySched2StageRightInverse(gcc_out);
    if (!res.matches) {
      return {.failed = true,
              .oracle_name = "Oracle F4-B (VerifySched2StageRightInverse)",
              .detail = res.diff_detail};
    }
  }
  return {};
}

// Oracle F4-C: Greedy line-deletion shrinker emitting a minimal reproducer
// (typically 3-8 insns) whenever any failure occurs.
RtlControlFlowGraph GreedyShrinkFailingCfg(
    const RtlControlFlowGraph& failing_y, const CompilationProfile& profile, Phase phase,
    const std::function<bool(const RtlControlFlowGraph&)>& still_fails, int max_passes = 6,
    int max_checks = 96) {
  RtlControlFlowGraph best = failing_y;
  int checks = 0;

  // Phase 1: If multi-block, try collapsing to block 0 alone when that reproduces.
  if (best.rpo_blocks.size() > 1 && checks < max_checks) {
    RtlControlFlowGraph single = best;
    single.rpo_blocks.resize(1);
    single.rpo_blocks[0].successors.clear();
    single.rpo_blocks[0].predecessors.clear();
    if (!single.rpo_blocks[0].instructions.empty()) {
      auto parsed = ParsePatternAndNotes(single.rpo_blocks[0].instructions.back());
      if (parsed.ok() && parsed->pattern.IsSet() && parsed->pattern.SetDest().op == RtlOp::kPc) {
        single.rpo_blocks[0].instructions.pop_back();
        if (!single.rpo_blocks[0].rtl_insns.empty()) single.rpo_blocks[0].rtl_insns.pop_back();
      }
    }
    RefreshLivenessPreservingMemDeadNotes(&single, profile, phase, /*update_reg_notes=*/true);
    ++checks;
    if (!single.rpo_blocks[0].instructions.empty() && still_fails(single)) {
      best = std::move(single);
    }
  }

  // Phase 2: Coarse chunk deletion (4 insns at a time) then single-line greedy deletion.
  for (size_t chunk : {size_t{4}, size_t{1}}) {
    for (int pass = 0; pass < max_passes && best.TotalInsnCount() > 3 && checks < max_checks;
         ++pass) {
      bool shrunk_this_pass = false;
      for (size_t b = 0; b < best.rpo_blocks.size() && checks < max_checks; ++b) {
        size_t rev = best.rpo_blocks[b].instructions.size();
        while (rev > 0 && checks < max_checks) {
          const size_t del_count = std::min(chunk, rev);
          const size_t start = rev - del_count;
          rev = start;
          if (best.rpo_blocks[b].instructions.size() <= del_count) continue;
          bool touches_jump = false;
          if (best.rpo_blocks.size() > 1) {
            for (size_t k = start; k < start + del_count; ++k) {
              auto parsed = ParsePatternAndNotes(best.rpo_blocks[b].instructions[k]);
              if (parsed.ok() && parsed->pattern.IsSet() &&
                  parsed->pattern.SetDest().op == RtlOp::kPc) {
                touches_jump = true;
                break;
              }
            }
          }
          if (touches_jump) continue;
          RtlControlFlowGraph cand = best;
          cand.rpo_blocks[b].instructions.erase(
              cand.rpo_blocks[b].instructions.begin() + static_cast<std::ptrdiff_t>(start),
              cand.rpo_blocks[b].instructions.begin() +
                  static_cast<std::ptrdiff_t>(start + del_count));
          if (start + del_count <= cand.rpo_blocks[b].rtl_insns.size()) {
            cand.rpo_blocks[b].rtl_insns.erase(
                cand.rpo_blocks[b].rtl_insns.begin() + static_cast<std::ptrdiff_t>(start),
                cand.rpo_blocks[b].rtl_insns.begin() +
                    static_cast<std::ptrdiff_t>(start + del_count));
          }
          if (phase == Phase::kSched1) {
            RefreshLivenessPreservingMemDeadNotes(&cand, profile, phase, /*update_reg_notes=*/true);
            // Skip candidates with dead/unpaired `(set (subreg:SI (reg:DI p) ...) (const_int ...))`
            // splits, which trip GCC `sched.c:update_flow_info` `abort()` on dead subreg defs.
            bool has_dead_subreg = false;
            for (const RtlBasicBlock& cb : cand.rpo_blocks) {
              for (const std::string& raw : cb.instructions) {
                auto p = ParsePatternAndNotes(raw);
                if (!p.ok() || !p->pattern.IsSet() || !p->pattern.SetDest().IsSubreg()) continue;
                for (const RtlRegNote& rn : p->reg_notes) {
                  if (rn.kind == RtlMachineMode::kRegUnused) {
                    has_dead_subreg = true;
                    break;
                  }
                }
                if (has_dead_subreg) break;
              }
              if (has_dead_subreg) break;
            }
            if (has_dead_subreg) continue;
          }
          ++checks;
          if (still_fails(cand)) {
            best = std::move(cand);
            shrunk_this_pass = true;
            if (best.TotalInsnCount() <= 3) break;
          }
        }
      }
      if (!shrunk_this_pass) break;
    }
  }
  return best;
}

// =============================================================================
// 6. Test Fixtures & Entrypoints (`seed = 0..999` + Non-Vacuity + Shrinker Test)
// =============================================================================

class SchedModelFuzzTest : public ::testing::TestWithParam<int> {
 protected:
  static void SetUpTestSuite() {
    trace_ctx = new GccContext();
    trace_hooks = new FunctionHooks(trace_ctx);
    verify_ctx = new GccContext();
    verify_hooks = new FunctionHooks(verify_ctx);
    provider = new FuzzSchedInverseProvider();
  }

  static void TearDownTestSuite() {
    delete provider;
    provider = nullptr;
    delete verify_hooks;
    verify_hooks = nullptr;
    delete verify_ctx;
    verify_ctx = nullptr;
    delete trace_hooks;
    trace_hooks = nullptr;
    delete trace_ctx;
    trace_ctx = nullptr;
  }

  static GccContext* trace_ctx;
  static FunctionHooks* trace_hooks;
  static GccContext* verify_ctx;
  static FunctionHooks* verify_hooks;
  static FuzzSchedInverseProvider* provider;
};

GccContext* SchedModelFuzzTest::trace_ctx = nullptr;
FunctionHooks* SchedModelFuzzTest::trace_hooks = nullptr;
GccContext* SchedModelFuzzTest::verify_ctx = nullptr;
FunctionHooks* SchedModelFuzzTest::verify_hooks = nullptr;
FuzzSchedInverseProvider* SchedModelFuzzTest::provider = nullptr;

TEST_P(SchedModelFuzzTest, SeededDifferentialAndRightInverseFuzz) {
  const int seed = GetParam();
  const SeedPlan plan = BuildSeedPlan(seed);
  const CachedSeedSnapshot& snap = GetCachedSeedSnapshot(plan.pool_idx, trace_ctx, trace_hooks);
  ASSERT_TRUE(snap.valid) << "Failed to capture seed snapshot for pool_idx=" << plan.pool_idx
                          << " (" << snap.name << ")";

  const RtlControlFlowGraph& base_cfg =
      (plan.phase == Phase::kSched1) ? snap.sched1_pre : snap.sched2_pre;
  const MutationOutcome outcome = ApplyMutationPlan(base_cfg, snap.base_profile, plan);

  // Validate legality via real GCC `recog` before invoking oracles.
  verify_ctx->BeginFunction("fuzz_recog_check");
  verify_hooks->ApplyCompilationProfile(outcome.mutated_profile);
  regclass_init();
  verify_ctx->MaterializeFromCfg(outcome.mutated_cfg);
  std::string bad_insn;
  ASSERT_TRUE(verify_hooks->ValidateMaterializedInsnsRecognizable(&bad_insn))
      << "Illegal mutated RTL on seed=" << seed << " (" << snap.name << ", "
      << (plan.phase == Phase::kSched1 ? "kSched1" : "kSched2") << "): " << bad_insn << "\nCFG:\n"
      << outcome.mutated_cfg.FormatSummary();

  const OracleFailure fail = EvaluateOracles(outcome.mutated_cfg, outcome.mutated_profile,
                                             plan.phase, verify_ctx, verify_hooks, *provider);
  if (fail.failed) {
    std::string fams_str;
    for (MutationFamily f : outcome.applied_families) {
      if (!fams_str.empty()) absl::StrAppend(&fams_str, "+");
      absl::StrAppend(&fams_str, MutationFamilyName(f));
    }
    const RtlControlFlowGraph shrunk =
        GreedyShrinkFailingCfg(outcome.mutated_cfg, outcome.mutated_profile, plan.phase,
                               [&](const RtlControlFlowGraph& cand) {
                                 verify_ctx->BeginFunction("fuzz_shrink_check");
                                 verify_hooks->ApplyCompilationProfile(outcome.mutated_profile);
                                 regclass_init();
                                 verify_ctx->MaterializeFromCfg(cand);
                                 if (!verify_hooks->ValidateMaterializedInsnsRecognizable()) {
                                   return false;
                                 }
                                 return EvaluateOracles(cand, outcome.mutated_profile, plan.phase,
                                                        verify_ctx, verify_hooks, *provider)
                                     .failed;
                               });
    FAIL() << fail.oracle_name << " FAILED on seed=" << seed << " (pool_idx=" << plan.pool_idx
           << ", name=" << snap.name
           << ", phase=" << (plan.phase == Phase::kSched1 ? "kSched1" : "kSched2")
           << ", mutations=[" << fams_str << "])\nDetail:\n"
           << fail.detail << "\nShrunk reproducer (" << shrunk.TotalInsnCount() << " insns):\n"
           << shrunk.FormatSummary();
  }
}

INSTANTIATE_TEST_SUITE_P(SchedModelFuzz1000Seeds, SchedModelFuzzTest,
                         ::testing::Range(0, kNumFuzzSeeds),
                         [](const ::testing::TestParamInfo<int>& info) {
                           return absl::StrFormat("Seed%04d", info.param);
                         });

// Non-vacuity test: proves that across `seed = 0..999`, all 25 synthetic
// micro-kernels, all 200 stratified corpus snapshots across all 7 compilation
// profiles, both `kSched1` and `kSched2`, and all 6 mutation families (M1-M6)
// are genuinely selected AND produce non-trivial mutations when executed.
TEST(SchedModelFuzzCoverageTest, ExercisesAllSeedPoolsProfilesPhasesAndMutationFamilies) {
  EXPECT_EQ(GetSyntheticMicroKernels().size(), static_cast<size_t>(kNumMicroKernels));
  const auto& corpus_indices = GetStratifiedCorpusCaseIndices();
  EXPECT_EQ(corpus_indices.size(), static_cast<size_t>(kNumStratifiedCorpusSeeds));

  // Verify all 7 compilation profiles are represented in the 200 corpus seeds.
  const auto& all_cases = GetSharedRoundTripTestCases();
  const auto& profiles = GetRoundTripCompilerFlagProfiles();
  absl::btree_map<size_t, int> profile_counts;
  for (size_t idx : corpus_indices) {
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
    EXPECT_GE(profile_counts[p], 28) << "Profile " << profiles[p].suffix << " under-represented";
  }

  // Verify plan coverage across all 1,000 seeds.
  absl::btree_set<int> micro_hits;
  absl::btree_set<int> corpus_hits;
  int sched1_count = 0;
  int sched2_count = 0;
  absl::btree_map<int, int> family_plan_counts;
  for (int seed = 0; seed < kNumFuzzSeeds; ++seed) {
    const SeedPlan plan = BuildSeedPlan(seed);
    if (plan.pool_idx < kNumMicroKernels) {
      micro_hits.insert(plan.pool_idx);
    } else {
      corpus_hits.insert(plan.pool_idx - kNumMicroKernels);
    }
    if (plan.phase == Phase::kSched1) {
      ++sched1_count;
    } else {
      ++sched2_count;
    }
    for (MutationFamily fam : plan.families) {
      ++family_plan_counts[static_cast<int>(fam)];
    }
  }
  EXPECT_EQ(micro_hits.size(), static_cast<size_t>(kNumMicroKernels));
  EXPECT_EQ(corpus_hits.size(), static_cast<size_t>(kNumStratifiedCorpusSeeds));
  EXPECT_GT(sched1_count, 400);
  EXPECT_GT(sched2_count, 400);
  for (int f = 0; f < kNumMutationFamilies; ++f) {
    EXPECT_GT(family_plan_counts[f], 200) << "Family " << f << " under-represented";
  }

  // Execute all 25 synthetic micro-kernels to verify every one compiles cleanly
  // to non-trivial `.06.combine` and `.09.greg` snapshots, and verify every
  // mutation family M1-M6 genuinely produces non-trivial mutated inputs.
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  for (int k = 0; k < kNumMicroKernels; ++k) {
    const CachedSeedSnapshot& mk = GetCachedSeedSnapshot(k, &ctx, &hooks);
    ASSERT_TRUE(mk.valid) << "Micro-kernel " << k << " (" << mk.name << ") invalid";
    EXPECT_GE(mk.sched1_pre.TotalInsnCount(), 3u) << mk.name;
    EXPECT_GE(mk.sched2_pre.TotalInsnCount(), 3u) << mk.name;
  }
  // Kernel 7 (`Mk08Sched1704CalleeSavedCall`) has calls, ALU, FP, and multiple
  // independent ops so all 6 mutation families apply cleanly on both phases.
  const CachedSeedSnapshot& mk8 = GetCachedSeedSnapshot(/*pool_idx=*/7, &ctx, &hooks);
  ASSERT_TRUE(mk8.valid);
  for (Phase phase : {Phase::kSched1, Phase::kSched2}) {
    const RtlControlFlowGraph& base_cfg =
        (phase == Phase::kSched1) ? mk8.sched1_pre : mk8.sched2_pre;
    for (int f = 0; f < kNumMutationFamilies; ++f) {
      SeedPlan single_plan;
      single_plan.seed = 100 + f * 7 + (phase == Phase::kSched2 ? 1 : 0);
      single_plan.pool_idx = 7;
      single_plan.phase = phase;
      single_plan.families = {static_cast<MutationFamily>(f)};
      const MutationOutcome out = ApplyMutationPlan(base_cfg, mk8.base_profile, single_plan);
      const bool changed = (!out.applied_families.empty()) &&
                           (out.mutated_profile != mk8.base_profile ||
                            !out.mutated_cfg.CompareAgainst(base_cfg).MatchesExactly());
      EXPECT_TRUE(changed) << "Mutation family "
                           << MutationFamilyName(static_cast<MutationFamily>(f))
                           << " was vacuous in "
                           << (phase == Phase::kSched1 ? "kSched1" : "kSched2");
    }
  }
}

// Oracle F4-C self-test: verifies `GreedyShrinkFailingCfg` shrinks a 12-insn
// synthetic failing block down to a minimal 3-insn reproducer.
TEST(SchedModelFuzzShrinkerTest, GreedyShrinkerReducesFailingBlockToMinimalReproducer) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  for (int i = 0; i < 12; ++i) {
    const int reg = 8 + (i % 6);
    if (i == 5) {
      cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 14 t6) (const_int 31337))");
    } else {
      cfg.rpo_blocks[0].AddPatternOnly(
          absl::StrCat("(set ", FormatRegAtom(RtlMachineMode::kSi, reg), " (const_int ", i, "))"));
    }
  }
  const CompilationProfile prof{.optimize_level = 2, .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};
  const RtlControlFlowGraph shrunk =
      GreedyShrinkFailingCfg(cfg, prof, Phase::kSched2, [](const RtlControlFlowGraph& cand) {
        for (const std::string& insn : cand.rpo_blocks[0].instructions) {
          if (insn.find("31337") != std::string::npos) return true;
        }
        return false;
      });
  EXPECT_LE(shrunk.TotalInsnCount(), 3u);
  bool has_trigger = false;
  for (const std::string& insn : shrunk.rpo_blocks[0].instructions) {
    if (insn.find("31337") != std::string::npos) has_trigger = true;
  }
  EXPECT_TRUE(has_trigger);
}

}  // namespace
}  // namespace rom_nom_nom::lifter::sched_model
