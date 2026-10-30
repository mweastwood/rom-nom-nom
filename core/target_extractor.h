#ifndef CORE_TARGET_EXTRACTOR_H_
#define CORE_TARGET_EXTRACTOR_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/candidate_compiler.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/rom.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

// Represents target function instructions extracted from the original ROM or ASM.
struct TargetFunction {
  std::string name;
  uint32_t vram = 0;
  uint32_t rom_offset = 0;
  uint32_t size = 0;
  std::vector<uint32_t> raw_words;
  std::vector<Instruction> instructions;
  std::vector<std::string> formatted_instructions;
};

struct TargetExtractorOptions {
  std::filesystem::path repo_root;
  std::string game_name;
  std::filesystem::path config_path;
  std::filesystem::path symbols_path;
  std::filesystem::path rom_path;
  std::filesystem::path asm_dir;
};

// Extracts target function machine code and instructions from original retail ROM or assembly.
class TargetExtractor {
 public:
  static absl::StatusOr<std::unique_ptr<TargetExtractor>> Create(TargetExtractorOptions options);

  TargetExtractor(TargetExtractorOptions options, const GameConfig* config,
                  const SymbolIndex* symbols);

  // Resolves a function name or address label (e.g. "MessageInit", "0x800266C0", "func_XXXXXXXX")
  // to a 32-bit VRAM address.
  absl::StatusOr<uint32_t> ResolveVram(std::string_view func_name_or_addr) const;

  // Converts a VRAM address to a ROM byte offset using GameConfig segment mappings.
  absl::StatusOr<uint32_t> VramToRomOffset(uint32_t vram) const;

  // Extracts the target function from the ROM file on disk (or ASM fallback).
  absl::StatusOr<TargetFunction> ExtractTarget(std::string_view func_name_or_addr) const;

  // Extracts the target function directly from an in-memory ROM buffer.
  absl::StatusOr<TargetFunction> ExtractFromRom(absl::Span<const uint8_t> rom_bytes,
                                                std::string_view func_name_or_addr) const;

  // Extracts the target function from generated assembly files (bazel-bin/asm or asm).
  absl::StatusOr<TargetFunction> ExtractFromAsm(std::string_view func_name) const;

  // Compiles a single C source file with the retail toolchain, extracts the named function,
  // and applies relocations against the symbol table.
  absl::StatusOr<CompiledCandidateFunction> CompileCandidateFunction(
      const std::filesystem::path& source_file, std::string_view function_name,
      uint32_t target_vram, std::optional<uint32_t> target_rodata_vram = std::nullopt) const;

  // Reads a 32-bit big-endian word from ROM at the specified VRAM address.
  std::optional<uint32_t> ReadRomWord(uint32_t vram) const;

  // Returns the loaded retail ROM bytes (loading from disk on first call if available).
  absl::Span<const uint8_t> RomBytes() const;

  const TargetExtractorOptions& Options() const { return options_; }
  const GameConfig* Config() const { return config_; }
  const SymbolIndex* Symbols() const { return symbols_; }

 private:
  void EnsureRomLoadedAndPrescanned() const;
  absl::Span<const uint8_t> RomData() const;

  TargetExtractorOptions options_;
  const GameConfig* config_ = nullptr;
  const SymbolIndex* symbols_ = nullptr;
  std::unique_ptr<GameConfig> owned_config_;
  std::unique_ptr<SymbolIndex> owned_symbols_;
  mutable std::optional<Rom> cached_rom_;
  mutable std::vector<uint8_t> cached_rom_bytes_;
  mutable absl::flat_hash_set<uint32_t> known_entrypoints_;
};

}  // namespace rom_nom_nom

#endif  // CORE_TARGET_EXTRACTOR_H_
