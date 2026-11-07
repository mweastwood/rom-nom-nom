#ifndef ROM_NOM_NOM_LIFTER_GCC_RTL_DUMP_INSPECTOR_H_
#define ROM_NOM_NOM_LIFTER_GCC_RTL_DUMP_INSPECTOR_H_

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/compiler.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/process.h"
#include "core/rtl_sexpr_utils.h"
#include "core/toolchain.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Options for compiling and inspecting a C snippet with KMC GCC 2.7.2 `-da`.
struct GccRtlInspectOptions {
  std::vector<std::string> opt_flags = {"-O2", "-mips2", "-mcpu=r4000"};
  std::string extra_types_header;
  bool include_types_header = true;
};

// Holds all parsed per-pass RTL dumps and emitted assembly produced by a single GCC 2.7.2 `-da`
// compilation run.
class GccCompilationDump {
 public:
  GccCompilationDump() = default;
  GccCompilationDump(std::string assembly_text,
                     absl::flat_hash_map<RtlPassKind, RtlPassDump> pass_dumps);

  const std::string& AssemblyText() const { return assembly_text_; }

  bool HasPass(RtlPassKind pass) const;
  const RtlPassDump* GetPass(RtlPassKind pass) const;
  const RtlFunctionDump* GetFunctionPass(std::string_view func_name, RtlPassKind pass) const;

  // Returns true if the normalized active instruction stream for `func_name` in `pass`
  // matches between `this` and `other`.
  bool PassMatches(const GccCompilationDump& other, std::string_view func_name,
                   RtlPassKind pass) const;

  // Compares `func_name` across all GCC 2.7.2 RTL passes in chronological compilation order
  // (`.rtl` -> `.jump` -> ... -> `.dbr`) and returns the first `RtlPassKind` where the
  // normalized active instruction stream of `this` and `other` diverges, or `std::nullopt`
  // if all available passes match structurally.
  std::optional<RtlPassKind> FindFirstDivergentPass(const GccCompilationDump& other,
                                                    std::string_view func_name) const;

 private:
  std::string assembly_text_;
  absl::flat_hash_map<RtlPassKind, RtlPassDump> pass_dumps_;
};

// Compiles C snippets with KMC GCC 2.7.2 `-da` and parses the resulting per-pass RTL dump files
// (`.rtl`, `.jump`, `.cse`, `.loop`, `.cse2`, `.flow`, `.combine`, `.sched`, `.lreg`, `.greg`,
// `.sched2`, `.jump2`, `.dbr`) into structured snapshots for compiler-inversion diagnostics.
class GccRtlDumpInspector {
 public:
  explicit GccRtlDumpInspector(Toolchain toolchain, Compiler::ProcessRunner runner = &RunProcess);

  // Discovers the KMC GCC 2.7.2 original toolchain and constructs a `GccRtlDumpInspector`.
  static absl::StatusOr<GccRtlDumpInspector> Create(const std::filesystem::path& repo_root = {});

  // Returns the ordered list of all RTL passes in GCC 2.7.2's compilation pipeline.
  static absl::Span<const RtlPassKind> AllPasses();

  // Returns the file extension (including leading dot, e.g. `".rtl"`, `".dbr"`) for `pass`.
  static std::string_view PassExtension(RtlPassKind pass);

  // Parses the raw text of a GCC 2.7.2 RTL pass dump file into an `RtlPassDump`.
  static RtlPassDump ParsePassDump(RtlPassKind pass, std::string_view raw_text);

  // Compiles `c_code` with KMC GCC 2.7.2 `-da` in an isolated temporary directory and parses
  // all generated RTL pass dumps.
  absl::StatusOr<GccCompilationDump> InspectC(std::string_view c_code,
                                              const GccRtlInspectOptions& options = {}) const;

  // Builds the canonical RPO basic-block graph from a GCC 2.7.2 `RtlFunctionDump` (works uniformly
  // across all 13 passes `.rtl` .. `.dbr`).
  static RtlControlFlowGraph BuildCanonicalCfgFromRtlDump(const RtlFunctionDump& dump);

  // Lifts a `ControlFlowGraph` into the same canonical RPO RTL basic-block representation
  // via `InverseFinalAndGasStage::LiftToRtlCfg`.
  static RtlControlFlowGraph LiftControlFlowGraphToCanonicalRtl(const ControlFlowGraph& cfg);

  // Uniformly compares an `RtlControlFlowGraph` against any GCC 2.7.2 `RtlFunctionDump` pass.
  static RtlCfgPassComparison CompareRtlCfgToDump(const RtlControlFlowGraph& rtl_cfg,
                                                  const RtlFunctionDump* dump);

  // Uniformly compares a `ControlFlowGraph` against any GCC 2.7.2 `RtlFunctionDump` pass.
  static RtlCfgPassComparison CompareCfgToRtlDump(const ControlFlowGraph& cfg,
                                                  const RtlFunctionDump* dump);

 private:
  Toolchain toolchain_;
  Compiler::ProcessRunner runner_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_GCC_RTL_DUMP_INSPECTOR_H_
