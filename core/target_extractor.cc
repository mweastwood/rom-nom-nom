#include "core/target_extractor.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/candidate_compiler.h"
#include "core/disassembler.h"
#include "core/endian.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/rom.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

TargetExtractor::TargetExtractor(TargetExtractorOptions options, const GameConfig* config,
                                 const SymbolIndex* symbols)
    : options_(std::move(options)), config_(config), symbols_(symbols) {
  options_.repo_root = FindRepoRoot(options_.repo_root);
}

absl::StatusOr<std::unique_ptr<TargetExtractor>> TargetExtractor::Create(
    TargetExtractorOptions options) {
  options.repo_root = FindRepoRoot(options.repo_root);
  std::unique_ptr<GameConfig> owned_cfg;
  std::unique_ptr<SymbolIndex> owned_syms;

  std::filesystem::path cfg_path =
      !options.config_path.empty()
          ? options.config_path
          : (options.repo_root / "config" / absl::StrCat(options.game_name, ".textproto"));
  if (!std::filesystem::exists(cfg_path)) {
    std::filesystem::path alt_cfg =
        options.repo_root / "configs" / absl::StrCat(options.game_name, ".textproto");
    if (std::filesystem::exists(alt_cfg)) {
      cfg_path = alt_cfg;
    }
  }
  if (std::filesystem::exists(cfg_path)) {
    auto cfg_or = LoadGameConfig(cfg_path);
    if (cfg_or.ok()) {
      owned_cfg = std::make_unique<GameConfig>(std::move(*cfg_or));
    }
  }

  SymbolLoadOptions sym_opts{
      .config = owned_cfg.get(),
      .repo_root = options.repo_root,
      .game_name = options.game_name,
  };
  if (!options.symbols_path.empty() && std::filesystem::exists(options.symbols_path)) {
    sym_opts.symbols_path = options.symbols_path;
  }
  auto syms_or = LoadSymbolIndex(sym_opts);
  if (syms_or.ok() && !syms_or->Empty()) {
    owned_syms = std::make_unique<SymbolIndex>(std::move(*syms_or));
  }

  auto extractor = std::make_unique<TargetExtractor>(options, owned_cfg.get(), owned_syms.get());
  extractor->owned_config_ = std::move(owned_cfg);
  extractor->owned_symbols_ = std::move(owned_syms);
  return extractor;
}

absl::StatusOr<uint32_t> TargetExtractor::ResolveVram(std::string_view func_name_or_addr) const {
  auto resolved = ResolveSymbolOrSynthesizedAddress(func_name_or_addr, symbols_);
  if (resolved.has_value()) {
    return *resolved;
  }

  return absl::NotFoundError(absl::StrFormat(
      "TargetExtractor: Could not resolve symbol or address '%s'", func_name_or_addr));
}

absl::StatusOr<uint32_t> TargetExtractor::VramToRomOffset(uint32_t vram) const {
  if (config_ != nullptr) {
    auto offset = rom_nom_nom::VramToRomOffset(*config_, vram);
    if (offset.has_value()) {
      return *offset;
    }
  }

  // Fallback defaults if config is unavailable
  if (options_.game_name == "harvest-moon-64" && vram >= 0x80025C50) {
    return vram - 0x80025C50 + 0x1050;
  }
  if (options_.game_name == "ogre-battle-64" && vram >= 0x80070C60) {
    return vram - 0x80070C60 + 0x1060;
  }

  return absl::NotFoundError(absl::StrFormat(
      "TargetExtractor: VRAM address 0x%08X does not map to any ROM segment.", vram));
}

