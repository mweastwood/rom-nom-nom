#include "core/compiler.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"

namespace rom_nom_nom {

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

  if (options.out_obj.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_obj.parent_path(), ec);
    if (ec) {
      return absl::InternalError(
          absl::StrFormat("CompileC: Failed to create output directory '%s': %s",
                          options.out_obj.parent_path().string(), ec.message()));
    }
  }

  if (toolchain_.Mode() == ToolchainMode::kOriginal) {
    std::filesystem::path temp_s272 = options.out_obj;
    temp_s272.replace_extension(".s272");

    auto gcc_args = toolchain_.BuildGcc272CompileArgs(options.opt_flags, options.include_dirs,
                                                      options.src_file, temp_s272);

    ProcessOptions gcc_opts{
        .program = toolchain_.Gcc272().string(),
        .args = std::move(gcc_args),
        .working_directory = toolchain_.RepoRoot(),
        .timeout = options.timeout,
    };

    auto gcc_result = runner_(gcc_opts);
    if (!gcc_result.ok()) {
      std::error_code ec;
      std::filesystem::remove(temp_s272, ec);
      return gcc_result.status();
    }
    if (!gcc_result->Ok()) {
      std::error_code ec;
      std::filesystem::remove(temp_s272, ec);
      return absl::InternalError(
          absl::StrFormat("CompileC: GCC 2.7.2 compilation failed for '%s' (exit code %d):\n%s\n%s",
                          options.src_file.string(), gcc_result->exit_code,
                          gcc_result->stdout_output, gcc_result->stderr_output));
    }

    std::vector<std::filesystem::path> as_include_dirs;
    for (const auto& inc : options.include_dirs) {
      std::error_code ec;
      if (std::filesystem::exists(inc / "macro.inc", ec)) {
        as_include_dirs.push_back(inc);
      }
    }
    if (as_include_dirs.empty()) {
      as_include_dirs = options.include_dirs;
    }

    auto as_args = toolchain_.BuildKmcAsArgs(options.opt_flags, as_include_dirs, options.macro_inc,
                                             temp_s272, options.out_obj);

    ProcessOptions as_opts{
        .program = toolchain_.KmcAs().string(),
        .args = std::move(as_args),
        .working_directory = toolchain_.RepoRoot(),
        .timeout = options.timeout,
    };

    auto as_result = runner_(as_opts);
    std::error_code ec;
    std::filesystem::remove(temp_s272, ec);

    if (!as_result.ok()) {
      return as_result.status();
    }
    if (!as_result->Ok()) {
      return absl::InternalError(absl::StrFormat(
          "CompileC: KMC AS assembly failed for '%s' (exit code %d):\n%s\n%s\nCommand: %s %s",
          options.src_file.string(), as_result->exit_code, as_result->stdout_output,
          as_result->stderr_output, as_opts.program, absl::StrJoin(as_opts.args, " ")));
    }
    return absl::OkStatus();
  }

  // Modern toolchain mode
  auto args = toolchain_.BuildModernCompileArgs(
      /*is_cpp=*/false, options.include_dirs, options.src_file, options.out_obj);

  ProcessOptions proc_opts{
      .program = toolchain_.GnuGcc().string(),
      .args = std::move(args),
      .working_directory = toolchain_.RepoRoot(),
      .timeout = options.timeout,
  };

  auto result = runner_(proc_opts);
  if (!result.ok()) {
    return result.status();
  }
  if (!result->Ok()) {
    return absl::InternalError(
        absl::StrFormat("CompileC: Modern GCC compilation failed for '%s' (exit code %d):\n%s\n%s",
                        options.src_file.string(), result->exit_code, result->stdout_output,
                        result->stderr_output));
  }
  return absl::OkStatus();
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

  if (toolchain_.Mode() != ToolchainMode::kOriginal) {
    return CompileC({
        .src_file = options.src_file,
        .out_obj = options.out_obj,
        .opt_flags = options.default_flags,
        .include_dirs = options.include_dirs,
        .macro_inc = options.macro_inc,
        .timeout = options.timeout,
    });
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
    size_t idx;
    if (it == unique_flag_sets.end()) {
      idx = unique_flag_sets.size();
      unique_flag_sets.push_back(flags);
    } else {
      idx = std::distance(unique_flag_sets.begin(), it);
    }
    func_to_flag_set[func_name] = idx;
  }

  if (unique_flag_sets.size() <= 1) {
    return CompileC({
        .src_file = options.src_file,
        .out_obj = options.out_obj,
        .opt_flags = options.default_flags,
        .include_dirs = options.include_dirs,
        .macro_inc = options.macro_inc,
        .timeout = options.timeout,
    });
  }

  if (options.out_obj.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_obj.parent_path(), ec);
    if (ec) {
      return absl::InternalError(absl::StrFormat(
          "CompileCWithPerFunctionFlags: Failed to create output directory '%s': %s",
          options.out_obj.parent_path().string(), ec.message()));
    }
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
  }

  // Helper to read all lines from a file.
  auto read_lines =
      [](const std::filesystem::path& path) -> absl::StatusOr<std::vector<std::string>> {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
      return absl::NotFoundError(absl::StrFormat("Failed to open file: %s", path.string()));
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
      line.push_back('\n');
      lines.push_back(std::move(line));
    }
    return lines;
  };

  struct FunctionSpan {
    std::string name;
    size_t start_line = 0;
    size_t end_line = 0;
  };

  auto parse_functions =
      [](const std::vector<std::string>& lines) -> absl::flat_hash_map<std::string, FunctionSpan> {
    absl::flat_hash_map<std::string, FunctionSpan> funcs;
    size_t i = 0;
    while (i < lines.size()) {
      std::string_view l = lines[i];
      while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) {
        l.remove_prefix(1);
      }
      if (absl::StartsWith(l, ".ent\t") || absl::StartsWith(l, ".ent ")) {
        std::vector<std::string_view> parts =
            absl::StrSplit(l, absl::ByAnyChar(" \t\r\n"), absl::SkipEmpty());
        if (parts.size() >= 2) {
          std::string fn_name(parts[1]);
          size_t start_idx = i;
          size_t end_idx = start_idx;
          bool found_end = false;
          for (size_t j = start_idx + 1; j < lines.size(); ++j) {
            std::string_view jl = lines[j];
            while (!jl.empty() && (jl.front() == ' ' || jl.front() == '\t')) {
              jl.remove_prefix(1);
            }
            if (absl::StartsWith(jl, absl::StrCat(".end\t", fn_name)) ||
                absl::StartsWith(jl, absl::StrCat(".end ", fn_name))) {
              end_idx = j;
              found_end = true;
              for (size_t k = j + 1; k < lines.size() && k <= j + 5; ++k) {
                std::string_view kl = lines[k];
                while (!kl.empty() && (kl.front() == ' ' || kl.front() == '\t')) {
                  kl.remove_prefix(1);
                }
                if (absl::StartsWith(kl, absl::StrCat(".size\t", fn_name)) ||
                    absl::StartsWith(kl, absl::StrCat(".size ", fn_name))) {
                  end_idx = k;
                  break;
                }
              }
              break;
            }
          }
          if (found_end) {
            funcs[fn_name] = FunctionSpan{
                .name = fn_name,
                .start_line = start_idx,
                .end_line = end_idx,
            };
            i = end_idx + 1;
            continue;
          }
        }
      }
      ++i;
    }
    return funcs;
  };

  // 3. Read assembly for each flag set.
  std::vector<std::vector<std::string>> all_lines(unique_flag_sets.size());
  std::vector<absl::flat_hash_map<std::string, FunctionSpan>> all_funcs(unique_flag_sets.size());
  for (size_t j = 0; j < unique_flag_sets.size(); ++j) {
    auto lines_or = read_lines(temp_s272_files[j]);
    if (!lines_or.ok()) {
      cleanup_temps();
      return lines_or.status();
    }
    all_funcs[j] = parse_functions(*lines_or);
    all_lines[j] = std::move(*lines_or);
  }

  // 4. Splice functions from F_j into F0.
  const auto& lines0 = all_lines[0];
  const auto& funcs0 = all_funcs[0];

  std::vector<FunctionSpan> sorted_funcs;
  sorted_funcs.reserve(funcs0.size());
  for (const auto& [_, span] : funcs0) {
    sorted_funcs.push_back(span);
  }
  std::sort(
      sorted_funcs.begin(), sorted_funcs.end(),
      [](const FunctionSpan& a, const FunctionSpan& b) { return a.start_line < b.start_line; });

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

  auto replace_word_token = [](std::string& line, std::string_view target,
                               std::string_view replacement) {
    size_t pos = 0;
    while ((pos = line.find(target, pos)) != std::string::npos) {
      if (pos > 0 &&
          (std::isalnum(static_cast<unsigned char>(line[pos - 1])) || line[pos - 1] == '_')) {
        pos += target.size();
        continue;
      }
      size_t after = pos + target.size();
      if (after < line.size() &&
          (std::isalnum(static_cast<unsigned char>(line[after])) || line[after] == '_')) {
        pos += target.size();
        continue;
      }
      line.replace(pos, target.size(), replacement);
      pos += replacement.size();
    }
  };

  std::vector<std::string> spliced_lines;
  size_t last_idx = 0;

  for (const auto& span : sorted_funcs) {
    for (size_t i = last_idx; i < span.start_line; ++i) {
      spliced_lines.push_back(lines0[i]);
    }

    auto it_flag = func_to_flag_set.find(span.name);
    size_t flag_set_idx = (it_flag != func_to_flag_set.end()) ? it_flag->second : 0;
    const auto& fn_flags = unique_flag_sets[flag_set_idx];

    std::vector<std::string> fn_lines;
    if (flag_set_idx == 0) {
      for (size_t i = span.start_line; i <= span.end_line; ++i) {
        fn_lines.push_back(lines0[i]);
      }
    } else {
      const auto& funcs_j = all_funcs[flag_set_idx];
      auto it_j = funcs_j.find(span.name);
      if (it_j != funcs_j.end()) {
        const auto& src_lines = all_lines[flag_set_idx];
        for (size_t i = it_j->second.start_line; i <= it_j->second.end_line; ++i) {
          fn_lines.push_back(src_lines[i]);
        }
        std::vector<std::string> defined_labels;
        for (const auto& line : fn_lines) {
          size_t colon = line.find(':');
          if (colon != std::string::npos) {
            size_t dot = line.rfind('.', colon);
            if (dot != std::string::npos && dot + 1 < colon && line[dot + 1] == 'L') {
              bool valid = true;
              for (size_t c = dot + 2; c < colon; ++c) {
                if (!std::isalnum(static_cast<unsigned char>(line[c])) && line[c] != '_') {
                  valid = false;
                  break;
                }
              }
              if (valid) {
                defined_labels.push_back(line.substr(dot, colon - dot));
              }
            }
          }
        }
        std::sort(defined_labels.begin(), defined_labels.end(),
                  [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        for (const auto& lbl : defined_labels) {
          std::string replacement = absl::StrCat(".L_sp_", span.name, "_", lbl.substr(2));
          for (auto& line : fn_lines) {
            replace_word_token(line, lbl, replacement);
          }
        }
      } else {
        for (size_t i = span.start_line; i <= span.end_line; ++i) {
          fn_lines.push_back(lines0[i]);
        }
      }
    }

    bool fn_has_wa_g = false;
    bool fn_has_wa_opt = false;
    for (const auto& f : fn_flags) {
      if (f == "-Wa,-g" || f == "-O0") {
        fn_has_wa_g = true;
      }
      if (absl::StartsWith(f, "-Wa,-O")) {
        fn_has_wa_opt = true;
      }
    }
    bool needs_nomove = fn_has_wa_g && !fn_has_wa_opt && overall_has_wa_opt;
    if (needs_nomove) {
      std::vector<std::string> wrapped_fn_lines;
      wrapped_fn_lines.reserve(fn_lines.size() + 2);
      std::string entry_label = absl::StrCat(span.name, ":");
      std::string end_directive1 = absl::StrCat(".end\t", span.name);
      std::string end_directive2 = absl::StrCat(".end ", span.name);
      for (const auto& l : fn_lines) {
        std::string_view trimmed = absl::StripAsciiWhitespace(l);
        if (trimmed == end_directive1 || trimmed == end_directive2) {
          wrapped_fn_lines.push_back("\t.set\tmove\n");
        }
        bool is_entry = (trimmed == entry_label);
        wrapped_fn_lines.push_back(l);
        if (is_entry) {
          wrapped_fn_lines.push_back("\t.set\tnomove\n");
        }
      }
      fn_lines = std::move(wrapped_fn_lines);
    }

    for (auto& l : fn_lines) {
      spliced_lines.push_back(std::move(l));
    }
    last_idx = span.end_line + 1;
  }

  for (size_t i = last_idx; i < lines0.size(); ++i) {
    spliced_lines.push_back(lines0[i]);
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
    for (const auto& line : spliced_lines) {
      ofs << line;
    }
  }

  // 6. Assemble spliced assembly via KMC AS.
  std::vector<std::filesystem::path> as_include_dirs;
  for (const auto& inc : options.include_dirs) {
    std::error_code ec;
    if (std::filesystem::exists(inc / "macro.inc", ec)) {
      as_include_dirs.push_back(inc);
    }
  }
  if (as_include_dirs.empty()) {
    as_include_dirs = options.include_dirs;
  }

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

  ProcessOptions as_opts{
      .program = toolchain_.KmcAs().string(),
      .args = std::move(as_args),
      .working_directory = toolchain_.RepoRoot(),
      .timeout = options.timeout,
  };

  auto as_result = runner_(as_opts);
  cleanup_temps();

  if (!as_result.ok()) {
    return as_result.status();
  }
  if (!as_result->Ok()) {
    return absl::InternalError(absl::StrFormat(
        "CompileCWithPerFunctionFlags: KMC AS assembly failed for '%s' (exit code "
        "%d):\n%s\n%s\nCommand: %s %s",
        options.src_file.string(), as_result->exit_code, as_result->stdout_output,
        as_result->stderr_output, as_opts.program, absl::StrJoin(as_opts.args, " ")));
  }

  return absl::OkStatus();
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

  if (options.out_obj.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_obj.parent_path(), ec);
    if (ec) {
      return absl::InternalError(
          absl::StrFormat("CompileCpp: Failed to create output directory '%s': %s",
                          options.out_obj.parent_path().string(), ec.message()));
    }
  }

  auto args = toolchain_.BuildModernCompileArgs(
      /*is_cpp=*/true, options.include_dirs, options.src_file, options.out_obj);

  ProcessOptions proc_opts{
      .program = toolchain_.GnuGxx().string(),
      .args = std::move(args),
      .working_directory = toolchain_.RepoRoot(),
      .timeout = options.timeout,
  };

  auto result = runner_(proc_opts);
  if (!result.ok()) {
    return result.status();
  }
  if (!result->Ok()) {
    return absl::InternalError(absl::StrFormat(
        "CompileCpp: Modern G++ compilation failed for '%s' (exit code %d):\n%s\n%s",
        options.src_file.string(), result->exit_code, result->stdout_output,
        result->stderr_output));
  }
  return absl::OkStatus();
}

