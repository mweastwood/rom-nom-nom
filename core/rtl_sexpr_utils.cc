#include "core/rtl_sexpr_utils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom {
namespace {

constexpr std::array<std::string_view, 32> kMipsGprRtlNames = {
    "$0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

constexpr int kFirstMipsPseudoRegister = kMipsFirstPseudoRegister;

}  // namespace

size_t RtlFunctionDump::ActiveInsnCount() const {
  size_t count = 0;
  for (const auto& insn : insns) {
    if (insn.is_deleted) {
      continue;
    }
    if (insn.kind == RtlInsnKind::kInsn || insn.kind == RtlInsnKind::kJumpInsn ||
        insn.kind == RtlInsnKind::kCallInsn) {
      ++count;
    }
  }
  return count;
}

size_t RtlFunctionDump::SequenceCount() const {
  size_t count = 0;
  for (const auto& insn : insns) {
    if (!insn.is_deleted && insn.is_sequence) {
      ++count;
    }
  }
  return count;
}

std::vector<const RtlInsnEntry*> RtlFunctionDump::FindNotes(
    std::string_view target_note_kind) const {
  std::vector<const RtlInsnEntry*> matches;
  for (const auto& insn : insns) {
    if (insn.kind == RtlInsnKind::kNote && insn.note_kind == target_note_kind) {
      matches.push_back(&insn);
    }
  }
  return matches;
}

bool RtlFunctionDump::HasLoopNotes() const {
  return !FindNotes("NOTE_INSN_LOOP_BEG").empty();
}

RtlPassDump::RtlPassDump(RtlPassKind pass, std::string raw_text, std::string preamble_text,
                         std::vector<RtlFunctionDump> functions)
    : pass_(pass),
      raw_text_(std::move(raw_text)),
      preamble_text_(std::move(preamble_text)),
      functions_(std::move(functions)) {}

const RtlFunctionDump* RtlPassDump::FindFunction(std::string_view func_name) const {
  for (const auto& fn : functions_) {
    if (fn.function_name == func_name) {
      return &fn;
    }
  }
  return nullptr;
}

void SkipRtlWhitespace(std::string_view text, size_t* pos) {
  while (*pos < text.size() && absl::ascii_isspace(text[*pos])) {
    ++(*pos);
  }
}

std::string_view ReadRtlToken(std::string_view text, size_t* pos) {
  SkipRtlWhitespace(text, pos);
  const size_t start = *pos;
  while (*pos < text.size()) {
    const char c = text[*pos];
    if (absl::ascii_isspace(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == ':' ||
        c == '/' || c == '"') {
      break;
    }
    ++(*pos);
  }
  return text.substr(start, *pos - start);
}

int UpdateRtlParenBalance(std::string_view line, int current_depth, bool* in_quotes) {
  int depth = current_depth;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (*in_quotes) {
      if (c == '\\' && i + 1 < line.size()) {
        ++i;
      } else if (c == '"') {
        *in_quotes = false;
      }
      continue;
    }
    if (c == '"') {
      *in_quotes = true;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    }
  }
  return depth;
}

size_t FindMatchingCloseParen(std::string_view text, size_t open_pos) {
  if (open_pos >= text.size() || text[open_pos] != '(') {
    return std::string_view::npos;
  }
  int depth = 0;
  bool in_quotes = false;
  for (size_t i = open_pos; i < text.size(); ++i) {
    const char c = text[i];
    if (in_quotes) {
      if (c == '\\' && i + 1 < text.size()) {
        ++i;
      } else if (c == '"') {
        in_quotes = false;
      }
      continue;
    }
    if (c == '"') {
      in_quotes = true;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
      if (depth == 0) {
        return i;
      }
    }
  }
  return std::string_view::npos;
}

std::vector<std::string_view> ExtractBracketedSexprElements(std::string_view text,
                                                            size_t open_bracket_pos) {
  std::vector<std::string_view> elements;
  const size_t bracket = text.find('[', open_bracket_pos);
  if (bracket == std::string_view::npos) {
    return elements;
  }
  size_t pos = bracket + 1;
  while (pos < text.size()) {
    SkipRtlWhitespace(text, &pos);
    if (pos >= text.size() || text[pos] == ']') {
      break;
    }
    if (text[pos] != '(') {
      ++pos;
      continue;
    }
    const size_t close_pos = FindMatchingCloseParen(text, pos);
    if (close_pos == std::string_view::npos) {
      elements.push_back(text.substr(pos));
      break;
    }
    elements.push_back(text.substr(pos, close_pos - pos + 1));
    pos = close_pos + 1;
  }
  return elements;
}

RtlInsnEntry ParseRtlSexprEntry(std::string raw_sexpr) {
  RtlInsnEntry entry;
  std::string_view view = absl::StripAsciiWhitespace(raw_sexpr);
  if (view.empty() || view.front() != '(') {
    entry.raw_sexpr = std::move(raw_sexpr);
    return entry;
  }

  std::string_view inner = view.substr(1);
  size_t pos = 0;
  auto next_token = [&]() -> std::string_view {
    while (pos < inner.size() && absl::ascii_isspace(inner[pos])) {
      ++pos;
    }
    if (pos >= inner.size()) {
      return {};
    }
    const size_t start = pos;
    while (pos < inner.size() && !absl::ascii_isspace(inner[pos]) && inner[pos] != '(' &&
           inner[pos] != ')') {
      ++pos;
    }
    return inner.substr(start, pos - start);
  };

  const std::string_view tag_with_flags = next_token();
  std::string_view base_tag = tag_with_flags;
  bool has_top_level_v_flag = false;
  if (const size_t sep_pos = tag_with_flags.find_first_of("/:");
      sep_pos != std::string_view::npos) {
    base_tag = tag_with_flags.substr(0, sep_pos);
    std::string_view rest_flags = tag_with_flags.substr(sep_pos);
    if (const size_t colon_pos = rest_flags.find(':'); colon_pos != std::string_view::npos) {
      rest_flags = rest_flags.substr(0, colon_pos);
    }
    for (std::string_view flag_tok : absl::StrSplit(rest_flags, '/', absl::SkipEmpty())) {
      if (flag_tok == "s") {
        entry.in_annulled_branch_delay = true;
      } else if (flag_tok == "v") {
        has_top_level_v_flag = true;
      }
    }
  }

  if (base_tag == "insn") {
    entry.kind = RtlInsnKind::kInsn;
  } else if (base_tag == "jump_insn") {
    entry.kind = RtlInsnKind::kJumpInsn;
  } else if (base_tag == "call_insn") {
    entry.kind = RtlInsnKind::kCallInsn;
  } else if (base_tag == "code_label") {
    entry.kind = RtlInsnKind::kCodeLabel;
  } else if (base_tag == "barrier") {
    entry.kind = RtlInsnKind::kBarrier;
  } else if (base_tag == "note") {
    entry.kind = RtlInsnKind::kNote;
  } else {
    entry.kind = RtlInsnKind::kOther;
  }

  // `rtl.h:277`: `#define INSN_DELETED_P(INSN) ((INSN)->volatil)` applies to
  // `INSN`, `JUMP_INSN`, `CALL_INSN`, `CODE_LABEL`, and `BARRIER` entries.
  if (has_top_level_v_flag &&
      (entry.kind == RtlInsnKind::kInsn || entry.kind == RtlInsnKind::kJumpInsn ||
       entry.kind == RtlInsnKind::kCallInsn || entry.kind == RtlInsnKind::kCodeLabel ||
       entry.kind == RtlInsnKind::kBarrier)) {
    entry.is_deleted = true;
  }

  int uid = -1;
  int prev_uid = -1;
  int next_uid = -1;
  if (absl::SimpleAtoi(next_token(), &uid)) {
    entry.uid = uid;
  }
  if (absl::SimpleAtoi(next_token(), &prev_uid)) {
    entry.prev_uid = prev_uid;
  }
  if (absl::SimpleAtoi(next_token(), &next_uid)) {
    entry.next_uid = next_uid;
  }

  if (absl::StrContains(view, "(sequence[") || absl::StrContains(view, "(sequence [")) {
    entry.is_sequence = true;
    if (absl::StrContains(view, "(insn/s ") || absl::StrContains(view, "(jump_insn/s ") ||
        absl::StrContains(view, "(call_insn/s ")) {
      entry.in_annulled_branch_delay = true;
    }
  }

  if (entry.kind == RtlInsnKind::kNote) {
    const size_t note_pos = view.find("NOTE_INSN_");
    if (note_pos != std::string_view::npos) {
      size_t end_pos = note_pos;
      while (end_pos < view.size() &&
             (absl::ascii_isalnum(view[end_pos]) || view[end_pos] == '_')) {
        ++end_pos;
      }
      entry.note_kind = std::string(view.substr(note_pos, end_pos - note_pos));
    }
  }

  entry.raw_sexpr = std::move(raw_sexpr);
  return entry;
}

RtlPassDump ParseRtlPassDump(RtlPassKind pass, std::string_view raw_text) {
  std::string preamble;
  std::vector<RtlFunctionDump> functions;
  RtlFunctionDump* current_fn = nullptr;

  std::string current_sexpr;
  int paren_depth = 0;
  bool in_quotes = false;

  for (std::string_view line : absl::StrSplit(raw_text, '\n')) {
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    if (paren_depth > 0) {
      absl::StrAppend(&current_sexpr, "\n", line);
      paren_depth = UpdateRtlParenBalance(line, paren_depth, &in_quotes);
      if (paren_depth <= 0) {
        paren_depth = 0;
        in_quotes = false;
        if (current_fn != nullptr) {
          current_fn->insns.push_back(ParseRtlSexprEntry(std::move(current_sexpr)));
        }
        current_sexpr.clear();
      }
      continue;
    }

    constexpr std::string_view kFuncMarker = ";; Function ";
    if (absl::StartsWith(line, kFuncMarker)) {
      const std::string_view fn_name = absl::StripAsciiWhitespace(line.substr(kFuncMarker.size()));
      functions.push_back(RtlFunctionDump{
          .pass = pass,
          .function_name = std::string(fn_name),
          .header_text = {},
          .insns = {},
      });
      current_fn = &functions.back();
      continue;
    }

    if (!line.empty() && line.front() == '(') {
      if (current_fn == nullptr) {
        functions.push_back(RtlFunctionDump{
            .pass = pass,
            .function_name = "<anonymous>",
            .header_text = {},
            .insns = {},
        });
        current_fn = &functions.back();
      }
      current_sexpr = std::string(line);
      in_quotes = false;
      paren_depth = UpdateRtlParenBalance(line, 0, &in_quotes);
      if (paren_depth <= 0) {
        paren_depth = 0;
        in_quotes = false;
        current_fn->insns.push_back(ParseRtlSexprEntry(std::move(current_sexpr)));
        current_sexpr.clear();
      }
      continue;
    }

    const std::string_view trimmed = absl::StripAsciiWhitespace(line);
    if (trimmed.empty()) {
      continue;
    }
    if (current_fn == nullptr) {
      if (!preamble.empty()) {
        preamble.push_back('\n');
      }
      absl::StrAppend(&preamble, line);
    } else if (current_fn->insns.empty()) {
      if (!current_fn->header_text.empty()) {
        current_fn->header_text.push_back('\n');
      }
      absl::StrAppend(&current_fn->header_text, line);
    }
  }

  if (!current_sexpr.empty() && current_fn != nullptr) {
    current_fn->insns.push_back(ParseRtlSexprEntry(std::move(current_sexpr)));
  }

  return RtlPassDump(pass, std::string(raw_text), std::move(preamble), std::move(functions));
}

absl::flat_hash_map<int, std::string> BuildSequentialLabelMap(
    absl::Span<const RtlInsnEntry> insns) {
  absl::flat_hash_map<int, std::string> label_map;
  size_t next_idx = 0;
  std::string current_group_label;
  for (size_t i = 0; i < insns.size(); ++i) {
    const auto& entry = insns[i];
    if (entry.is_deleted) {
      continue;
    }
    if (entry.kind == RtlInsnKind::kCodeLabel && entry.uid >= 0) {
      size_t next_active = i + 1;
      while (next_active < insns.size() && insns[next_active].is_deleted) {
        ++next_active;
      }
      const bool is_tablejump_label =
          (next_active < insns.size() && insns[next_active].kind == RtlInsnKind::kJumpInsn &&
           (absl::StrContains(insns[next_active].raw_sexpr, "(addr_vec:") ||
            absl::StrContains(insns[next_active].raw_sexpr, "(addr_diff_vec:")));
      if (is_tablejump_label || current_group_label.empty()) {
        current_group_label = absl::StrCat("L", next_idx++);
      }
      if (!label_map.contains(entry.uid)) {
        label_map.emplace(entry.uid, current_group_label);
      }
      if (is_tablejump_label) {
        current_group_label.clear();
      }
    } else if (entry.kind == RtlInsnKind::kInsn || entry.kind == RtlInsnKind::kJumpInsn ||
               entry.kind == RtlInsnKind::kCallInsn || entry.kind == RtlInsnKind::kBarrier ||
               (entry.kind == RtlInsnKind::kNote &&
                absl::StartsWith(entry.note_kind, "NOTE_INSN_LOOP_"))) {
      current_group_label.clear();
    }
  }
  return label_map;
}

absl::flat_hash_map<int, std::string> BuildSequentialLabelMap(const RtlFunctionDump& dump) {
  return BuildSequentialLabelMap(dump.insns);
}

std::string NormalizeInsnSexprForComparison(
    std::string_view sexpr, const absl::flat_hash_map<int, std::string>& label_uid_to_canon) {
  std::string out;
  out.reserve(sexpr.size());

  size_t i = 0;
  while (i < sexpr.size()) {
    if (absl::ascii_isspace(sexpr[i])) {
      if (!out.empty() && out.back() != ' ') {
        out.push_back(' ');
      }
      while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
        ++i;
      }
      continue;
    }

    if (sexpr[i] == '(') {
      const std::string_view rest = sexpr.substr(i + 1);
      auto starts_with_insn_tag = [&](std::string_view prefix) {
        if (!absl::StartsWith(rest, prefix)) {
          return false;
        }
        const size_t after = prefix.size();
        return after < rest.size() &&
               (rest[after] == ' ' || rest[after] == '/' || rest[after] == ':');
      };
      if (starts_with_insn_tag("insn") || starts_with_insn_tag("jump_insn") ||
          starts_with_insn_tag("call_insn")) {
        out.push_back('(');
        ++i;
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != '(' &&
               sexpr[i] != ')') {
          out.push_back(sexpr[i++]);
        }
        for (int k = 0; k < 3; ++k) {
          const size_t save = i;
          while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
            ++i;
          }
          const size_t num_start = i;
          if (i < sexpr.size() && sexpr[i] == '-') {
            ++i;
          }
          while (i < sexpr.size() && absl::ascii_isdigit(sexpr[i])) {
            ++i;
          }
          if (i == num_start || (i == num_start + 1 && sexpr[num_start] == '-')) {
            i = save;
            break;
          }
        }
        continue;
      }

