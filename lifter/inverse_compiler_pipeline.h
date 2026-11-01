#ifndef LIFTER_INVERSE_COMPILER_PIPELINE_H_
#define LIFTER_INVERSE_COMPILER_PIPELINE_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "lifter/decompilation_context.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Identifies each ordered stage in the inverse KMC GCC 2.7.2 decompilation pipeline.
enum class InverseStageId {
  kStage00FinalAndGas = 0,
  kStage01DbrDelaySlot = 1,
  kStage02Jump2CrossJump = 2,
  kStage03Sched2PostReload = 3,
  kStage04GregReload = 4,
  kStage05LregLocalAlloc = 5,
  kStage06Sched1PreAlloc = 6,
  kStage07Combine = 7,
  kStage08FlowLifeAnalysis = 8,
  kStage09Cse2PostLoop = 9,
  kStage10LoopOpt = 10,
  kStage11Cse1BasicBlock = 11,
  kStage12Jump1CfgCleanup = 12,
  kStage13RtlToCAst = 13,
};

// Metadata describing a single stage in the inverse GCC 2.7.2 pipeline.
struct InverseStageDescriptor {
  InverseStageId id;
  int inverse_stage_index;                     // 0..13 (decompiler execution order)
  int gcc_stage_number;                        // 13..0 (GCC forward compilation order)
  std::optional<RtlPassKind> target_gcc_pass;  // Target GCC RTL dump after this inverse stage
  std::optional<RtlPassKind> source_gcc_pass;  // Source GCC RTL dump before this inverse stage
  std::string_view stage_name;
  std::string_view gcc_stage_description;
};

// Polymorphic base class for an ordered stage in `InverseCompilerPipeline`.
class InverseCompilerStage {
 public:
  virtual ~InverseCompilerStage() = default;

  virtual InverseStageId Id() const = 0;
  const InverseStageDescriptor& Descriptor() const;
  std::string_view Name() const { return Descriptor().stage_name; }

  virtual absl::Status Run(DecompilationContext* context) = 0;
};

// Stage 0: Inverts GCC Stage 13 (`final.c` + `mips.c:function_epilogue` + `gas`, `.dbr` -> `.o`).
class InverseFinalAndGasStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage00FinalAndGas; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 1: Inverts GCC Stage 12 (`reorg.c:dbr_schedule`, `.jump2` -> `.dbr`).
class InverseDbrDelaySlotStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage01DbrDelaySlot; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 2: Inverts GCC Stage 11 (`jump.c:jump_optimize` post-reload cross-jumping, `.sched2` ->
// `.jump2`).
class InverseJump2CrossJumpStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage02Jump2CrossJump; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 3: Inverts GCC Stage 10 (`sched.c:schedule_insns` post-reload scheduling, `.greg` ->
// `.sched2`).
class InverseSched2PostReloadStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage03Sched2PostReload; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 4: Inverts GCC Stage 9 (`global.c` + `reload1.c`, `.lreg` -> `.greg`).
class InverseGregReloadStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage04GregReload; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 5: Inverts GCC Stage 8 (`local-alloc.c:local_alloc`, `.sched` -> `.lreg`).
class InverseLregLocalAllocStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage05LregLocalAlloc; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 6: Inverts GCC Stage 7 (`sched.c:schedule_insns` pre-alloc scheduling, `.combine` ->
// `.sched`).
class InverseSched1PreAllocStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage06Sched1PreAlloc; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 7: Inverts GCC Stage 6 (`combine.c:combine_instructions`, `.flow` -> `.combine`).
class InverseCombineStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage07Combine; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 8: Inverts GCC Stage 5 (`flow.c:life_analysis`, `.cse2` -> `.flow`).
class InverseFlowLifeAnalysisStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage08FlowLifeAnalysis; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 9: Inverts GCC Stage 4 (`cse.c:cse_main` post-loop CSE, `.loop` -> `.cse2`).
class InverseCse2PostLoopStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage09Cse2PostLoop; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 10: Inverts GCC Stage 3 (`loop.c:loop_optimize`, `.cse` -> `.loop`).
class InverseLoopOptStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage10LoopOpt; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 11: Inverts GCC Stage 2 (`cse.c:cse_main` pass 1 + `expmed.c`, `.jump` -> `.cse`).
class InverseCse1BasicBlockStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage11Cse1BasicBlock; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 12: Inverts GCC Stage 1 (`jump.c:jump_optimize` pass 1, `.rtl` -> `.jump`).
class InverseJump1CfgCleanupStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage12Jump1CfgCleanup; }
  absl::Status Run(DecompilationContext* context) override;
};

// Stage 13: Inverts GCC Stage 0 (`stmt.c` + `expr.c`, C AST -> `.rtl`) via `ControlFlowStructurer`
// and `AstConverter`.
class InverseRtlToCAstStage : public InverseCompilerStage {
 public:
  InverseStageId Id() const override { return InverseStageId::kStage13RtlToCAst; }
  absl::Status Run(DecompilationContext* context) override;
};

// Orchestrates the 14 ordered stages (`Stage 0` through `Stage 13`) of the inverse KMC GCC 2.7.2
// decompilation pipeline.
class InverseCompilerPipeline {
 public:
  using StageSnapshotCallback =
      std::function<void(const InverseStageDescriptor& stage, const DecompilationContext& context)>;

  InverseCompilerPipeline() = default;

  void AddStage(std::unique_ptr<InverseCompilerStage> stage);
  absl::Span<const std::unique_ptr<InverseCompilerStage>> Stages() const { return stages_; }

  // Returns descriptors for all 14 stages (`kStage00FinalAndGas` .. `kStage13RtlToCAst`).
  static absl::Span<const InverseStageDescriptor> AllStageDescriptors();

  // Returns descriptors for the 13 RTL-targeting stages (`kStage00FinalAndGas` ..
  // `kStage12Jump1CfgCleanup`, targeting `.dbr` .. `.rtl`).
  static absl::Span<const InverseStageDescriptor> RtlStageDescriptors();

  // Returns the descriptor for a specific `InverseStageId`.
  static const InverseStageDescriptor& GetStageDescriptor(InverseStageId id);

  // Constructs the complete 14-stage production inverse compiler pipeline.
  static InverseCompilerPipeline BuildDefaultPipeline();

  // Executes all stages in order on `context`, invoking `snapshot_cb` (if non-null) after each
  // stage completes.
  absl::Status Run(DecompilationContext* context,
                   StageSnapshotCallback snapshot_cb = nullptr) const;

 private:
  std::vector<std::unique_ptr<InverseCompilerStage>> stages_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_COMPILER_PIPELINE_H_
