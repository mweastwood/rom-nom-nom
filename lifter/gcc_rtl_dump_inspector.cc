#include "lifter/gcc_rtl_dump_inspector.h"

#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/compiler.h"
#include "core/control_flow_graph.h"
#include "core/division_pattern.h"
#include "core/mips.h"
#include "core/process.h"
#include "core/toolchain.h"
#include "lifter/c_emitter.h"

namespace rom_nom_nom {
namespace {

constexpr RtlPassKind kOrderedPasses[] = {
    RtlPassKind::kRtl,  RtlPassKind::kJump, RtlPassKind::kCse,     RtlPassKind::kLoop,
    RtlPassKind::kCse2, RtlPassKind::kFlow, RtlPassKind::kCombine, RtlPassKind::kSched,
    RtlPassKind::kLreg, RtlPassKind::kGreg, RtlPassKind::kSched2,  RtlPassKind::kJump2,
    RtlPassKind::kDbr,
};

std::string ReadFileToString(const std::filesystem::path& path) {
  std::ifstream ifs(path, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    return "";
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

void WriteStringToFile(const std::filesystem::path& path, std::string_view content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc | std::ios::binary);
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

int UpdateParenBalance(std::string_view line, int current_depth, bool* in_quotes) {
  int depth = current_depth;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
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

RtlInsnEntry ParseSexprEntry(std::string raw_sexpr) {
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
    size_t start = pos;
    while (pos < inner.size() && !absl::ascii_isspace(inner[pos]) && inner[pos] != '(' &&
           inner[pos] != ')') {
      ++pos;
    }
    return inner.substr(start, pos - start);
  };

  std::string_view tag_with_flags = next_token();
  std::string_view base_tag = tag_with_flags;
  if (size_t sep_pos = tag_with_flags.find_first_of("/:"); sep_pos != std::string_view::npos) {
    base_tag = tag_with_flags.substr(0, sep_pos);
    std::string_view rest_flags = tag_with_flags.substr(sep_pos);
    if (absl::StrContains(rest_flags, "/s")) {
      entry.in_annulled_branch_delay = true;
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
    size_t note_pos = view.find("NOTE_INSN_");
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

// Normalizes an active RTL instruction S-expression for structural comparison across compilations
// by canonicalizing instruction UIDs, label_ref numbers, and virtual pseudo-register numbers
// (>= 72 in MIPS GCC 2.7.2) while preserving RTL opcodes, machine modes, constants, and hard
// register assignments.
std::string NormalizeInsnSexprForComparison(std::string_view sexpr) {
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

    // Strip the 3 UID numbers following `(insn...`, `(jump_insn...`, `(call_insn...`
    if (sexpr[i] == '(') {
      std::string_view rest = sexpr.substr(i + 1);
      auto starts_with_insn_tag = [&](std::string_view prefix) {
        if (!absl::StartsWith(rest, prefix)) {
          return false;
        }
        size_t after = prefix.size();
        return after < rest.size() &&
               (rest[after] == ' ' || rest[after] == '/' || rest[after] == ':');
      };
      if (starts_with_insn_tag("insn") || starts_with_insn_tag("jump_insn") ||
          starts_with_insn_tag("call_insn")) {
        out.push_back('(');
        ++i;
        // Copy base tag (`insn`, `jump_insn`, `call_insn`) and `/s` flag if present, stripping
        // scheduler `:QI` / `:HI` mode annotations so pre-sched and post-sched dumps compare equal.
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != ':' &&
               sexpr[i] != '/') {
          out.push_back(sexpr[i++]);
        }
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i])) {
          ++i;
        }
        // Skip up to 3 integer tokens (uid, prev_uid, next_uid).
        for (int k = 0; k < 3; ++k) {
          size_t save = i;
          while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
            ++i;
          }
          size_t num_start = i;
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

      if (absl::StartsWith(rest, "label_ref ")) {
        out.append("(label_ref)");
        i += 1 + std::string_view("label_ref ").size();
        while (i < sexpr.size() && sexpr[i] != ')') {
          ++i;
        }
        if (i < sexpr.size() && sexpr[i] == ')') {
          ++i;
        }
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

      if (absl::StartsWith(rest, "reg")) {
        // Check if this is `(reg...:MODE <num>)` where <num> >= 72 (virtual pseudo-register).
        out.push_back('(');
        ++i;
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i]) && sexpr[i] != ')') {
          out.push_back(sexpr[i++]);
        }
        while (i < sexpr.size() && absl::ascii_isspace(sexpr[i])) {
          ++i;
        }
        size_t num_start = i;
        while (i < sexpr.size() && absl::ascii_isdigit(sexpr[i])) {
          ++i;
        }
        if (i > num_start) {
          int reg_num = -1;
          std::string_view num_str = sexpr.substr(num_start, i - num_start);
          if (absl::SimpleAtoi(num_str, &reg_num) && reg_num >= 72) {
            out.append(" pseudo");
          } else {
            out.push_back(' ');
            out.append(num_str);
          }
        }
        continue;
      }
    }

    out.push_back(sexpr[i++]);
  }

  return out;
}

}  // namespace

