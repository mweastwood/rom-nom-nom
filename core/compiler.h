#ifndef ROM_NOM_NOM_CORE_COMPILER_H_
#define ROM_NOM_NOM_CORE_COMPILER_H_

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "core/process.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

// Finds the macro include file for a game in repo_root / "macros" / "<game_name>_macro.inc".
std::filesystem::path FindMacroInc(const std::filesystem::path& repo_root,
                                   std::string_view game_name);

// Options for compiling a C source file.
struct CCompileOptions {
  std::filesystem::path src_file;
  std::filesystem::path out_obj;
  std::vector<std::string> opt_flags;
  std::vector<std::filesystem::path> include_dirs;
  std::filesystem::path macro_inc;
  absl::Duration timeout = absl::ZeroDuration();
};

// Options for compiling a C++ source file.
struct CppCompileOptions {
  std::filesystem::path src_file;
  std::filesystem::path out_obj;
  std::vector<std::filesystem::path> include_dirs;
  absl::Duration timeout = absl::ZeroDuration();
};

// Options for assembling a MIPS assembly source file.
struct AssemblyOptions {
  std::filesystem::path src_file;
  std::filesystem::path out_obj;
  std::vector<std::filesystem::path> include_dirs;
  absl::Duration timeout = absl::ZeroDuration();
};

// Options for converting a raw binary asset into an ELF object file.
struct BinaryAssetOptions {
  std::filesystem::path src_file;
  std::filesystem::path out_obj;
  absl::Duration timeout = absl::ZeroDuration();
};

// Options for linking objects into an ELF binary.
struct LinkOptions {
  std::vector<std::filesystem::path> script_paths;
  std::filesystem::path main_ld_script;
  std::filesystem::path out_elf;
  absl::Duration timeout = absl::ZeroDuration();
};

// Options for extracting a raw ROM binary from an ELF.
struct ExtractRomOptions {
  std::filesystem::path in_elf;
  std::filesystem::path out_rom;
  absl::Duration timeout = absl::ZeroDuration();
};

// Orchestrates single-unit compilation, assembly, and conversion tasks.
class Compiler {
 public:
  using ProcessRunner = std::function<absl::StatusOr<ProcessResult>(const ProcessOptions&)>;

  explicit Compiler(Toolchain toolchain, ProcessRunner runner = &RunProcess);

  const Toolchain& GetToolchain() const { return toolchain_; }

  // Compiles a C source file to an object file (.c -> .o).
  // In original toolchain mode: executes GCC 2.7.2 to generate a temporary .s272 assembly
  // file, then executes KMC AS to assemble it to .o, and removes the temporary .s272 file.
  // In modern toolchain mode: executes modern GNU MIPS GCC directly to .o.
  absl::Status CompileC(const CCompileOptions& options) const;

  // Compiles a C++ source file to an object file (.cc/.cpp -> .o).
  // Requires modern toolchain mode.
  absl::Status CompileCpp(const CppCompileOptions& options) const;

  // Assembles a MIPS assembly file to an object file (.s -> .o) using GNU MIPS AS.
  absl::Status Assemble(const AssemblyOptions& options) const;

  // Converts a raw binary asset to an ELF object file (.bin -> .o) using GNU objcopy.
  absl::Status ConvertBinary(const BinaryAssetOptions& options) const;

  // Links object files using GNU LD into an ELF binary.
  absl::Status Link(const LinkOptions& options) const;

  // Extracts raw ROM binary from an ELF binary using GNU objcopy.
  absl::Status ExtractRom(const ExtractRomOptions& options) const;

 private:
  Toolchain toolchain_;
  ProcessRunner runner_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_CORE_COMPILER_H_
