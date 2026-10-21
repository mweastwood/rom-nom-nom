#include "core/compiler.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "core/assembly_aligner.h"

namespace rom_nom_nom {
namespace {

absl::Status EnsureOutputDirectoryExists(const std::filesystem::path& output_path,
                                         std::string_view caller_name) {
  if (!output_path.has_parent_path()) {
    return absl::OkStatus();
  }
  std::error_code ec;
  std::filesystem::create_directories(output_path.parent_path(), ec);
  if (ec) {
    return absl::InternalError(absl::StrFormat("%s: Failed to create output directory '%s': %s",
                                               caller_name, output_path.parent_path().string(),
                                               ec.message()));
  }
  return absl::OkStatus();
}

std::vector<std::filesystem::path> ResolveKmcAsIncludeDirs(
    const std::vector<std::filesystem::path>& include_dirs) {
  std::vector<std::filesystem::path> as_include_dirs;
  for (const auto& inc : include_dirs) {
    std::error_code ec;
    if (std::filesystem::exists(inc / "macro.inc", ec)) {
      as_include_dirs.push_back(inc);
    }
  }
  if (as_include_dirs.empty()) {
    return include_dirs;
  }
  return as_include_dirs;
}

absl::Status RunToolProcess(const Compiler::ProcessRunner& runner, const std::filesystem::path& cwd,
                            const std::filesystem::path& program, std::vector<std::string> args,
                            absl::Duration timeout, std::string_view error_prefix,
                            const std::filesystem::path& target_path,
                            bool include_command_in_error = false) {
  ProcessOptions proc_opts{
      .program = program.string(),
      .args = std::move(args),
      .working_directory = cwd,
      .timeout = timeout,
  };
  auto result = runner(proc_opts);
  if (!result.ok()) {
    return result.status();
  }
  if (!result->Ok()) {
    if (include_command_in_error) {
      return absl::InternalError(absl::StrFormat(
          "%s for '%s' (exit code %d):\n%s\n%s\nCommand: %s %s", error_prefix, target_path.string(),
          result->exit_code, result->stdout_output, result->stderr_output, proc_opts.program,
          absl::StrJoin(proc_opts.args, " ")));
    }
    return absl::InternalError(absl::StrFormat("%s for '%s' (exit code %d):\n%s\n%s", error_prefix,
                                               target_path.string(), result->exit_code,
                                               result->stdout_output, result->stderr_output));
  }
  return absl::OkStatus();
}

std::string CompactO0FramePointerFunctionBlock(std::string_view fn_block, bool* modified) {
  size_t frame_pos = fn_block.find(".frame\t$fp,");
  if (frame_pos == std::string_view::npos) {
    return std::string(fn_block);
  }
  size_t frame_line_end = fn_block.find('\n', frame_pos);
  if (frame_line_end == std::string_view::npos) {
    return std::string(fn_block);
  }
  std::string_view frame_line = fn_block.substr(frame_pos, frame_line_end - frame_pos);
  size_t fp_comma = frame_line.find("$fp,");
  size_t ra_comma = frame_line.find(",$31");
  size_t vars_tag = frame_line.find("vars=");
  size_t regs_tag = frame_line.find(", regs=");
  size_t args_tag = frame_line.find("args=");
  size_t extra_tag = frame_line.find(", extra=");
  if (fp_comma == std::string_view::npos || ra_comma == std::string_view::npos ||
      vars_tag == std::string_view::npos || regs_tag == std::string_view::npos ||
      args_tag == std::string_view::npos || extra_tag == std::string_view::npos) {
    return std::string(fn_block);
  }

  int frame_size = 0;
  int vars_size = 0;
  int args_size = 0;
  if (!absl::SimpleAtoi(frame_line.substr(fp_comma + 4, ra_comma - (fp_comma + 4)), &frame_size) ||
      !absl::SimpleAtoi(frame_line.substr(vars_tag + 5, regs_tag - (vars_tag + 5)), &vars_size) ||
      !absl::SimpleAtoi(frame_line.substr(args_tag + 5, extra_tag - (args_tag + 5)), &args_size) ||
      vars_size <= 0) {
    return std::string(fn_block);
  }

  int used_local_max = 0;
  size_t search_pos = 0;
  while ((search_pos = fn_block.find("($fp)", search_pos)) != std::string_view::npos) {
    size_t num_end = search_pos;
    size_t num_start = num_end;
    while (num_start > 0 && fn_block[num_start - 1] >= '0' && fn_block[num_start - 1] <= '9') {
      --num_start;
    }
    if (num_start < num_end && (num_start == 0 || fn_block[num_start - 1] != '-')) {
      int off = 0;
      if (absl::SimpleAtoi(fn_block.substr(num_start, num_end - num_start), &off)) {
        if (off >= args_size && off < args_size + vars_size) {
          used_local_max = std::max(used_local_max, (off - args_size) + 4);
        }
      }
    }
    search_pos += 5;
  }

  int actual_vars_size = (used_local_max + 7) & ~7;
  int delta = vars_size - actual_vars_size;
  if (delta <= 0) {
    return std::string(fn_block);
  }

  *modified = true;
  int new_frame_size = frame_size - delta;
  std::string result(fn_block);

  auto replace_all = [&](std::string_view from, std::string_view to) {
    size_t p = 0;
    while ((p = result.find(from, p)) != std::string::npos) {
      result.replace(p, from.size(), to);
      p += to.size();
    }
  };

  replace_all(absl::StrFormat(".frame\t$fp,%d,$31", frame_size),
              absl::StrFormat(".frame\t$fp,%d,$31", new_frame_size));
  replace_all(absl::StrFormat("subu\t$sp,$sp,%d\n", frame_size),
              absl::StrFormat("subu\t$sp,$sp,%d\n", new_frame_size));
  replace_all(absl::StrFormat("addu\t$sp,$sp,%d\n", frame_size),
              absl::StrFormat("addu\t$sp,$sp,%d\n", new_frame_size));

  for (std::string_view reg_suffix : {"($sp)", "($fp)"}) {
    std::string updated;
    updated.reserve(result.size());
    size_t pos = 0;
    while (pos < result.size()) {
      size_t match_pos = result.find(reg_suffix, pos);
      if (match_pos == std::string::npos) {
        updated.append(result, pos, std::string::npos);
        break;
      }
      size_t num_end = match_pos;
      size_t num_start = num_end;
      while (num_start > pos && result[num_start - 1] >= '0' && result[num_start - 1] <= '9') {
        --num_start;
      }
      updated.append(result, pos, num_start - pos);
      if (num_start < num_end && (num_start == 0 || result[num_start - 1] != '-')) {
        int off = 0;
        if (absl::SimpleAtoi(result.substr(num_start, num_end - num_start), &off) &&
            off >= args_size + vars_size) {
          absl::StrAppendFormat(&updated, "%d%s", off - delta, reg_suffix);
          pos = match_pos + reg_suffix.size();
          continue;
        }
      }
      updated.append(result, num_start, (match_pos + reg_suffix.size()) - num_start);
      pos = match_pos + reg_suffix.size();
    }
    result = std::move(updated);
  }
  return result;
}

void StripDuplicateRomSymbolSuffixesInAssemblyFile(const std::filesystem::path& s272_path) {
  std::ifstream ifs(s272_path);
  if (!ifs.is_open()) {
    return;
  }
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ifs.close();

  bool modified = false;
  if (absl::StrContains(content, "__dup")) {
    std::string cleaned;
    cleaned.reserve(content.size());
    size_t pos = 0;
    while (pos < content.size()) {
      size_t dup_pos = content.find("__dup", pos);
      if (dup_pos == std::string::npos) {
        cleaned.append(content, pos, std::string::npos);
        break;
      }
      cleaned.append(content, pos, dup_pos - pos);
      size_t digit_end = dup_pos + 5;
      while (digit_end < content.size() && content[digit_end] >= '0' && content[digit_end] <= '9') {
        ++digit_end;
      }
      if (digit_end > dup_pos + 5) {
        pos = digit_end;
        modified = true;
      } else {
        cleaned.append("__dup");
        pos = dup_pos + 5;
      }
    }
    content = std::move(cleaned);
  }

  if (absl::StrContains(content, ".frame\t$fp,")) {
    std::string compacted;
    compacted.reserve(content.size());
    size_t pos = 0;
    while (pos < content.size()) {
      size_t ent_pos = content.find("\t.ent\t", pos);
      if (ent_pos == std::string::npos) {
        compacted.append(content, pos, std::string::npos);
        break;
      }
      compacted.append(content, pos, ent_pos - pos);
      size_t end_pos = content.find("\t.end\t", ent_pos);
      if (end_pos == std::string::npos) {
        compacted.append(content, ent_pos, std::string::npos);
        break;
      }
      size_t end_line = content.find('\n', end_pos);
      size_t block_end = (end_line == std::string::npos) ? content.size() : (end_line + 1);
      compacted.append(CompactO0FramePointerFunctionBlock(
          content.substr(ent_pos, block_end - ent_pos), &modified));
      pos = block_end;
    }
    content = std::move(compacted);
  }

  if (!modified) {
    return;
  }

  std::ofstream ofs(s272_path, std::ios::out | std::ios::trunc);
  if (ofs.is_open()) {
    ofs << content;
  }
}

}  // namespace

std::filesystem::path FindMacroInc(const std::filesystem::path& repo_root,
                                   std::string_view game_name) {
  if (game_name.empty()) {
    return {};
  }

  const std::string macro_filename = absl::StrCat(game_name, "_macro.inc");
  std::filesystem::path candidate = repo_root / "macros" / macro_filename;
  if (std::filesystem::exists(candidate)) {
    return candidate;
  }

  candidate = repo_root / macro_filename;
  if (std::filesystem::exists(candidate)) {
    return candidate;
  }

  return {};
}

Compiler::Compiler(Toolchain toolchain, ProcessRunner runner)
    : toolchain_(std::move(toolchain)), runner_(std::move(runner)) {}

absl::Status Compiler::CompileC(const CCompileOptions& options) const {
  if (options.src_file.empty()) {
    return absl::InvalidArgumentError("CompileC: Source file path must not be empty.");
  }
  if (options.out_obj.empty()) {
    return absl::InvalidArgumentError("CompileC: Output object path must not be empty.");
  }

  if (absl::Status dir_status = EnsureOutputDirectoryExists(options.out_obj, "CompileC");
      !dir_status.ok()) {
    return dir_status;
  }

  if (toolchain_.Mode() == ToolchainMode::kOriginal) {
    std::filesystem::path temp_s272 = options.out_obj;
    temp_s272.replace_extension(".s272");

    auto gcc_args = toolchain_.BuildGcc272CompileArgs(options.opt_flags, options.include_dirs,
                                                      options.src_file, temp_s272);
    if (absl::Status gcc_status = RunToolProcess(
            runner_, toolchain_.RepoRoot(), toolchain_.Gcc272(), std::move(gcc_args),
            options.timeout, "CompileC: GCC 2.7.2 compilation failed", options.src_file);
        !gcc_status.ok()) {
      std::error_code ec;
      std::filesystem::remove(temp_s272, ec);
      return gcc_status;
    }
    StripDuplicateRomSymbolSuffixesInAssemblyFile(temp_s272);

    std::vector<std::filesystem::path> as_include_dirs =
        ResolveKmcAsIncludeDirs(options.include_dirs);
    auto as_args = toolchain_.BuildKmcAsArgs(options.opt_flags, as_include_dirs, options.macro_inc,
                                             temp_s272, options.out_obj);
    if (absl::Status as_status =
            RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.KmcAs(), std::move(as_args),
                           options.timeout, "CompileC: KMC AS assembly failed", options.src_file,
                           /*include_command_in_error=*/true);
        !as_status.ok()) {
      std::error_code ec;
      std::filesystem::remove(temp_s272, ec);
      return as_status;
    }

    auto align_status = AlignFunctionsInAssemblyAndReassemble(
        toolchain_, runner_, temp_s272, options.out_obj, options.opt_flags, as_include_dirs,
        options.macro_inc, options.timeout, options.function_target_vrams,
        options.module_target_size, options.initial_vram_offset, options.external_jal_counts);
    std::error_code ec;
    std::filesystem::remove(temp_s272, ec);
    return align_status;
  }