      if (absl::StartsWith(rest, "label_ref")) {
        out.push_back('(');
        ++i;
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != ')') {
          out.push_back(sexpr[i++]);
        }
        while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
          ++i;
        }
        const size_t num_start = i;
        while (i < sexpr.size() && absl::ascii_isdigit(sexpr[i])) {
          ++i;
        }
        int label_uid = -1;
        if (i > num_start && absl::SimpleAtoi(sexpr.substr(num_start, i - num_start), &label_uid)) {
          auto it = label_uid_to_canon.find(label_uid);
          if (it != label_uid_to_canon.end()) {
            absl::StrAppend(&out, " ", it->second);
          } else {
            absl::StrAppend(&out, " ", label_uid);
          }
        }
        continue;
      }

      if (i > 0 &&
          (absl::StartsWith(rest, "code_label ") || absl::StartsWith(rest, "code_label\t"))) {
        const size_t close_pos = FindMatchingCloseParen(sexpr, i);
        size_t p = i + 1 + std::string_view("code_label").size();
        while (p < sexpr.size() && absl::ascii_isspace(sexpr[p])) {
          ++p;
        }
        const size_t tok_start = p;
        while (p < sexpr.size() && !absl::ascii_isspace(sexpr[p]) && sexpr[p] != ')') {
          ++p;
        }
        const std::string_view tok = sexpr.substr(tok_start, p - tok_start);
        int label_uid = -1;
        if (!tok.empty() && absl::SimpleAtoi(tok, &label_uid)) {
          auto it = label_uid_to_canon.find(label_uid);
          if (it != label_uid_to_canon.end()) {
            absl::StrAppend(&out, "(code_label ", it->second, ")");
          } else {
            absl::StrAppend(&out, "(code_label ", label_uid, ")");
          }
        } else {
          absl::StrAppend(&out, "(code_label ", tok, ")");
        }
        i = (close_pos != std::string_view::npos) ? close_pos + 1 : p;
        continue;
      }

      if (absl::StartsWith(rest, "insn_list")) {
        out.push_back('(');
        ++i;
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != ')') {
          out.push_back(sexpr[i++]);
        }
        while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
          ++i;
        }
        while (i < sexpr.size() && absl::ascii_isdigit(sexpr[i])) {
          ++i;
        }
        continue;
      }
    }

    out.push_back(sexpr[i++]);
  }

  return out;
}

