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
#include "core/process.h"
#include "core/toolchain.h"

namespace rom_nom_nom {

// Identifies each RTL dump pass emitted by KMC GCC 2.7.2 (`toplev.c:rest_of_compilation`)
// when invoked with `-da`, ordered chronologically along the forward compilation pipeline.
enum class RtlPassKind {
  kRtl,      // .rtl     - Initial RTL generation (stmt.c / expr.c)
  kJump,     // .jump    - First jump optimization pass (jump.c)
  kCse,      // .cse     - First common subexpression elimination pass (cse.c)
  kLoop,     // .loop    - Loop optimization pass (loop.c)
  kCse2,     // .cse2    - Second CSE pass (cse.c)
  kFlow,     // .flow    - Dataflow and life analysis pass (flow.c)
  kCombine,  // .combine - Instruction combination pass (combine.c)
  kSched,    // .sched   - Pre-allocation instruction scheduling pass (sched.c)
  kLreg,     // .lreg    - Local register allocation pass (local-alloc.c)
  kGreg,     // .greg    - Global register allocation & reload pass (global.c / reload1.c)
  kSched2,   // .sched2  - Post-reload instruction scheduling pass (sched.c)
  kJump2,    // .jump2   - Post-reload jump optimization pass (jump.c)
  kDbr,      // .dbr     - Delayed-branch scheduling pass (reorg.c)
};

// Classifies a top-level RTL chain entry in a GCC 2.7.2 RTL dump.
enum class RtlInsnKind {
  kInsn,
  kJumpInsn,
  kCallInsn,
  kCodeLabel,
  kBarrier,
  kNote,
  kOther,
};

// Represents a single parsed top-level RTL S-expression entry within a function dump.
struct RtlInsnEntry {
  RtlInsnKind kind = RtlInsnKind::kOther;
  int uid = -1;
  int prev_uid = -1;
  int next_uid = -1;
  bool is_sequence = false;
  bool in_annulled_branch_delay = false;
  std::string note_kind;
  std::string raw_sexpr;
};

// Represents the parsed RTL dump for a single function within a pass dump file.
struct RtlFunctionDump {
  std::string function_name;
  std::string header_text;
  std::vector<RtlInsnEntry> insns;

  // Returns the number of active executable RTL instructions (`kInsn`, `kJumpInsn`, `kCallInsn`),
  // excluding `kNote`, `kCodeLabel`, and `kBarrier` entries.
  size_t ActiveInsnCount() const;

  // Returns the number of `(sequence [...])` delay-slot bundles (produced by `reorg.c` in `.dbr`).
  size_t SequenceCount() const;

  // Returns all `kNote` entries whose `note_kind` matches `target_note_kind`
  // (e.g., `"NOTE_INSN_LOOP_BEG"`, `"NOTE_INSN_LOOP_CONT"`, `"NOTE_INSN_PROLOGUE_END"`).
  std::vector<const RtlInsnEntry*> FindNotes(std::string_view target_note_kind) const;

  // Returns true if the function contains at least one `NOTE_INSN_LOOP_BEG` note.
  bool HasLoopNotes() const;
};

// Represents the parsed contents of a single GCC 2.7.2 RTL pass dump file (e.g. `<file>.c.dbr`).
class RtlPassDump {
 public:
  RtlPassDump() = default;
  RtlPassDump(RtlPassKind pass, std::string raw_text, std::string preamble_text,
              std::vector<RtlFunctionDump> functions);

  RtlPassKind Pass() const { return pass_; }
  const std::string& RawText() const { return raw_text_; }
  const std::string& PreambleText() const { return preamble_text_; }
  const std::vector<RtlFunctionDump>& Functions() const { return functions_; }

  const RtlFunctionDump* FindFunction(std::string_view func_name) const;

 private:
  RtlPassKind pass_ = RtlPassKind::kRtl;
  std::string raw_text_;
  std::string preamble_text_;
  std::vector<RtlFunctionDump> functions_;
};

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

 private:
  Toolchain toolchain_;
  Compiler::ProcessRunner runner_;
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_GCC_RTL_DUMP_INSPECTOR_H_
