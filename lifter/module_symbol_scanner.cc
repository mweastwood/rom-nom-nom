#include "lifter/module_symbol_scanner.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

std::vector<std::string> ModuleSymbolScanner::DiscoverAllCodeModules(const GameConfig& config) {
  std::vector<std::string> modules;
  for (const auto& seg : config.segments()) {
    if (seg.type() != SEGMENT_CODE) {
      continue;
    }
    for (const auto& subseg : seg.subsegments()) {
      if (subseg.type() == SUBSEGMENT_C || subseg.type() == SUBSEGMENT_ASM) {
        modules.push_back(std::string(subseg.name()));
      }
    }
  }
  return modules;
}

absl::StatusOr<std::vector<std::string>> ModuleSymbolScanner::DiscoverFunctionsInModule(
    std::string_view module_name, const GameConfig& config, const SymbolIndex& symbols,
    const std::filesystem::path& repo_root, std::string_view game_name,
    const std::filesystem::path& asm_dir) {
  std::vector<std::string> func_names;
  absl::flat_hash_set<std::string> seen;

  // Find subsegment matching module_name (or filename stem) in config
  const Subsegment* matched_subseg = nullptr;
  const Segment* matched_seg = nullptr;
  uint32_t next_rom_start = 0;

  for (const auto& seg : config.segments()) {
    for (int i = 0; i < seg.subsegments_size(); ++i) {
      const auto& subseg = seg.subsegments(i);
      if (subseg.name() == module_name ||
          std::filesystem::path(subseg.name()).filename().string() == module_name) {
        matched_subseg = &subseg;
        matched_seg = &seg;
        if (i + 1 < seg.subsegments_size()) {
          next_rom_start = seg.subsegments(i + 1).rom_start();
        } else {
          next_rom_start = seg.rom_end();
        }
        break;
      }
    }
    if (matched_subseg != nullptr) {
      break;
    }
  }

  std::vector<std::string> candidate_rel_names = {std::string(module_name)};
  if (matched_subseg != nullptr && matched_subseg->name() != module_name) {
    candidate_rel_names.push_back(std::string(matched_subseg->name()));
  }

  // 1. Check nonmatchings directory (e.g. <asm_dir>/nonmatchings/<module_name>/*.s)
  std::vector<std::filesystem::path> nonmatching_dirs;
  for (const auto& rel_name : candidate_rel_names) {
    if (!asm_dir.empty()) {
      nonmatching_dirs.push_back(asm_dir / "nonmatchings" / rel_name);
    }
    nonmatching_dirs.push_back(repo_root / "bazel-bin" / "asm" / game_name / "nonmatchings" /
                               rel_name);
    nonmatching_dirs.push_back(repo_root / "asm" / game_name / "nonmatchings" / rel_name);
  }
  for (const auto& dir : nonmatching_dirs) {
    if (std::filesystem::exists(dir)) {
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".s") {
          std::string stem = entry.path().stem().string();
          if (!stem.empty() && seen.insert(stem).second) {
            func_names.push_back(stem);
          }
        }
      }
    }
  }

  if (!func_names.empty()) {
    return func_names;
  }

  // 2. Check standalone module assembly file (e.g. <asm_dir>/<module_name>.s)
  std::vector<std::filesystem::path> asm_files;
  for (const auto& rel_name : candidate_rel_names) {
    if (!asm_dir.empty()) {
      asm_files.push_back(asm_dir / absl::StrCat(rel_name, ".s"));
    }
    asm_files.push_back(repo_root / "bazel-bin" / "asm" / game_name / absl::StrCat(rel_name, ".s"));
    asm_files.push_back(repo_root / "asm" / game_name / absl::StrCat(rel_name, ".s"));
  }
  for (const auto& file : asm_files) {
    if (std::filesystem::exists(file)) {
      std::ifstream in(file);
      std::string line;
      while (std::getline(in, line)) {
        std::string_view sv = line;
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
          sv.remove_prefix(1);
        }
        std::string func_name;
        if (absl::ConsumePrefix(&sv, ".ent ")) {
          func_name = std::string(absl::StripTrailingAsciiWhitespace(sv));
        } else if (absl::ConsumePrefix(&sv, "glabel ")) {
          func_name = std::string(absl::StripTrailingAsciiWhitespace(sv));
        }
        if (!func_name.empty() && seen.insert(func_name).second) {
          func_names.push_back(func_name);
        }
      }
      if (!func_names.empty()) {
        return func_names;
      }
    }
  }

  if (matched_subseg == nullptr) {
    return absl::NotFoundError(
        absl::StrFormat("Module '%s' not found in split configuration.", module_name));
  }

  uint32_t seg_offset = matched_subseg->rom_start() - matched_seg->rom_start();
  uint32_t start_vram = matched_seg->vram() + seg_offset;
  uint32_t end_vram = start_vram + (next_rom_start - matched_subseg->rom_start());

  for (const auto* entry : symbols.SortedEntries()) {
    if (entry->type() == SYMBOL_FUNC && entry->address() >= start_vram &&
        entry->address() < end_vram) {
      func_names.push_back(std::string(entry->name()));
    }
  }

  if (func_names.empty()) {
    // If no individual functions are declared in symbols, fallback to synthesized name
    func_names.push_back(symbols.LookupOrSynthesizeName(start_vram, SYMBOL_FUNC));
  }

  return func_names;
}

