#ifndef ROM_NOM_NOM_CORE_ASSEMBLY_ALIGNER_H_
#define ROM_NOM_NOM_CORE_ASSEMBLY_ALIGNER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "core/process.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

// Line range [start_line, end_line] (inclusive) of a `.ent <name>` .. `.end <name>` block in a
// `.s272` assembly file.
struct AssemblyFunctionSpan {
  std::string name;
  size_t start_line = 0;
  size_t end_line = 0;
};

// Reads all lines (retaining trailing '\n') from an assembly file.
absl::StatusOr<std::vector<std::string>> ReadAssemblyLines(const std::filesystem::path& path);

// Parses `.ent <name>` .. `.end <name>` (including trailing `.size`) function spans from assembly
// lines.
absl::flat_hash_map<std::string, AssemblyFunctionSpan> ParseAssemblyFunctions(
    const std::vector<std::string>& lines);

// Splices per-function assembly blocks from multiple flag-set compilations into the base (index 0)
// assembly file, namespacing local `.L` labels and wrapping `-Wa,-g` functions in `.set nomove` /
// `.set move` when assembled under `-Wa,-O2`.
absl::StatusOr<std::vector<std::string>> SplicePerFunctionAssembly(
    const std::vector<std::filesystem::path>& s272_files,
    const std::vector<std::vector<std::string>>& unique_flag_sets,
    const absl::flat_hash_map<std::string, size_t>& func_to_flag_set, bool overall_has_wa_opt);

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_CORE_ASSEMBLY_ALIGNER_H_