absl::Status Compiler::Assemble(const AssemblyOptions& options) const {
  if (options.src_file.empty()) {
    return absl::InvalidArgumentError("Assemble: Source file path must not be empty.");
  }
  if (options.out_obj.empty()) {
    return absl::InvalidArgumentError("Assemble: Output object path must not be empty.");
  }

  if (options.out_obj.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_obj.parent_path(), ec);
    if (ec) {
      return absl::InternalError(
          absl::StrFormat("Assemble: Failed to create output directory '%s': %s",
                          options.out_obj.parent_path().string(), ec.message()));
    }
  }

  auto args = toolchain_.BuildGnuAsArgs(options.include_dirs, options.src_file, options.out_obj);

  ProcessOptions proc_opts{
      .program = toolchain_.GnuAs().string(),
      .args = std::move(args),
      .working_directory = toolchain_.RepoRoot(),
      .timeout = options.timeout,
  };

  auto result = runner_(proc_opts);
  if (!result.ok()) {
    return result.status();
  }
  if (!result->Ok()) {
    return absl::InternalError(
        absl::StrFormat("Assemble: GNU MIPS AS assembly failed for '%s' (exit code %d):\n%s\n%s",
                        options.src_file.string(), result->exit_code, result->stdout_output,
                        result->stderr_output));
  }
  return absl::OkStatus();
}

