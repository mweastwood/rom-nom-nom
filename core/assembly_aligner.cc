#include "core/assembly_aligner.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include "core/elf_reader.h"
#include "core/process.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

absl::StatusOr<std::vector<std::string>> ReadAssemblyLines(const std::filesystem::path& path) {
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
}

absl::flat_hash_map<std::string, AssemblyFunctionSpan> ParseAssemblyFunctions(
    const std::vector<std::string>& lines) {
  absl::flat_hash_map<std::string, AssemblyFunctionSpan> funcs;
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
              if (absl::StartsWith(kl, ".size\t") || absl::StartsWith(kl, ".size ")) {
                std::string_view rest = absl::StripLeadingAsciiWhitespace(kl.substr(5));
                if (absl::StartsWith(rest, fn_name)) {
                  end_idx = k;
                  break;
                }
              }
            }
            break;
          }
        }
        if (found_end) {
          funcs[fn_name] = AssemblyFunctionSpan{
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
}

absl::StatusOr<std::vector<std::string>> SplicePerFunctionAssembly(
    const std::vector<std::filesystem::path>& s272_files,
    const std::vector<std::vector<std::string>>& unique_flag_sets,
    const absl::flat_hash_map<std::string, size_t>& func_to_flag_set, bool overall_has_wa_opt) {
  std::vector<std::vector<std::string>> all_lines(unique_flag_sets.size());
  std::vector<absl::flat_hash_map<std::string, AssemblyFunctionSpan>> all_funcs(
      unique_flag_sets.size());
  for (size_t j = 0; j < unique_flag_sets.size(); ++j) {
    auto lines_or = ReadAssemblyLines(s272_files[j]);
    if (!lines_or.ok()) {
      return lines_or.status();
    }
    all_funcs[j] = ParseAssemblyFunctions(*lines_or);
    all_lines[j] = std::move(*lines_or);
  }

  const auto& lines0 = all_lines[0];
  const auto& funcs0 = all_funcs[0];

  std::vector<AssemblyFunctionSpan> sorted_funcs;
  sorted_funcs.reserve(funcs0.size());
  for (const auto& [_, span] : funcs0) {
    sorted_funcs.push_back(span);
  }
  std::sort(sorted_funcs.begin(), sorted_funcs.end(),
            [](const AssemblyFunctionSpan& a, const AssemblyFunctionSpan& b) {
              return a.start_line < b.start_line;
            });

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
  return spliced_lines;
}

}  // namespace rom_nom_nom