absl::StatusOr<TargetFunction> TargetExtractor::ExtractFromRom(
    absl::Span<const uint8_t> rom_bytes, std::string_view func_name_or_addr) const {
  auto vram_or = ResolveVram(func_name_or_addr);
  if (!vram_or.ok()) {
    return vram_or.status();
  }
  uint32_t vram = *vram_or;

  auto rom_offset_or = VramToRomOffset(vram);
  if (!rom_offset_or.ok()) {
    return rom_offset_or.status();
  }
  uint32_t rom_offset = *rom_offset_or;

  if (rom_offset >= rom_bytes.size()) {
    return absl::OutOfRangeError(absl::StrFormat(
        "TargetExtractor: ROM offset 0x%X exceeds ROM size %zu", rom_offset, rom_bytes.size()));
  }

  size_t slice_size = rom_bytes.size() - rom_offset;
  if (config_ != nullptr) {
    for (const auto& seg : ResolveSegments(*config_, rom_bytes.size())) {
      if (rom_offset >= seg.rom_start && rom_offset < seg.rom_end &&
          seg.rom_end <= rom_bytes.size()) {
        slice_size = seg.rom_end - rom_offset;
        break;
      }
    }
  }

  absl::Span<const uint8_t> code_slice = rom_bytes.subspan(rom_offset, slice_size);

  DisassemblerOptions disasm_opts;
  disasm_opts.emit_line_comments = false;
  disasm_opts.emit_function_framing = false;
  disasm_opts.emit_relocations = false;
  Disassembler disasm(symbols_, disasm_opts);
  if (!known_entrypoints_.empty()) {
    disasm.SetKnownEntrypoints(known_entrypoints_);
  }

  auto fn_or = disasm.DisassembleFunction(code_slice, vram);
  if (!fn_or.ok()) {
    return fn_or.status();
  }

  TargetFunction target;
  target.name = std::string(func_name_or_addr);
  target.vram = vram;
  target.rom_offset = rom_offset;
  target.size = fn_or->Size();

  target.raw_words.reserve(fn_or->instructions.size());
  target.instructions.reserve(fn_or->instructions.size());
  target.formatted_instructions.reserve(fn_or->instructions.size());

  for (const auto& inst_line : fn_or->instructions) {
    target.raw_words.push_back(inst_line.raw_word);
    target.instructions.push_back(inst_line.instruction);
    target.formatted_instructions.push_back(inst_line.formatted_asm);
  }

  return target;
}

absl::StatusOr<TargetFunction> TargetExtractor::ExtractFromAsm(std::string_view func_name) const {
  std::vector<std::filesystem::path> search_dirs;
  if (!options_.asm_dir.empty()) {
    search_dirs.push_back(options_.asm_dir);
  }
  search_dirs.push_back(options_.repo_root / "bazel-bin" / "asm" / options_.game_name);
  search_dirs.push_back(options_.repo_root / "asm" / options_.game_name);

  auto find_label = [&](const std::string& content) -> size_t {
    std::string glabel = absl::StrCat("glabel ", func_name);
    size_t pos = content.find(glabel);
    if (pos != std::string::npos) return pos;

    std::string ent = absl::StrCat(".ent ", func_name);
    pos = content.find(ent);
    if (pos != std::string::npos) return pos;

    std::string colon = absl::StrCat(func_name, ":");
    return content.find(colon);
  };

  std::filesystem::path found_file;
  std::string file_content;
  size_t start_pos = std::string::npos;

  for (const auto& dir : search_dirs) {
    if (!std::filesystem::exists(dir)) continue;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".s") {
        std::ifstream ifs(entry.path());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        size_t pos = find_label(content);
        if (pos != std::string::npos) {
          found_file = entry.path();
          file_content = std::move(content);
          start_pos = pos;
          break;
        }
      }
    }
    if (!found_file.empty()) break;
  }

  if (found_file.empty() || start_pos == std::string::npos) {
    return absl::NotFoundError(
        absl::StrFormat("TargetExtractor: Function '%s' not found in ASM files.", func_name));
  }

  // Parse lines between function label and endlabel/.end
  size_t line_start = file_content.find('\n', start_pos);
  if (line_start == std::string::npos) {
    line_start = start_pos;
  } else {
    line_start += 1;
  }

  size_t end_pos = file_content.find("endlabel", line_start);
  if (end_pos == std::string::npos) {
    end_pos = file_content.find(".end ", line_start);
  }
  if (end_pos == std::string::npos) {
    end_pos = file_content.size();
  }

  std::string body = file_content.substr(line_start, end_pos - line_start);
  TargetFunction target;
  target.name = std::string(func_name);

  // Parse lines with /* VRAM RAW_WORD */ formatted_asm
  std::istringstream stream(body);
  std::string line;
  while (std::getline(stream, line)) {
    size_t comment_start = line.find("/*");
    size_t comment_end = line.find("*/");
    if (comment_start != std::string::npos && comment_end != std::string::npos &&
        comment_end > comment_start) {
      std::string comment = line.substr(comment_start + 2, comment_end - (comment_start + 2));
      std::istringstream cstream(comment);
      std::string vram_hex, word_hex;
      cstream >> vram_hex >> word_hex;
      uint32_t pc = 0;
      uint32_t word = 0;
      if (absl::SimpleHexAtoi(vram_hex, &pc) && absl::SimpleHexAtoi(word_hex, &word)) {
        if (target.vram == 0) {
          target.vram = pc;
        }
        target.raw_words.push_back(word);
        auto inst_or = DecodeInstruction(word, pc);
        if (inst_or.ok()) {
          target.instructions.push_back(*inst_or);
        }
        std::string asm_text = line.substr(comment_end + 2);
        // Trim leading and trailing whitespace
        size_t first = asm_text.find_first_not_of(" \t");
        size_t last = asm_text.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && last != std::string::npos) {
          asm_text = asm_text.substr(first, last - first + 1);
        }
        target.formatted_instructions.push_back(std::move(asm_text));
      }
    }
  }

  target.size = target.raw_words.size() * 4;
  return target;
}