size_t RtlFunctionDump::ActiveInsnCount() const {
  size_t count = 0;
  for (const auto& insn : insns) {
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
    if (insn.is_sequence) {
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

GccCompilationDump::GccCompilationDump(std::string assembly_text,
                                       absl::flat_hash_map<RtlPassKind, RtlPassDump> pass_dumps)
    : assembly_text_(std::move(assembly_text)), pass_dumps_(std::move(pass_dumps)) {}

bool GccCompilationDump::HasPass(RtlPassKind pass) const {
  return pass_dumps_.contains(pass);
}

const RtlPassDump* GccCompilationDump::GetPass(RtlPassKind pass) const {
  auto it = pass_dumps_.find(pass);
  return it != pass_dumps_.end() ? &it->second : nullptr;
}

const RtlFunctionDump* GccCompilationDump::GetFunctionPass(std::string_view func_name,
                                                           RtlPassKind pass) const {
  const RtlPassDump* pass_dump = GetPass(pass);
  if (pass_dump == nullptr) {
    return nullptr;
  }
  return pass_dump->FindFunction(func_name);
}

bool GccCompilationDump::PassMatches(const GccCompilationDump& other, std::string_view func_name,
                                     RtlPassKind pass) const {
  const RtlFunctionDump* lhs_fn = GetFunctionPass(func_name, pass);
  const RtlFunctionDump* rhs_fn = other.GetFunctionPass(func_name, pass);
  if (lhs_fn == nullptr && rhs_fn == nullptr) {
    return true;
  }
  if (lhs_fn == nullptr || rhs_fn == nullptr) {
    return false;
  }

  std::vector<std::string> lhs_active;
  for (const auto& insn : lhs_fn->insns) {
    if (insn.kind == RtlInsnKind::kInsn || insn.kind == RtlInsnKind::kJumpInsn ||
        insn.kind == RtlInsnKind::kCallInsn) {
      lhs_active.push_back(NormalizeInsnSexprForComparison(insn.raw_sexpr));
    }
  }
  std::vector<std::string> rhs_active;
  for (const auto& insn : rhs_fn->insns) {
    if (insn.kind == RtlInsnKind::kInsn || insn.kind == RtlInsnKind::kJumpInsn ||
        insn.kind == RtlInsnKind::kCallInsn) {
      rhs_active.push_back(NormalizeInsnSexprForComparison(insn.raw_sexpr));
    }
  }

  return lhs_active == rhs_active;
}

std::optional<RtlPassKind> GccCompilationDump::FindFirstDivergentPass(
    const GccCompilationDump& other, std::string_view func_name) const {
  for (RtlPassKind pass : GccRtlDumpInspector::AllPasses()) {
    if (!PassMatches(other, func_name, pass)) {
      return pass;
    }
  }
  return std::nullopt;
}

GccRtlDumpInspector::GccRtlDumpInspector(Toolchain toolchain, Compiler::ProcessRunner runner)
    : toolchain_(std::move(toolchain)), runner_(std::move(runner)) {}

absl::StatusOr<GccRtlDumpInspector> GccRtlDumpInspector::Create(
    const std::filesystem::path& repo_root) {
  ToolchainOptions opts;
  opts.repo_root = repo_root;
  if (opts.repo_root.empty()) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    if (test_srcdir != nullptr && *test_srcdir != '\0') {
      std::filesystem::path runfiles(test_srcdir);
      runfiles /= (test_workspace != nullptr && *test_workspace != '\0') ? test_workspace : "_main";
      if (std::filesystem::exists(runfiles / "toolchain" / "gcc-2.7.2" / "gcc")) {
        opts.repo_root = runfiles;
      }
    }
  }
  opts.mode = ToolchainMode::kOriginal;
  auto toolchain_or = Toolchain::Discover(std::move(opts));
  if (!toolchain_or.ok()) {
    return toolchain_or.status();
  }
  return GccRtlDumpInspector(std::move(*toolchain_or));
}

absl::Span<const RtlPassKind> GccRtlDumpInspector::AllPasses() {
  return absl::MakeConstSpan(kOrderedPasses);
}

std::string_view GccRtlDumpInspector::PassExtension(RtlPassKind pass) {
  switch (pass) {
    case RtlPassKind::kRtl:
      return ".rtl";
    case RtlPassKind::kJump:
      return ".jump";
    case RtlPassKind::kCse:
      return ".cse";
    case RtlPassKind::kLoop:
      return ".loop";
    case RtlPassKind::kCse2:
      return ".cse2";
    case RtlPassKind::kFlow:
      return ".flow";
    case RtlPassKind::kCombine:
      return ".combine";
    case RtlPassKind::kSched:
      return ".sched";
    case RtlPassKind::kLreg:
      return ".lreg";
    case RtlPassKind::kGreg:
      return ".greg";
    case RtlPassKind::kSched2:
      return ".sched2";
    case RtlPassKind::kJump2:
      return ".jump2";
    case RtlPassKind::kDbr:
      return ".dbr";
  }
  return ".rtl";
}

RtlPassDump GccRtlDumpInspector::ParsePassDump(RtlPassKind pass, std::string_view raw_text) {
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
      paren_depth = UpdateParenBalance(line, paren_depth, &in_quotes);
      if (paren_depth <= 0) {
        paren_depth = 0;
        in_quotes = false;
        if (current_fn != nullptr) {
          current_fn->insns.push_back(ParseSexprEntry(std::move(current_sexpr)));
        }
        current_sexpr.clear();
      }
      continue;
    }

    constexpr std::string_view kFuncMarker = ";; Function ";
    if (absl::StartsWith(line, kFuncMarker)) {
      std::string_view fn_name = absl::StripAsciiWhitespace(line.substr(kFuncMarker.size()));
      functions.push_back(RtlFunctionDump{
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
            .function_name = "<anonymous>",
            .header_text = {},
            .insns = {},
        });
        current_fn = &functions.back();
      }
      current_sexpr = std::string(line);
      in_quotes = false;
      paren_depth = UpdateParenBalance(line, 0, &in_quotes);
      if (paren_depth <= 0) {
        paren_depth = 0;
        in_quotes = false;
        current_fn->insns.push_back(ParseSexprEntry(std::move(current_sexpr)));
        current_sexpr.clear();
      }
      continue;
    }

    std::string_view trimmed = absl::StripAsciiWhitespace(line);
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
    current_fn->insns.push_back(ParseSexprEntry(std::move(current_sexpr)));
  }

  return RtlPassDump(pass, std::string(raw_text), std::move(preamble), std::move(functions));
}

absl::StatusOr<GccCompilationDump> GccRtlDumpInspector::InspectC(
    std::string_view c_code, const GccRtlInspectOptions& options) const {
  if (toolchain_.Mode() != ToolchainMode::kOriginal) {
    return absl::FailedPreconditionError(
        "GccRtlDumpInspector::InspectC requires ToolchainMode::kOriginal (KMC GCC 2.7.2).");
  }

  static std::atomic<uint64_t> seq_counter{0};
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  std::filesystem::path tmp_base = (test_tmpdir != nullptr && *test_tmpdir != '\0')
                                       ? std::filesystem::path(test_tmpdir)
                                       : std::filesystem::temp_directory_path();
  std::filesystem::path work_dir =
      tmp_base / absl::StrFormat("gcc_rtl_inspect_%d_%d", getpid(),
                                 seq_counter.fetch_add(1, std::memory_order_relaxed));

  std::error_code ec;
  std::filesystem::create_directories(work_dir, ec);
  if (ec) {
    return absl::InternalError(
        absl::StrFormat("GccRtlDumpInspector::InspectC: Failed to create temp directory '%s': %s",
                        work_dir.string(), ec.message()));
  }

  auto cleanup = [&]() {
    std::error_code remove_ec;
    std::filesystem::remove_all(work_dir, remove_ec);
  };

  if (options.include_types_header) {
    TypesHeaderOptions types_opts;
    types_opts.include_math_intrinsics = false;
    types_opts.extra_declarations = options.extra_types_header;
    WriteStringToFile(work_dir / "types.h", CEmitter::EmitTypesHeader(types_opts));
  }

  std::string full_c = (options.include_types_header && !absl::StrContains(c_code, "types.h"))
                           ? absl::StrCat("#include \"types.h\"\n", c_code)
                           : std::string(c_code);

  std::filesystem::path src_file = work_dir / "snippet.c";
  std::filesystem::path out_s = work_dir / "snippet.s272";
  WriteStringToFile(src_file, full_c);

  std::vector<std::string> flags = options.opt_flags;
  flags.push_back("-da");

  std::vector<std::string> gcc_args =
      toolchain_.BuildGcc272CompileArgs(flags, {work_dir}, src_file, out_s);

  ProcessOptions proc_opts{
      .program = toolchain_.Gcc272().string(),
      .args = std::move(gcc_args),
      .working_directory = work_dir,
  };
  auto proc_result = runner_(proc_opts);
  if (!proc_result.ok()) {
    cleanup();
    return proc_result.status();
  }
  if (!proc_result->Ok()) {
    std::string err_msg = absl::StrFormat(
        "GccRtlDumpInspector::InspectC: GCC 2.7.2 compilation failed (exit code %d):\n%s\n%s",
        proc_result->exit_code, proc_result->stdout_output, proc_result->stderr_output);
    cleanup();
    return absl::InternalError(err_msg);
  }

  std::string assembly_text = ReadFileToString(out_s);
  absl::flat_hash_map<RtlPassKind, RtlPassDump> pass_dumps;
  for (RtlPassKind pass : AllPasses()) {
    std::filesystem::path dump_path = work_dir / absl::StrCat("snippet.c", PassExtension(pass));
    if (std::filesystem::exists(dump_path, ec)) {
      std::string raw_dump = ReadFileToString(dump_path);
      pass_dumps.emplace(pass, ParsePassDump(pass, raw_dump));
    }
  }

  cleanup();
  return GccCompilationDump(std::move(assembly_text), std::move(pass_dumps));
}

