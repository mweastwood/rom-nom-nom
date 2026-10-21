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

absl::Status AlignFunctionsInAssemblyAndReassemble(
    const Toolchain& toolchain,
    const std::function<absl::StatusOr<ProcessResult>(const ProcessOptions&)>& runner,
    const std::filesystem::path& s272_path, const std::filesystem::path& out_obj,
    const std::vector<std::string>& as_opt_flags,
    const std::vector<std::filesystem::path>& as_include_dirs,
    const std::filesystem::path& macro_inc, absl::Duration timeout,
    const absl::flat_hash_map<std::string, uint32_t>& function_target_vrams,
    uint32_t module_target_size, uint32_t initial_vram_offset) {
  std::error_code ec;
  if (function_target_vrams.empty() || !std::filesystem::exists(out_obj, ec) ||
      !std::filesystem::exists(s272_path, ec)) {
    return absl::OkStatus();
  }

  auto lookup_vram = [&](std::string_view name) -> std::optional<uint32_t> {
    auto it = function_target_vrams.find(name);
    if (it != function_target_vrams.end()) {
      return it->second;
    }
    if (name.size() == 13 && absl::StartsWith(name, "func_80")) {
      uint32_t addr = 0;
      if (absl::SimpleHexAtoi(name.substr(5), &addr) && addr != 0) {
        return addr;
      }
    }
    return std::nullopt;
  };

  auto elf_or = ElfReader::LoadFromFile(out_obj);
  if (!elf_or.ok()) {
    return absl::OkStatus();
  }

  auto lines_or = ReadAssemblyLines(s272_path);
  if (!lines_or.ok()) {
    return absl::OkStatus();
  }
  const auto& lines = *lines_or;
  auto funcs_map = ParseAssemblyFunctions(lines);
  if (funcs_map.size() < 2) {
    return absl::OkStatus();
  }

  std::vector<AssemblyFunctionSpan> sorted_funcs;
  sorted_funcs.reserve(funcs_map.size());
  for (const auto& [_, span] : funcs_map) {
    sorted_funcs.push_back(span);
  }
  std::sort(sorted_funcs.begin(), sorted_funcs.end(),
            [](const AssemblyFunctionSpan& a, const AssemblyFunctionSpan& b) {
              return a.start_line < b.start_line;
            });

  auto first_vram_opt = lookup_vram(sorted_funcs[0].name);
  if (!first_vram_opt.has_value()) {
    return absl::OkStatus();
  }
  uint32_t module_base_vram = *first_vram_opt;

  std::vector<uint32_t> sym_offsets(sorted_funcs.size(), 0);
  for (size_t i = 0; i < sorted_funcs.size(); ++i) {
    const ElfSymbol* sym = elf_or->FindSymbol(sorted_funcs[i].name);
    if (sym == nullptr || !sym->is_defined) {
      return absl::OkStatus();
    }
    sym_offsets[i] = sym->value;
    if (i > 0 && sym_offsets[i] < sym_offsets[i - 1]) {
      return absl::OkStatus();
    }
  }

  const ElfSection* text_sec = elf_or->FindSection(".text");
  if (text_sec == nullptr) {
    return absl::OkStatus();
  }
  const ElfSymbol* last_sym = elf_or->FindSymbol(sorted_funcs.back().name);
  uint32_t unpadded_text_size = static_cast<uint32_t>(text_sec->data.size());
  if (last_sym != nullptr && last_sym->size > 0 &&
      sym_offsets.back() + last_sym->size <= text_sec->data.size()) {
    unpadded_text_size = sym_offsets.back() + last_sym->size;
  }
  uint32_t effective_used = initial_vram_offset + unpadded_text_size;
  constexpr int64_t kMaxSoftOverflowBytes = 512;
  int64_t free_pad_bytes =
      (module_target_size > 0)
          ? ((module_target_size > effective_used)
                 ? static_cast<int64_t>((module_target_size - effective_used) & ~3u)
                 : 0)
          : (1LL << 30);
  int64_t max_pad_bytes =
      (module_target_size > 0)
          ? ((effective_used > module_target_size) ? kMaxSoftOverflowBytes : free_pad_bytes)
          : (1LL << 30);

  absl::flat_hash_map<std::string, int32_t> jal_counts;
  for (const auto& raw_line : lines) {
    std::string_view sv = absl::StripAsciiWhitespace(raw_line);
    if (absl::StartsWith(sv, "jal\t") || absl::StartsWith(sv, "jal ")) {
      std::string_view target = absl::StripAsciiWhitespace(sv.substr(4));
      jal_counts[std::string(target)]++;
    }
  }

  const size_t n = sorted_funcs.size();
  std::vector<int64_t> req_pad_before(n, -1);
  std::vector<int32_t> func_weight(n, 2);
  std::vector<int32_t> internal_j_count(n, 0);
  for (size_t i = 0; i < n; ++i) {
    int32_t j_cnt = 0;
    for (size_t ln = sorted_funcs[i].start_line;
         ln <= sorted_funcs[i].end_line && ln < lines.size(); ++ln) {
      std::string_view sv = absl::StripAsciiWhitespace(lines[ln]);
      if (absl::StartsWith(sv, "j\t.L") || absl::StartsWith(sv, "j .L")) {
        j_cnt += 1;
      }
    }
    internal_j_count[i] = j_cnt;
    func_weight[i] = 2 + jal_counts[sorted_funcs[i].name] * 2;

    auto vram_opt = lookup_vram(sorted_funcs[i].name);
    if (!vram_opt.has_value() || *vram_opt < module_base_vram) {
      continue;
    }
    int64_t target_off = static_cast<int64_t>(*vram_opt - module_base_vram);
    int64_t unpadded_off = static_cast<int64_t>(initial_vram_offset + sym_offsets[i]);
    int64_t needed = target_off - unpadded_off;
    if (needed >= 0 && needed <= max_pad_bytes && (needed % 4) == 0) {
      req_pad_before[i] = needed;
    }
  }
  for (size_t i = 0; i + 1 < n; ++i) {
    if (req_pad_before[i] >= 0 && req_pad_before[i] == req_pad_before[i + 1]) {
      func_weight[i] += internal_j_count[i] * 2;
    }
  }

  std::vector<int32_t> dp(n, 0);
  std::vector<int32_t> parent(n, -1);
  int32_t best_score = 0;
  int32_t best_end = -1;
  for (size_t i = 0; i < n; ++i) {
    if (req_pad_before[i] < 0) {
      continue;
    }
    dp[i] = func_weight[i];
    for (size_t j = 0; j < i; ++j) {
      if (req_pad_before[j] >= 0 && req_pad_before[j] <= req_pad_before[i] &&
          dp[j] + func_weight[i] > dp[i]) {
        dp[i] = dp[j] + func_weight[i];
        parent[i] = static_cast<int32_t>(j);
      }
    }
    int64_t overflow_bytes = std::max<int64_t>(0, req_pad_before[i] - free_pad_bytes);
    int32_t net_score = dp[i] - static_cast<int32_t>(overflow_bytes / 8);
    if (net_score > best_score) {
      best_score = net_score;
      best_end = static_cast<int32_t>(i);
    }
  }

  std::vector<uint32_t> pad_words_after(n, 0);
  bool any_padding = false;
  if (best_end >= 0) {
    std::vector<size_t> chosen;
    for (int32_t cur = best_end; cur >= 0; cur = parent[cur]) {
      chosen.push_back(static_cast<size_t>(cur));
    }
    std::reverse(chosen.begin(), chosen.end());
    int64_t prev_pad = 0;
    for (size_t idx : chosen) {
      int64_t delta_pad = req_pad_before[idx] - prev_pad;
      if (delta_pad > 0 && idx > 0) {
        pad_words_after[idx - 1] = static_cast<uint32_t>(delta_pad / 4);
        any_padding = true;
      }
      prev_pad = req_pad_before[idx];
    }
  }

  if (!any_padding) {
    return absl::OkStatus();
  }

  std::vector<std::string> padded_lines;
  padded_lines.reserve(lines.size() + 256);
  size_t line_idx = 0;
  for (size_t i = 0; i < sorted_funcs.size(); ++i) {
    while (line_idx <= sorted_funcs[i].end_line && line_idx < lines.size()) {
      padded_lines.push_back(lines[line_idx]);
      ++line_idx;
    }
    for (uint32_t p = 0; p < pad_words_after[i]; ++p) {
      padded_lines.push_back("\t.word\t0\n");
    }
  }
  while (line_idx < lines.size()) {
    padded_lines.push_back(lines[line_idx]);
    ++line_idx;
  }

  {
    std::ofstream ofs(s272_path);
    if (!ofs.is_open()) {
      return absl::InternalError(absl::StrFormat(
          "AlignFunctionsInAssemblyAndReassemble: Failed to write padded assembly '%s'.",
          s272_path.string()));
    }
    for (const auto& l : padded_lines) {
      ofs << l;
    }
  }

  auto as_args =
      toolchain.BuildKmcAsArgs(as_opt_flags, as_include_dirs, macro_inc, s272_path, out_obj);
  ProcessOptions as_opts{
      .program = toolchain.KmcAs().string(),
      .args = std::move(as_args),
      .working_directory = toolchain.RepoRoot(),
      .timeout = timeout,
  };
  auto as_result = runner(as_opts);
  if (!as_result.ok()) {
    return as_result.status();
  }
  if (!as_result->Ok()) {
    return absl::InternalError(absl::StrFormat(
        "AlignFunctionsInAssemblyAndReassemble: KMC AS re-assembly failed for '%s' (exit code "
        "%d):\n%s\n%s",
        s272_path.string(), as_result->exit_code, as_result->stdout_output,
        as_result->stderr_output));
  }
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
