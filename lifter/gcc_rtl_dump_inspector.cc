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
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/compiler.h"
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
  if (size_t slash_pos = tag_with_flags.find('/'); slash_pos != std::string_view::npos) {
    base_tag = tag_with_flags.substr(0, slash_pos);
    std::string_view flags = tag_with_flags.substr(slash_pos + 1);
    if (absl::StrContains(flags, 's')) {
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
        return after < rest.size() && (rest[after] == ' ' || rest[after] == '/');
      };
      if (starts_with_insn_tag("insn") || starts_with_insn_tag("jump_insn") ||
          starts_with_insn_tag("call_insn")) {
        out.push_back('(');
        ++i;
        while (i < sexpr.size() && !absl::ascii_isspace(sexpr[i])) {
          out.push_back(sexpr[i++]);
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

}  // namespace rom_nom_nom
