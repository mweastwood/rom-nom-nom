#ifndef ROM_NOM_NOM_BUILDER_ROM_BUILDER_H_
#define ROM_NOM_NOM_BUILDER_ROM_BUILDER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "builder/linker_script_adapter.h"
#include "builder/source_resolver.h"
#include "core/compiler.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

// Configuration options for the complete ROM build pipeline.
struct RomBuilderOptions {
  std::string game_name;
  std::filesystem::path config_path;
  std::filesystem::path symbols_path;
  std::filesystem::path asm_dir;
  std::filesystem::path build_dir;
  std::filesystem::path assets_dir;
  std::filesystem::path out_elf;
  std::filesystem::path out_rom;
  ToolchainMode toolchain_mode = ToolchainMode::kOriginal;
  std::filesystem::path verify_rom;  // Optional retail ROM to verify bit-exact against
  bool is_test = false;              // Fails with error status if bit-exact match fails
};

// Summary metrics and verification output of a completed ROM build.
struct RomBuildResult {
  bool success = false;
  std::string built_sha1;
  std::string expected_sha1;
  bool sha1_matches = false;
  bool byte_matches = false;
  size_t differing_bytes = 0;
  size_t total_objects = 0;
};

// Computes the lowercase 40-character SHA-1 hex digest of a file.
absl::StatusOr<std::string> ComputeFileSha1(const std::filesystem::path& path);

// Coordinates compilation, linking, extraction, and verification of an N64 ROM.
class RomBuilder {
 public:
  explicit RomBuilder(Toolchain toolchain, RomBuilderOptions options);
  RomBuilder(Toolchain toolchain, RomBuilderOptions options, Compiler compiler);

  const RomBuilderOptions& Options() const { return options_; }
  const Toolchain& GetToolchain() const { return toolchain_; }
  const Compiler& GetCompiler() const { return compiler_; }

  // Executes the complete build pipeline:
  // 1. Loads SplitConfig (.textproto) for c_flags and expected SHA-1.
  // 2. Prepares isolated object working directory (_<game>_objs).
  // 3. Obtains symbols.ld and discovers auxiliary linker scripts.
  // 4. Adapts and rewrites main linker script to point to isolated objects.
  // 5. Resolves and compiles all referenced objects via Compiler and SourceResolver.
  // 6. Links the final ELF.
  // 7. Extracts the raw ROM binary.
  // 8. Performs SHA-1 and bit-exact retail ROM verification.
  absl::StatusOr<RomBuildResult> Build() const;

 private:
  Toolchain toolchain_;
  RomBuilderOptions options_;
  Compiler compiler_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_BUILDER_ROM_BUILDER_H_