absl::Status Compiler::ConvertBinary(const BinaryAssetOptions& options) const {
  if (options.src_file.empty()) {
    return absl::InvalidArgumentError("ConvertBinary: Source file path must not be empty.");
  }
  if (options.out_obj.empty()) {
    return absl::InvalidArgumentError("ConvertBinary: Output object path must not be empty.");
  }

  if (options.out_obj.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_obj.parent_path(), ec);
    if (ec) {
      return absl::InternalError(
          absl::StrFormat("ConvertBinary: Failed to create output directory '%s': %s",
                          options.out_obj.parent_path().string(), ec.message()));
    }
  }

  auto args = toolchain_.BuildObjcopyBinaryArgs(options.src_file, options.out_obj);

  ProcessOptions proc_opts{
      .program = toolchain_.GnuObjcopy().string(),
      .args = std::move(args),
      .working_directory = toolchain_.RepoRoot(),
      .timeout = options.timeout,
  };

  auto result = runner_(proc_opts);
  if (!result.ok()) {
    return result.status();
  }
  if (!result->Ok()) {
    return absl::InternalError(absl::StrFormat(
        "ConvertBinary: GNU objcopy binary conversion failed for '%s' (exit code %d):\n%s\n%s",
        options.src_file.string(), result->exit_code, result->stdout_output,
        result->stderr_output));
  }
  return absl::OkStatus();
}

