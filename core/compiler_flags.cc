#include "core/compiler_flags.h"

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

void ParsedCompilerFlags::ApplyOptimizationLevelDefaults(int level) {
  if (level < 0) level = 0;
  opt_level = level;

  // Mirror GCC 2.7.2 `toplev.c:main` optimization level initialization.
  flag_delayed_branch = (level >= 1);
  flag_omit_frame_pointer = (level >= 1);
  flag_thread_jumps = (level >= 1);

  flag_schedule_insns = (level >= 2);
  flag_schedule_insns_after_reload = (level >= 2);
  flag_rerun_cse_after_loop = (level >= 2);
  flag_cse_follow_jumps = (level >= 2);
  flag_cse_skip_blocks = (level >= 2);
  flag_expensive_optimizations = (level >= 2);
  flag_strength_reduce = (level >= 2);
  flag_caller_saves = (level >= 2);

  flag_inline_functions = (level >= 3);
}

ParsedCompilerFlags ParsedCompilerFlags::DefaultO2WaG() {
  return Parse("-O2 -Wa,-g");
}

ParsedCompilerFlags ParsedCompilerFlags::Parse(std::string_view flags_str) {
  std::vector<std::string> tokens;
  for (std::string_view part : absl::StrSplit(flags_str, ' ', absl::SkipEmpty())) {
    tokens.emplace_back(part);
  }
  return Parse(absl::MakeConstSpan(tokens));
}

