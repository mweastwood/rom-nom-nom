#include "lifter/symbol_folder.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "core/mips.h"
#include "lifter/register_tracker.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

void SymbolFolder::Fold(absl::Span<const Instruction> instructions,
                        const SymbolIndex* symbol_index) {
  folded_.clear();
  hi_to_folded_idx_.clear();
  lo_to_folded_idx_.clear();

  RegisterTracker tracker;

  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& inst = instructions[i];

    if (inst.rs.has_value()) {
      Register base_reg = *inst.rs;
      TrackedValue val = tracker.GetRegisterValue(base_reg);

      if (val.kind == ValueKind::kSymbolHi) {
        auto def = tracker.GetReachingDefinition(base_reg);
        if (def.has_value()) {
          size_t hi_idx = def->instruction_index;
          uint32_t hi_val = val.symbol_hi;

          if (inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) {
            uint32_t addr = hi_val + static_cast<int32_t>(inst.immediate);
            FoldedSymbolAccess access;
            access.type = FoldedPatternType::kAddressLoad;
            access.address = addr;
            access.dest_reg = inst.rt.value_or(Register::kZero);
            access.hi_inst_index = hi_idx;
            access.lo_inst_index = i;
            if (symbol_index != nullptr) {
              access.symbol_name = symbol_index->LookupOrSynthesizeName(addr);
            }
            folded_.push_back(std::move(access));
          } else if (inst.opcode == Opcode::kOri) {
            uint32_t const_val = hi_val | static_cast<uint16_t>(inst.immediate);
            FoldedSymbolAccess access;
            access.type = FoldedPatternType::kConstantLiteral;
            access.address = const_val;
            access.dest_reg = inst.rt.value_or(Register::kZero);
            access.hi_inst_index = hi_idx;
            access.lo_inst_index = i;
            folded_.push_back(std::move(access));
          } else if (inst.IsLoad()) {
            uint32_t addr = hi_val + static_cast<int32_t>(inst.immediate);
            FoldedSymbolAccess access;
            access.type = FoldedPatternType::kGlobalLoad;
            access.address = addr;
            access.dest_reg = inst.rt.value_or(Register::kZero);
            access.hi_inst_index = hi_idx;
            access.lo_inst_index = i;
            if (symbol_index != nullptr) {
              access.symbol_name = symbol_index->LookupOrSynthesizeName(addr);
            }
            folded_.push_back(std::move(access));
          } else if (inst.IsStore()) {
            uint32_t addr = hi_val + static_cast<int32_t>(inst.immediate);
            FoldedSymbolAccess access;
            access.type = FoldedPatternType::kGlobalStore;
            access.address = addr;
            access.src_reg = inst.rt.value_or(Register::kZero);
            access.hi_inst_index = hi_idx;
            access.lo_inst_index = i;
            if (symbol_index != nullptr) {
              access.symbol_name = symbol_index->LookupOrSynthesizeName(addr);
            }
            folded_.push_back(std::move(access));
          }
        }
      }
    }

    tracker.Analyze(instructions.subspan(i, 1));
  }

  for (size_t f = 0; f < folded_.size(); ++f) {
    hi_to_folded_idx_[folded_[f].hi_inst_index] = f;
    lo_to_folded_idx_[folded_[f].lo_inst_index] = f;
  }
}

bool SymbolFolder::IsFoldedHi(size_t instruction_index) const {
  return hi_to_folded_idx_.contains(instruction_index);
}

const FoldedSymbolAccess* SymbolFolder::GetFoldedLo(size_t instruction_index) const {
  auto it = lo_to_folded_idx_.find(instruction_index);
  if (it != lo_to_folded_idx_.end()) {
    return &folded_[it->second];
  }
  return nullptr;
}

}  // namespace rom_nom_nom