void ModuleSymbolScanner::WriteLiftedSymbolsLinkerScript(
    const std::filesystem::path& output_dir, const FunctionLoader* loader,
    const SymbolIndex* symbols, const absl::flat_hash_set<std::string>& defined_function_names,
    absl::Span<const std::pair<std::string, std::string>> undefined_auto_symbols) {
  std::filesystem::path lifted_symbols_file_path = output_dir / "lifted_symbols.ld";
  std::ofstream symbols_output_stream(lifted_symbols_file_path);
  if (!symbols_output_stream.is_open()) {
    return;
  }
  symbols_output_stream << "/* Auto-generated symbol definitions for lifted C */\n";

  std::map<uint32_t, std::string> all_known_functions;
  if (symbols != nullptr) {
    for (const auto* entry : symbols->SortedEntries()) {
      if (entry->type() == SYMBOL_FUNC) {
        all_known_functions[entry->address()] = entry->name();
      }
    }
  }
  for (const auto& func_name : defined_function_names) {
    uint32_t addr = 0;
    if (func_name.size() == 13 && absl::StartsWith(func_name, "func_") &&
        absl::SimpleHexAtoi(func_name.substr(5), &addr)) {
      all_known_functions[addr] = func_name;
    }
  }

  // Data and BSS subsegments are each emitted as one contiguous blob with a single base label.
  // When lifted C code is larger or smaller than the original, these blobs move, so every
  // symbol inside them must be defined relative to the blob's base label rather than at its
  // original absolute address.
  struct DataSubsegmentRange {
    uint32_t vram_start = 0;
    uint32_t vram_end = 0;
    std::string label_name;
  };
  std::vector<DataSubsegmentRange> data_subsegments;
  auto label_for_vram = [&](uint32_t vram) {
    if (symbols != nullptr) {
      const auto* sym = symbols->FindByAddress(vram);
      if (sym != nullptr) {
        return std::string(sym->name());
      }
    }
    return absl::StrFormat("D_%08X", vram);
  };
  if (loader != nullptr && loader->Extractor() != nullptr) {
    const auto* cfg = loader->Extractor()->Config();
    if (cfg != nullptr) {
      for (const auto& seg : cfg->segments()) {
        if (seg.type() != SEGMENT_CODE) {
          continue;
        }
        size_t seg_start = seg.rom_start();
        size_t seg_end = seg.rom_end();
        uint32_t seg_vram = seg.vram();
        for (int j = 0; j < seg.subsegments_size(); ++j) {
          const auto& sub = seg.subsegments(j);
          if (sub.type() == SUBSEGMENT_DATA) {
            size_t sub_start = sub.rom_start();
            size_t sub_end = seg_end;
            for (int k = j + 1; k < seg.subsegments_size(); ++k) {
              if (seg.subsegments(k).type() != SUBSEGMENT_BSS &&
                  seg.subsegments(k).rom_start() > 0) {
                sub_end = seg.subsegments(k).rom_start();
                break;
              }
            }
            uint32_t sub_vram = sub.vram();
            if (sub_vram == 0 && seg_vram != 0) {
              sub_vram = seg_vram + static_cast<uint32_t>(sub_start - seg_start);
            }
            uint32_t sub_size = static_cast<uint32_t>(sub_end - sub_start);
            data_subsegments.push_back({sub_vram, sub_vram + sub_size, label_for_vram(sub_vram)});
          } else if (sub.type() == SUBSEGMENT_BSS && sub.vram() != 0 && seg.bss_size() != 0) {
            data_subsegments.push_back(
                {sub.vram(), sub.vram() + seg.bss_size(), label_for_vram(sub.vram())});
          }
        }
      }
    }
  }

  // Returns "<label> + 0xOFF" (or "<label>") for an address inside a relocatable data/BSS
  // range, or nullopt if the address is not inside any such range.
  auto relative_data_expr = [&](uint32_t addr) -> std::optional<std::pair<std::string, bool>> {
    for (const auto& ds : data_subsegments) {
      if (addr >= ds.vram_start && addr < ds.vram_end) {
        uint32_t offset = addr - ds.vram_start;
        if (offset == 0) {
          return std::make_pair(ds.label_name, true);
        }
        return std::make_pair(absl::StrFormat("%s + 0x%X", ds.label_name, offset), false);
      }
    }
    return std::nullopt;
  };

  for (const auto& [symbol_name, address_hex] : undefined_auto_symbols) {
    bool resolved = false;
    uint32_t addr = 0;
    if (absl::SimpleHexAtoi(address_hex, &addr)) {
      if (absl::StartsWith(symbol_name, "func_") && loader != nullptr) {
        // If this is an unexported internal code label or sub-entrypoint inside a known function,
        // resolve it relative to the preceding registered or defined function symbol.
        auto it = all_known_functions.upper_bound(addr);
        if (it != all_known_functions.begin()) {
          const auto& prev = *std::prev(it);
          auto prev_loaded = loader->LoadFunction(prev.second);
          if (prev_loaded.ok()) {
            uint32_t prev_end =
                prev.first + static_cast<uint32_t>(prev_loaded->instructions.size() * 4);
            if (addr > prev.first && addr < prev_end &&
                defined_function_names.contains(prev.second)) {
              uint32_t offset = addr - prev.first;
              symbols_output_stream << absl::StrFormat("PROVIDE(%s = %s + 0x%X);\n", symbol_name,
                                                       prev.second, offset);
              resolved = true;
            }
          }
        }
      } else if (absl::StartsWith(symbol_name, "D_")) {
        if (auto rel = relative_data_expr(addr); rel.has_value()) {
          // A symbol that *is* the base label is already defined by the data/BSS object.
          if (!(rel->second && rel->first == symbol_name)) {
            symbols_output_stream << absl::StrFormat("%s = %s;\n", symbol_name, rel->first);
          }
          resolved = true;
        }
      }
    }
    if (!resolved) {
      symbols_output_stream << absl::StrFormat("PROVIDE(%s = 0x%s);\n", symbol_name, address_hex);
    }
  }

  // Registered (named) data symbols are PROVIDEd at their original absolute address by the
  // splitter's symbols.ld, which is only correct when nothing before them changes size. Re-anchor
  // every named data symbol that lives inside a data/BSS blob to that blob's base label. A plain
  // assignment is used because, in GNU ld, it overrides a PROVIDE regardless of script order.
  if (symbols != nullptr) {
    for (const auto* entry : symbols->SortedEntries()) {
      if (entry->type() != SYMBOL_DATA) {
        continue;
      }
      auto rel = relative_data_expr(entry->address());
      if (!rel.has_value()) {
        continue;
      }
      if (rel->second && rel->first == entry->name()) {
        continue;  // This is the base label.
      }
      symbols_output_stream << absl::StrFormat("%s = %s;\n", entry->name(), rel->first);
    }
  }
}

}  // namespace rom_nom_nom
