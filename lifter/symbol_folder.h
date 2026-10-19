#ifndef LIFTER_SYMBOL_FOLDER_H_
#define LIFTER_SYMBOL_FOLDER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

// Type of folded multi-instruction pattern.
enum class FoldedPatternType {
  kNone,
  kAddressLoad,         // lui + addiu: &symbol or pointer
  kConstantLiteral,     // lui + ori: 32-bit integer constant
  kGlobalLoad,          // lui + lw/lh/lb/lbu/lhu/lwc1: symbol value
  kGlobalStore,         // lui + sw/sh/sb/swc1: symbol = value
  kGlobalIndexedLoad,   // lui + addu + lw/lh/lb/lbu/lhu/lwc1: *(type*)((u8*)&symbol + index)
  kGlobalIndexedStore,  // lui + addu + sw/sh/sb/swc1: *(type*)((u8*)&symbol + index) = value
};

// Represents a matched and folded symbol / constant access.
struct FoldedSymbolAccess {
  FoldedPatternType type = FoldedPatternType::kNone;
  uint32_t address = 0;
  std::string symbol_name;
  std::string access_type = "s32";

  Register dest_reg = Register::kZero;
  Register src_reg = Register::kZero;  // For stores
  std::optional<FpRegister> dest_fp_reg;
  std::optional<FpRegister> source_float_register;

  Register index_reg = Register::kZero;

  size_t hi_inst_index = 0;
  std::optional<size_t> mid_inst_index;
  size_t lo_inst_index = 0;
  bool hi_is_local = true;
  bool mid_is_local = true;
};

// Analyzes instructions to fold multi-instruction lui/lo relocation pairs
// into semantic symbol accesses and 32-bit constants.
class SymbolFolder {
 public:
  SymbolFolder() = default;

  // Folds symbol accesses across a sequence of instructions.
  // If symbol_index is provided, addresses are mapped to semantic symbol names.
  // If game_config is provided, unregistered address loads are classified as code (func_*) or
  // data (D_*).
  // If initial_tracker is provided, tracked register values (such as %hi from dominating
  // blocks) are used to fold cross-block symbol references.
  void Fold(absl::Span<const Instruction> instructions, const SymbolIndex* symbol_index = nullptr,
            const GameConfig* game_config = nullptr,
            const RegisterTracker* initial_tracker = nullptr);

  // Returns all folded patterns found.
  const std::vector<FoldedSymbolAccess>& AllFolded() const { return folded_; }

  // Returns true if the instruction at index is the 'lui' half of a folded pair.
  bool IsFoldedHi(size_t instruction_index) const;

  // Returns true if the instruction at index is the 'addu' mid half of a folded indexed access.
  bool IsFoldedMid(size_t instruction_index) const;

  // Returns the folded access if the instruction at index is the 'lo' half of a folded pair.
  const FoldedSymbolAccess* GetFoldedLo(size_t instruction_index) const;

 private:
  std::vector<FoldedSymbolAccess> folded_;
  absl::flat_hash_map<size_t, size_t> hi_to_folded_idx_;
  absl::flat_hash_map<size_t, size_t> mid_to_folded_idx_;
  absl::flat_hash_map<size_t, size_t> lo_to_folded_idx_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_SYMBOL_FOLDER_H_