absl::StatusOr<CompiledCandidateFunction> TargetExtractor::CompileCandidateFunction(
    const std::filesystem::path& source_file, std::string_view function_name, uint32_t target_vram,
    std::optional<uint32_t> target_rodata_vram) const {
  CandidateCompiler compiler({
      .repo_root = options_.repo_root,
      .game_name = options_.game_name,
      .config = config_,
      .symbols = symbols_,
  });
  return compiler.CompileFunction(source_file, function_name, target_vram, {}, {},
                                  target_rodata_vram);
}

absl::Span<const uint8_t> TargetExtractor::RomData() const {
  if (cached_rom_.has_value()) {
    return cached_rom_->Data();
  }
  return absl::MakeConstSpan(cached_rom_bytes_);
}

void TargetExtractor::EnsureRomLoadedAndPrescanned() const {
  if (cached_rom_.has_value() || !cached_rom_bytes_.empty()) {
    return;
  }
  std::filesystem::path rom_path =
      !options_.rom_path.empty()
          ? options_.rom_path
          : (options_.repo_root / "roms" / absl::StrCat(options_.game_name, ".z64"));
  if (std::filesystem::exists(rom_path)) {
    auto rom_or = Rom::OpenFile(rom_path);
    if (rom_or.ok()) {
      cached_rom_ = std::move(*rom_or);
    } else {
      std::ifstream ifs(rom_path, std::ios::binary | std::ios::ate);
      if (ifs.is_open()) {
        std::streamsize size = ifs.tellg();
        if (size > 0) {
          ifs.seekg(0, std::ios::beg);
          cached_rom_bytes_.resize(static_cast<size_t>(size));
          ifs.read(reinterpret_cast<char*>(cached_rom_bytes_.data()), size);
        }
      }
    }
  }

  absl::Span<const uint8_t> rom_data = RomData();
  if (rom_data.empty()) {
    return;
  }

  if (config_ != nullptr) {
    for (const auto& seg : ResolveSegments(*config_, rom_data.size())) {
      if (seg.type == SEGMENT_CODE && seg.vram_start != 0 && seg.RomSize() > 0 &&
          seg.rom_end <= rom_data.size()) {
        absl::Span<const uint8_t> code_span = rom_data.subspan(seg.rom_start, seg.RomSize());
        Disassembler disasm(symbols_);
        disasm.PreScanCode(code_span, seg.vram_start);
        for (uint32_t ep : disasm.KnownEntrypoints()) {
          known_entrypoints_.insert(ep);
        }
      }
    }
  }
}

absl::StatusOr<TargetFunction> TargetExtractor::ExtractTarget(
    std::string_view func_name_or_addr) const {
  EnsureRomLoadedAndPrescanned();
  absl::Span<const uint8_t> rom_data = RomData();
  if (!rom_data.empty()) {
    auto target_or = ExtractFromRom(rom_data, func_name_or_addr);
    if (target_or.ok()) {
      return target_or;
    }
  }

  return ExtractFromAsm(func_name_or_addr);
}

std::optional<uint32_t> TargetExtractor::ReadRomWord(uint32_t vram) const {
  EnsureRomLoadedAndPrescanned();
  absl::Span<const uint8_t> rom_data = RomData();
  if (rom_data.empty()) {
    return std::nullopt;
  }

  auto offset_or = VramToRomOffset(vram);
  if (!offset_or.ok()) {
    return std::nullopt;
  }
  uint32_t offset = *offset_or;
  if (offset + 4 > rom_data.size()) {
    return std::nullopt;
  }
  return ReadBigEndian32(rom_data.data() + offset);
}

}  // namespace rom_nom_nom
