#ifndef ROM_NOM_NOM_LIFTER_COP1_LIFTER_H_
#define ROM_NOM_NOM_LIFTER_COP1_LIFTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/expression_builder.h"

namespace rom_nom_nom {

struct Cop1LiftState {
  const FunctionLiftContext& ctx;
  absl::Span<const Instruction> instructions_to_process;
  size_t loop_end = 0;
  bool has_frame_pointer = false;
  bool sp_address_taken = false;
  uint32_t frame_size = 0;
  int32_t sp_base_offset = 0;
  int32_t sp_buffer_size = 0;
  std::optional<size_t> sp_alloc_idx;
  RegisterTracker& tracker;
  absl::flat_hash_set<size_t>& skipped_instruction_indices;
  const absl::flat_hash_set<Register>& gprs_defined_in_block;
  std::optional<uint32_t>* fpr_const_bits = nullptr;
  std::unique_ptr<LiftedExpression>* pending_int_to_fp = nullptr;
  std::unique_ptr<LiftedExpression>* pending_fp_to_int = nullptr;
  std::vector<LiftedStatement>& statements;
};

void LiftCop1MemoryAndMoveInstruction(const Instruction& inst, size_t i, Cop1LiftState& state);
void LiftCop1ArithmeticAndConvertInstruction(const Instruction& inst, size_t i,
                                             Cop1LiftState& state);
void LiftCop1ComparisonInstruction(const Instruction& inst,
                                   std::vector<LiftedStatement>& statements);

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_COP1_LIFTER_H_
