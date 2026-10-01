#include "core/compiler.h"

#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

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

    auto as_args = toolchain_.BuildKmcAsArgs(options.opt_flags, options.include_dirs,
                                             options.macro_inc, temp_s272, options.out_obj);

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
      return absl::InternalError(
          absl::StrFormat("CompileC: KMC AS assembly failed for '%s' (exit code %d):\n%s\n%s",
                          options.src_file.string(), as_result->exit_code, as_result->stdout_output,
                          as_result->stderr_output));
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
