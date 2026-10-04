#ifndef DIFFER_TARGET_EXTRACTOR_H_
#define DIFFER_TARGET_EXTRACTOR_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

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

  TargetExtractor(TargetExtractorOptions options, const SplitConfig* config,
                  const SymbolIndex* symbols);

  // Resolves a function name or address label (e.g. "MessageInit", "0x800266C0", "func_XXXXXXXX")
  // to a 32-bit VRAM address.
  absl::StatusOr<uint32_t> ResolveVram(std::string_view func_name_or_addr) const;

  // Converts a VRAM address to a ROM byte offset using SplitConfig segment mappings.
  absl::StatusOr<uint32_t> VramToRomOffset(uint32_t vram) const;

  // Extracts the target function from the ROM file on disk (or ASM fallback).
  absl::StatusOr<TargetFunction> ExtractTarget(std::string_view func_name_or_addr) const;

  // Extracts the target function directly from an in-memory ROM buffer.
  absl::StatusOr<TargetFunction> ExtractFromRom(absl::Span<const uint8_t> rom_bytes,
                                                std::string_view func_name_or_addr) const;

  // Extracts the target function from generated assembly files (bazel-bin/asm or asm).
  absl::StatusOr<TargetFunction> ExtractFromAsm(std::string_view func_name) const;

  // Reads a 32-bit big-endian word from ROM at the specified VRAM address.
  std::optional<uint32_t> ReadRomWord(uint32_t vram) const;

  const SplitConfig* Config() const { return config_; }
  const SymbolIndex* Symbols() const { return symbols_; }

 private:
  TargetExtractorOptions options_;
  const SplitConfig* config_ = nullptr;
  const SymbolIndex* symbols_ = nullptr;
  std::unique_ptr<SplitConfig> owned_config_;
  std::unique_ptr<SymbolIndex> owned_symbols_;
  mutable std::vector<uint8_t> cached_rom_bytes_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_TARGET_EXTRACTOR_H_
