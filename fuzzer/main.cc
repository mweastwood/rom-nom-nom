#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/game_config.h"
#include "core/symbol_registry.h"
#include "core/target_extractor.h"
#include "fuzzer/candidate_symbol_map.h"
#include "fuzzer/differential_fuzzer.h"
#include "fuzzer/equivalence_runner.h"
#include "fuzzer/relocation_rebaser.h"
#include "fuzzer/rom_function_scanner.h"

ABSL_FLAG(std::string, game, "harvest-moon-64", "Target game identifier.");
ABSL_FLAG(std::string, config, "", "Path to game config textproto.");
ABSL_FLAG(std::string, base_rom, "", "Path to base retail ROM binary.");
ABSL_FLAG(std::string, candidate_rom, "", "Path to candidate comparison ROM binary.");
ABSL_FLAG(std::string, candidate_elf, "", "Path to candidate comparison ELF binary.");
ABSL_FLAG(std::string, source, "", "Path to C source file containing target function.");
ABSL_FLAG(std::string, function, "", "Target function name or VRAM address to fuzz.");
ABSL_FLAG(std::string, module, "", "Target module subsegment to evaluate.");
ABSL_FLAG(size_t, vectors, 100,
          "Number of random fuzz vectors to evaluate per nonmatching function.");
ABSL_FLAG(size_t, pointer_vectors, 20, "Number of pointer/buffer payload vectors per function.");
ABSL_FLAG(uint32_t, seed, 42, "PRNG seed for deterministic test vector generation.");
ABSL_FLAG(std::string, report, "", "Optional file path to output detailed markdown/text report.");
ABSL_FLAG(bool, color, true, "Enable ANSI color output.");
ABSL_FLAG(bool, verbose, false, "Print detailed information for all functions.");
ABSL_FLAG(int, jobs, 0,
          "Number of concurrent worker threads (0 = auto-detect hardware concurrency).");
ABSL_FLAG(int, j, 0, "Alias for --jobs.");

namespace {

std::filesystem::path FindRepoRoot() {
  const char* build_workspace_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  if (build_workspace_dir != nullptr && build_workspace_dir[0] != '\0') {
    return std::filesystem::path(build_workspace_dir);
  }
  const char* build_working_dir = std::getenv("BUILD_WORKING_DIRECTORY");
  if (build_working_dir != nullptr && build_working_dir[0] != '\0') {
    return std::filesystem::path(build_working_dir);
  }
  std::filesystem::path cur = std::filesystem::current_path();
  std::error_code ec;
  while (!cur.empty()) {
    if (std::filesystem::exists(cur / "MODULE.bazel", ec) ||
        std::filesystem::exists(cur / "WORKSPACE", ec)) {
      return cur;
    }
    auto parent = cur.parent_path();
    if (parent == cur) break;
    cur = parent;
  }
  return std::filesystem::current_path();
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return {};
  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(size);
  if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return buffer;
  }
  return {};
}

