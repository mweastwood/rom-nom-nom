#ifndef LIFTER_RTL_DEF_USE_ANALYSIS_H_
#define LIFTER_RTL_DEF_USE_ANALYSIS_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/symbol_registry.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Register def/use and instruction classification summary for a single `RtlInsn`.
struct RtlInsnDefUse {
  bool is_call = false;
  bool is_jump = false;
  std::vector<std::string> defs;
  std::vector<std::string> uses;
  std::string callee;
  bool is_reg_to_reg_move = false;
  bool is_si_reg_to_reg_move = false;
  bool is_mfc1_to_arg_gpr = false;
  bool is_mtc1_from_gpr = false;
  bool is_sll = false;
  bool is_addu_or_subu_reg = false;
  bool is_addiu_minus_1 = false;
  bool is_and_reg = false;
  bool is_andi_a0_1 = false;
  bool is_df_di_a2 = false;
  bool is_sp16_store = false;
  bool is_sp16_fpr_store = false;
  bool is_sp_buf_16_addr = false;
  bool is_sp_buf_32_addr = false;
  bool is_float_in_gpr_def = false;
  bool is_store = false;
  bool is_sw_v0 = false;
  bool is_symbol_store = false;
  bool is_global_lock_zero_store = false;
  bool is_load = false;
  bool is_lw_a0_from_0_s0 = false;
  bool is_v0_plus_non_one = false;
  bool is_beq_a1_zero = false;
  bool compares_a1_to_reg = false;
};

// Returns true if `vec` contains `item`.
bool HasRegItem(const std::vector<std::string>& vec, std::string_view item);

// Analyzes register definitions, uses, and structural properties of `insn`.
RtlInsnDefUse AnalyzeRtlInsnDefUse(const RtlInsn& insn, const SymbolIndex* symbols = nullptr);

// Returns true if `reg_name` is upward-exposed (read before being killed by a definition or call)
// in any transitive successor basic block reachable from `start_blk`.
bool IsRegLiveInSuccessors(const RtlControlFlowGraph& cfg,
                           const std::vector<std::vector<RtlInsnDefUse>>& parsed_map,
                           size_t start_blk, std::string_view reg_name);

}  // namespace rom_nom_nom

#endif  // LIFTER_RTL_DEF_USE_ANALYSIS_H_
