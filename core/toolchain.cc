#include "core/toolchain.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"

namespace rom_nom_nom {

std::filesystem::path FindExecutableInPath(std::string_view name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }
  for (std::string_view dir : absl::StrSplit(path_env, ':')) {
    if (dir.empty()) {
      continue;
    }
    std::filesystem::path candidate = std::filesystem::path(dir) / name;
    if (std::filesystem::exists(candidate) && access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return {};
}

namespace {

std::filesystem::path ResolveTool(const std::filesystem::path& explicit_path,
                                  const std::filesystem::path& repo_root,
                                  std::string_view relative_or_venv, std::string_view system_name) {
  if (!explicit_path.empty() && std::filesystem::exists(explicit_path)) {
    return explicit_path;
  }
  if (!repo_root.empty()) {
    std::filesystem::path repo_candidate = repo_root / relative_or_venv;
    if (std::filesystem::exists(repo_candidate) && access(repo_candidate.c_str(), X_OK) == 0) {
      return repo_candidate;
    }
    std::filesystem::path venv_candidate = repo_root / ".venv" / "bin" / system_name;
    if (std::filesystem::exists(venv_candidate) && access(venv_candidate.c_str(), X_OK) == 0) {
      return venv_candidate;
    }
  }
  return FindExecutableInPath(system_name);
}

}  // namespace

absl::StatusOr<Toolchain> Toolchain::Discover(ToolchainOptions options) {
  if (options.repo_root.empty()) {
    const char* ws_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY");
    if (ws_dir != nullptr && *ws_dir != '\0') {
      options.repo_root = ws_dir;
    } else {
      options.repo_root = std::filesystem::current_path();
    }
  }

  Toolchain toolchain(options);

  // GNU common tools required in all modes
  toolchain.gnu_as_path_ = ResolveTool(options.gnu_as_path, options.repo_root,
                                       "bin/mips-linux-gnu-as", "mips-linux-gnu-as");
  if (toolchain.gnu_as_path_.empty()) {
    return absl::NotFoundError(
        "Toolchain::Discover: 'mips-linux-gnu-as' not found in PATH or repository .venv.");
  }

  toolchain.gnu_ld_path_ = ResolveTool(options.gnu_ld_path, options.repo_root,
                                       "bin/mips-linux-gnu-ld", "mips-linux-gnu-ld");
  if (toolchain.gnu_ld_path_.empty()) {
    return absl::NotFoundError(
        "Toolchain::Discover: 'mips-linux-gnu-ld' not found in PATH or repository .venv.");
  }

  toolchain.gnu_objcopy_path_ = ResolveTool(options.gnu_objcopy_path, options.repo_root,
                                            "bin/mips-linux-gnu-objcopy", "mips-linux-gnu-objcopy");
  if (toolchain.gnu_objcopy_path_.empty()) {
    return absl::NotFoundError(
        "Toolchain::Discover: 'mips-linux-gnu-objcopy' not found in PATH or repository .venv.");
  }

  if (options.mode == ToolchainMode::kOriginal) {
    toolchain.gcc_272_path_ = ResolveTool(options.gcc_272_path, options.repo_root,
                                          "toolchain/gcc-2.7.2/gcc", "gcc-2.7.2");
    if (toolchain.gcc_272_path_.empty()) {
      return absl::NotFoundError(
          "Toolchain::Discover: KMC GCC 2.7.2 compiler ('toolchain/gcc-2.7.2/gcc') not found. "
          "Run 'bazel run //tools:install_toolchain' to download it.");
    }
    toolchain.gcc_272_dir_ = toolchain.gcc_272_path_.parent_path();

    toolchain.kmc_as_path_ =
        ResolveTool(options.kmc_as_path, options.repo_root, "toolchain/binutils-2.6/as", "kmc-as");
    if (toolchain.kmc_as_path_.empty()) {
      // Check if gcc-2.7.2 directory has symlink 'as'
      std::filesystem::path gcc_as = toolchain.gcc_272_dir_ / "as";
      if (std::filesystem::exists(gcc_as) && access(gcc_as.c_str(), X_OK) == 0) {
        toolchain.kmc_as_path_ = gcc_as;
      }
    }
    if (toolchain.kmc_as_path_.empty()) {
      return absl::NotFoundError(
          "Toolchain::Discover: KMC binutils 2.6 assembler ('toolchain/binutils-2.6/as') not "
          "found. "
          "Run 'bazel run //tools:install_toolchain' to download it.");
    }
  } else {
    toolchain.gnu_gcc_path_ = ResolveTool(options.gnu_gcc_path, options.repo_root,
                                          "bin/mips-linux-gnu-gcc", "mips-linux-gnu-gcc");
    if (toolchain.gnu_gcc_path_.empty()) {
      return absl::NotFoundError(
          "Toolchain::Discover: Modern compiler 'mips-linux-gnu-gcc' not found in PATH.");
    }
    toolchain.gnu_gxx_path_ = ResolveTool(options.gnu_gxx_path, options.repo_root,
                                          "bin/mips-linux-gnu-g++", "mips-linux-gnu-g++");
    if (toolchain.gnu_gxx_path_.empty()) {
      return absl::NotFoundError(
          "Toolchain::Discover: Modern compiler 'mips-linux-gnu-g++' not found in PATH.");
    }
  }

  return toolchain;
}

std::vector<std::string> Toolchain::BuildGcc272CompileArgs(
    const std::vector<std::string>& opt_flags,
    const std::vector<std::filesystem::path>& include_dirs, const std::filesystem::path& src_file,
    const std::filesystem::path& out_s272) const {
  std::vector<std::string> args;
  args.push_back(absl::StrCat("-B", gcc_272_dir_.string(), "/"));
  args.push_back("-x");
  args.push_back("c");
  args.push_back("-S");

  for (const auto& f : opt_flags) {
    if (!absl::StartsWith(f, "-Wa,")) {
      args.push_back(f);
    }
  }

  args.push_back("-G");
  args.push_back("0");

  for (const auto& inc : include_dirs) {
    args.push_back(absl::StrCat("-I", inc.string()));
  }

  args.push_back(src_file.string());
  args.push_back("-o");
  args.push_back(out_s272.string());

  return args;
}

std::vector<std::string> Toolchain::BuildKmcAsArgs(
    const std::vector<std::string>& opt_flags,
    const std::vector<std::filesystem::path>& include_dirs, const std::filesystem::path& macro_inc,
    const std::filesystem::path& in_s272, const std::filesystem::path& out_obj) const {
  std::vector<std::string> args;

  std::string mcpu = "-mcpu=vr4300";
  std::string mips_isa = "-mips3";
  bool is_o0 = false;
  std::vector<std::string> as_extra_flags;

  for (const auto& f : opt_flags) {
    if (absl::StartsWith(f, "-mcpu=")) {
      mcpu = f;
    } else if (f == "-mips2") {
      mips_isa = "-mips2";
    } else if (f == "-O0") {
      is_o0 = true;
    } else if (absl::StartsWith(f, "-Wa,")) {
      std::string_view rest = f;
      rest.remove_prefix(4);
      for (std::string_view part : absl::StrSplit(rest, ',')) {
        if (!part.empty()) {
          as_extra_flags.emplace_back(part);
        }
      }
    }
  }

  args.push_back(mcpu);
  args.push_back(mips_isa);
  args.push_back("-EB");
  args.push_back("-G");
  args.push_back("0");
  args.push_back("-N");

  if (is_o0) {
    bool has_opt_or_g = false;
    for (const auto& f : as_extra_flags) {
      if (absl::StartsWith(f, "-O") || f == "-g") {
        has_opt_or_g = true;
        break;
      }
    }
    if (!has_opt_or_g) {
      args.push_back("-O0");
    }
  }

  for (const auto& f : as_extra_flags) {
    args.push_back(f);
  }

  for (const auto& inc : include_dirs) {
    args.push_back(absl::StrCat("-I", inc.string()));
  }

  if (!macro_inc.empty()) {
    args.push_back(macro_inc.string());
  }

  args.push_back(in_s272.string());
  args.push_back("-o");
  args.push_back(out_obj.string());

  return args;
}

std::vector<std::string> Toolchain::BuildModernCompileArgs(
    bool is_cpp, const std::vector<std::filesystem::path>& include_dirs,
    const std::filesystem::path& src_file, const std::filesystem::path& out_obj) const {
  std::vector<std::string> args = {
      "-c",
      "-march=vr4300",
      "-mabi=32",
      "-EB",
      "-fno-PIC",
      "-mno-abicalls",
      "-ffreestanding",
      "-DMODERN_TOOLCHAIN=1",
      "-DNON_MATCHING=1",
  };

  for (const auto& inc : include_dirs) {
    args.push_back(absl::StrCat("-I", inc.string()));
  }

  args.push_back(src_file.string());
  args.push_back("-o");
  args.push_back(out_obj.string());

  return args;
}

std::vector<std::string> Toolchain::BuildGnuAsArgs(
    const std::vector<std::filesystem::path>& include_dirs, const std::filesystem::path& in_s,
    const std::filesystem::path& out_obj) const {
  std::vector<std::string> args = {
      "-march=vr4300",
      "-mabi=32",
      "-EB",
  };

  for (const auto& inc : include_dirs) {
    args.push_back(absl::StrCat("-I", inc.string()));
  }

  args.push_back(in_s.string());
  args.push_back("-o");
  args.push_back(out_obj.string());

  return args;
}

std::vector<std::string> Toolchain::BuildObjcopyBinaryArgs(
    const std::filesystem::path& in_bin, const std::filesystem::path& out_obj) const {
  return {
      "-I", "binary", "-O", "elf32-tradbigmips", "-B", "mips", in_bin.string(), out_obj.string(),
  };
}

std::vector<std::string> Toolchain::BuildExtractRomArgs(
    const std::filesystem::path& in_elf, const std::filesystem::path& out_rom) const {
  return {
      "-O",
      "binary",
      in_elf.string(),
      out_rom.string(),
  };
}

std::vector<std::string> Toolchain::BuildLdArgs(
    const std::vector<std::filesystem::path>& script_paths,
    const std::filesystem::path& main_ld_script, const std::filesystem::path& out_elf) const {
  std::vector<std::string> args;
  for (const auto& p : script_paths) {
    if (!p.empty()) {
      args.push_back("-T");
      args.push_back(p.string());
    }
  }
  args.push_back("-T");
  args.push_back(main_ld_script.string());
  args.push_back("--no-check-sections");
  args.push_back("-o");
  args.push_back(out_elf.string());
  return args;
}

}  // namespace rom_nom_nom