  // Modern toolchain mode
  auto args = toolchain_.BuildModernCompileArgs(
      /*is_cpp=*/false, options.include_dirs, options.src_file, options.out_obj);
  return RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.GnuGcc(), std::move(args),
                        options.timeout, "CompileC: Modern GCC compilation failed",
                        options.src_file);
}

absl::Status Compiler::CompileCWithPerFunctionFlags(
    const PerFunctionCCompileOptions& options) const {
  if (options.src_file.empty()) {
    return absl::InvalidArgumentError(
        "CompileCWithPerFunctionFlags: Source file path must not be empty.");
  }
  if (options.out_obj.empty()) {
    return absl::InvalidArgumentError(
        "CompileCWithPerFunctionFlags: Output object path must not be empty.");
  }

  auto fallback_to_compile_c = [&]() {
    return CompileC({
        .src_file = options.src_file,
        .out_obj = options.out_obj,
        .opt_flags = options.default_flags,
        .include_dirs = options.include_dirs,
        .macro_inc = options.macro_inc,
        .timeout = options.timeout,
        .function_target_vrams = options.function_target_vrams,
        .module_target_size = options.module_target_size,
        .initial_vram_offset = options.initial_vram_offset,
        .external_jal_counts = options.external_jal_counts,
    });
  };

  if (toolchain_.Mode() != ToolchainMode::kOriginal) {
    return fallback_to_compile_c();
  }

  // 1. Collect unique flag sets {F0, F1, ..., Fk} where F0 = default_flags.
  std::vector<std::vector<std::string>> unique_flag_sets;
  unique_flag_sets.push_back(options.default_flags);

  absl::flat_hash_map<std::string, size_t> func_to_flag_set;
  for (const auto& [func_name, flags] : options.function_flags) {
    if (flags.empty() || flags == options.default_flags) {
      continue;
    }
    auto it = std::find(unique_flag_sets.begin(), unique_flag_sets.end(), flags);
    size_t idx = 0;
    if (it == unique_flag_sets.end()) {
      idx = unique_flag_sets.size();
      unique_flag_sets.push_back(flags);
    } else {
      idx = static_cast<size_t>(std::distance(unique_flag_sets.begin(), it));
    }
    func_to_flag_set[func_name] = idx;
  }

  if (unique_flag_sets.size() <= 1) {
    return fallback_to_compile_c();
  }

  if (absl::Status dir_status =
          EnsureOutputDirectoryExists(options.out_obj, "CompileCWithPerFunctionFlags");
      !dir_status.ok()) {
    return dir_status;
  }

  std::vector<std::filesystem::path> temp_s272_files;
  auto cleanup_temps = [&]() {
    std::error_code ec;
    for (const auto& p : temp_s272_files) {
      std::filesystem::remove(p, ec);
    }
  };

  // 2. Compile src_file with GCC 2.7.2 under each unique flag set.
  for (size_t j = 0; j < unique_flag_sets.size(); ++j) {
    std::filesystem::path temp_s272 = options.out_obj;
    temp_s272.replace_extension(absl::StrFormat(".%d.s272", j));
    temp_s272_files.push_back(temp_s272);

    auto gcc_args = toolchain_.BuildGcc272CompileArgs(unique_flag_sets[j], options.include_dirs,
                                                      options.src_file, temp_s272);

    ProcessOptions gcc_opts{
        .program = toolchain_.Gcc272().string(),
        .args = std::move(gcc_args),
        .working_directory = toolchain_.RepoRoot(),
        .timeout = options.timeout,
    };

    auto gcc_result = runner_(gcc_opts);
    if (!gcc_result.ok()) {
      cleanup_temps();
      return gcc_result.status();
    }
    if (!gcc_result->Ok()) {
      cleanup_temps();
      return absl::InternalError(
          absl::StrFormat("CompileCWithPerFunctionFlags: GCC 2.7.2 compilation failed for '%s' "
                          "(flag set %d, exit code %d):\n%s\n%s",
                          options.src_file.string(), j, gcc_result->exit_code,
                          gcc_result->stdout_output, gcc_result->stderr_output));
    }
    if (!std::filesystem::exists(temp_s272)) {
      std::ofstream(temp_s272) << "";
    }
    StripDuplicateRomSymbolSuffixesInAssemblyFile(temp_s272);
  }

  // 3. Determine whether any flag set enables KMC AS optimization (`-Wa,-O...`).
  bool overall_has_wa_opt = false;
  for (const auto& f : options.default_flags) {
    if (absl::StartsWith(f, "-Wa,-O")) {
      overall_has_wa_opt = true;
      break;
    }
  }
  if (!overall_has_wa_opt) {
    for (const auto& [_, flags] : options.function_flags) {
      for (const auto& f : flags) {
        if (absl::StartsWith(f, "-Wa,-O")) {
          overall_has_wa_opt = true;
          break;
        }
      }
      if (overall_has_wa_opt) {
        break;
      }
    }
  }

  // 4. Splice per-function assembly blocks into F0.
  auto spliced_lines_or = SplicePerFunctionAssembly(temp_s272_files, unique_flag_sets,
                                                    func_to_flag_set, overall_has_wa_opt);
  if (!spliced_lines_or.ok()) {
    cleanup_temps();
    return spliced_lines_or.status();
  }

  // 5. Write spliced assembly to temporary file.
  std::filesystem::path temp_spliced_s272 = options.out_obj;
  temp_spliced_s272.replace_extension(".spliced.s272");
  temp_s272_files.push_back(temp_spliced_s272);

  {
    std::ofstream ofs(temp_spliced_s272);
    if (!ofs.is_open()) {
      cleanup_temps();
      return absl::InternalError(absl::StrFormat(
          "CompileCWithPerFunctionFlags: Failed to open '%s' for writing spliced assembly.",
          temp_spliced_s272.string()));
    }
    for (const auto& line : *spliced_lines_or) {
      ofs << line;
    }
  }

  // 6. Assemble spliced assembly via KMC AS.
  std::vector<std::filesystem::path> as_include_dirs =
      ResolveKmcAsIncludeDirs(options.include_dirs);

  std::vector<std::string> as_opt_flags;
  for (const auto& f : options.default_flags) {
    if (overall_has_wa_opt && f == "-Wa,-g") {
      continue;
    }
    as_opt_flags.push_back(f);
  }
  if (overall_has_wa_opt) {
    bool has_opt = false;
    for (const auto& f : as_opt_flags) {
      if (absl::StartsWith(f, "-Wa,-O")) {
        has_opt = true;
        break;
      }
    }
    if (!has_opt) {
      as_opt_flags.push_back("-Wa,-O2");
    }
  }

  auto as_args = toolchain_.BuildKmcAsArgs(as_opt_flags, as_include_dirs, options.macro_inc,
                                           temp_spliced_s272, options.out_obj);
  if (absl::Status as_status =
          RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.KmcAs(), std::move(as_args),
                         options.timeout, "CompileCWithPerFunctionFlags: KMC AS assembly failed",
                         options.src_file, /*include_command_in_error=*/true);
      !as_status.ok()) {
    cleanup_temps();
    return as_status;
  }

  auto align_status = AlignFunctionsInAssemblyAndReassemble(
      toolchain_, runner_, temp_spliced_s272, options.out_obj, as_opt_flags, as_include_dirs,
      options.macro_inc, options.timeout, options.function_target_vrams, options.module_target_size,
      options.initial_vram_offset, options.external_jal_counts);
  cleanup_temps();
  return align_status;
}

