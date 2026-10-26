#ifndef CORE_ROUND_TRIP_RUNNER_H_
#define CORE_ROUND_TRIP_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "core/compiler.h"
#include "core/elf_reader.h"
#include "core/register_tracker.h"
#include "core/round_trip_test_cases.h"
#include "core/symbol_registry.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

// Encapsulates the compiled ELF object, extracted function, synthesized symbol table, and
// relocated instruction words for a single `RoundTripTestCase`.
struct PreparedRoundTripCase {
  RoundTripTestCase test_case;
  std::filesystem::path work_dir;
  const Toolchain* toolchain = nullptr;
  Compiler* compiler = nullptr;
  std::string full_orig_c;
  std::filesystem::path orig_c_path;
  std::filesystem::path orig_o_path;
  ElfReader orig_elf;
  ElfFunction orig_func;
  std::unique_ptr<SymbolIndex> symbol_index;
  absl::flat_hash_map<std::string, uint32_t> sym_to_vram;
  std::vector<uint32_t> patched_orig_words;
  std::function<std::optional<uint32_t>(uint32_t)> rodata_memory_reader;
};

// Outcome of executing a `RoundTripStage` on a single `PreparedRoundTripCase`.
struct RoundTripCaseOutcome {
  std::string test_name;
  std::string function_name;
  bool passed = false;
  bool structurally_isomorphic = false;
  bool bit_exact = false;
  FunctionStructuralComparison structural_comparison;
  std::string candidate_c;
  std::string diagnostic;
};

// Polymorphic interface for a pipeline stage under round-trip evaluation (e.g., Splitter,
// Lifter, or Tuner).
class RoundTripStage {
 public:
  virtual ~RoundTripStage() = default;
  virtual std::string_view Name() const = 0;
  virtual absl::StatusOr<RoundTripCaseOutcome> Execute(const PreparedRoundTripCase& prepared) = 0;
};

// General runner that compiles `RoundTripTestCase` inputs with the N64 toolchain, executes a
// `RoundTripStage`, compares structural isomorphism and bit-exactness, and aggregates suite-level
// metrics.
class RoundTripRunner {
 public:
  static absl::StatusOr<std::unique_ptr<RoundTripRunner>> Create();

  explicit RoundTripRunner(Toolchain toolchain);

  const Toolchain& GetToolchain() const { return toolchain_; }
  Compiler& GetCompiler() { return compiler_; }
  const Compiler& GetCompiler() const { return compiler_; }

  // Compiles `test_case.original_c` into `work_dir`, extracts `test_case.function_name`, and
  // prepares relocations and `.rodata` memory readers.
  absl::StatusOr<PreparedRoundTripCase> PrepareCase(const RoundTripTestCase& test_case,
                                                    const std::filesystem::path& work_dir);

  // Runs `stage` on `test_case` in a temporary directory, records the outcome in the suite
  // accumulator, and cleans up temporary files.
  absl::StatusOr<RoundTripCaseOutcome> RunCase(const RoundTripTestCase& test_case,
                                               RoundTripStage* stage);

  // Records a case outcome for the suite-level summary report.
  void RecordOutcome(const RoundTripCaseOutcome& outcome);

  // Formats a human-readable summary report of structural isomorphism and bit-exactness across all
  // recorded test cases.
  std::string FormatSuiteSummaryReport(std::string_view suite_title) const;

  size_t TotalExecuted() const;
  size_t StructurallyIsomorphicCount() const;
  size_t BitExactCount() const;

  static std::filesystem::path FindWorkspaceFile(std::string_view relative_path);
  static std::string EmitBaseTypesHeader(std::string_view extra_declarations = "");

  // Applies the synthesized symbol addresses from `prepared` onto `cand_func` so its words can be
  // structurally compared at `base_vram`.
  static std::vector<uint32_t> ApplyCandidateRelocations(const PreparedRoundTripCase& prepared,
                                                         const ElfFunction& cand_func);

  // Evaluates structural isomorphism (`frame_size_matches`, `saved_registers_match`,
  // `cfg_topology_matches`) between `prepared.orig_func` and `cand_func`.
  static FunctionStructuralComparison EvaluateStructuralIsomorphism(
      const PreparedRoundTripCase& prepared, const ElfFunction& cand_func, std::string_view cand_c,
      std::string* diagnostic_out);

  // Compares `orig` and `cand` for instruction-level and relocation-level bit-exactness.
  static bool CompareFunctionsBitExact(const ElfFunction& orig, const ElfFunction& cand,
                                       uint32_t base_vram, std::string_view orig_c,
                                       std::string_view cand_c, std::string* diagnostic_out);

 private:
  Toolchain toolchain_;
  Compiler compiler_;

  mutable std::mutex mutex_;
  std::vector<RoundTripCaseOutcome> outcomes_;
};

}  // namespace rom_nom_nom

#endif  // CORE_ROUND_TRIP_RUNNER_H_
