#ifndef ROM_NOM_NOM_BUILDER_LINKER_SCRIPT_ADAPTER_H_
#define ROM_NOM_NOM_BUILDER_LINKER_SCRIPT_ADAPTER_H_

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace rom_nom_nom {

// Discovered auxiliary linker scripts and symbol files required during linking.
struct AuxiliaryScripts {
  std::filesystem::path symbols_ld;
  std::filesystem::path hardware_regs_ld;
  std::filesystem::path undefined_syms_auto;
  std::filesystem::path undefined_funcs_auto;

  // Returns all existing auxiliary script paths in the linker command order.
  std::vector<std::filesystem::path> ToList() const;
};

// Parses, inspects, and adapts GNU LD linker scripts for build sandboxing.
class LinkerScriptAdapter {
 public:
  // Parses a linker script from an in-memory string.
  static absl::StatusOr<LinkerScriptAdapter> Parse(std::string_view content);

  // Reads and parses a linker script from a file on disk.
  static absl::StatusOr<LinkerScriptAdapter> Load(const std::filesystem::path& script_path);

  // Finds the main linker script for a game in build_dir.
  // Checks build_dir / "<game_name>.ld", then falls back to any primary .ld file in build_dir.
  static absl::StatusOr<std::filesystem::path> FindMainScript(
      const std::filesystem::path& build_dir, std::string_view game_name);

  // Discovers auxiliary linker scripts and symbol files (symbols.ld, hardware_regs.ld,
  // undefined_syms_auto.txt, undefined_funcs_auto.txt) within build_dir.
  static AuxiliaryScripts DiscoverAuxiliaryScripts(const std::filesystem::path& build_dir);

  // Returns all distinct object references (*.o) found in the linker script in sorted order.
  const std::vector<std::string>& ObjectReferences() const { return object_refs_; }

  // Rewrites all object references to point into obj_dir:
  // For each reference "path/to/stem.o", replaces it with "obj_dir/stem.o".
  std::string RewriteObjectPaths(const std::filesystem::path& obj_dir) const;

  // Rewrites object references using a custom mapping function.
  std::string RewriteObjectPathsWithMapping(
      const std::function<std::filesystem::path(std::string_view original_ref)>& mapper) const;

  // Original linker script content.
  std::string_view Content() const { return content_; }

 private:
  explicit LinkerScriptAdapter(std::string content);

  void ExtractObjectReferences();

  std::string content_;
  std::vector<std::string> object_refs_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_BUILDER_LINKER_SCRIPT_ADAPTER_H_