std::string ExtractInsnTagWithFlags(std::string_view sexpr) {
  std::string_view s = absl::StripAsciiWhitespace(sexpr);
  if (s.empty() || s.front() != '(') {
    return "insn";
  }
  size_t pos = 1;
  while (pos < s.size() && !absl::ascii_isspace(s[pos]) && s[pos] != '(' && s[pos] != ')') {
    ++pos;
  }
  return std::string(s.substr(1, pos - 1));
}

std::string ExtractTopLevelPatternFromInsnSexpr(std::string_view sexpr) {
  std::string_view s = absl::StripAsciiWhitespace(sexpr);
  if (s.empty() || s.front() != '(') {
    return std::string(s);
  }
  size_t pos = 1;
  while (pos < s.size() && !absl::ascii_isspace(s[pos]) && s[pos] != '(' && s[pos] != ')') {
    ++pos;
  }
  for (int k = 0; k < 3; ++k) {
    while (pos < s.size() && absl::ascii_isspace(s[pos])) {
      ++pos;
    }
    if (pos < s.size() && s[pos] == '-') {
      ++pos;
    }
    while (pos < s.size() && absl::ascii_isdigit(s[pos])) {
      ++pos;
    }
  }
  while (pos < s.size() && absl::ascii_isspace(s[pos])) {
    ++pos;
  }
  if (pos >= s.size() || s[pos] != '(') {
    return std::string(s.substr(pos));
  }
  const size_t close_pos = FindMatchingCloseParen(s, pos);
  if (close_pos != std::string_view::npos) {
    return std::string(s.substr(pos, close_pos - pos + 1));
  }
  return std::string(s.substr(pos));
}

