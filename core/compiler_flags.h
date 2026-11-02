#ifndef ROM_NOM_NOM_CORE_COMPILER_FLAGS_H_
#define ROM_NOM_NOM_CORE_COMPILER_FLAGS_H_

#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"

namespace rom_nom_nom {

// Structured representation of KMC GCC 2.7.2 (`toplev.c`, `config/mips/mips.c`) and KMC GNU `as`
// (`gas/config/tc-mips.c`) command-line flags.
//
// In GCC 2.7.2, `toplev.c:main` initializes `optimize` and per-pass `flag_*` globals from `-O0`,
// `-O1`, `-O2`, `-O3`, then applies explicit `-f...` / `-fno-...` and `-m...` overrides in command-
// line order. Each stage in `InverseCompilerPipeline` inspects `ParsedCompilerFlags` to know which
// forward passes/splits were active when compiling the target function.
struct ParsedCompilerFlags {
  // Primary GCC optimization level (`optimize` in `toplev.c`: 0, 1, 2, or 3).
  int opt_level = 2;

  // Per-pass GCC 2.7.2 flags (`toplev.c`).
  bool flag_delayed_branch = true;
  bool flag_omit_frame_pointer = true;
  bool flag_schedule_insns = true;
  bool flag_schedule_insns_after_reload = true;
  bool flag_rerun_cse_after_loop = true;
  bool flag_cse_follow_jumps = true;
  bool flag_cse_skip_blocks = true;
  bool flag_expensive_optimizations = true;
  bool flag_strength_reduce = true;
  bool flag_thread_jumps = true;
  bool flag_caller_saves = true;
  bool flag_inline_functions = false;
  bool flag_peephole = true;

  // Target MIPS architecture & ABI flags (`config/mips/mips.h`).
  int mips_isa = 3;
  std::string mcpu = "vr4300";
  bool signed_char = true;
  int small_data_threshold = 0;

  // KMC binutils 2.6 assembler (`-Wa,...`) flags.
  int gas_opt_level = 0;
  bool gas_debug = true;

  // Raw command-line flag tokens as parsed.
  std::vector<std::string> raw_tokens;

  // Parses a whitespace-delimited compiler flag string (e.g. "-O2 -Wa,-g -signed-char").
  static ParsedCompilerFlags Parse(std::string_view flags_str);

  // Parses a span of individual compiler flag tokens (e.g. {"-O2", "-Wa,-O2"}).
  static ParsedCompilerFlags Parse(absl::Span<const std::string> flag_tokens);

  // Returns the default baseline N64 compiler configuration (`-O2 -Wa,-g`).
  static ParsedCompilerFlags DefaultO2WaG();

  // Applies the baseline `toplev.c` defaults for `-O<level>`.
  void ApplyOptimizationLevelDefaults(int level);

  // Stage-level queries mirroring `toplev.c:rest_of_compilation` and `sched.c`:

  // True if GCC Stage 12 (`reorg.c:dbr_schedule`, `.12.dbr`) runs (`optimize > 0 &&
  // flag_delayed_branch`).
  bool RunsDbrSchedule() const { return opt_level > 0 && flag_delayed_branch; }

  // True if GCC Stage 7 (`sched.c:schedule_insns` pre-alloc, `.07.sched`) runs.
  bool RunsSched1() const { return opt_level > 0 && flag_schedule_insns; }

  // True if GCC Stage 10 (`sched.c:schedule_insns` post-reload, `.10.sched2`) runs.
  bool RunsSched2() const { return opt_level > 0 && flag_schedule_insns_after_reload; }

  // In GCC 2.7.2, `split_all_insns(0)` (which splits 2-instruction `LARGE_INT` `movsi` constants
  // into `lui + ori` and `mips_move_2words` `DFmode`/`DImode` moves into word pairs) is only
  // invoked inside `schedule_insns` (`sched.c`). When neither `sched1` nor `sched2` runs (e.g. at
  // `-O1` or `-O0`), `split_all_insns` does not run before `dbr_schedule` / `final`.
  bool SplitsMultiInsnMovesBeforeDbr() const { return RunsSched1() || RunsSched2(); }

  // True if GCC Stage 4 (`cse.c:cse_main` post-loop, `.04.cse2`) runs.
  bool RunsCse2() const { return opt_level > 0 && flag_rerun_cse_after_loop; }

  // True if GCC Stage 2/3/6 (`cse1`, `loop`, `combine`) run (`optimize > 0`).
  bool RunsOptimizerPasses() const { return opt_level > 0; }

  // True if KMC GAS (`-Wa,-O2` without `-Wa,-g`) performs post-`final.c` instruction scheduling,
  // `jal` delay-slot filling, and coprocessor `nop` elimination.
  bool GasOptimizesAndReordersInsns() const { return gas_opt_level >= 2 && !gas_debug; }

  // Returns a canonical short profile identifier (e.g. "O0_WaG", "O1_WaG", "O2_WaG", "O2_WaO2",
  // "O3_WaG").
  std::string ProfileKey() const;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_CORE_COMPILER_FLAGS_H_