namespace {

std::string ExtractTopLevelPatternFromInsnSexpr(std::string_view sexpr) {
  std::string_view s = absl::StripAsciiWhitespace(sexpr);
  if (s.empty() || s.front() != '(') {
    return std::string(s);
  }
  size_t pos = 1;
  // Skip tag (`insn`, `jump_insn`, `call_insn`, plus `/flags`).
  while (pos < s.size() && !absl::ascii_isspace(s[pos]) && s[pos] != '(' && s[pos] != ')') {
    ++pos;
  }
  // Skip 3 integer tokens (`uid`, `prev_uid`, `next_uid`).
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
  const size_t start = pos;
  int depth = 0;
  bool in_quotes = false;
  for (size_t i = start; i < s.size(); ++i) {
    char c = s[i];
    if (in_quotes) {
      if (c == '\\' && i + 1 < s.size()) {
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
        return std::string(s.substr(start, i - start + 1));
      }
    }
  }
  return std::string(s.substr(start));
}

std::vector<int> ExtractLabelRefsFromPattern(std::string_view pattern) {
  std::vector<int> refs;
  size_t pos = 0;
  while (pos < pattern.size()) {
    size_t idx = pattern.find("(label_ref", pos);
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
    size_t num_start = p;
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

std::string CanonicalizeRtlPattern(std::string_view pattern) {
  // Collapse whitespace and strip `/v`, `/s`, `/u`, `/i` flags on `reg`, `mem`, `symbol_ref`,
  // `label_ref`, plus canonicalize `(label_ref ...)` targets (since edge targets are verified
  // via `successors`).
  std::string collapsed;
  collapsed.reserve(pattern.size());
  for (size_t i = 0; i < pattern.size();) {
    if (absl::ascii_isspace(pattern[i])) {
      if (!collapsed.empty() && collapsed.back() != ' ') {
        collapsed.push_back(' ');
      }
      while (i < pattern.size() && absl::ascii_isspace(pattern[i])) {
        ++i;
      }
      continue;
    }
    if (pattern[i] == '(') {
      std::string_view rest = pattern.substr(i + 1);
      if (absl::StartsWith(rest, "label_ref")) {
        collapsed.append("(label_ref)");
        i += 1 + std::string_view("label_ref").size();
        while (i < pattern.size() && pattern[i] != ')') {
          ++i;
        }
        if (i < pattern.size() && pattern[i] == ')') {
          ++i;
        }
        continue;
      }
    }
    collapsed.push_back(pattern[i++]);
  }
  return collapsed;
}

std::string GccRegExpr(Register r, std::string_view mode = "SI") {
  static constexpr std::string_view kGccGprNames[32] = {
      "$0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
      "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
      "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
  };
  const int regno = static_cast<int>(r);
  if (regno == 0) {
    return "(const_int 0)";
  }
  return absl::StrFormat("(reg:%s %d %s)", mode, regno, kGccGprNames[regno]);
}

std::string LiftMipsInstToCanonicalRtl(const Instruction& inst) {
  if (inst.IsReturn()) {
    return "(parallel[ (return) (use (reg:SI 31 ra)) ] )";
  }
  if (inst.opcode == Opcode::kJ) {
    return "(set (pc) (label_ref))";
  }
  if (inst.opcode == Opcode::kJr && inst.rs.has_value() && *inst.rs != Register::kRa) {
    return absl::StrFormat("(parallel[ (set (pc) %s) (use (label_ref)) ] )", GccRegExpr(*inst.rs));
  }
  if (inst.IsBranch()) {
    std::string cond;
    switch (inst.opcode) {
      case Opcode::kBeq:
      case Opcode::kBeql:
        if (inst.rs == Register::kZero && inst.rt == Register::kZero) {
          return "(set (pc) (label_ref))";
        }
        if (inst.rt == Register::kZero) {
          cond = absl::StrFormat("(eq:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        } else if (inst.rs == Register::kZero) {
          cond = absl::StrFormat("(eq:SI %s (const_int 0))", GccRegExpr(*inst.rt));
        } else {
          cond = absl::StrFormat("(eq:SI %s %s)", GccRegExpr(*inst.rs), GccRegExpr(*inst.rt));
        }
        break;
      case Opcode::kBne:
      case Opcode::kBnel:
        if (inst.rt == Register::kZero) {
          cond = absl::StrFormat("(ne:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        } else if (inst.rs == Register::kZero) {
          cond = absl::StrFormat("(ne:SI %s (const_int 0))", GccRegExpr(*inst.rt));
        } else {
          cond = absl::StrFormat("(ne:SI %s %s)", GccRegExpr(*inst.rs), GccRegExpr(*inst.rt));
        }
        break;
      case Opcode::kBlez:
      case Opcode::kBlezl:
        cond = absl::StrFormat("(le:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBgtz:
      case Opcode::kBgtzl:
        cond = absl::StrFormat("(gt:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBltz:
      case Opcode::kBltzl:
        cond = absl::StrFormat("(lt:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBgez:
      case Opcode::kBgezl:
        if (inst.rs == Register::kZero) {
          return "(set (pc) (label_ref))";
        }
        cond = absl::StrFormat("(ge:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBc1t:
      case Opcode::kBc1tl:
        cond = "(ne:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0))";
        break;
      case Opcode::kBc1f:
      case Opcode::kBc1fl:
        cond = "(eq:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0))";
        break;
      default:
        break;
    }
    if (!cond.empty()) {
      return absl::StrFormat("(set (pc) (if_then_else %s (label_ref) (pc)))", cond);
    }
  }
  return inst.Disassemble();
}

std::vector<RtlInsnEntry> ExpandSequenceSexpr(const RtlInsnEntry& seq_entry) {
  std::vector<RtlInsnEntry> inner_entries;
  const std::string pat = ExtractTopLevelPatternFromInsnSexpr(seq_entry.raw_sexpr);
  size_t bracket_pos = pat.find('[');
  if (bracket_pos == std::string::npos) {
    return {seq_entry};
  }
  size_t i = bracket_pos + 1;
  while (i < pat.size()) {
    while (i < pat.size() && absl::ascii_isspace(pat[i])) {
      ++i;
    }
    if (i >= pat.size() || pat[i] == ']') {
      break;
    }
    if (pat[i] != '(') {
      ++i;
      continue;
    }
    size_t start = i;
    int depth = 0;
    bool in_quotes = false;
    for (; i < pat.size(); ++i) {
      char c = pat[i];
      if (in_quotes) {
        if (c == '\\' && i + 1 < pat.size()) {
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
          ++i;
          break;
        }
      }
    }
    inner_entries.push_back(ParseSexprEntry(pat.substr(start, i - start)));
  }
  // If the sequence bundles a `jump_insn` with its delay-slot `insn`, place the delay-slot `insn`
  // immediately before the `jump_insn` so that both remain in the same basic block before
  // `jump_insn` terminates the block.
  if (inner_entries.size() == 2 && inner_entries[0].kind == RtlInsnKind::kJumpInsn &&
      inner_entries[1].kind == RtlInsnKind::kInsn) {
    std::swap(inner_entries[0], inner_entries[1]);
  }
  return inner_entries;
}

}  // namespace

RtlCanonicalCfg GccRtlDumpInspector::BuildCanonicalCfgFromRtlDump(const RtlFunctionDump& dump) {
  RtlCanonicalCfg result;

  // Flatten top-level entries (expanding `(sequence [...])` if present in `.dbr`).
  std::vector<RtlInsnEntry> flat_entries;
  flat_entries.reserve(dump.insns.size());
  for (const auto& entry : dump.insns) {
    if (entry.is_sequence) {
      for (auto& sub : ExpandSequenceSexpr(entry)) {
        flat_entries.push_back(std::move(sub));
      }
    } else {
      flat_entries.push_back(entry);
    }
  }

  struct FlatRtlItem {
    RtlInsnKind kind = RtlInsnKind::kOther;
    int uid = -1;
    std::string pattern;
  };
  std::vector<FlatRtlItem> items;
  for (size_t idx = 0; idx < flat_entries.size(); ++idx) {
    const auto& entry = flat_entries[idx];
    if (entry.kind == RtlInsnKind::kNote || entry.kind == RtlInsnKind::kOther) {
      continue;
    }
    // Skip jump-table label + `addr_vec` / `addr_diff_vec` + trailing barrier so jump-table data
    // does not create a spurious basic block in the RTL CFG.
    if (entry.kind == RtlInsnKind::kCodeLabel && idx + 1 < flat_entries.size() &&
        flat_entries[idx + 1].kind == RtlInsnKind::kJumpInsn) {
      const std::string next_pat =
          ExtractTopLevelPatternFromInsnSexpr(flat_entries[idx + 1].raw_sexpr);
      if (absl::StartsWith(next_pat, "(addr_vec:") ||
          absl::StartsWith(next_pat, "(addr_diff_vec:")) {
        ++idx;
        while (idx + 1 < flat_entries.size() &&
               flat_entries[idx + 1].kind == RtlInsnKind::kBarrier) {
          ++idx;
        }
        continue;
      }
    }
    const std::string pat = ExtractTopLevelPatternFromInsnSexpr(entry.raw_sexpr);
    // Ignore non-machine `(use ...)` and `(clobber ...)` pseudo-insns and jump-table `addr_vec`
    // data.
    if (entry.kind == RtlInsnKind::kInsn &&
        (absl::StartsWith(pat, "(use ") || absl::StartsWith(pat, "(clobber "))) {
      continue;
    }
    if (entry.kind == RtlInsnKind::kJumpInsn &&
        (absl::StartsWith(pat, "(addr_vec:") || absl::StartsWith(pat, "(addr_diff_vec:"))) {
      continue;
    }
    items.push_back(FlatRtlItem{
        .kind = entry.kind,
        .uid = entry.uid,
        .pattern = pat,
    });
  }

  if (items.empty()) {
    result.rpo_blocks.push_back(RtlCfgBlock{});
    return result;
  }

  // Partition into basic blocks using `flow.c:find_basic_blocks` rules:
  // A new block begins at `kCodeLabel` or at the first executable instruction after `kJumpInsn` /
  // `kBarrier`.
  struct RawRtlBlock {
    std::vector<int> label_uids;
    std::vector<std::string> instructions;
    std::vector<int> jump_target_labels;
    bool ends_with_jump = false;
    bool ends_with_uncond_jump = false;
    bool ends_with_return = false;
    bool followed_by_barrier = false;
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
      if (it.kind == RtlInsnKind::kInsn || it.kind == RtlInsnKind::kCallInsn) {
        const std::string canon_pat = CanonicalizeRtlPattern(it.pattern);
        // In `.dbr`, `reorg.c` may call `try_split` on `movstrsi_internal` (`(use (const_int 0))` =
        // `BLOCK_MOVE_NORMAL`) to fill `jr $ra`'s delay slot, splitting it into
        // `movstrsi_internal2`
        // (`(use (const_int 1))` = `BLOCK_MOVE_NOT_LAST`) + `movstrsi_internal3` (`(use (const_int
        // 2))` = `BLOCK_MOVE_LAST`). Skip the split `BLOCK_MOVE_LAST` companion so every
        // `movstrsi_internal` block move is 1 canonical RTL instruction across all stages.
        if (!absl::EndsWith(canon_pat, "(use (const_int 2)) ] )")) {
          blk.instructions.push_back(canon_pat);
        }
        ++i;
      } else if (it.kind == RtlInsnKind::kJumpInsn) {
        blk.ends_with_jump = true;
        if (absl::StrContains(it.pattern, "(return)")) {
          blk.ends_with_return = true;
        } else {
          const std::string canon_pat = CanonicalizeRtlPattern(it.pattern);
          blk.instructions.push_back(canon_pat);
          if (absl::StartsWith(canon_pat, "(set (pc) (label_ref")) {
            blk.ends_with_uncond_jump = true;
            blk.jump_target_labels = ExtractLabelRefsFromPattern(it.pattern);
          } else if (absl::StartsWith(canon_pat, "(parallel[ (set (pc) (reg:")) {
            // Tablejump `(parallel [(set (pc) (reg:SI ...)) (use (label_ref L_table))])`:
            // extract targets from the associated `addr_vec` following `L_table`.
            blk.ends_with_uncond_jump = true;
            std::vector<int> table_refs = ExtractLabelRefsFromPattern(it.pattern);
            if (!table_refs.empty()) {
              const int table_uid = table_refs[0];
              bool found_table_label = false;
              for (const auto& e : dump.insns) {
                if (e.kind == RtlInsnKind::kCodeLabel && e.uid == table_uid) {
                  found_table_label = true;
                  continue;
                }
                if (found_table_label && e.kind == RtlInsnKind::kJumpInsn) {
                  std::string p = ExtractTopLevelPatternFromInsnSexpr(e.raw_sexpr);
                  if (absl::StartsWith(p, "(addr_vec:") || absl::StartsWith(p, "(addr_diff_vec:")) {
                    for (int t_uid : ExtractLabelRefsFromPattern(p)) {
                      if (std::find(blk.jump_target_labels.begin(), blk.jump_target_labels.end(),
                                    t_uid) == blk.jump_target_labels.end()) {
                        blk.jump_target_labels.push_back(t_uid);
                      }
                    }
                  }
                  break;
                }
              }
            }
          } else {
            blk.jump_target_labels = ExtractLabelRefsFromPattern(it.pattern);
          }
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

    if (blk.instructions.empty() && blk.label_uids.empty() && !blk.ends_with_return) {
      continue;
    }
    raw_blocks.push_back(std::move(blk));
  }

  // Coalesce contiguous blocks where `b` is only reached via fallthrough from a non-jumping block
  // `b - 1` and none of `b`'s labels are referenced by any jump or tablejump (e.g. unreferenced
  // trailing `return_label` at `-O0`).
  absl::flat_hash_set<int> referenced_labels;
  for (const auto& blk : raw_blocks) {
    for (int t_uid : blk.jump_target_labels) {
      referenced_labels.insert(t_uid);
    }
  }
  std::vector<RawRtlBlock> coalesced_blocks;
  coalesced_blocks.reserve(raw_blocks.size());
  for (auto& blk : raw_blocks) {
    bool has_ref_label = false;
    for (int l_uid : blk.label_uids) {
      if (referenced_labels.contains(l_uid)) {
        has_ref_label = true;
        break;
      }
    }
    if (!coalesced_blocks.empty() && !has_ref_label) {
      auto& prev = coalesced_blocks.back();
      if (!prev.ends_with_jump && !prev.ends_with_return && !prev.followed_by_barrier) {
        prev.instructions.insert(prev.instructions.end(), blk.instructions.begin(),
                                 blk.instructions.end());
        prev.jump_target_labels = std::move(blk.jump_target_labels);
        prev.ends_with_jump = blk.ends_with_jump;
        prev.ends_with_uncond_jump = blk.ends_with_uncond_jump;
        prev.ends_with_return = blk.ends_with_return;
        prev.followed_by_barrier = blk.followed_by_barrier;
        continue;
      }
    }
    coalesced_blocks.push_back(std::move(blk));
  }
  raw_blocks = std::move(coalesced_blocks);
  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    for (int l_uid : raw_blocks[b].label_uids) {
      label_to_block[l_uid] = b;
    }
  }

  // If the last block in the RTL stream ends with a conditional branch (e.g., a `do-while` loop
  // latch at the end of a function in pre-`.jump2` passes or non-leaf functions where
  // `function_epilogue` is emitted after the last RTL insn), append the fallthrough exit block.
  if (!raw_blocks.empty()) {
    const auto& last = raw_blocks.back();
    const bool last_is_cond_jump =
        last.ends_with_jump && !last.ends_with_uncond_jump && !last.ends_with_return;
    if (last_is_cond_jump) {
      RawRtlBlock exit_blk;
      exit_blk.ends_with_return = true;
      raw_blocks.push_back(std::move(exit_blk));
    }
  }

  // Merge empty label-only blocks at the end or falling through into the next block.
  for (size_t b = raw_blocks.size(); b > 0; --b) {
    const size_t idx = b - 1;
    if (raw_blocks[idx].instructions.empty() && !raw_blocks[idx].followed_by_barrier &&
        idx + 1 < raw_blocks.size()) {
      for (int l_uid : raw_blocks[idx].label_uids) {
        label_to_block[l_uid] = idx + 1;
      }
      for (auto& kv : label_to_block) {
        if (kv.second == idx) {
          kv.second = idx + 1;
        }
      }
    }
  }

  // Build successor edges on `raw_blocks`.
  std::vector<std::vector<size_t>> raw_succs(raw_blocks.size());
  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    const auto& blk = raw_blocks[b];
    if (blk.instructions.empty() && b + 1 < raw_blocks.size()) {
      continue;
    }
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
      size_t ft = b + 1;
      while (ft + 1 < raw_blocks.size() && raw_blocks[ft].instructions.empty() &&
             !raw_blocks[ft].followed_by_barrier) {
        ++ft;
      }
      raw_succs[b].push_back(ft);
    }
  }

  // Compute Reverse Post-Order (RPO) from entry block 0.
  std::vector<bool> visited(raw_blocks.size(), false);
  std::vector<size_t> post_order;
  auto dfs = [&](auto& self, size_t u) -> void {
    visited[u] = true;
    for (size_t v : raw_succs[u]) {
      if (v < raw_blocks.size() && !visited[v]) {
        self(self, v);
      }
    }
    post_order.push_back(u);
  };
  if (!raw_blocks.empty()) {
    dfs(dfs, 0);
  }
  std::vector<size_t> rpo(post_order.rbegin(), post_order.rend());
  absl::flat_hash_map<size_t, size_t> raw_to_rpo;
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    raw_to_rpo[rpo[idx]] = idx;
  }

  result.rpo_blocks.reserve(rpo.size());
  for (size_t raw_idx : rpo) {
    RtlCfgBlock out_blk;
    out_blk.instructions = raw_blocks[raw_idx].instructions;
    for (size_t s : raw_succs[raw_idx]) {
      if (auto it = raw_to_rpo.find(s); it != raw_to_rpo.end()) {
        out_blk.successors.push_back(it->second);
      }
    }
    result.rpo_blocks.push_back(std::move(out_blk));
  }
  return result;
}

RtlCanonicalCfg GccRtlDumpInspector::LiftControlFlowGraphToCanonicalRtl(
    const ControlFlowGraph& input_cfg) {
  // In GCC 2.7.2 (`flow.c:find_basic_blocks`), `call_insn` (`jal` / `jalr`) does not terminate a
  // basic block unless the following instruction is targeted by a label (`predecessors.size() > 1`
  // or jump-table target). Coalesce linear fallthrough chains across `IsCall()` on a copy of
  // `input_cfg` before computing canonical RPO blocks.
  ControlFlowGraph cfg = input_cfg;
  absl::flat_hash_set<uint32_t> jt_vrams;
  for (const auto& jt : cfg.JumpTables()) {
    if (jt.default_target_vram != 0) {
      jt_vrams.insert(jt.default_target_vram);
    }
    for (const auto& entry : jt.entries) {
      jt_vrams.insert(entry.target_vram);
    }
  }
  bool merged_call_chain = true;
  while (merged_call_chain) {
    merged_call_chain = false;
    std::vector<uint32_t> rpo_ids = cfg.ReversePostOrder();
    absl::flat_hash_set<uint32_t> reachable(rpo_ids.begin(), rpo_ids.end());
    for (uint32_t a_id : rpo_ids) {
      BasicBlock* blk_a = cfg.GetMutableBlock(a_id);
      if (blk_a == nullptr || blk_a->instructions.empty() || blk_a->successors.size() != 1 ||
          blk_a->outgoing_edges.size() != 1 ||
          blk_a->outgoing_edges[0].type != EdgeType::kFallthrough || blk_a->HasBranch() ||
          blk_a->HasReturn() || blk_a->HasSwitch()) {
        continue;
      }
      const Instruction* a_term = blk_a->Terminator();
      if (a_term != nullptr && (a_term->IsBranch() || (a_term->IsJump() && !a_term->IsCall()))) {
        continue;
      }
      const uint32_t b_id = blk_a->successors[0];
      if (b_id == a_id || b_id == cfg.EntryBlockId() || !reachable.contains(b_id)) {
        continue;
      }
      BasicBlock* blk_b = cfg.GetMutableBlock(b_id);
      if (blk_b == nullptr || blk_b->instructions.empty() || blk_b->predecessors.size() != 1 ||
          blk_b->predecessors[0] != a_id || jt_vrams.contains(blk_b->start_vram)) {
        continue;
      }
      blk_a->instructions.insert(blk_a->instructions.end(), blk_b->instructions.begin(),
                                 blk_b->instructions.end());
      blk_a->end_vram = blk_b->end_vram;
      if (blk_b->jump_table != nullptr) {
        blk_a->jump_table = blk_b->jump_table;
      }
      blk_a->successors = blk_b->successors;
      blk_a->outgoing_edges = blk_b->outgoing_edges;
      for (auto& edge : blk_a->outgoing_edges) {
        edge.from_block_id = a_id;
      }
      for (uint32_t succ_id : blk_a->successors) {
        BasicBlock* succ_blk = cfg.GetMutableBlock(succ_id);
        if (succ_blk != nullptr) {
          for (uint32_t& pred : succ_blk->predecessors) {
            if (pred == b_id) {
              pred = a_id;
            }
          }
        }
      }
      blk_b->instructions.clear();
      blk_b->predecessors.clear();
      blk_b->successors.clear();
      blk_b->outgoing_edges.clear();
      merged_call_chain = true;
      break;
    }
  }

  RtlCanonicalCfg result;
  std::vector<uint32_t> rpo = cfg.ReversePostOrder();
  absl::flat_hash_map<uint32_t, size_t> id_to_rpo;
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    id_to_rpo[rpo[idx]] = idx;
  }

  auto is_epilogue_candidate = [](const Instruction& inst) -> bool {
    if (inst.IsReturn()) {
      return true;
    }
    if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
         inst.opcode == Opcode::kAddu) &&
        (inst.rt == Register::kSp || inst.rd == Register::kSp) && inst.rs == Register::kSp &&
        inst.immediate > 0) {
      return true;
    }
    if ((inst.opcode == Opcode::kOr || inst.opcode == Opcode::kAddu ||
         inst.opcode == Opcode::kDaddu) &&
        inst.rd == Register::kSp && inst.rs == Register::kFp && inst.rt == Register::kZero) {
      return true;
    }
    if (inst.opcode == Opcode::kLw && (inst.rs == Register::kSp || inst.rs == Register::kFp) &&
        inst.rt.has_value() &&
        ((*inst.rt >= Register::kS0 && *inst.rt <= Register::kS7) || *inst.rt == Register::kFp ||
         *inst.rt == Register::kRa)) {
      return true;
    }
    if ((inst.opcode == Opcode::kLdc1 || inst.opcode == Opcode::kLwc1) &&
        (inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.ft.has_value() &&
        *inst.ft >= FpRegister::kF20 && *inst.ft <= FpRegister::kF31) {
      return true;
    }
    return false;
  };

  result.rpo_blocks.reserve(rpo.size());
  for (uint32_t bid : rpo) {
    const BasicBlock* blk = cfg.GetBlock(bid);
    if (blk == nullptr) {
      continue;
    }
    const auto& raw_insts = blk->instructions;
    bool block_has_return = false;
    size_t last_call_idx = 0;
    bool has_call = false;
    for (size_t i = 0; i < raw_insts.size(); ++i) {
      if (raw_insts[i].IsReturn()) {
        block_has_return = true;
      }
      if (raw_insts[i].IsCall()) {
        last_call_idx = i;
        has_call = true;
      }
    }

    // Detect Stage 0 `.dbr` `current_function_epilogue_delay_list` (`reorg.c:3208-3216`,
    // `mips.c:5189-5202`): when `jr $ra` still has `addiu $sp, $sp, tsize` in its delay slot and is
    // preceded by `lw $ra, offset($sp)` + `<epilogue_delay>`, `reorg.c` unlinked `<epilogue_delay>`
    // from `.dbr`'s `insns` chain into `current_function_epilogue_delay_list`.
    std::optional<size_t> dbr_epilogue_delay_idx;
    if (block_has_return && raw_insts.size() >= 4) {
      size_t ret_idx = raw_insts.size();
      for (size_t i = 0; i < raw_insts.size(); ++i) {
        if (raw_insts[i].IsReturn()) {
          ret_idx = i;
          break;
        }
      }
      if (ret_idx >= 2 && ret_idx + 1 < raw_insts.size()) {
        const auto& sp_dealloc = raw_insts[ret_idx + 1];
        const auto& epilogue_delay = raw_insts[ret_idx - 1];
        const auto& ra_restore = raw_insts[ret_idx - 2];
        if ((sp_dealloc.opcode == Opcode::kAddiu || sp_dealloc.opcode == Opcode::kAddi) &&
            sp_dealloc.rt == Register::kSp && sp_dealloc.rs == Register::kSp &&
            sp_dealloc.immediate > 0 && !epilogue_delay.IsNop() &&
            !is_epilogue_candidate(epilogue_delay) && ra_restore.opcode == Opcode::kLw &&
            ra_restore.rs == Register::kSp && ra_restore.rt == Register::kRa) {
          dbr_epilogue_delay_idx = ret_idx - 1;
        }
      }
    }

    // Step 1: Collapse `gas` division trap sequences and filter out NOPs and `function_epilogue`
    // instructions (which only occur after all calls in a return block).
    std::vector<Instruction> insts;
    insts.reserve(raw_insts.size());
    for (size_t i = 0; i < raw_insts.size(); ++i) {
      const auto& inst = raw_insts[i];
      if (inst.IsNop()) {
        continue;
      }
      if (dbr_epilogue_delay_idx.has_value() && i == *dbr_epilogue_delay_idx) {
        continue;
      }
      if (block_has_return && (inst.IsReturn() || (!has_call || i > last_call_idx)) &&
          is_epilogue_candidate(inst)) {
        continue;
      }
      if (auto div_pat = MatchDivisionPattern(raw_insts, i); div_pat.has_value()) {
        insts.push_back(inst);
        i = div_pat->end_index;
        continue;
      }
      insts.push_back(inst);
    }

    // Step 2: Fuse multi-instruction RTL patterns (`mips.md` & `gas` macro expansions).
    RtlCfgBlock out_blk;
    for (size_t idx = 0; idx < insts.size(); ++idx) {
      const auto& inst = insts[idx];
      // 1. Fuse `mulsi3_internal` (`mult` + `mflo` without `mfhi`).
      if ((inst.opcode == Opcode::kMult || inst.opcode == Opcode::kMultu) &&
          idx + 1 < insts.size() && insts[idx + 1].opcode == Opcode::kMflo &&
          (idx + 2 >= insts.size() || insts[idx + 2].opcode != Opcode::kMfhi)) {
        out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(inst));
        ++idx;
        continue;
      }
      // 2. Fuse `floatsisf2` / `floatsidf2` (`mips.md:2629`: `mtc1`/`lwc1` + `cvt.s.w`/`cvt.d.w`).
      if ((inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kLwc1) &&
          idx + 1 < insts.size()) {
        const auto& next = insts[idx + 1];
        const auto loaded_fp = (inst.opcode == Opcode::kMtc1) ? inst.fs : inst.ft;
        if ((next.opcode == Opcode::kCvtSW || next.opcode == Opcode::kCvtDW) &&
            loaded_fp.has_value() && next.fs == loaded_fp && next.fd == loaded_fp) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(next));
          ++idx;
          continue;
        }
      }
      // 2b. Fuse `mips_move_2words` (`DFmode` / `DImode` 2-word moves between GPR pair and
      // even/odd FPR pair: `mtc1` + `mtc1` or `mfc1` + `mfc1`).
      if (inst.opcode == Opcode::kMtc1 && idx + 1 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kMtc1 && inst.fs.has_value() &&
          insts[idx + 1].fs.has_value()) {
        const int f0 = static_cast<int>(*inst.fs);
        const int f1 = static_cast<int>(*insts[idx + 1].fs);
        if ((f0 & ~1) == (f1 & ~1) && f0 != f1) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(inst));
          ++idx;
          continue;
        }
      }
      if (inst.opcode == Opcode::kMfc1 && idx + 1 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kMfc1 && inst.fs.has_value() &&
          insts[idx + 1].fs.has_value()) {
        const int f0 = static_cast<int>(*inst.fs);
        const int f1 = static_cast<int>(*insts[idx + 1].fs);
        if ((f0 & ~1) == (f1 & ~1) && f0 != f1) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(inst));
          ++idx;
          continue;
        }
      }
      // 3. Fuse `fix_truncsfsi2` / `fix_truncdfsi2` (`mips.md:2510`: `trunc.w.s`/`trunc.w.d` +
      // `mfc1`/`swc1`, or `lui $at` + `swc1`, or `-mips1` `cfc1`..`mfc1` 7-insn macro).
      if ((inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kTruncWD) &&
          idx + 1 < insts.size()) {
        const auto& next = insts[idx + 1];
        if ((next.opcode == Opcode::kMfc1 && next.fs == inst.fd) ||
            (next.opcode == Opcode::kSwc1 && next.ft == inst.fd)) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(inst));
          ++idx;
          continue;
        }
        if (next.opcode == Opcode::kLui && next.rt == Register::kAt && idx + 2 < insts.size() &&
            insts[idx + 2].opcode == Opcode::kSwc1 && insts[idx + 2].rs == Register::kAt &&
            insts[idx + 2].ft == inst.fd) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(inst));
          idx += 2;
          continue;
        }
      }
      if (inst.opcode == Opcode::kCfc1 && idx + 6 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kOri && insts[idx + 2].opcode == Opcode::kXori &&
          insts[idx + 3].opcode == Opcode::kCtc1 &&
          (insts[idx + 4].opcode == Opcode::kCvtWS || insts[idx + 4].opcode == Opcode::kCvtWD) &&
          insts[idx + 5].opcode == Opcode::kCtc1 &&
          (insts[idx + 6].opcode == Opcode::kMfc1 || insts[idx + 6].opcode == Opcode::kSwc1)) {
        out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(insts[idx + 4]));
        idx += 6;
        continue;
      }
      // 4. Fuse `movstrsi_internal` (`mips.c:output_block_move`: unaligned `lwl`/`lwr` block copy).
      if (inst.opcode == Opcode::kLwl && inst.rs.has_value() && idx + 3 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kLwr && insts[idx + 1].rs == inst.rs &&
          insts[idx + 1].rt == inst.rt) {
        const Register src_reg = *inst.rs;
        std::optional<Register> dst_reg;
        int16_t max_src_off = -1;
        size_t scan = idx;
        size_t last_store_end = idx;
        while (scan < insts.size()) {
          const auto& s = insts[scan];
          if (s.opcode == Opcode::kLwl && scan + 1 < insts.size() &&
              insts[scan + 1].opcode == Opcode::kLwr && s.rs == src_reg &&
              insts[scan + 1].rs == src_reg && s.rt == insts[scan + 1].rt) {
            const int16_t word_off = std::min(s.immediate, insts[scan + 1].immediate);
            if (word_off <= max_src_off) {
              break;
            }
            max_src_off = word_off;
            scan += 2;
            continue;
          }
          if ((s.opcode == Opcode::kLb || s.opcode == Opcode::kLbu || s.opcode == Opcode::kLh ||
               s.opcode == Opcode::kLhu) &&
              s.rs == src_reg && s.immediate > max_src_off) {
            max_src_off = s.immediate;
            ++scan;
            continue;
          }
          if (s.opcode == Opcode::kSwl && scan + 1 < insts.size() &&
              insts[scan + 1].opcode == Opcode::kSwr && s.rs.has_value() &&
              insts[scan + 1].rs == s.rs && s.rt == insts[scan + 1].rt &&
              (!dst_reg.has_value() || *dst_reg == *s.rs)) {
            dst_reg = *s.rs;
            last_store_end = scan + 1;
            scan += 2;
            continue;
          }
          if ((s.opcode == Opcode::kSb || s.opcode == Opcode::kSh || s.opcode == Opcode::kSwl ||
               s.opcode == Opcode::kSwr) &&
              s.rs.has_value() && (!dst_reg.has_value() || *dst_reg == *s.rs)) {
            dst_reg = *s.rs;
            last_store_end = scan;
            ++scan;
            continue;
          }
          break;
        }
        if (last_store_end > idx) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(inst));
          idx = last_store_end;
          continue;
        }
      }
      // 5. Fuse `gas` `lui` macro expansions (`la`, `li.s`, `CONST_DOUBLE` negative 32-bit `li`,
      // `mips_move_2words` global `DFmode` load, and direct/indexed symbol loads/stores).
      if (inst.opcode == Opcode::kLui && inst.rt.has_value() && idx + 1 < insts.size()) {
        const Register hi_reg = *inst.rt;
        const auto& next = insts[idx + 1];
        // `li.s $fX, const` -> `lui $at, hi; [ori $at, $at, lo;] mtc1 $at, $fX`
        if (hi_reg == Register::kAt && next.opcode == Opcode::kMtc1 && next.rt == Register::kAt) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(next));
          ++idx;
          continue;
        }
        if (hi_reg == Register::kAt && next.opcode == Opcode::kOri && next.rt == Register::kAt &&
            next.rs == Register::kAt && idx + 2 < insts.size() &&
            insts[idx + 2].opcode == Opcode::kMtc1 && insts[idx + 2].rt == Register::kAt) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(insts[idx + 2]));
          idx += 2;
          continue;
        }
        // `CONST_DOUBLE` 32-bit unsigned magic multiplier with bit 31 set (`inst.immediate < 0`)
        // feeding `multu`: `large_int` (`mips.c:422`) returns `FALSE` because
        // `GET_CODE (op) != CONST_INT`, so `mips.md:3208` `define_split` does not split it and
        // `mips_move_1word` (`mips.c:1051`) emits `li reg, const` (which `gas` expands to
        // `lui reg, hi; ori reg, reg, lo`). Note: signed magic multipliers feeding `mult` are
        // `CONST_INT`, which `define_split` DOES split into 2 RTL `insn`s (`lui` + `ori`).
        if (inst.immediate < 0 && next.opcode == Opcode::kOri && next.rt == hi_reg &&
            next.rs == hi_reg) {
          bool feeds_multu = false;
          for (size_t k = idx + 2; k < insts.size(); ++k) {
            if (insts[k].opcode == Opcode::kMultu &&
                (insts[k].rs == hi_reg || insts[k].rt == hi_reg)) {
              feeds_multu = true;
              break;
            }
            if (insts[k].opcode == Opcode::kMult &&
                (insts[k].rs == hi_reg || insts[k].rt == hi_reg)) {
              break;
            }
            if (insts[k].rd == hi_reg ||
                ((insts[k].IsLoad() || insts[k].opcode == Opcode::kLui ||
                  insts[k].opcode == Opcode::kAddiu || insts[k].opcode == Opcode::kAddi ||
                  insts[k].opcode == Opcode::kOri || insts[k].opcode == Opcode::kAndi ||
                  insts[k].opcode == Opcode::kXori || insts[k].opcode == Opcode::kSlti ||
                  insts[k].opcode == Opcode::kSltiu) &&
                 insts[k].rt == hi_reg)) {
              break;
            }
          }
          if (feeds_multu) {
            out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(next));
            ++idx;
            continue;
          }
        }
        // `la reg, sym` -> `lui reg, %hi(sym); addiu reg, reg, %lo(sym)`
        if ((next.opcode == Opcode::kAddiu || next.opcode == Opcode::kAddi) && next.rt == hi_reg &&
            next.rs == hi_reg) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(next));
          ++idx;
          continue;
        }
        // `lw/sw ..., sym(idx_reg)` ->
        // `lui hi_reg, %hi(sym); addu hi_reg, hi_reg, idx_reg; lw/sw ..., %lo(sym)(hi_reg)`
        if (next.opcode == Opcode::kAddu && next.rd == hi_reg &&
            (next.rs == hi_reg || next.rt == hi_reg) && idx + 2 < insts.size() &&
            (insts[idx + 2].IsLoad() || insts[idx + 2].IsStore()) && insts[idx + 2].rs == hi_reg &&
            (hi_reg == Register::kAt || (insts[idx + 2].IsLoad() && insts[idx + 2].rt == hi_reg))) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(insts[idx + 2]));
          idx += 2;
          continue;
        }
        // `mips_move_2words` global `DFmode` / `DImode` load into an even/odd GPR pair:
        // `lui even, %hi(sym); lw even, %lo(sym)(even); lui odd, %hi(sym+4); lw odd,
        // %lo(sym+4)(odd)`
        if (next.opcode == Opcode::kLw && next.rs == hi_reg && next.rt == hi_reg &&
            (static_cast<int>(hi_reg) & 1) == 0 && idx + 3 < insts.size() &&
            insts[idx + 2].opcode == Opcode::kLui && insts[idx + 2].rt.has_value() &&
            static_cast<int>(*insts[idx + 2].rt) == static_cast<int>(hi_reg) + 1 &&
            insts[idx + 3].opcode == Opcode::kLw && insts[idx + 3].rs == insts[idx + 2].rt &&
            insts[idx + 3].rt == insts[idx + 2].rt &&
            insts[idx + 3].immediate == next.immediate + 4) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(next));
          idx += 3;
          continue;
        }
        // Direct symbol load/store `lw/lh/lhu/lb/lbu/sw/sh/sb/lwc1/swc1 ..., sym` ->
        // `lui $at (or dest), %hi(sym); load/store ..., %lo(sym)($at or dest)`
        if ((next.IsLoad() || next.IsStore()) && next.rs == hi_reg &&
            (hi_reg == Register::kAt || (next.IsLoad() && next.rt == hi_reg))) {
          out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(next));
          ++idx;
          continue;
        }
      }
      out_blk.instructions.push_back(LiftMipsInstToCanonicalRtl(inst));
    }
    for (uint32_t succ_id : blk->successors) {
      if (auto it = id_to_rpo.find(succ_id); it != id_to_rpo.end()) {
        out_blk.successors.push_back(it->second);
      }
    }
    result.rpo_blocks.push_back(std::move(out_blk));
  }
  if (result.rpo_blocks.empty()) {
    result.rpo_blocks.push_back(RtlCfgBlock{});
  }
  return result;
}