namespace {

std::string_view PeelLastSexprFromPrefix(std::string_view* prefix) {
  std::string_view s = absl::StripAsciiWhitespace(*prefix);
  if (s.empty() || s.back() != ')') {
    return {};
  }
  int depth = 0;
  bool in_quotes = false;
  for (size_t idx = s.size(); idx > 0; --idx) {
    const char c = s[idx - 1];
    if (c == '"' && (idx < 2 || s[idx - 2] != '\\')) {
      in_quotes = !in_quotes;
      continue;
    }
    if (in_quotes) {
      continue;
    }
    if (c == ')') {
      ++depth;
    } else if (c == '(') {
      --depth;
      if (depth == 0) {
        const std::string_view last = s.substr(idx - 1);
        *prefix = absl::StripAsciiWhitespace(s.substr(0, idx - 1));
        return last;
      }
    }
  }
  return {};
}

bool IsCallInsnContainerSexpr(std::string_view sexpr) {
  std::string_view s = absl::StripAsciiWhitespace(sexpr);
  if (s.size() < 10 || s.front() != '(') {
    return false;
  }
  s.remove_prefix(1);
  if (!absl::StartsWith(s, "call_insn")) {
    return false;
  }
  const size_t n = std::string_view("call_insn").size();
  return s.size() == n || absl::ascii_isspace(s[n]) || s[n] == '/' || s[n] == ':';
}

}  // namespace

std::string ExtractRegNotesFromInsnSexpr(std::string_view sexpr) {
  std::string_view s = absl::StripAsciiWhitespace(sexpr);
  if (s.empty() || s.back() != ')') {
    return "";
  }
  const bool is_call_insn = IsCallInsnContainerSexpr(s);
  s = absl::StripAsciiWhitespace(s.substr(0, s.size() - 1));
  std::string_view candidate = PeelLastSexprFromPrefix(&s);
  if (candidate.empty()) {
    return "";
  }
  if (is_call_insn && (candidate == "(nil)" || absl::StartsWith(candidate, "(expr_list ") ||
                       absl::StartsWith(candidate, "(expr_list("))) {
    candidate = PeelLastSexprFromPrefix(&s);
  }
  if (absl::StartsWith(candidate, "(expr_list:") || absl::StartsWith(candidate, "(insn_list:")) {
    return std::string(candidate);
  }
  return "";
}

std::string ExtractCallFusageFromInsnSexpr(std::string_view sexpr) {
  std::string_view s = absl::StripAsciiWhitespace(sexpr);
  if (!IsCallInsnContainerSexpr(s) || s.back() != ')') {
    return "";
  }
  s = absl::StripAsciiWhitespace(s.substr(0, s.size() - 1));
  const std::string_view last = PeelLastSexprFromPrefix(&s);
  if (last == "(nil)" || absl::StartsWith(last, "(expr_list ") ||
      absl::StartsWith(last, "(expr_list(")) {
    return std::string(last);
  }
  return "";
}

std::pair<std::string_view, std::string_view> SplitPatternAndTrailingRegNotes(
    std::string_view text) {
  std::string_view s = absl::StripAsciiWhitespace(text);
  if (s.empty() || s.front() != '(') {
    return {s, {}};
  }
  const size_t close_pos = FindMatchingCloseParen(s, 0);
  if (close_pos == std::string_view::npos) {
    return {s, {}};
  }
  const std::string_view first = s.substr(0, close_pos + 1);
  const std::string_view rest = absl::StripAsciiWhitespace(s.substr(close_pos + 1));
  if (absl::StartsWith(rest, "(expr_list") || absl::StartsWith(rest, "(insn_list")) {
    return {first, rest};
  }
  return {s, {}};
}

std::string StripTrailingRegNotesFromPattern(std::string_view pat) {
  auto [core, notes] = SplitPatternAndTrailingRegNotes(pat);
  if (!notes.empty()) {
    return std::string(core);
  }
  return std::string(absl::StripAsciiWhitespace(pat));
}

std::string CanonicalizeLabelRefsInPattern(std::string_view pattern,
                                           const absl::flat_hash_map<int, std::string>& label_map) {
  std::string out;
  out.reserve(pattern.size());
  size_t i = 0;
  while (i < pattern.size()) {
    if (pattern[i] == '(' && absl::StartsWith(pattern.substr(i + 1), "label_ref")) {
      out.push_back('(');
      ++i;
      while (i < pattern.size() && !absl::ascii_isspace(pattern[i]) && pattern[i] != ')') {
        out.push_back(pattern[i++]);
      }
      while (i < pattern.size() && absl::ascii_isspace(pattern[i])) {
        ++i;
      }
      const size_t num_start = i;
      while (i < pattern.size() && absl::ascii_isdigit(pattern[i])) {
        ++i;
      }
      int label_uid = -1;
      if (i > num_start && absl::SimpleAtoi(pattern.substr(num_start, i - num_start), &label_uid)) {
        auto it = label_map.find(label_uid);
        if (it != label_map.end()) {
          absl::StrAppend(&out, " ", it->second);
        } else {
          absl::StrAppend(&out, " ", label_uid);
        }
      }
      continue;
    }
    if (pattern[i] == '(' && (absl::StartsWith(pattern.substr(i + 1), "code_label ") ||
                              absl::StartsWith(pattern.substr(i + 1), "code_label\t"))) {
      const size_t close_pos = FindMatchingCloseParen(pattern, i);
      size_t p = i + 1 + std::string_view("code_label").size();
      while (p < pattern.size() && absl::ascii_isspace(pattern[p])) {
        ++p;
      }
      const size_t tok_start = p;
      while (p < pattern.size() && !absl::ascii_isspace(pattern[p]) && pattern[p] != ')') {
        ++p;
      }
      const std::string_view tok = pattern.substr(tok_start, p - tok_start);
      int label_uid = -1;
      if (!tok.empty() && absl::SimpleAtoi(tok, &label_uid)) {
        auto it = label_map.find(label_uid);
        if (it != label_map.end()) {
          absl::StrAppend(&out, "(code_label ", it->second, ")");
        } else {
          absl::StrAppend(&out, "(code_label ", label_uid, ")");
        }
      } else {
        absl::StrAppend(&out, "(code_label ", tok, ")");
      }
      i = (close_pos != std::string_view::npos) ? close_pos + 1 : p;
      continue;
    }
    if (pattern[i] == '(' && absl::StartsWith(pattern.substr(i + 1), "insn_list")) {
      out.push_back('(');
      ++i;
      while (i < pattern.size() && !absl::ascii_isspace(pattern[i]) && pattern[i] != ')') {
        out.push_back(pattern[i++]);
      }
      while (i < pattern.size() && absl::ascii_isspace(pattern[i])) {
        ++i;
      }
      while (i < pattern.size() && absl::ascii_isdigit(pattern[i])) {
        ++i;
      }
      continue;
    }
    out.push_back(pattern[i++]);
  }
  return out;
}

