#ifndef ROM_NOM_NOM_BUILDER_SOURCE_RESOLVER_H_
#define ROM_NOM_NOM_BUILDER_SOURCE_RESOLVER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"

namespace rom_nom_nom {

// Type of compilation unit corresponding to a resolved source file.
enum class SourceType {
  kC,
  kCpp,
  kAssembly,
  kBinary,
};

// Returns a human-readable string representation of a SourceType.
std::string_view SourceTypeName(SourceType type);

// Result of resolving an object reference to a source file.
struct ResolvedSource {
  std::filesystem::path source_path;
  SourceType type;
  std::string object_stem;
};

// Configuration options specifying search directories for source resolution.
struct SourceResolverOptions {
  std::filesystem::path src_dir;
  std::filesystem::path asm_dir;
  std::filesystem::path assets_dir;
  bool prefer_c = false;
};

// Maps object file references from linker scripts to concrete source files on disk.
class SourceResolver {
 public:
  explicit SourceResolver(SourceResolverOptions options);

  // Resolves the source file corresponding to an object reference token from a linker script.
  // Respects splitter subsegment hints if present in obj_ref:
  // - "/src/" or "src/" prefers C/C++ candidates
  // - "/asm/" or "asm/" prefers assembly (.s) candidates
  // - "/assets/" or "assets/" prefers binary (.bin) candidates
  // If no hint is matched or the hinted candidate is missing, falls back to priority:
  // C/C++ -> Assembly -> Binary.
  absl::StatusOr<ResolvedSource> Resolve(std::string_view obj_ref) const;

  // Resolves a batch of object references in order.
  absl::StatusOr<std::vector<ResolvedSource>> ResolveAll(
      const std::vector<std::string>& obj_refs) const;

  const SourceResolverOptions& Options() const { return options_; }

 private:
  void IndexFiles();

  SourceResolverOptions options_;

  // Pre-indexed map of stem -> list of candidate paths
  absl::flat_hash_map<std::string, std::vector<std::filesystem::path>> c_sources_;
  absl::flat_hash_map<std::string, std::vector<std::filesystem::path>> cpp_sources_;
  absl::flat_hash_map<std::string, std::vector<std::filesystem::path>> asm_sources_;
  absl::flat_hash_map<std::string, std::vector<std::filesystem::path>> bin_sources_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_BUILDER_SOURCE_RESOLVER_H_
