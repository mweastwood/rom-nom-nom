#ifndef CORE_CANDIDATE_COMPILER_H_
#define CORE_CANDIDATE_COMPILER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

// Represents a candidate C function compiled and relocated against the game's symbol table.
struct CompiledCandidateFunction {
  uint32_t section_offset = 0;
  uint32_t size = 0;
  std::vector<uint32_t> patched_words;
  std::vector<Instruction> instructions;
};

struct CandidateCompileOptions {
  std::filesystem::path repo_root;
  std::string game_name;
  const GameConfig* config = nullptr;
  const SymbolIndex* symbols = nullptr;
};

// Compiles a single candidate C source file with the retail toolchain, extracts a target
// function from the resulting ELF object, and applies symbol relocations.
class CandidateCompiler {
 public:
  explicit CandidateCompiler(CandidateCompileOptions options);

  absl::StatusOr<CompiledCandidateFunction> CompileFunction(
      const std::filesystem::path& source_file, std::string_view func_name, uint32_t target_vram,
      const std::vector<std::string>& c_flags_override = {},
      const std::vector<std::filesystem::path>& extra_include_dirs = {}) const;

  absl::StatusOr<CompiledCandidateFunction> CompileCandidateFunction(
      const std::filesystem::path& source_file, std::string_view func_name, uint32_t target_vram,
      const std::vector<std::string>& c_flags_override = {},
      const std::vector<std::filesystem::path>& extra_include_dirs = {}) const {
    return CompileFunction(source_file, func_name, target_vram, c_flags_override,
                           extra_include_dirs);
  }

 private:
  CandidateCompileOptions options_;
};

}  // namespace rom_nom_nom

#endif  // CORE_CANDIDATE_COMPILER_H_