std::string ExtractPatternAndNotesFromInsnSexpr(
    std::string_view sexpr, const absl::flat_hash_map<int, std::string>& label_map) {
  std::string pat =
      CanonicalizeLabelRefsInPattern(ExtractTopLevelPatternFromInsnSexpr(sexpr), label_map);
  if (!IsCallInsnContainerSexpr(sexpr)) {
    const std::string notes =
        CanonicalizeLabelRefsInPattern(ExtractRegNotesFromInsnSexpr(sexpr), label_map);
    if (!notes.empty()) {
      absl::StrAppend(&pat, " ", notes);
    }
  }
  return CanonicalizeRtlPattern(pat);
}

std::vector<int> ExtractLabelRefsFromPattern(std::string_view pattern) {
  std::vector<int> refs;
  size_t pos = 0;
  while (pos < pattern.size()) {
    const size_t idx = pattern.find("(label_ref", pos);
    if (idx == std::string_view::npos) {
      break;
    }
    size_t p = idx + std::string_view("(label_ref").size();
    while (p < pattern.size() && !absl::ascii_isspace(pattern[p]) && pattern[p] != ')') {
      ++p;
    }
    while (p < pattern.size() && absl::ascii_isspace(pattern[p])) {
      ++p;
    }
    const size_t num_start = p;
    while (p < pattern.size() && absl::ascii_isdigit(pattern[p])) {
      ++p;
    }
    int label_uid = -1;
    if (p > num_start && absl::SimpleAtoi(pattern.substr(num_start, p - num_start), &label_uid)) {
      refs.push_back(label_uid);
    }
    pos = p;
  }
  return refs;
}

std::vector<std::string> ExtractLabelRefTokens(std::string_view text) {
  std::vector<std::string> tokens;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t idx_lref = text.find("(label_ref", pos);
    const size_t idx_clbl = text.find("(code_label", pos);
    size_t idx = std::string_view::npos;
    std::string_view prefix;
    if (idx_lref != std::string_view::npos &&
        (idx_clbl == std::string_view::npos || idx_lref <= idx_clbl)) {
      idx = idx_lref;
      prefix = "(label_ref";
    } else if (idx_clbl != std::string_view::npos) {
      idx = idx_clbl;
      prefix = "(code_label";
    } else {
      break;
    }
    size_t p = idx + prefix.size();
    while (p < text.size() && !absl::ascii_isspace(text[p]) && text[p] != ')') {
      ++p;
    }
    while (p < text.size() && absl::ascii_isspace(text[p])) {
      ++p;
    }
    const size_t tok_start = p;
    while (p < text.size() && !absl::ascii_isspace(text[p]) && text[p] != ')' && text[p] != '(') {
      ++p;
    }
    if (p > tok_start) {
      tokens.emplace_back(text.substr(tok_start, p - tok_start));
    }
    pos = p;
  }
  return tokens;
}

std::vector<RtlInsnEntry> ParseSequenceInnerEntries(const RtlInsnEntry& seq_entry) {
  const std::string pat = ExtractTopLevelPatternFromInsnSexpr(seq_entry.raw_sexpr);
  const size_t bracket_pos = pat.find('[');
  if (bracket_pos == std::string::npos) {
    return {seq_entry};
  }
  std::vector<RtlInsnEntry> inner_entries;
  for (std::string_view elem : ExtractBracketedSexprElements(pat, bracket_pos)) {
    inner_entries.push_back(ParseRtlSexprEntry(std::string(elem)));
  }
  return inner_entries;
}

bool ExtractOuterSequenceParts(std::string_view seq_pat, std::string* out_jump_pat,
                               std::string* out_delay_tag, std::string* out_delay_pat) {
  std::string_view s = absl::StripAsciiWhitespace(seq_pat);
  if (!absl::StartsWith(s, "(sequence")) {
    return false;
  }
  const size_t bracket_pos = s.find('[');
  if (bracket_pos == std::string_view::npos) {
    return false;
  }
  const std::vector<std::string_view> elems = ExtractBracketedSexprElements(s, bracket_pos);
  if (elems.size() < 2) {
    return false;
  }

  auto unwrap_container = [](std::string_view container, std::string* tag_out) -> std::string {
    std::string_view c = absl::StripAsciiWhitespace(container);
    if (c.size() < 2 || c.front() != '(' || c.back() != ')') {
      return std::string(c);
    }
    size_t p = 1;
    while (p < c.size() && !absl::ascii_isspace(c[p]) && c[p] != '(') {
      ++p;
    }
    if (tag_out != nullptr) {
      *tag_out = std::string(c.substr(1, p - 1));
    }
    while (p < c.size() && c[p] != '(') {
      ++p;
    }
    if (p >= c.size()) {
      return "";
    }
    return std::string(absl::StripAsciiWhitespace(c.substr(p, c.size() - p - 1)));
  };

  std::string jump_tag;
  *out_jump_pat = unwrap_container(elems[0], &jump_tag);
  *out_delay_pat = unwrap_container(elems[1], out_delay_tag);
  return !out_jump_pat->empty() && !out_delay_pat->empty();
}

