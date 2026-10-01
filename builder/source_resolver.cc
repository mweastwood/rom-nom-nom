#include "builder/source_resolver.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace rom_nom_nom {
namespace {

const std::filesystem::path* FindBestCandidate(const std::vector<std::filesystem::path>& candidates,
                                               std::string_view obj_ref) {
  if (candidates.empty()) {
    return nullptr;
  }
  if (candidates.size() == 1) {
    return &candidates[0];
  }
  std::filesystem::path obj_path(obj_ref);
  std::filesystem::path parent = obj_path.parent_path();
  if (!parent.empty()) {
    std::string parent_str = parent.string();
    for (const auto& c : candidates) {
      if (absl::StrContains(c.string(), parent_str)) {
        return &c;
      }
    }
  }
  return &candidates[0];
}

}  // namespace

std::string_view SourceTypeName(SourceType type) {
  switch (type) {
    case SourceType::kC:
      return "C";
    case SourceType::kCpp:
      return "C++";
    case SourceType::kAssembly:
      return "Assembly";
    case SourceType::kBinary:
      return "Binary";
  }
  return "Unknown";
}

SourceResolver::SourceResolver(SourceResolverOptions options) : options_(std::move(options)) {
  IndexFiles();
}

void SourceResolver::IndexFiles() {
  std::error_code ec;

  if (!options_.src_dir.empty() && std::filesystem::exists(options_.src_dir, ec)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(options_.src_dir, ec)) {
      if (entry.is_regular_file()) {
        const auto& path = entry.path();
        const std::string ext = path.extension().string();
        const std::string stem = path.stem().string();
        if (ext == ".c") {
          c_sources_[stem].push_back(path);
        } else if (ext == ".cc" || ext == ".cpp") {
          cpp_sources_[stem].push_back(path);
        }
      }
    }
  }

  if (!options_.asm_dir.empty() && std::filesystem::exists(options_.asm_dir, ec)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(options_.asm_dir, ec)) {
      if (entry.is_regular_file()) {
        const auto& path = entry.path();
        if (path.extension() == ".s") {
          asm_sources_[path.stem().string()].push_back(path);
        }
      }
    }
  }

  if (!options_.assets_dir.empty() && std::filesystem::exists(options_.assets_dir, ec)) {
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(options_.assets_dir, ec)) {
      if (entry.is_regular_file()) {
        const auto& path = entry.path();
        if (path.extension() == ".bin") {
          bin_sources_[path.stem().string()].push_back(path);
        }
      }
    }
  }

  for (auto& [stem, paths] : c_sources_) {
    std::sort(paths.begin(), paths.end());
  }
  for (auto& [stem, paths] : cpp_sources_) {
    std::sort(paths.begin(), paths.end());
  }
  for (auto& [stem, paths] : asm_sources_) {
    std::sort(paths.begin(), paths.end());
  }
  for (auto& [stem, paths] : bin_sources_) {
    std::sort(paths.begin(), paths.end());
  }
}

absl::StatusOr<ResolvedSource> SourceResolver::Resolve(std::string_view obj_ref) const {
  if (obj_ref.empty()) {
    return absl::InvalidArgumentError("SourceResolver: Object reference cannot be empty.");
  }

  std::filesystem::path obj_path(obj_ref);
  std::string stem = obj_path.stem().string();
  if (stem.empty()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("SourceResolver: Invalid object reference '%s' with empty stem.", obj_ref));
  }

  auto c_it = c_sources_.find(stem);
  auto cpp_it = cpp_sources_.find(stem);
  auto asm_it = asm_sources_.find(stem);
  auto bin_it = bin_sources_.find(stem);

  const std::vector<std::filesystem::path>* c_candidates =
      (c_it != c_sources_.end()) ? &c_it->second : nullptr;
  const std::vector<std::filesystem::path>* cpp_candidates =
      (cpp_it != cpp_sources_.end()) ? &cpp_it->second : nullptr;
  const std::vector<std::filesystem::path>* asm_candidates =
      (asm_it != asm_sources_.end()) ? &asm_it->second : nullptr;
  const std::vector<std::filesystem::path>* bin_candidates =
      (bin_it != bin_sources_.end()) ? &bin_it->second : nullptr;

  bool has_c = (c_candidates != nullptr && !c_candidates->empty());
  bool has_cpp = (cpp_candidates != nullptr && !cpp_candidates->empty());
  bool has_asm = (asm_candidates != nullptr && !asm_candidates->empty());
  bool has_bin = (bin_candidates != nullptr && !bin_candidates->empty());

  bool hint_src = absl::StrContains(obj_ref, "/src/") || absl::StartsWith(obj_ref, "src/");
  bool hint_asm = absl::StrContains(obj_ref, "/asm/") || absl::StartsWith(obj_ref, "asm/");
  bool hint_assets = absl::StrContains(obj_ref, "/assets/") || absl::StartsWith(obj_ref, "assets/");

  bool use_c = false;
  bool use_cpp = false;
  bool use_asm = false;
  bool use_bin = false;

  if (hint_src) {
    use_c = has_c;
    use_cpp = !use_c && has_cpp;
  } else if (hint_asm) {
    use_asm = has_asm;
  } else if (hint_assets) {
    use_bin = has_bin;
  }

  if (!use_c && !use_cpp && !use_asm && !use_bin) {
    if (has_c) {
      use_c = true;
    } else if (has_cpp) {
      use_cpp = true;
    } else if (has_asm) {
      use_asm = true;
    } else if (has_bin) {
      use_bin = true;
    }
  }

  if (use_c) {
    const auto* best = FindBestCandidate(*c_candidates, obj_ref);
    return ResolvedSource{.source_path = *best, .type = SourceType::kC, .object_stem = stem};
  }
  if (use_cpp) {
    const auto* best = FindBestCandidate(*cpp_candidates, obj_ref);
    return ResolvedSource{.source_path = *best, .type = SourceType::kCpp, .object_stem = stem};
  }
  if (use_asm) {
    const auto* best = FindBestCandidate(*asm_candidates, obj_ref);
    return ResolvedSource{.source_path = *best, .type = SourceType::kAssembly, .object_stem = stem};
  }
  if (use_bin) {
    const auto* best = FindBestCandidate(*bin_candidates, obj_ref);
    return ResolvedSource{.source_path = *best, .type = SourceType::kBinary, .object_stem = stem};
  }

  return absl::NotFoundError(absl::StrFormat("Source file not found for object: %s", obj_ref));
}

absl::StatusOr<std::vector<ResolvedSource>> SourceResolver::ResolveAll(
    const std::vector<std::string>& obj_refs) const {
  std::vector<ResolvedSource> results;
  results.reserve(obj_refs.size());
  for (const auto& obj_ref : obj_refs) {
    auto resolved_or = Resolve(obj_ref);
    if (!resolved_or.ok()) {
      return resolved_or.status();
    }
    results.push_back(std::move(*resolved_or));
  }
  return results;
}

}  // namespace rom_nom_nom
