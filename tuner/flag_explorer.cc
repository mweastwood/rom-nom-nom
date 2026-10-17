#include "tuner/flag_explorer.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"

namespace rom_nom_nom {

namespace {

bool IsOptLevelFlag(std::string_view flag) {
  return flag == "-O0" || flag == "-O1" || flag == "-O2" || flag == "-O3" || flag == "-Os" ||
         flag == "-O";
}

bool IsAssemblerWaFlag(std::string_view flag) {
  return absl::StartsWith(flag, "-Wa,");
}

std::vector<std::string> ReplaceOptLevel(const std::vector<std::string>& flags,
                                         std::string_view new_opt) {
  std::vector<std::string> out;
  out.reserve(flags.size() + 1);
  bool replaced = false;
  for (const auto& f : flags) {
    if (IsOptLevelFlag(f)) {
      if (!replaced) {
        out.emplace_back(new_opt);
        replaced = true;
      }
    } else {
      out.push_back(f);
    }
  }
  if (!replaced) {
    out.insert(out.begin(), std::string(new_opt));
  }
  return out;
}

std::vector<std::string> ReplaceAssemblerFlag(const std::vector<std::string>& flags,
                                              std::string_view new_wa) {
  std::vector<std::string> out;
  out.reserve(flags.size() + 1);
  bool replaced = false;
  for (const auto& f : flags) {
    if (IsAssemblerWaFlag(f)) {
      if (!replaced) {
        out.emplace_back(new_wa);
        replaced = true;
      }
    } else {
      out.push_back(f);
    }
  }
  if (!replaced) {
    out.emplace_back(new_wa);
  }
  return out;
}

}  // namespace

std::vector<std::vector<std::string>> FlagExplorer::GenerateCandidateFlags(
    const std::vector<std::string>& base_flags, const FlagExplorerOptions& options) {
  std::vector<std::vector<std::string>> candidates;
  absl::flat_hash_set<std::string> seen;

  auto add_candidate = [&](std::vector<std::string> profile) {
    if (options.max_profiles > 0 && candidates.size() >= options.max_profiles) {
      return;
    }
    std::string key = absl::StrJoin(profile, " ");
    if (seen.insert(std::move(key)).second) {
      candidates.push_back(std::move(profile));
    }
  };

  std::vector<std::string> seed = base_flags;
  if (seed.empty()) {
    seed = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  }
  add_candidate(seed);

  if (options.explore_assembler_flags) {
    static constexpr std::string_view kAssemblerVariants[] = {
        "-Wa,-g",
        "-Wa,-O2",
        "-Wa,-O1",
    };
    for (std::string_view wa : kAssemblerVariants) {
      add_candidate(ReplaceAssemblerFlag(seed, wa));
    }
  }

  if (options.explore_opt_levels) {
    static constexpr std::string_view kOptLevels[] = {"-O2", "-O1", "-O0", "-O3"};
    for (std::string_view opt : kOptLevels) {
      auto with_opt = ReplaceOptLevel(seed, opt);
      add_candidate(with_opt);
      if (options.explore_assembler_flags) {
        add_candidate(ReplaceAssemblerFlag(with_opt, "-Wa,-O2"));
      }
    }
  }

  if (options.explore_codegen_flags) {
    static constexpr std::string_view kCodegenFlags[] = {
        "-fno-schedule-insns",  "-fno-delayed-branch", "-fomit-frame-pointer",
        "-fno-strength-reduce", "-fforce-mem",         "-fforce-addr",
        "-fcaller-saves",       "-fcse-follow-jumps",  "-fexpensive-optimizations",
    };
    for (std::string_view cg : kCodegenFlags) {
      std::vector<std::string> with_cg = seed;
      with_cg.emplace_back(cg);
      add_candidate(std::move(with_cg));
    }
  }

  return candidates;
}

std::vector<std::vector<std::string>> FlagExplorer::GenerateCandidateFlagsForModule(
    const GameConfig* config, std::string_view module_name, const FlagExplorerOptions& options) {
  std::vector<std::string> base_flags;
  if (config != nullptr) {
    base_flags = ResolveCFlags(*config, module_name);
  }
  return GenerateCandidateFlags(base_flags, options);
}

}  // namespace rom_nom_nom