RtlControlFlowGraph BuildCanonicalCfgFromFlatRtlItems(
    RtlPassKind pass, absl::Span<const FlatRtlItem> items,
    absl::Span<const RtlInsnEntry> raw_insns_for_tablejumps) {
  RtlControlFlowGraph result;
  result.pass = pass;

  if (items.empty()) {
    result.rpo_blocks.push_back(RtlBasicBlock{});
    return result;
  }

  struct RawRtlBlock {
    std::vector<int> label_uids;
    std::vector<RtlInsn> rtl_insns;
    std::vector<int> jump_target_labels;
    std::vector<int> addr_vec_target_labels;
    bool ends_with_jump = false;
    bool ends_with_uncond_jump = false;
    bool ends_with_return = false;
    bool followed_by_barrier = false;
  };

  auto make_rtl_insn = [](const FlatRtlItem& item, std::string canon_pat) {
    RtlInsn insn;
    insn.canonical_pattern = std::move(canon_pat);
    insn.is_synthetic_rtl_expansion = true;
    insn.loop_note_kind = ParseRtlLoopNoteKind(insn.canonical_pattern);
    if (item.has_explicit_call_fusage) {
      insn.has_explicit_call_fusage = true;
      insn.call_fusage = item.call_fusage;
    }
    return insn;
  };

  std::vector<RawRtlBlock> raw_blocks;
  absl::flat_hash_map<int, size_t> label_to_block;

  size_t i = 0;
  while (i < items.size()) {
    while (i < items.size() && items[i].kind == RtlInsnKind::kBarrier) {
      if (!raw_blocks.empty()) {
        raw_blocks.back().followed_by_barrier = true;
      }
      ++i;
    }
    if (i >= items.size()) {
      break;
    }

    RawRtlBlock blk;
    while (i < items.size() && items[i].kind == RtlInsnKind::kCodeLabel) {
      blk.label_uids.push_back(items[i].uid);
      ++i;
    }
    while (i < items.size() && items[i].kind != RtlInsnKind::kCodeLabel &&
           items[i].kind != RtlInsnKind::kBarrier) {
      const auto& it = items[i];
      if (it.kind == RtlInsnKind::kInsn || it.kind == RtlInsnKind::kCallInsn ||
          it.kind == RtlInsnKind::kNote) {
        blk.rtl_insns.push_back(make_rtl_insn(it, CanonicalizeRtlPattern(it.pattern)));
        ++i;
      } else if (it.kind == RtlInsnKind::kJumpInsn) {
        blk.ends_with_jump = true;
        std::string canon_pat = CanonicalizeRtlPattern(it.pattern);
        blk.rtl_insns.push_back(make_rtl_insn(it, std::move(canon_pat)));
        const std::string raw_jump_pat = CanonicalizeRtlPattern(it.raw_pattern_for_targets);
        if (absl::StrContains(raw_jump_pat, "(return)")) {
          blk.ends_with_return = true;
        } else if (absl::StartsWith(raw_jump_pat, "(set (pc) (label_ref")) {
          blk.ends_with_uncond_jump = true;
          blk.jump_target_labels = ExtractLabelRefsFromPattern(it.raw_pattern_for_targets);
        } else if (absl::StartsWith(raw_jump_pat, "(parallel[ (set (pc) (reg:") ||
                   absl::StartsWith(raw_jump_pat, "(parallel [(set (pc) (reg:")) {
          blk.ends_with_uncond_jump = true;
          const std::vector<int> table_refs =
              ExtractLabelRefsFromPattern(it.raw_pattern_for_targets);
          if (!table_refs.empty() && !raw_insns_for_tablejumps.empty()) {
            const int table_uid = table_refs[0];
            bool found_table_label = false;
            for (const auto& e : raw_insns_for_tablejumps) {
              if (e.is_deleted) {
                continue;
              }
              if (e.kind == RtlInsnKind::kCodeLabel && e.uid == table_uid) {
                found_table_label = true;
                continue;
              }
              if (found_table_label && e.kind == RtlInsnKind::kJumpInsn) {
                const std::string p = ExtractTopLevelPatternFromInsnSexpr(e.raw_sexpr);
                if (absl::StartsWith(p, "(addr_vec:") || absl::StartsWith(p, "(addr_diff_vec:")) {
                  for (int t_uid : ExtractLabelRefsFromPattern(p)) {
                    blk.addr_vec_target_labels.push_back(t_uid);
                    if (std::find(blk.jump_target_labels.begin(), blk.jump_target_labels.end(),
                                  t_uid) == blk.jump_target_labels.end()) {
                      blk.jump_target_labels.push_back(t_uid);
                    }
                  }
                }
                break;
              }
            }
          } else {
            blk.jump_target_labels = table_refs;
          }
        } else {
          blk.jump_target_labels = ExtractLabelRefsFromPattern(it.raw_pattern_for_targets);
        }
        ++i;
        break;
      } else {
        ++i;
      }
    }
    while (i < items.size() && items[i].kind == RtlInsnKind::kBarrier) {
      blk.followed_by_barrier = true;
      ++i;
    }

    if (blk.rtl_insns.empty() && blk.label_uids.empty() && !blk.ends_with_return) {
      continue;
    }
    raw_blocks.push_back(std::move(blk));
  }

  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    for (int l_uid : raw_blocks[b].label_uids) {
      label_to_block[l_uid] = b;
    }
  }

  std::vector<std::vector<size_t>> raw_succs(raw_blocks.size());
  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    const auto& blk = raw_blocks[b];
    if (blk.ends_with_return) {
      continue;
    }
    for (int target_lbl : blk.jump_target_labels) {
      auto it = label_to_block.find(target_lbl);
      if (it != label_to_block.end()) {
        raw_succs[b].push_back(it->second);
      }
    }
    const bool has_fallthrough =
        !blk.followed_by_barrier && !blk.ends_with_uncond_jump && !blk.ends_with_return;
    if (has_fallthrough && b + 1 < raw_blocks.size()) {
      raw_succs[b].push_back(b + 1);
    }
  }

  result.rpo_blocks.reserve(raw_blocks.size());
  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    RtlBasicBlock out_blk;
    out_blk.id = static_cast<uint32_t>(b);
    if (pass == RtlPassKind::kRtl) {
      out_blk.has_label = !raw_blocks[b].label_uids.empty();
    }
    out_blk.has_trailing_barrier = raw_blocks[b].followed_by_barrier;
    for (auto& rinsn : raw_blocks[b].rtl_insns) {
      out_blk.AddInsn(std::move(rinsn));
    }
    for (int target_lbl : raw_blocks[b].addr_vec_target_labels) {
      auto it = label_to_block.find(target_lbl);
      if (it != label_to_block.end()) {
        out_blk.addr_vec_targets.push_back(it->second);
      }
    }
    out_blk.successors = std::move(raw_succs[b]);
    result.rpo_blocks.push_back(std::move(out_blk));
  }
  for (size_t b = 0; b < result.rpo_blocks.size(); ++b) {
    for (size_t s : result.rpo_blocks[b].successors) {
      if (s < result.rpo_blocks.size()) {
        result.rpo_blocks[s].predecessors.push_back(b);
      }
    }
  }
  return result;
}

