#include "lifter/symbol_folder.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

void SymbolFolder::Fold(absl::Span<const Instruction> instructions, const SymbolIndex* symbol_index,
                        const GameConfig* game_config) {
  folded_.clear();
  hi_to_folded_idx_.clear();
  mid_to_folded_idx_.clear();
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
            uint32_t target_address = hi_val + static_cast<int32_t>(inst.immediate);
            FoldedSymbolAccess access;
            access.type = FoldedPatternType::kAddressLoad;
            access.address = target_address;
            access.dest_reg = inst.rt.value_or(Register::kZero);
            access.hi_inst_index = hi_idx;
            access.lo_inst_index = i;
            if (symbol_index != nullptr) {
              const auto* registered_entry = symbol_index->FindByAddress(target_address);
              if (registered_entry != nullptr) {
                access.symbol_name = std::string(registered_entry->name());
              } else if (game_config != nullptr &&
                         ClassifyVramAddress(*game_config, target_address) ==
                             SectionVramClassification::kCode) {
                access.symbol_name =
                    symbol_index->LookupOrSynthesizeName(target_address, SYMBOL_FUNC);
              } else {
                access.symbol_name =
                    symbol_index->LookupOrSynthesizeName(target_address, SYMBOL_DATA);
              }
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
            uint32_t target_address = hi_val + static_cast<int32_t>(inst.immediate);
            FoldedSymbolAccess access;
            access.type = FoldedPatternType::kGlobalLoad;
            access.address = target_address;
            if (inst.opcode == Opcode::kLwc1 || inst.opcode == Opcode::kLdc1) {
              access.dest_fp_reg = inst.ft;
            } else {
              access.dest_reg = inst.rt.value_or(Register::kZero);
            }
            access.hi_inst_index = hi_idx;
            access.lo_inst_index = i;
            switch (inst.opcode) {
              case Opcode::kLb:
                access.access_type = "s8";
                break;
              case Opcode::kLbu:
                access.access_type = "u8";
                break;
              case Opcode::kLh:
                access.access_type = "s16";
                break;
              case Opcode::kLhu:
                access.access_type = "u16";
                break;
              case Opcode::kLwc1:
                access.access_type = "f32";
                break;
              case Opcode::kLdc1:
                access.access_type = "f64";
                break;
              case Opcode::kLd:
                access.access_type = "s64";
                break;
              case Opcode::kLw:
              default:
                access.access_type = "s32";
                break;
            }
            if (symbol_index != nullptr) {
              access.symbol_name =
                  symbol_index->LookupOrSynthesizeName(target_address, SYMBOL_DATA);
            }
            folded_.push_back(std::move(access));
          } else if (inst.IsStore()) {
            uint32_t target_address = hi_val + static_cast<int32_t>(inst.immediate);
            FoldedSymbolAccess access;
            access.type = FoldedPatternType::kGlobalStore;
            access.address = target_address;
            if (inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) {
              access.src_fp_reg = inst.ft;
            } else {
              access.src_reg = inst.rt.value_or(Register::kZero);
            }
            access.hi_inst_index = hi_idx;
            access.lo_inst_index = i;
            switch (inst.opcode) {
              case Opcode::kSb:
                access.access_type = "u8";
                break;
              case Opcode::kSh:
                access.access_type = "u16";
                break;
              case Opcode::kSwc1:
                access.access_type = "f32";
                break;
              case Opcode::kSdc1:
                access.access_type = "f64";
                break;
              case Opcode::kSd:
                access.access_type = "s64";
                break;
              case Opcode::kSw:
              default:
                access.access_type = "s32";
                break;
            }
            if (symbol_index != nullptr) {
              access.symbol_name =
                  symbol_index->LookupOrSynthesizeName(target_address, SYMBOL_DATA);
            }
            folded_.push_back(std::move(access));
          }
        }
      } else if (val.kind == ValueKind::kSymbolHiIndex) {
        uint32_t target_address = val.symbol_hi + static_cast<int32_t>(inst.immediate);
        auto index_def = tracker.GetReachingDefinition(val.index_reg);
        bool index_clobbered =
            index_def.has_value() && index_def->instruction_index >= val.mid_inst_index;
        if (!index_clobbered && (inst.IsLoad() || inst.IsStore())) {
          FoldedSymbolAccess access;
          access.address = target_address;
          access.index_reg = val.index_reg;
          access.hi_inst_index = val.hi_inst_index;
          access.mid_inst_index = val.mid_inst_index;
          access.lo_inst_index = i;

          if (inst.IsLoad()) {
            access.type = FoldedPatternType::kGlobalIndexedLoad;
            if (inst.opcode == Opcode::kLwc1 || inst.opcode == Opcode::kLdc1) {
              access.dest_fp_reg = inst.ft;
            } else {
              access.dest_reg = inst.rt.value_or(Register::kZero);
            }
            switch (inst.opcode) {
              case Opcode::kLb:
                access.access_type = "s8";
                break;
              case Opcode::kLbu:
                access.access_type = "u8";
                break;
              case Opcode::kLh:
                access.access_type = "s16";
                break;
              case Opcode::kLhu:
                access.access_type = "u16";
                break;
              case Opcode::kLwc1:
                access.access_type = "f32";
                break;
              case Opcode::kLdc1:
                access.access_type = "f64";
                break;
              case Opcode::kLd:
                access.access_type = "s64";
                break;
              case Opcode::kLw:
              default:
                access.access_type = "s32";
                break;
            }
          } else {
            access.type = FoldedPatternType::kGlobalIndexedStore;
            if (inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) {
              access.src_fp_reg = inst.ft;
            } else {
              access.src_reg = inst.rt.value_or(Register::kZero);
            }
            switch (inst.opcode) {
              case Opcode::kSb:
                access.access_type = "u8";
                break;
              case Opcode::kSh:
                access.access_type = "u16";
                break;
              case Opcode::kSwc1:
                access.access_type = "f32";
                break;
              case Opcode::kSdc1:
                access.access_type = "f64";
                break;
              case Opcode::kSd:
                access.access_type = "s64";
                break;
              case Opcode::kSw:
              default:
                access.access_type = "s32";
                break;
            }
          }

          if (symbol_index != nullptr) {
            access.symbol_name = symbol_index->LookupOrSynthesizeName(target_address, SYMBOL_DATA);
          }
          folded_.push_back(std::move(access));
        }
      }
    }

    tracker.Step(inst);
  }

  absl::flat_hash_set<size_t> folded_uses;
  for (const auto& access : folded_) {
    folded_uses.insert(access.lo_inst_index);
    if (access.mid_inst_index.has_value()) {
      folded_uses.insert(*access.mid_inst_index);
    }
  }

  for (size_t f = 0; f < folded_.size(); ++f) {
    bool all_hi_uses_folded = true;
    for (size_t use_idx : tracker.GetUses(folded_[f].hi_inst_index)) {
      if (!folded_uses.contains(use_idx)) {
        all_hi_uses_folded = false;
        break;
      }
    }
    bool all_mid_uses_folded = true;
    if (folded_[f].mid_inst_index.has_value()) {
      for (size_t use_idx : tracker.GetUses(*folded_[f].mid_inst_index)) {
        if (!folded_uses.contains(use_idx)) {
          all_mid_uses_folded = false;
          all_hi_uses_folded = false;
          break;
        }
      }
    }
    if (all_hi_uses_folded && (folded_[f].type == FoldedPatternType::kGlobalStore ||
                               folded_[f].type == FoldedPatternType::kGlobalIndexedStore)) {
      Register hi_reg = instructions[folded_[f].hi_inst_index].rt.value_or(Register::kZero);
      if (hi_reg != Register::kAt && hi_reg != Register::kZero) {
        for (size_t k = folded_[f].lo_inst_index + 1; k < instructions.size(); ++k) {
          if (k > 0 && instructions[k - 1].IsBranchLikely()) {
            RegisterUseDef ud = GetInstructionUseDef(instructions[k]);
            for (Register r : ud.gpr_defs) {
              if (r == hi_reg) {
                all_hi_uses_folded = false;
                break;
              }
            }
          }
          if (!all_hi_uses_folded) break;
        }
      }
    }
    if (all_hi_uses_folded) {
      hi_to_folded_idx_[folded_[f].hi_inst_index] = f;
    }
    if (folded_[f].mid_inst_index.has_value() && all_mid_uses_folded) {
      mid_to_folded_idx_[*folded_[f].mid_inst_index] = f;
    }
    lo_to_folded_idx_[folded_[f].lo_inst_index] = f;
  }
}

bool SymbolFolder::IsFoldedHi(size_t instruction_index) const {
  return hi_to_folded_idx_.contains(instruction_index);
}

bool SymbolFolder::IsFoldedMid(size_t instruction_index) const {
  return mid_to_folded_idx_.contains(instruction_index);
}

const FoldedSymbolAccess* SymbolFolder::GetFoldedLo(size_t instruction_index) const {
  auto it = lo_to_folded_idx_.find(instruction_index);
  if (it != lo_to_folded_idx_.end()) {
    return &folded_[it->second];
  }
  return nullptr;
}

}  // namespace rom_nom_nom