absl::Status Compiler::CompileCpp(const CppCompileOptions& options) const {
  if (options.src_file.empty()) {
    return absl::InvalidArgumentError("CompileCpp: Source file path must not be empty.");
  }
  if (options.out_obj.empty()) {
    return absl::InvalidArgumentError("CompileCpp: Output object path must not be empty.");
  }
  if (toolchain_.Mode() != ToolchainMode::kModern) {
    return absl::FailedPreconditionError(
        "CompileCpp: C++ compilation is only supported in modern toolchain mode.");
  }

  if (absl::Status dir_status = EnsureOutputDirectoryExists(options.out_obj, "CompileCpp");
      !dir_status.ok()) {
    return dir_status;
  }

  auto args = toolchain_.BuildModernCompileArgs(
      /*is_cpp=*/true, options.include_dirs, options.src_file, options.out_obj);
  return RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.GnuGxx(), std::move(args),
                        options.timeout, "CompileCpp: Modern G++ compilation failed",
                        options.src_file);
}

absl::Status Compiler::Assemble(const AssemblyOptions& options) const {
  if (options.src_file.empty()) {
    return absl::InvalidArgumentError("Assemble: Source file path must not be empty.");
  }
  if (options.out_obj.empty()) {
    return absl::InvalidArgumentError("Assemble: Output object path must not be empty.");
  }

  if (absl::Status dir_status = EnsureOutputDirectoryExists(options.out_obj, "Assemble");
      !dir_status.ok()) {
    return dir_status;
  }

  auto args = toolchain_.BuildGnuAsArgs(options.include_dirs, options.src_file, options.out_obj);
  return RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.GnuAs(), std::move(args),
                        options.timeout, "Assemble: GNU MIPS AS assembly failed", options.src_file);
}

