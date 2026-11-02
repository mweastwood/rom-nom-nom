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
#include "core/mips.h"
#include "core/process.h"
#include "core/toolchain.h"
#include "lifter/c_emitter.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/rtl_cfg.h"

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
    // Ignore non-machine `(use ...)`, `(clobber ...)`, `(const_int 0)` (`gen_nop()`) pseudo-insns,
    // noop register self-moves `(set (reg:M R) (reg:M R))` skipped by `final.c`, and jump-table
    // `addr_vec` data.
    if (entry.kind == RtlInsnKind::kInsn) {
      if (absl::StartsWith(pat, "(use ") || absl::StartsWith(pat, "(clobber ") ||
          pat == "(const_int 0)") {
        continue;
      }
      const std::string canon_pat = CanonicalizeRtlPattern(pat);
      if (absl::StartsWith(canon_pat, "(set (reg:") && absl::EndsWith(canon_pat, ")")) {
        const std::string_view sv = canon_pat;
        const size_t close_lhs = sv.find(')', 5);
        if (close_lhs != std::string_view::npos && close_lhs + 2 < sv.size() - 1 &&
            sv[close_lhs + 1] == ' ') {
          const std::string_view lhs = sv.substr(5, close_lhs - 5 + 1);
          const std::string_view rhs = sv.substr(close_lhs + 2, sv.size() - 1 - (close_lhs + 2));
          if (lhs == rhs) {
            continue;
          }
        }
      }
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
    RtlBasicBlock out_blk;
    for (const auto& pat : raw_blocks[raw_idx].instructions) {
      out_blk.AddPatternOnly(pat);
    }
    for (size_t s : raw_succs[raw_idx]) {
      if (auto it = raw_to_rpo.find(s); it != raw_to_rpo.end()) {
        out_blk.successors.push_back(it->second);
      }
    }
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

RtlControlFlowGraph GccRtlDumpInspector::LiftControlFlowGraphToCanonicalRtl(
    const ControlFlowGraph& input_cfg) {
  return InverseFinalAndGasLifter::LiftToRtlCfg(input_cfg);
}

RtlCfgPassComparison GccRtlDumpInspector::CompareRtlCfgToDump(const RtlControlFlowGraph& rtl_cfg,
                                                              const RtlFunctionDump* dump) {
  if (dump == nullptr) {
    RtlCfgPassComparison cmp;
    cmp.pass_present = false;
    return cmp;
  }
  RtlControlFlowGraph expected = BuildCanonicalCfgFromRtlDump(*dump);
  return rtl_cfg.CompareAgainst(expected);
}

RtlCfgPassComparison GccRtlDumpInspector::CompareCfgToRtlDump(const ControlFlowGraph& cfg,
                                                              const RtlFunctionDump* dump) {
  if (dump == nullptr) {
    RtlCfgPassComparison cmp;
    cmp.pass_present = false;
    return cmp;
  }
  RtlControlFlowGraph actual = LiftControlFlowGraphToCanonicalRtl(cfg);
  return CompareRtlCfgToDump(actual, dump);
}

}  // namespace rom_nom_nom
