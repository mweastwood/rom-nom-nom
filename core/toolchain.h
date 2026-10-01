#ifndef ROM_NOM_NOM_CORE_TOOLCHAIN_H_
#define ROM_NOM_NOM_CORE_TOOLCHAIN_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace rom_nom_nom {

// Compilation toolchain mode.
enum class ToolchainMode {
  kOriginal,  // Authentic KMC GCC 2.7.2 + KMC AS
  kModern,    // GNU MIPS GCC / G++ (-march=vr4300)
};

// Options for discovering and configuring toolchain binaries.
struct ToolchainOptions {
  std::filesystem::path repo_root;
  ToolchainMode mode = ToolchainMode::kOriginal;

  // Optional explicit overrides for specific binary paths
  std::filesystem::path gcc_272_path;
  std::filesystem::path kmc_as_path;
  std::filesystem::path gnu_as_path;
  std::filesystem::path gnu_ld_path;
  std::filesystem::path gnu_objcopy_path;
  std::filesystem::path gnu_gcc_path;
  std::filesystem::path gnu_gxx_path;
};

// Represents an installed and validated N64 cross-compilation toolchain.
class Toolchain {
 public:
  // Discovers and validates all required toolchain binaries based on options.
  static absl::StatusOr<Toolchain> Discover(ToolchainOptions options);

  ToolchainMode Mode() const { return options_.mode; }
  const std::filesystem::path& RepoRoot() const { return options_.repo_root; }

  const std::filesystem::path& Gcc272() const { return gcc_272_path_; }
  const std::filesystem::path& Gcc272Dir() const { return gcc_272_dir_; }
  const std::filesystem::path& KmcAs() const { return kmc_as_path_; }
  const std::filesystem::path& GnuAs() const { return gnu_as_path_; }
  const std::filesystem::path& GnuLd() const { return gnu_ld_path_; }
  const std::filesystem::path& GnuObjcopy() const { return gnu_objcopy_path_; }
  const std::filesystem::path& GnuGcc() const { return gnu_gcc_path_; }
  const std::filesystem::path& GnuGxx() const { return gnu_gxx_path_; }

  // Constructs the command arguments for compiling C with GCC 2.7.2.
  std::vector<std::string> BuildGcc272CompileArgs(
      const std::vector<std::string>& opt_flags,
      const std::vector<std::filesystem::path>& include_dirs, const std::filesystem::path& src_file,
      const std::filesystem::path& out_s272) const;

  // Constructs the command arguments for assembling with KMC AS.
  std::vector<std::string> BuildKmcAsArgs(const std::vector<std::string>& opt_flags,
                                          const std::vector<std::filesystem::path>& include_dirs,
                                          const std::filesystem::path& macro_inc,
                                          const std::filesystem::path& in_s272,
                                          const std::filesystem::path& out_obj) const;

  // Constructs the command arguments for compiling with modern MIPS GCC/G++.
  std::vector<std::string> BuildModernCompileArgs(
      bool is_cpp, const std::vector<std::filesystem::path>& include_dirs,
      const std::filesystem::path& src_file, const std::filesystem::path& out_obj) const;

  // Constructs the command arguments for assembling with GNU MIPS AS.
  std::vector<std::string> BuildGnuAsArgs(const std::vector<std::filesystem::path>& include_dirs,
                                          const std::filesystem::path& in_s,
                                          const std::filesystem::path& out_obj) const;

  // Constructs arguments for converting raw binary asset to ELF object.
  std::vector<std::string> BuildObjcopyBinaryArgs(const std::filesystem::path& in_bin,
                                                  const std::filesystem::path& out_obj) const;

  // Constructs arguments for extracting raw ROM binary from linked ELF.
  std::vector<std::string> BuildExtractRomArgs(const std::filesystem::path& in_elf,
                                               const std::filesystem::path& out_rom) const;

  // Constructs arguments for linking the ELF binary with GNU LD.
  std::vector<std::string> BuildLdArgs(const std::vector<std::filesystem::path>& script_paths,
                                       const std::filesystem::path& main_ld_script,
                                       const std::filesystem::path& out_elf) const;

 private:
  explicit Toolchain(ToolchainOptions options) : options_(std::move(options)) {}

  ToolchainOptions options_;
  std::filesystem::path gcc_272_path_;
  std::filesystem::path gcc_272_dir_;
  std::filesystem::path kmc_as_path_;
  std::filesystem::path gnu_as_path_;
  std::filesystem::path gnu_ld_path_;
  std::filesystem::path gnu_objcopy_path_;
  std::filesystem::path gnu_gcc_path_;
  std::filesystem::path gnu_gxx_path_;
};

// Searches the filesystem PATH environment variable for an executable by name.
std::filesystem::path FindExecutableInPath(std::string_view name);

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_CORE_TOOLCHAIN_H_