absl::Status Compiler::Link(const LinkOptions& options) const {
  if (options.main_ld_script.empty()) {
    return absl::InvalidArgumentError("Link: Main linker script path must not be empty.");
  }
  if (options.out_elf.empty()) {
    return absl::InvalidArgumentError("Link: Output ELF path must not be empty.");
  }

  if (options.out_elf.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_elf.parent_path(), ec);
    if (ec) {
      return absl::InternalError(absl::StrFormat("Link: Failed to create output directory '%s': %s",
                                                 options.out_elf.parent_path().string(),
                                                 ec.message()));
    }
  }

  auto args = toolchain_.BuildLdArgs(options.script_paths, options.main_ld_script, options.out_elf);

  ProcessOptions proc_opts{
      .program = toolchain_.GnuLd().string(),
      .args = std::move(args),
      .working_directory = toolchain_.RepoRoot(),
      .timeout = options.timeout,
  };

  auto result = runner_(proc_opts);
  if (!result.ok()) {
    return result.status();
  }
  if (!result->Ok()) {
    return absl::InternalError(absl::StrFormat(
        "Link: GNU LD link failed for '%s' (exit code %d):\n%s\n%s", options.out_elf.string(),
        result->exit_code, result->stdout_output, result->stderr_output));
  }
  return absl::OkStatus();
}

