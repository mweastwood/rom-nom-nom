#include "lifter/gcc_rtl_dump_inspector.h"

#include <unistd.h>

#include <atomic>
#include <cstdint>
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
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/compiler.h"
#include "core/control_flow_graph.h"
#include "core/process.h"
#include "core/rtl_sexpr_utils.h"
#include "core/toolchain.h"
#include "lifter/c_emitter.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

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

}  // namespace

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

  auto build_stream = [](const RtlFunctionDump& fn) {
    const absl::flat_hash_map<int, std::string> label_map = BuildSequentialLabelMap(fn);
    std::vector<std::string> stream;
    for (const auto& insn : fn.insns) {
      if (insn.kind == RtlInsnKind::kInsn || insn.kind == RtlInsnKind::kJumpInsn ||
          insn.kind == RtlInsnKind::kCallInsn) {
        stream.push_back(NormalizeInsnSexprForComparison(insn.raw_sexpr, label_map));
      } else if (insn.kind == RtlInsnKind::kCodeLabel) {
        auto it = label_map.find(insn.uid);
        stream.push_back(
            absl::StrCat("(code_label ", it != label_map.end() ? it->second : "L?", ")"));
      } else if (insn.kind == RtlInsnKind::kBarrier) {
        stream.push_back("(barrier)");
      } else if (insn.kind == RtlInsnKind::kNote &&
                 absl::StartsWith(insn.note_kind, "NOTE_INSN_LOOP_")) {
        stream.push_back(absl::StrCat("(note ", insn.note_kind, ")"));
      }
    }
    return stream;
  };

  return build_stream(*lhs_fn) == build_stream(*rhs_fn);
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
  return AllRtlPasses();
}

std::string_view GccRtlDumpInspector::PassExtension(RtlPassKind pass) {
  return RtlPassExtension(pass);
}

RtlPassDump GccRtlDumpInspector::ParsePassDump(RtlPassKind pass, std::string_view raw_text) {
  return ParseRtlPassDump(pass, raw_text);
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

RtlCanonicalCfg GccRtlDumpInspector::BuildCanonicalCfgFromRtlDump(const RtlFunctionDump& dump) {
  return ::rom_nom_nom::BuildCanonicalCfgFromRtlDump(dump);
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