RtlControlFlowGraph BuildCanonicalCfgFromRtlDump(const RtlFunctionDump& dump) {
  const absl::flat_hash_map<int, std::string> label_map = BuildSequentialLabelMap(dump);

  std::vector<FlatRtlItem> items;
  for (size_t idx = 0; idx < dump.insns.size(); ++idx) {
    const auto& entry = dump.insns[idx];
    if (entry.is_deleted || entry.kind == RtlInsnKind::kOther) {
      continue;
    }
    if (entry.kind == RtlInsnKind::kNote) {
      if (absl::StartsWith(entry.note_kind, "NOTE_INSN_LOOP_")) {
        items.push_back(FlatRtlItem{
            .kind = RtlInsnKind::kNote,
            .uid = entry.uid,
            .raw_pattern_for_targets = "",
            .pattern = absl::StrCat("(note ", entry.note_kind, ")"),
        });
      }
      continue;
    }
    if (entry.kind == RtlInsnKind::kCodeLabel) {
      size_t next_active = idx + 1;
      while (next_active < dump.insns.size() && dump.insns[next_active].is_deleted) {
        ++next_active;
      }
      if (next_active < dump.insns.size() &&
          dump.insns[next_active].kind == RtlInsnKind::kJumpInsn) {
        const std::string next_pat =
            ExtractTopLevelPatternFromInsnSexpr(dump.insns[next_active].raw_sexpr);
        if (absl::StartsWith(next_pat, "(addr_vec:") ||
            absl::StartsWith(next_pat, "(addr_diff_vec:")) {
          idx = next_active;
          while (idx + 1 < dump.insns.size() &&
                 (dump.insns[idx + 1].is_deleted ||
                  dump.insns[idx + 1].kind == RtlInsnKind::kBarrier)) {
            ++idx;
          }
          continue;
        }
      }
    }
    if (entry.is_sequence) {
      const std::vector<RtlInsnEntry> inner_entries = ParseSequenceInnerEntries(entry);
      std::vector<std::string> formatted_inner;
      formatted_inner.reserve(inner_entries.size());
      RtlInsnKind effective_kind = entry.kind;
      std::string jump_raw_pat;
      bool has_explicit_call_fusage = false;
      std::vector<RtlExpr> call_fusage;
      for (const auto& sub : inner_entries) {
        if (sub.is_deleted) {
          continue;
        }
        std::string tag = ExtractInsnTagWithFlags(sub.raw_sexpr);
        if (const size_t colon_pos = tag.find(':'); colon_pos != std::string::npos) {
          tag.erase(colon_pos);
        }
        const std::string sub_pat = ExtractPatternAndNotesFromInsnSexpr(sub.raw_sexpr, label_map);
        formatted_inner.push_back(absl::StrCat("(", tag, " ", sub_pat, ")"));
        if (sub.kind == RtlInsnKind::kJumpInsn) {
          effective_kind = RtlInsnKind::kJumpInsn;
          jump_raw_pat = ExtractTopLevelPatternFromInsnSexpr(sub.raw_sexpr);
        } else if (sub.kind == RtlInsnKind::kCallInsn) {
          const std::string fusage_str = CanonicalizeLabelRefsInPattern(
              ExtractCallFusageFromInsnSexpr(sub.raw_sexpr), label_map);
          if (!fusage_str.empty()) {
            if (auto parsed_fusage = ParseCallFusageChainSexpr(fusage_str); parsed_fusage.ok()) {
              has_explicit_call_fusage = true;
              call_fusage = std::move(*parsed_fusage);
            }
          }
        }
      }
      items.push_back(FlatRtlItem{
          .kind = effective_kind,
          .uid = entry.uid,
          .raw_pattern_for_targets = jump_raw_pat,
          .pattern = absl::StrCat("(sequence [", absl::StrJoin(formatted_inner, " "), "])"),
          .has_explicit_call_fusage = has_explicit_call_fusage,
          .call_fusage = std::move(call_fusage),
      });
      continue;
    }

    const std::string raw_pat = ExtractTopLevelPatternFromInsnSexpr(entry.raw_sexpr);
    if (entry.kind == RtlInsnKind::kJumpInsn &&
        (absl::StartsWith(raw_pat, "(addr_vec:") || absl::StartsWith(raw_pat, "(addr_diff_vec:"))) {
      continue;
    }
    bool has_explicit_call_fusage = false;
    std::vector<RtlExpr> call_fusage;
    if (entry.kind == RtlInsnKind::kCallInsn) {
      const std::string fusage_str = CanonicalizeLabelRefsInPattern(
          ExtractCallFusageFromInsnSexpr(entry.raw_sexpr), label_map);
      if (!fusage_str.empty()) {
        if (auto parsed_fusage = ParseCallFusageChainSexpr(fusage_str); parsed_fusage.ok()) {
          has_explicit_call_fusage = true;
          call_fusage = std::move(*parsed_fusage);
        }
      }
    }
    items.push_back(FlatRtlItem{
        .kind = entry.kind,
        .uid = entry.uid,
        .raw_pattern_for_targets = raw_pat,
        .pattern = (entry.kind == RtlInsnKind::kInsn || entry.kind == RtlInsnKind::kJumpInsn ||
                    entry.kind == RtlInsnKind::kCallInsn)
                       ? ExtractPatternAndNotesFromInsnSexpr(entry.raw_sexpr, label_map)
                       : raw_pat,
        .has_explicit_call_fusage = has_explicit_call_fusage,
        .call_fusage = std::move(call_fusage),
    });
  }

  return BuildCanonicalCfgFromFlatRtlItems(dump.pass, items, dump.insns);
}