absl::Status Compiler::ExtractRom(const ExtractRomOptions& options) const {
  if (options.in_elf.empty()) {
    return absl::InvalidArgumentError("ExtractRom: Input ELF path must not be empty.");
  }
  if (options.out_rom.empty()) {
    return absl::InvalidArgumentError("ExtractRom: Output ROM path must not be empty.");
  }

  if (options.out_rom.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_rom.parent_path(), ec);
    if (ec) {
      return absl::InternalError(
          absl::StrFormat("ExtractRom: Failed to create output directory '%s': %s",
                          options.out_rom.parent_path().string(), ec.message()));
    }
  }

  auto args = toolchain_.BuildExtractRomArgs(options.in_elf, options.out_rom);

  ProcessOptions proc_opts{
      .program = toolchain_.GnuObjcopy().string(),
      .args = std::move(args),
      .working_directory = toolchain_.RepoRoot(),
      .timeout = options.timeout,
  };

  auto result = runner_(proc_opts);
  if (!result.ok()) {
    return result.status();
  }
  if (!result->Ok()) {
    return absl::InternalError(absl::StrFormat(
        "ExtractRom: GNU objcopy ROM extraction failed for '%s' (exit code %d):\n%s\n%s",
        options.out_rom.string(), result->exit_code, result->stdout_output, result->stderr_output));
  }
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