ParsedCompilerFlags ParsedCompilerFlags::Parse(absl::Span<const std::string> flag_tokens) {
  ParsedCompilerFlags flags;
  flags.ApplyOptimizationLevelDefaults(0);
  flags.raw_tokens.assign(flag_tokens.begin(), flag_tokens.end());

  // Pass 1: Determine final `-O<level>` just as `toplev.c` scans `-O` flags before `-f` flags.
  int detected_opt_level = 0;
  bool has_explicit_opt = false;
  for (const std::string& token : flag_tokens) {
    if (token == "-O" || token == "-O1") {
      detected_opt_level = 1;
      has_explicit_opt = true;
    } else if (token == "-O0") {
      detected_opt_level = 0;
      has_explicit_opt = true;
    } else if (token == "-O2") {
      detected_opt_level = 2;
      has_explicit_opt = true;
    } else if (token == "-O3") {
      detected_opt_level = 3;
      has_explicit_opt = true;
    }
  }
  flags.ApplyOptimizationLevelDefaults(has_explicit_opt ? detected_opt_level : 2);
  if (!has_explicit_opt || detected_opt_level == 0) {
    flags.gas_opt_level = 0;
    flags.gas_debug = true;
  }

  // Pass 2: Apply explicit `-f`, `-m`, `-G`, and `-Wa,` flags in command-line order.
  bool saw_wa_flag = false;
  for (size_t i = 0; i < flag_tokens.size(); ++i) {
    const std::string& token = flag_tokens[i];
    if (token == "-fdelayed-branch") {
      flags.flag_delayed_branch = true;
    } else if (token == "-fno-delayed-branch") {
      flags.flag_delayed_branch = false;
    } else if (token == "-fomit-frame-pointer") {
      flags.flag_omit_frame_pointer = true;
    } else if (token == "-fno-omit-frame-pointer" || token == "-fkeep-frame-pointer") {
      flags.flag_omit_frame_pointer = false;
    } else if (token == "-fschedule-insns") {
      flags.flag_schedule_insns = true;
    } else if (token == "-fno-schedule-insns") {
      flags.flag_schedule_insns = false;
    } else if (token == "-fschedule-insns2") {
      flags.flag_schedule_insns_after_reload = true;
    } else if (token == "-fno-schedule-insns2") {
      flags.flag_schedule_insns_after_reload = false;
    } else if (token == "-frerun-cse-after-loop") {
      flags.flag_rerun_cse_after_loop = true;
    } else if (token == "-fno-rerun-cse-after-loop") {
      flags.flag_rerun_cse_after_loop = false;
    } else if (token == "-fcse-follow-jumps") {
      flags.flag_cse_follow_jumps = true;
    } else if (token == "-fno-cse-follow-jumps") {
      flags.flag_cse_follow_jumps = false;
    } else if (token == "-fcse-skip-blocks") {
      flags.flag_cse_skip_blocks = true;
    } else if (token == "-fno-cse-skip-blocks") {
      flags.flag_cse_skip_blocks = false;
    } else if (token == "-fexpensive-optimizations") {
      flags.flag_expensive_optimizations = true;
    } else if (token == "-fno-expensive-optimizations") {
      flags.flag_expensive_optimizations = false;
    } else if (token == "-fstrength-reduce") {
      flags.flag_strength_reduce = true;
    } else if (token == "-fno-strength-reduce") {
      flags.flag_strength_reduce = false;
    } else if (token == "-fthread-jumps") {
      flags.flag_thread_jumps = true;
    } else if (token == "-fno-thread-jumps") {
      flags.flag_thread_jumps = false;
    } else if (token == "-fcaller-saves") {
      flags.flag_caller_saves = true;
    } else if (token == "-fno-caller-saves") {
      flags.flag_caller_saves = false;
    } else if (token == "-finline-functions") {
      flags.flag_inline_functions = true;
    } else if (token == "-fno-inline-functions") {
      flags.flag_inline_functions = false;
    } else if (token == "-fpeephole") {
      flags.flag_peephole = true;
    } else if (token == "-fno-peephole") {
      flags.flag_peephole = false;
    } else if (token == "-signed-char" || token == "-fsigned-char") {
      flags.signed_char = true;
    } else if (token == "-unsigned-char" || token == "-funsigned-char") {
      flags.signed_char = false;
    } else if (token == "-mips1") {
      flags.mips_isa = 1;
    } else if (token == "-mips2") {
      flags.mips_isa = 2;
    } else if (token == "-mips3") {
      flags.mips_isa = 3;
    } else if (absl::StartsWith(token, "-mcpu=")) {
      flags.mcpu = token.substr(6);
    } else if (token == "-G" && i + 1 < flag_tokens.size()) {
      int val = 0;
      const std::string& next = flag_tokens[++i];
      std::from_chars(next.data(), next.data() + next.size(), val);
      flags.small_data_threshold = val;
    } else if (absl::StartsWith(token, "-G") && token.size() > 2) {
      int val = 0;
      const std::string_view token_sv = token;
      const std::string_view rest = token_sv.substr(2);
      std::from_chars(rest.data(), rest.data() + rest.size(), val);
      flags.small_data_threshold = val;
    } else if (absl::StartsWith(token, "-Wa,")) {
      saw_wa_flag = true;
      const std::string_view token_sv = token;
      const std::string_view rest = token_sv.substr(4);
      for (std::string_view sub : absl::StrSplit(rest, ',', absl::SkipEmpty())) {
        if (sub == "-g") {
          flags.gas_debug = true;
        } else if (sub == "-O0") {
          flags.gas_opt_level = 0;
        } else if (sub == "-O" || sub == "-O1") {
          flags.gas_opt_level = 1;
          flags.gas_debug = false;
        } else if (sub == "-O2") {
          flags.gas_opt_level = 2;
          flags.gas_debug = false;
        } else if (sub == "-O3") {
          flags.gas_opt_level = 3;
          flags.gas_debug = false;
        }
      }
    }
  }
  if (!saw_wa_flag && flags.opt_level == 0) {
    flags.gas_opt_level = 0;
    flags.gas_debug = true;
  }
  return flags;
}

std::string ParsedCompilerFlags::ProfileKey() const {
  std::string opt_part = absl::StrCat("O", opt_level);
  std::string wa_part = GasOptimizesAndReordersInsns() ? absl::StrCat("WaO", gas_opt_level) : "WaG";
  return absl::StrCat(opt_part, "_", wa_part);
}

}  // namespace rom_nom_nom
