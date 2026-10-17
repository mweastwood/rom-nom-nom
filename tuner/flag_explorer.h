#ifndef TUNER_FLAG_EXPLORER_H_
#define TUNER_FLAG_EXPLORER_H_

#include <string>
#include <string_view>
#include <vector>

#include "core/game_config.h"

namespace rom_nom_nom {

struct FlagExplorerOptions {
  // Include GCC optimization levels (-O0, -O1, -O2, -O3).
  bool explore_opt_levels = true;
  // Include KMC assembler optimization/debug flag combinations (-Wa,-g, -Wa,-O1, -Wa,-O2, -V).
  bool explore_assembler_flags = true;
  // Include GCC 2.7.2 codegen flags (-fomit-frame-pointer, -fno-strength-reduce, etc.).
  bool explore_codegen_flags = true;
  // Maximum number of candidate flag profiles to generate (0 = unlimited).
  size_t max_profiles = 24;
};

// Generates prioritized, deduplicated compiler and assembler flag profiles for N64 GCC 2.7.2 / KMC
// AS tuning.
class FlagExplorer {
 public:
  // Generates candidate flag sets starting from `base_flags` (which is always the first entry).
  static std::vector<std::vector<std::string>> GenerateCandidateFlags(
      const std::vector<std::string>& base_flags, const FlagExplorerOptions& options = {});

  // Convenience overload resolving `base_flags` from `GameConfig` for `module_name`.
  static std::vector<std::vector<std::string>> GenerateCandidateFlagsForModule(
      const GameConfig* config, std::string_view module_name,
      const FlagExplorerOptions& options = {});
};

}  // namespace rom_nom_nom

#endif  // TUNER_FLAG_EXPLORER_H_