bool IsJumpPatternText(std::string_view pat) {
  const std::string_view s = absl::StripAsciiWhitespace(pat);
  return absl::StartsWith(s, "(return)") || absl::StartsWith(s, "(set (pc)") ||
         absl::StartsWith(s, "(parallel [(set (pc)") ||
         absl::StartsWith(s, "(parallel[ (set (pc)") ||
         absl::StartsWith(s, "(parallel [(return)") || absl::StartsWith(s, "(parallel[ (return)");
}

bool IsUnconditionalJumpOrReturnPatternText(std::string_view pat) {
  const std::string_view s = absl::StripAsciiWhitespace(pat);
  if (absl::StartsWith(s, "(return)") || absl::StartsWith(s, "(parallel [(return)") ||
      absl::StartsWith(s, "(parallel[ (return)")) {
    return true;
  }
  if (absl::StartsWith(s, "(set (pc) (label_ref") ||
      absl::StartsWith(s, "(parallel [(set (pc) (reg:") ||
      absl::StartsWith(s, "(parallel[ (set (pc) (reg:")) {
    return true;
  }
  return false;
}

bool IsCallPatternText(std::string_view pat) {
  const std::string_view s = absl::StripAsciiWhitespace(pat);
  return absl::StartsWith(s, "(call ") ||
         (absl::StartsWith(s, "(set ") && absl::StrContains(s, " (call ")) ||
         absl::StartsWith(s, "(parallel [(call ") || absl::StartsWith(s, "(parallel[ (call ") ||
         (absl::StartsWith(s, "(parallel") && absl::StrContains(s, "(call (mem"));
}

std::string InvertRtlConditionCode(std::string_view cmp_code) {
  if (cmp_code == "eq") return "ne";
  if (cmp_code == "ne") return "eq";
  if (cmp_code == "lt") return "ge";
  if (cmp_code == "ge") return "lt";
  if (cmp_code == "le") return "gt";
  if (cmp_code == "gt") return "le";
  if (cmp_code == "ltu") return "geu";
  if (cmp_code == "geu") return "ltu";
  if (cmp_code == "leu") return "gtu";
  if (cmp_code == "gtu") return "leu";
  return std::string(cmp_code);
}

std::string InvertBranchConditionInRtlPattern(std::string_view pat) {
  constexpr std::string_view kPrefix = "(if_then_else (";
  const size_t pos = pat.find(kPrefix);
  if (pos == std::string_view::npos) {
    return std::string(pat);
  }
  const size_t op_start = pos + kPrefix.size();
  size_t op_end = op_start;
  while (op_end < pat.size() && !absl::ascii_isspace(pat[op_end]) && pat[op_end] != ':') {
    ++op_end;
  }
  const std::string_view cmp_code = pat.substr(op_start, op_end - op_start);
  const std::string inv_code = InvertRtlConditionCode(cmp_code);
  if (inv_code == cmp_code) {
    return std::string(pat);
  }
  std::string out(pat);
  out.replace(op_start, op_end - op_start, inv_code);
  return out;
}

std::string_view MipsGprRtlName(uint32_t regno) {
  if (regno < kMipsGprRtlNames.size()) {
    return kMipsGprRtlNames[regno];
  }
  return "";
}

std::string FormatHardRegRtl(uint32_t regno, std::string_view mode, bool integrated) {
  const std::string_view reg_tag = integrated ? "reg/i" : "reg";
  if (regno < kMipsGprRtlNames.size()) {
    return absl::StrFormat("(%s:%s %u %s)", reg_tag, mode, regno, kMipsGprRtlNames[regno]);
  }
  return absl::StrFormat("(%s:%s %u)", reg_tag, mode, regno);
}

int ParseMipsAsmRegNumber(std::string_view tok) {
  std::string_view s = absl::StripAsciiWhitespace(tok);
  if (s.empty() || s.front() != '$') {
    return -1;
  }
  s.remove_prefix(1);
  if (s == "sp") return 29;
  if (s == "fp" || s == "s8") return 30;
  if (s == "ra") return 31;
  if (s == "zero") return 0;
  int regno = -1;
  if (absl::SimpleAtoi(s, &regno) && regno >= 0 && regno < 32) {
    return regno;
  }
  for (size_t i = 0; i < kMipsGprRtlNames.size(); ++i) {
    if (s == kMipsGprRtlNames[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int MaxPseudoRegInCfg(const RtlControlFlowGraph& cfg) {
  int max_pseudo = -1;
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      const std::string clean = StripTrailingRegNotesFromPattern(pat);
      size_t pos = 0;
      while ((pos = clean.find("(reg", pos)) != std::string_view::npos) {
        size_t p = pos + 4;
        while (p < clean.size() && !absl::ascii_isspace(clean[p]) && clean[p] != ')') {
          ++p;
        }
        while (p < clean.size() && absl::ascii_isspace(clean[p])) {
          ++p;
        }
        const size_t num_start = p;
        while (p < clean.size() && absl::ascii_isdigit(clean[p])) {
          ++p;
        }
        int regno = -1;
        if (p > num_start && absl::SimpleAtoi(clean.substr(num_start, p - num_start), &regno) &&
            regno >= kFirstMipsPseudoRegister) {
          max_pseudo = std::max(max_pseudo, regno);
        }
        pos = p;
      }
    }
  }
  return max_pseudo;
}

}  // namespace rom_nom_nom