RtlCfgPassComparison GccRtlDumpInspector::CompareCfgToRtlDump(const ControlFlowGraph& cfg,
                                                              const RtlFunctionDump* dump) {
  RtlCfgPassComparison cmp;
  if (dump == nullptr) {
    cmp.pass_present = false;
    return cmp;
  }
  cmp.pass_present = true;
  RtlCanonicalCfg actual = LiftControlFlowGraphToCanonicalRtl(cfg);
  RtlCanonicalCfg expected = BuildCanonicalCfgFromRtlDump(*dump);

  cmp.actual_block_count = actual.rpo_blocks.size();
  cmp.expected_block_count = expected.rpo_blocks.size();
  for (const auto& b : actual.rpo_blocks) {
    cmp.actual_insn_count += b.instructions.size();
  }
  for (const auto& b : expected.rpo_blocks) {
    cmp.expected_insn_count += b.instructions.size();
  }

  cmp.block_count_matches = (cmp.actual_block_count == cmp.expected_block_count);
  if (!cmp.block_count_matches) {
    std::string act_desc;
    for (size_t b = 0; b < actual.rpo_blocks.size(); ++b) {
      absl::StrAppendFormat(&act_desc, "%s%zu(i=%zu->[%s])", b ? " " : "", b,
                            actual.rpo_blocks[b].instructions.size(),
                            absl::StrJoin(actual.rpo_blocks[b].successors, ","));
    }
    std::string exp_desc;
    for (size_t b = 0; b < expected.rpo_blocks.size(); ++b) {
      absl::StrAppendFormat(&exp_desc, "%s%zu(i=%zu->[%s])", b ? " " : "", b,
                            expected.rpo_blocks[b].instructions.size(),
                            absl::StrJoin(expected.rpo_blocks[b].successors, ","));
    }
    cmp.mismatch_detail =
        absl::StrFormat("RPO blocks got %zu {%s} != exp %zu {%s}", cmp.actual_block_count, act_desc,
                        cmp.expected_block_count, exp_desc);
    return cmp;
  }

  cmp.successors_match = true;
  cmp.instructions_match = true;
  for (size_t b = 0; b < actual.rpo_blocks.size(); ++b) {
    if (actual.rpo_blocks[b].successors != expected.rpo_blocks[b].successors) {
      cmp.successors_match = false;
      if (cmp.mismatch_detail.empty()) {
        cmp.mismatch_detail =
            absl::StrFormat("RPO blk %zu edges got [%s] != exp [%s]", b,
                            absl::StrJoin(actual.rpo_blocks[b].successors, ","),
                            absl::StrJoin(expected.rpo_blocks[b].successors, ","));
      }
    }
    if (actual.rpo_blocks[b].instructions.size() != expected.rpo_blocks[b].instructions.size()) {
      cmp.instructions_match = false;
      if (cmp.mismatch_detail.empty()) {
        cmp.mismatch_detail =
            absl::StrFormat("RPO blk %zu insn count got %zu [%s] != exp %zu [%s]", b,
                            actual.rpo_blocks[b].instructions.size(),
                            absl::StrJoin(actual.rpo_blocks[b].instructions, " ; "),
                            expected.rpo_blocks[b].instructions.size(),
                            absl::StrJoin(expected.rpo_blocks[b].instructions, " ; "));
      }
    }
  }
  return cmp;
}

}  // namespace rom_nom_nom
