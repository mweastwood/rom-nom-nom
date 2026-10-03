#include "differ/target_extractor.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"
#include "core/endian.h"
#include "core/mips.h"
#include "splitter/config.h"
#include "splitter/disassembler.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

TargetExtractor::TargetExtractor(TargetExtractorOptions options, const SplitConfig* config,
                                 const SymbolIndex* symbols)
    : options_(std::move(options)), config_(config), symbols_(symbols) {}

absl::StatusOr<std::unique_ptr<TargetExtractor>> TargetExtractor::Create(
    TargetExtractorOptions options) {
  std::unique_ptr<SplitConfig> owned_cfg;
  std::unique_ptr<SymbolIndex> owned_syms;

  std::filesystem::path cfg_path =
      !options.config_path.empty()
          ? options.config_path
          : (options.repo_root / "config" / absl::StrCat(options.game_name, ".textproto"));
  if (std::filesystem::exists(cfg_path)) {
    auto cfg_or = LoadSplitConfig(cfg_path);
    if (cfg_or.ok()) {
      owned_cfg = std::make_unique<SplitConfig>(std::move(*cfg_or));
    }
  }

  std::filesystem::path sym_proto_path =
      !options.symbols_path.empty()
          ? options.symbols_path
          : (options.repo_root / "symbols" / absl::StrCat(options.game_name, ".textproto"));
  if (std::filesystem::exists(sym_proto_path)) {
    auto syms_or = SymbolIndex::LoadFromTextproto(sym_proto_path);
    if (syms_or.ok()) {
      owned_syms = std::make_unique<SymbolIndex>(std::move(*syms_or));
    }
  }

  auto extractor = std::make_unique<TargetExtractor>(options, owned_cfg.get(), owned_syms.get());
  extractor->owned_config_ = std::move(owned_cfg);
  extractor->owned_symbols_ = std::move(owned_syms);
  return extractor;
}

absl::StatusOr<uint32_t> TargetExtractor::ResolveVram(std::string_view func_name_or_addr) const {
  if (symbols_ != nullptr) {
    const auto* entry = symbols_->FindByName(func_name_or_addr);
    if (entry != nullptr) {
      return entry->address();
    }
  }

  std::string_view addr_sv = func_name_or_addr;
  if (absl::ConsumePrefix(&addr_sv, "func_")) {
    uint32_t addr = 0;
    if (absl::SimpleHexAtoi(addr_sv, &addr)) {
      return addr;
    }
  }

  if (absl::ConsumePrefix(&addr_sv, "0x") || absl::ConsumePrefix(&addr_sv, "0X")) {
    uint32_t addr = 0;
    if (absl::SimpleHexAtoi(addr_sv, &addr)) {
      return addr;
    }
  }

  uint32_t direct_addr = 0;
  if (func_name_or_addr.size() == 8 && absl::SimpleHexAtoi(func_name_or_addr, &direct_addr)) {
    return direct_addr;
  }

  return absl::NotFoundError(absl::StrFormat(
      "TargetExtractor: Could not resolve symbol or address '%s'", func_name_or_addr));
}

absl::StatusOr<uint32_t> TargetExtractor::VramToRomOffset(uint32_t vram) const {
  if (config_ != nullptr) {
    for (const auto& seg : config_->segments()) {
      uint32_t seg_len = seg.rom_end() - seg.rom_start();
      if (vram >= seg.vram() && vram < seg.vram() + seg_len) {
        return seg.rom_start() + (vram - seg.vram());
      }
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
    for (const auto& seg : config_->segments()) {
      if (rom_offset >= seg.rom_start() && rom_offset < seg.rom_end()) {
        slice_size = seg.rom_end() - rom_offset;
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

absl::StatusOr<TargetFunction> TargetExtractor::ExtractTarget(
    std::string_view func_name_or_addr) const {
  std::filesystem::path rom_path =
      !options_.rom_path.empty()
          ? options_.rom_path
          : (options_.repo_root / "roms" / absl::StrCat(options_.game_name, ".z64"));
  if (std::filesystem::exists(rom_path)) {
    std::ifstream file(rom_path, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
      std::streamsize size = file.tellg();
      file.seekg(0, std::ios::beg);
      std::vector<uint8_t> buffer(size);
      if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        auto target_or = ExtractFromRom(buffer, func_name_or_addr);
        if (target_or.ok()) {
          return target_or;
        }
      }
    }
  }

  return ExtractFromAsm(func_name_or_addr);
}

}  // namespace rom_nom_nom