using rom_nom_nom::fuzzer::BuildCandidateRebase;
using rom_nom_nom::fuzzer::BuildCandidateSymbolMap;
using rom_nom_nom::fuzzer::CandidateRebase;
using rom_nom_nom::fuzzer::CandidateSymbolMap;
using rom_nom_nom::fuzzer::CollectDataBlobs;
using rom_nom_nom::fuzzer::ExtractCandidateDataPayloads;
using rom_nom_nom::fuzzer::ExtractTargetDataPayloads;
using rom_nom_nom::fuzzer::RebaseOptions;

}  // namespace

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Differential Fuzzer & Semantic Equivalence Testing for N64 Decompilation.\n"
      "Usage:\n"
      "  Single function fuzzing against compiled C:\n"
      "    fuzzer <function_name> [--source=<path>] [--game=<game>]\n"
      "  Module / Whole-ROM equivalence evaluation:\n"
      "    fuzzer --candidate_rom=<path> [--base_rom=<path>] [--module=<module>] [--game=<game>]\n"
      "    fuzzer <base_rom.z64> <candidate_rom.z64>");

  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);

  std::string game_name = absl::GetFlag(FLAGS_game);
  std::string config_path = absl::GetFlag(FLAGS_config);
  std::string base_rom_path = absl::GetFlag(FLAGS_base_rom);
  std::string candidate_rom_path = absl::GetFlag(FLAGS_candidate_rom);
  std::string source_path = absl::GetFlag(FLAGS_source);
  std::string func_flag = absl::GetFlag(FLAGS_function);
  std::string module_flag = absl::GetFlag(FLAGS_module);
  size_t num_vectors = absl::GetFlag(FLAGS_vectors);
  size_t num_pointer_vectors = absl::GetFlag(FLAGS_pointer_vectors);
  uint32_t seed = absl::GetFlag(FLAGS_seed);
  std::string report_path = absl::GetFlag(FLAGS_report);
  bool color = absl::GetFlag(FLAGS_color) && isatty(STDOUT_FILENO);
  bool verbose = absl::GetFlag(FLAGS_verbose);
  int jobs = absl::GetFlag(FLAGS_jobs);
  if (jobs <= 0) {
    jobs = absl::GetFlag(FLAGS_j);
  }

  std::filesystem::path repo_root = FindRepoRoot();

  // Positional arguments handling
  if (remaining_args.size() >= 3) {
    std::string_view arg1 = remaining_args[1];
    std::string_view arg2 = remaining_args[2];
    if (absl::EndsWith(arg1, ".z64") && absl::EndsWith(arg2, ".z64")) {
      base_rom_path = std::string(arg1);
      candidate_rom_path = std::string(arg2);
    }
  } else if (remaining_args.size() >= 2 && func_flag.empty()) {
    std::string_view arg1 = remaining_args[1];
    if (!absl::EndsWith(arg1, ".z64")) {
      func_flag = std::string(arg1);
    }
  }

  // Resolve default paths
  std::error_code ec;
  if (base_rom_path.empty()) {
    std::filesystem::path default_rom = repo_root / "roms" / absl::StrCat(game_name, ".z64");
    if (std::filesystem::exists(default_rom, ec)) {
      base_rom_path = default_rom.string();
    }
  }
  if (config_path.empty()) {
    std::filesystem::path default_config =
        repo_root / "config" / absl::StrCat(game_name, ".textproto");
    if (std::filesystem::exists(default_config, ec)) {
      config_path = default_config.string();
    }
  }

  auto resolve_relative = [&](const std::string& p) -> std::string {
    if (p.empty()) return "";
    std::filesystem::path fp(p);
    if (fp.is_absolute()) return p;
    if (std::filesystem::exists(fp, ec)) return fp.string();
    if (std::filesystem::exists(repo_root / fp, ec)) return (repo_root / fp).string();
    return p;
  };

  base_rom_path = resolve_relative(base_rom_path);
  candidate_rom_path = resolve_relative(candidate_rom_path);
  config_path = resolve_relative(config_path);

  // Initialize TargetExtractor
  auto extractor_or = rom_nom_nom::TargetExtractor::Create({
      .repo_root = repo_root,
      .game_name = game_name,
      .config_path = config_path,
      .rom_path = base_rom_path,
  });
  if (!extractor_or.ok()) {
    std::cerr << "Error initializing TargetExtractor: " << extractor_or.status().message() << "\n";
    return 1;
  }
  auto extractor = std::move(*extractor_or);

  std::string candidate_elf_path = absl::GetFlag(FLAGS_candidate_elf);
  candidate_elf_path = resolve_relative(candidate_elf_path);
  if (candidate_elf_path.empty() && !candidate_rom_path.empty()) {
    std::filesystem::path cand_rom_p(candidate_rom_path);
    if (cand_rom_p.extension() == ".z64") {
      std::filesystem::path potential_elf = cand_rom_p;
      potential_elf.replace_extension(".elf");
      if (std::filesystem::exists(potential_elf, ec)) {
        candidate_elf_path = potential_elf.string();
      }
    }
  }

  std::vector<uint8_t> elf_bytes;
  if (!candidate_elf_path.empty()) {
    elf_bytes = ReadBinaryFile(candidate_elf_path);
  }

  // Set up equivalence runner options
  rom_nom_nom::fuzzer::EquivalenceRunnerOptions runner_opts;
  runner_opts.random_vectors_per_function = num_vectors;
  runner_opts.pointer_vectors_per_function = num_pointer_vectors;
  runner_opts.seed = seed;
  runner_opts.num_threads = jobs > 0 ? static_cast<size_t>(jobs) : 0;

  // Prefer exact relocation-based rebasing: rewrite every candidate reference into a data/BSS
  // blob so it points at the blob's target address. Both emulators then share one data layout
  // and memory effects compare byte-for-byte without heuristic address translation.
  std::shared_ptr<const CandidateRebase> rebase;
  if (!elf_bytes.empty()) {
    const auto& fopts = runner_opts.fuzzer_options;
    RebaseOptions rebase_opts;
    rebase_opts.reserved_ranges = {
        {fopts.pointer_buffer_vram, fopts.pointer_buffer_vram + fopts.pointer_buffer_size},
        {fopts.initial_sp - 0x10000, fopts.initial_sp + 0x1000},
    };
    // Keep relocated compiler rodata far away from game data: fuzz vectors routinely index
    // arrays out of bounds, and the target reads zeros wherever the candidate keeps extra data.
    rebase_opts.scratch_base = fopts.pointer_buffer_vram + 0x100000;
    // Functions whose candidate code overlaps the target's data addresses run from here.
    rebase_opts.code_scratch_base = fopts.pointer_buffer_vram + 0x200000;
    auto rebase_or =
        BuildCandidateRebase(elf_bytes, CollectDataBlobs(extractor->Config(), extractor->Symbols()),
                             extractor->Config(), extractor->Symbols(), rebase_opts);
    if (!rebase_or.ok()) {
      std::cerr << "Error building candidate relocation rebase: " << rebase_or.status().message()
                << "\n";
      return 1;
    }
    rebase = std::make_shared<const CandidateRebase>(std::move(*rebase_or));
    std::cerr << absl::StrFormat(
        "Relocation rebasing: %zu relocations patched, %zu private payload(s), %zu unsupported "
        "relocation(s), %zu layout mismatch(es)\n",
        rebase->relocations_patched, rebase->private_payloads.size(),
        rebase->unsupported_relocations, rebase->layout_mismatches);
    for (const auto& w : rebase->warnings) {
      std::cerr << "  warning: " << w << "\n";
    }
    if (rebase->layout_mismatches > 0) {
      std::cerr << "Error: Candidate relocation rebase detected " << rebase->layout_mismatches
                << " layout mismatch(es).\n";
      return 1;
    }
  }

  std::vector<rom_nom_nom::fuzzer::MemoryPayload> rebased_candidate_payloads;
  bool rebase_hooks_installed = false;
  if (!elf_bytes.empty()) {
    auto symbol_map = BuildCandidateSymbolMap(elf_bytes, extractor->Symbols());
    if (!symbol_map.entries.empty()) {
      auto map_ptr = std::make_shared<CandidateSymbolMap>(std::move(symbol_map));

      std::vector<uint8_t> base_rom_for_payloads;
      if (!base_rom_path.empty()) {
        base_rom_for_payloads = ReadBinaryFile(base_rom_path);
      }
      auto target_payloads = ExtractTargetDataPayloads(base_rom_for_payloads, extractor->Config());
      if (rebase != nullptr) {
        rebased_candidate_payloads = target_payloads;
        rebased_candidate_payloads.insert(rebased_candidate_payloads.end(),
                                          rebase->private_payloads.begin(),
                                          rebase->private_payloads.end());
      }
      auto target_payloads_ptr = std::make_shared<std::vector<rom_nom_nom::fuzzer::MemoryPayload>>(
          std::move(target_payloads));

      uint32_t target_bss_start = 0;
      uint32_t target_bss_end = 0;
      if (extractor->Config() != nullptr) {
        for (const auto& seg : extractor->Config()->segments()) {
          for (int s = 0; s < seg.subsegments_size(); ++s) {
            const auto& sub = seg.subsegments(s);
            if (sub.type() == rom_nom_nom::SUBSEGMENT_BSS && sub.vram() != 0) {
              if (target_bss_start == 0 || sub.vram() < target_bss_start) {
                target_bss_start = sub.vram();
              }
              uint32_t sub_bss_size = sub.bss_size() > 0 ? sub.bss_size() : seg.bss_size();
              uint32_t bss_end = sub.vram() + (sub_bss_size != 0 ? sub_bss_size : 0x100000);
              if (bss_end > target_bss_end) {
                target_bss_end = bss_end;
              }
            }
          }
        }
      }

      auto read_payload = [payloads = target_payloads_ptr](uint32_t addr,
                                                           size_t size) -> std::optional<uint64_t> {
        for (const auto& p : *payloads) {
          if (addr >= p.address && addr + size <= p.address + p.data.size()) {
            size_t offset = addr - p.address;
            if (size == 1) return p.data[offset];
            if (size == 2) return rom_nom_nom::ReadBigEndian16(p.data.data() + offset);
            if (size == 4) return rom_nom_nom::ReadBigEndian32(p.data.data() + offset);
            if (size == 8) return rom_nom_nom::ReadBigEndian64(p.data.data() + offset);
          }
        }
        return std::nullopt;
      };

      auto shared_read_hook = [read_payload, target_bss_start, target_bss_end](
                                  uint32_t vram, size_t size,
                                  bool /*is_fp*/) -> std::optional<uint64_t> {
        if (target_bss_start != 0 && vram >= target_bss_start && vram < target_bss_end) {
          return 0;
        }
        return read_payload(vram, size);
      };
      runner_opts.fuzzer_options.target_read_hook = shared_read_hook;

      if (rebase != nullptr) {
        // Data addresses are already identical; only code addresses (e.g. returned or stored
        // function pointers) still differ between the two builds.
        auto function_map = std::make_shared<absl::flat_hash_map<uint32_t, uint32_t>>();
        for (const auto& e : map_ptr->entries) {
          if (!e.is_data && e.cand_addr != e.target_addr) {
            function_map->emplace(e.cand_addr, e.target_addr);
          }
        }
        runner_opts.fuzzer_options.candidate_to_target_address = [function_map](uint32_t addr) {
          auto it = function_map->find(addr);
          return it != function_map->end() ? it->second : addr;
        };
        runner_opts.fuzzer_options.candidate_read_hook = shared_read_hook;
        rebase_hooks_installed = true;
      } else {
        runner_opts.fuzzer_options.candidate_to_target_address = [map = map_ptr](uint32_t addr) {
          return map->Translate(addr);
        };
        runner_opts.fuzzer_options.candidate_read_hook =
            [map = map_ptr, read_payload, target_bss_start, target_bss_end,
             has_candidate_payloads = !candidate_rom_path.empty()](
                uint32_t cand_vram, size_t size, bool is_fp) -> std::optional<uint64_t> {
          // 1. If the address is within candidate's own rodata section, candidate emulator
          // already holds the compiled rodata (e.g. jump tables) directly.
          if (map != nullptr && map->IsCandidateRodata(cand_vram, is_fp)) {
            return std::nullopt;
          }

          // 2. Candidate address that translates to target rodata / data
          uint32_t target_vram = (map != nullptr) ? map->Translate(cand_vram) : cand_vram;
          if (target_bss_start != 0 && target_vram >= target_bss_start &&
              target_vram < target_bss_end) {
            return 0;
          }

          // If candidate data payloads were loaded from candidate ROM, candidate emulator
          // already holds candidate's own data directly.
          if (has_candidate_payloads && map != nullptr && map->IsCandidateData(cand_vram)) {
            return std::nullopt;
          }
          if (target_vram != cand_vram) {
            auto res = read_payload(target_vram, size);
            if (res.has_value()) return *res;
          }

          // 3. Direct target address dereference (via embedded retail pointer in data table)
          auto target_res = read_payload(cand_vram, size);
          if (target_res.has_value()) {
            return *target_res;
          }

          return std::nullopt;
        };
      }
    }
  }
  if (!rebase_hooks_installed) {
    // Without a symbol map the rebased hooks are not installed; keep words and memory unrebased.
    rebase.reset();
  }

  rom_nom_nom::fuzzer::EquivalenceRunner runner(runner_opts);

  // Mode 1: Single function fuzzing via C source file or ROM
  if (!func_flag.empty()) {
    rom_nom_nom::fuzzer::FunctionEquivalenceTarget target;
    target.name = func_flag;

    auto target_func_or = extractor->ExtractTarget(func_flag);
    if (!target_func_or.ok()) {
      std::cerr << "Error extracting target function '" << func_flag
                << "': " << target_func_or.status().message() << "\n";
      return 1;
    }
    target.vram = target_func_or->vram;
    target.target_words = target_func_or->raw_words;
    std::vector<uint8_t> base_rom_bytes = ReadBinaryFile(base_rom_path);
    target.target_initial_memory = ExtractTargetDataPayloads(base_rom_bytes, extractor->Config());

    // If source file given, compile via TargetExtractor
    if (!source_path.empty()) {
      auto compiled_or = extractor->CompileCandidateFunction(source_path, func_flag, target.vram);
      if (!compiled_or.ok()) {
        std::cerr << "Error compiling candidate source: " << compiled_or.status().message() << "\n";
        return 1;
      }
      target.candidate_words = std::move(compiled_or->patched_words);
    } else if (!candidate_rom_path.empty()) {
      std::vector<uint8_t> candidate_bytes = ReadBinaryFile(candidate_rom_path);
      if (candidate_bytes.empty()) {
        std::cerr << "Error reading candidate ROM file: " << candidate_rom_path << "\n";
        return 1;
      }
      if (!candidate_elf_path.empty() && !elf_bytes.empty()) {
        auto elf_cand_funcs_or = rom_nom_nom::fuzzer::RomFunctionScanner::ScanFromElfAndRom(
            candidate_bytes, elf_bytes, extractor->Config());
        if (elf_cand_funcs_or.ok()) {
          for (const auto& fn : *elf_cand_funcs_or) {
            if (fn.name == func_flag) {
              target.candidate_vram = fn.vram;
              target.candidate_words = fn.raw_words;
              break;
            }
          }
        }
        target.candidate_initial_memory =
            rebase != nullptr
                ? rebased_candidate_payloads
                : ExtractCandidateDataPayloads(candidate_bytes, elf_bytes, extractor->Config());
        if (rebase != nullptr && !target.candidate_words.empty()) {
          auto placed = rebase->PlaceFunction(target.candidate_vram, target.candidate_words);
          target.candidate_vram = placed.vram;
          target.candidate_words = std::move(placed.words);
          target.candidate_initial_memory.insert(target.candidate_initial_memory.end(),
                                                 placed.overlays.begin(), placed.overlays.end());
        }
      }
      if (target.candidate_words.empty()) {
        auto cand_or = extractor->ExtractFromRom(candidate_bytes, func_flag);
        if (!cand_or.ok()) {
          std::cerr << "Error extracting candidate function from candidate ROM: "
                    << cand_or.status().message() << "\n";
          return 1;
        }
        target.candidate_words = cand_or->raw_words;
      }
    } else {
      std::cerr << "Error: Fuzzing function '" << func_flag
                << "' requires either --source=<path.c> or --candidate_rom=<path.z64>\n";
      return 1;
    }

    auto eval_res = runner.EvaluateFunction(target);
    std::cout << "\n=== Differential Fuzzing Result: " << func_flag << " ===\n";
    std::cout << "Target instruction count:    " << eval_res.target_instruction_count << "\n";
    std::cout << "Candidate instruction count: " << eval_res.candidate_instruction_count << "\n";
    std::cout << "Vectors evaluated:           " << eval_res.fuzz_vectors_evaluated << "\n";
    std::cout << "Status:                      ";
    switch (eval_res.status) {
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kExactBinaryMatch:
        std::cout << (color ? "\033[1;32mEXACT BINARY MATCH (100%)\033[0m"
                            : "EXACT BINARY MATCH (100%)")
                  << "\n";
        break;
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kFuzzProvenEquivalent:
        std::cout << (color ? "\033[1;36mFUZZ-PROVEN EQUIVALENT (100% Semantic Match)\033[0m"
                            : "FUZZ-PROVEN EQUIVALENT")
                  << "\n";
        break;
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kDivergent:
        std::cout << (color ? "\033[1;31mDIVERGENT (Behavioral Discrepancy)\033[0m" : "DIVERGENT")
                  << "\n";
        std::cout << "Details: " << eval_res.details << "\n";
        return 1;
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kCompilationOrExtractError:
        std::cout << (color ? "\033[1;31mERROR\033[0m" : "ERROR") << "\n";
        std::cout << "Details: " << eval_res.details << "\n";
        return 1;
    }
    return 0;
  }

  // Mode 2: Whole-ROM or Module Batch Equivalence Evaluation
  if (candidate_rom_path.empty()) {
    std::filesystem::path default_built = repo_root / "bazel-bin" / absl::StrCat(game_name, ".z64");
    if (std::filesystem::exists(default_built, ec)) {
      candidate_rom_path = default_built.string();
    } else {
      std::cerr << "Error: Evaluation mode requires --candidate_rom=<path.z64>\n";
      return 1;
    }
  }

  std::vector<uint8_t> target_bytes = ReadBinaryFile(base_rom_path);
  if (target_bytes.empty()) {
    std::cerr << "Error reading base retail ROM: " << base_rom_path << "\n";
    return 1;
  }
  std::vector<uint8_t> built_bytes = ReadBinaryFile(candidate_rom_path);
  if (built_bytes.empty()) {
    std::cerr << "Error reading built candidate ROM: " << candidate_rom_path << "\n";
    return 1;
  }

  auto base_funcs_or = rom_nom_nom::fuzzer::RomFunctionScanner::Scan(
      target_bytes, extractor->Config(), extractor->Symbols());
  if (!base_funcs_or.ok()) {
    std::cerr << "Error scanning base ROM functions: " << base_funcs_or.status().message() << "\n";
    return 1;
  }
  const auto& base_funcs = *base_funcs_or;

  std::vector<rom_nom_nom::fuzzer::ScannedFunction> cand_funcs;
  if (!elf_bytes.empty()) {
    auto elf_cand_funcs_or = rom_nom_nom::fuzzer::RomFunctionScanner::ScanFromElfAndRom(
        built_bytes, elf_bytes, extractor->Config());
    if (elf_cand_funcs_or.ok()) {
      absl::flat_hash_map<std::string, const rom_nom_nom::fuzzer::ScannedFunction*> cand_map;
      for (const auto& fn : *elf_cand_funcs_or) {
        cand_map[fn.name] = &fn;
      }
      cand_funcs.reserve(base_funcs.size());
      for (const auto& base_fn : base_funcs) {
        auto it = cand_map.find(base_fn.name);
        if (it != cand_map.end()) {
          cand_funcs.push_back(*it->second);
        }
      }
    }
  }

  if (cand_funcs.empty()) {
    auto cand_funcs_or = rom_nom_nom::fuzzer::RomFunctionScanner::Scan(
        built_bytes, extractor->Config(), extractor->Symbols());
    if (!cand_funcs_or.ok()) {
      std::cerr << "Error scanning candidate ROM functions: " << cand_funcs_or.status().message()
                << "\n";
      return 1;
    }
    cand_funcs = std::move(*cand_funcs_or);
  }

  if (base_funcs.size() != cand_funcs.size()) {
    std::cerr << absl::StrFormat(
        "Error: Discovered function count mismatch: base ROM has %zu functions, but candidate ROM "
        "has %zu functions.\n",
        base_funcs.size(), cand_funcs.size());
    std::cerr << "First 10 functions comparison:\n";
    for (size_t i = 0; i < std::min<size_t>(10, std::min(base_funcs.size(), cand_funcs.size()));
         ++i) {
      std::cerr << absl::StrFormat(
          "  [%zu] BASE: %-20s (vram=0x%08X, size=%4zu) | CAND: %-20s (vram=0x%08X, size=%4zu)\n",
          i, base_funcs[i].name, base_funcs[i].vram, base_funcs[i].size, cand_funcs[i].name,
          cand_funcs[i].vram, cand_funcs[i].size);
    }
    return 1;
  }

  std::optional<uint32_t> mod_rom_start;
  std::optional<uint32_t> mod_rom_end;
  if (!module_flag.empty() && extractor->Config() != nullptr) {
    for (const auto& seg : extractor->Config()->segments()) {
      if (seg.name() == module_flag) {
        mod_rom_start = seg.rom_start();
        mod_rom_end = seg.rom_end();
        break;
      }
      for (int s = 0; s < seg.subsegments_size(); ++s) {
        const auto& sub = seg.subsegments(s);
        if (sub.name() == module_flag) {
          mod_rom_start = sub.rom_start();
          if (s + 1 < seg.subsegments_size()) {
            mod_rom_end = seg.subsegments(s + 1).rom_start();
          } else {
            mod_rom_end = seg.rom_end();
          }
          break;
        }
      }
      if (mod_rom_start.has_value()) break;
    }
  }

  auto target_payloads = ExtractTargetDataPayloads(target_bytes, extractor->Config());
  auto candidate_payloads =
      rebase != nullptr ? rebased_candidate_payloads
                        : ExtractCandidateDataPayloads(built_bytes, elf_bytes, extractor->Config());

  std::vector<rom_nom_nom::fuzzer::FunctionEquivalenceTarget> targets;
  targets.reserve(base_funcs.size());
  for (size_t i = 0; i < base_funcs.size(); ++i) {
    const auto& base_fn = base_funcs[i];
    const auto& cand_fn = cand_funcs[i];
    if (mod_rom_start.has_value() && mod_rom_end.has_value()) {
      if (base_fn.rom_offset < *mod_rom_start || base_fn.rom_offset >= *mod_rom_end) {
        continue;
      }
    }

    rom_nom_nom::fuzzer::FunctionEquivalenceTarget target;
    target.name = base_fn.name;
    target.vram = base_fn.vram;
    target.candidate_vram = cand_fn.vram;
    target.target_words = base_fn.raw_words;
    target.candidate_words = cand_fn.raw_words;
    target.target_initial_memory = target_payloads;
    target.candidate_initial_memory = candidate_payloads;
    if (rebase != nullptr) {
      auto placed = rebase->PlaceFunction(cand_fn.vram, cand_fn.raw_words);
      target.candidate_vram = placed.vram;
      target.candidate_words = std::move(placed.words);
      target.candidate_initial_memory.insert(target.candidate_initial_memory.end(),
                                             placed.overlays.begin(), placed.overlays.end());
    }

    targets.push_back(std::move(target));
  }

  std::string eval_name = module_flag.empty() ? game_name : module_flag;
  auto summary = runner.EvaluateModule(eval_name, targets);
  std::string report = rom_nom_nom::fuzzer::EquivalenceRunner::FormatReport(summary, verbose);
  std::cout << report << "\n";

  if (!report_path.empty()) {
    std::ofstream rfile(report_path);
    if (rfile.is_open()) {
      rfile << report;
    }
  }

  return (summary.divergent_functions == 0 && summary.error_functions == 0) ? 0 : 1;
}