absl::Status Compiler::ConvertBinary(const BinaryAssetOptions& options) const {
  if (options.src_file.empty()) {
    return absl::InvalidArgumentError("ConvertBinary: Source file path must not be empty.");
  }
  if (options.out_obj.empty()) {
    return absl::InvalidArgumentError("ConvertBinary: Output object path must not be empty.");
  }

  if (absl::Status dir_status = EnsureOutputDirectoryExists(options.out_obj, "ConvertBinary");
      !dir_status.ok()) {
    return dir_status;
  }

  auto args = toolchain_.BuildObjcopyBinaryArgs(options.src_file, options.out_obj);
  return RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.GnuObjcopy(), std::move(args),
                        options.timeout, "ConvertBinary: GNU objcopy binary conversion failed",
                        options.src_file);
}

absl::Status Compiler::Link(const LinkOptions& options) const {
  if (options.main_ld_script.empty()) {
    return absl::InvalidArgumentError("Link: Main linker script path must not be empty.");
  }
  if (options.out_elf.empty()) {
    return absl::InvalidArgumentError("Link: Output ELF path must not be empty.");
  }

  if (absl::Status dir_status = EnsureOutputDirectoryExists(options.out_elf, "Link");
      !dir_status.ok()) {
    return dir_status;
  }

  auto args = toolchain_.BuildLdArgs(options.script_paths, options.main_ld_script, options.out_elf);
  return RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.GnuLd(), std::move(args),
                        options.timeout, "Link: GNU LD link failed", options.out_elf);
}

absl::Status Compiler::ExtractRom(const ExtractRomOptions& options) const {
  if (options.in_elf.empty()) {
    return absl::InvalidArgumentError("ExtractRom: Input ELF path must not be empty.");
  }
  if (options.out_rom.empty()) {
    return absl::InvalidArgumentError("ExtractRom: Output ROM path must not be empty.");
  }

  if (absl::Status dir_status = EnsureOutputDirectoryExists(options.out_rom, "ExtractRom");
      !dir_status.ok()) {
    return dir_status;
  }

  auto args = toolchain_.BuildExtractRomArgs(options.in_elf, options.out_rom);
  return RunToolProcess(runner_, toolchain_.RepoRoot(), toolchain_.GnuObjcopy(), std::move(args),
                        options.timeout, "ExtractRom: GNU objcopy ROM extraction failed",
                        options.out_rom);
}

}  // namespace rom_nom_nom
