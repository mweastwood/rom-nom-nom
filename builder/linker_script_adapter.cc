#include "builder/linker_script_adapter.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace rom_nom_nom {
namespace {

std::filesystem::path FindFileShallowOrRecursive(const std::filesystem::path& dir,
                                                 std::string_view filename) {
  std::error_code ec;
  std::filesystem::path direct = dir / filename;
  if (std::filesystem::exists(direct, ec)) {
    return direct;
  }
  if (!std::filesystem::exists(dir, ec)) {
    return {};
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (entry.is_regular_file() && entry.path().filename() == filename) {
      return entry.path();
    }
  }
  return {};
}

}  // namespace

std::vector<std::filesystem::path> AuxiliaryScripts::ToList() const {
  std::vector<std::filesystem::path> list;
  if (!symbols_ld.empty()) {
    list.push_back(symbols_ld);
  }
  if (!hardware_regs_ld.empty()) {
    list.push_back(hardware_regs_ld);
  }
  if (!undefined_syms_auto.empty()) {
    list.push_back(undefined_syms_auto);
  }
  if (!undefined_funcs_auto.empty()) {
    list.push_back(undefined_funcs_auto);
  }
  return list;
}

LinkerScriptAdapter::LinkerScriptAdapter(std::string content) : content_(std::move(content)) {
  ExtractObjectReferences();
}

absl::StatusOr<LinkerScriptAdapter> LinkerScriptAdapter::Parse(std::string_view content) {
  return LinkerScriptAdapter(std::string(content));
}

absl::StatusOr<LinkerScriptAdapter> LinkerScriptAdapter::Load(
    const std::filesystem::path& script_path) {
  std::error_code ec;
  if (!std::filesystem::exists(script_path, ec)) {
    return absl::NotFoundError(
        absl::StrFormat("LinkerScriptAdapter: File '%s' not found.", script_path.string()));
  }
  std::ifstream ifs(script_path);
  if (!ifs) {
    return absl::InternalError(
        absl::StrFormat("LinkerScriptAdapter: Failed to open '%s'.", script_path.string()));
  }
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  return Parse(content);
}

absl::StatusOr<std::filesystem::path> LinkerScriptAdapter::FindMainScript(
    const std::filesystem::path& build_dir, std::string_view game_name) {
  std::error_code ec;
  if (!game_name.empty()) {
    std::filesystem::path direct = build_dir / absl::StrCat(game_name, ".ld");
    if (std::filesystem::exists(direct, ec)) {
      return direct;
    }
  }

  if (std::filesystem::exists(build_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(build_dir, ec)) {
      if (entry.is_regular_file() && entry.path().extension() == ".ld") {
        std::string fname = entry.path().filename().string();
        if (fname != "symbols.ld" && fname != "hardware_regs.ld") {
          return entry.path();
        }
      }
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(build_dir, ec)) {
      if (entry.is_regular_file() && entry.path().extension() == ".ld") {
        std::string fname = entry.path().filename().string();
        if (fname != "symbols.ld" && fname != "hardware_regs.ld") {
          return entry.path();
        }
      }
    }
  }

  return absl::NotFoundError(
      absl::StrFormat("LinkerScriptAdapter: Linker script not found in '%s'", build_dir.string()));
}

AuxiliaryScripts LinkerScriptAdapter::DiscoverAuxiliaryScripts(
    const std::filesystem::path& build_dir) {
  AuxiliaryScripts scripts;
  scripts.symbols_ld = FindFileShallowOrRecursive(build_dir, "symbols.ld");
  scripts.hardware_regs_ld = FindFileShallowOrRecursive(build_dir, "hardware_regs.ld");
  scripts.undefined_syms_auto = FindFileShallowOrRecursive(build_dir, "undefined_syms_auto.txt");
  scripts.undefined_funcs_auto = FindFileShallowOrRecursive(build_dir, "undefined_funcs_auto.txt");
  return scripts;
}

void LinkerScriptAdapter::ExtractObjectReferences() {
  static const std::regex obj_regex(R"([^\s()]+\.o)");
  std::set<std::string> unique_refs;

  auto words_begin = std::sregex_iterator(content_.begin(), content_.end(), obj_regex);
  auto words_end = std::sregex_iterator();

  for (auto it = words_begin; it != words_end; ++it) {
    unique_refs.insert(it->str());
  }

  object_refs_.assign(unique_refs.begin(), unique_refs.end());
}

std::string LinkerScriptAdapter::RewriteObjectPathsWithMapping(
    const std::function<std::filesystem::path(std::string_view original_ref)>& mapper) const {
  static const std::regex obj_regex(R"([^\s()]+\.o)");
  std::string result;
  auto words_begin = std::sregex_iterator(content_.begin(), content_.end(), obj_regex);
  auto words_end = std::sregex_iterator();

  size_t last_pos = 0;
  for (auto it = words_begin; it != words_end; ++it) {
    const std::smatch& match = *it;
    result.append(content_, last_pos, match.position() - last_pos);
    std::string original = match.str();
    std::filesystem::path mapped = mapper(original);
    result.append(mapped.string());
    last_pos = match.position() + match.length();
  }
  result.append(content_, last_pos, content_.size() - last_pos);
  return result;
}

std::string LinkerScriptAdapter::RewriteObjectPaths(const std::filesystem::path& obj_dir) const {
  return RewriteObjectPathsWithMapping([&](std::string_view original_ref) {
    std::filesystem::path orig_path(original_ref);
    return obj_dir / orig_path.filename();
  });
}

}  // namespace rom_nom_nom
