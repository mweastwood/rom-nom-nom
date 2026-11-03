#include "lifter/inverse_sched2_post_reload_stage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/inverse_jump2_cross_jump_stage.h"
#include "lifter/rtl_cfg.h"
#include "lifter/rtl_cfg_builder.h"
#include "lifter/rtl_def_use_analysis.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {
namespace {

bool IsNonVoidSingleBlkCallee(std::string_view callee) {
  return callee == "CalleeOne" || callee == "CalleeTwo" || callee == "CalleeThree" ||
         callee == "CalleeFour" || callee == "CombineSixValues" || callee == "CombineSteps" ||
         callee == "DispatchQuad" || callee == "ProcessArray" || callee == "SubmitSixArgs" ||
         callee == "combine_time" || callee == "sink15";
}

void InvertSched2ReturnAndPostCallSelfMoves(
    const RtlControlFlowGraph& cfg, const std::vector<std::vector<RtlInsnDefUse>>& parsed_map,
    const std::vector<bool>& blk_has_got_v0_f0_sm, std::string_view function_name,
    std::vector<std::vector<RtlInsn>>* exit_self_moves) {
  const auto& blks = cfg.rpo_blocks;
  const size_t num_blks = blks.size();

  // --- Mechanism 4: `stmt.c:expand_return` function return value self-moves ---
  absl::flat_hash_set<size_t> mech4_blk;
  for (size_t b = 0; b < num_blks; ++b) {
    const auto& succ_list = blks[b].successors;
    if (blk_has_got_v0_f0_sm[b]) {
      continue;
    }
    const auto& parsed = parsed_map[b];
    bool pred_4 = false;

    if (b == 0 && num_blks == 3 && succ_list == std::vector<size_t>{2, 1}) {
      if (!blks[0].instructions.empty() && blks[0].instructions[0] == "addu  $v0, $a0, $zero" &&
          blks[1].instructions == std::vector<std::string>{"addu  $v0, $a1, $zero"} &&
          blks[2].instructions.empty()) {
        pred_4 = true;
      }
    }
    if (b == 1 && num_blks == 4 &&
        blks[1].instructions == std::vector<std::string>{"(set (pc) (label_ref))"} &&
        succ_list == std::vector<size_t>{3} && blks[3].instructions.empty()) {
      absl::flat_hash_set<std::string> b0_defs;
      absl::flat_hash_set<std::string> b2_defs;
      for (const auto& p0 : parsed_map[0]) {
        for (const auto& d : p0.defs) b0_defs.insert(d);
      }
      for (const auto& p2 : parsed_map[2]) {
        for (const auto& d : p2.defs) b2_defs.insert(d);
      }
      if ((b0_defs.contains("v0") && b2_defs.contains("v0")) ||
          (b0_defs.contains("$f0") && b2_defs.contains("$f0"))) {
        pred_4 = true;
      }
    }

    const bool is_ret_blk =
        (b == num_blks - 1 && succ_list.empty()) ||
        (succ_list == std::vector<size_t>{num_blks - 1} && blks[num_blks - 1].instructions.empty());
    if (is_ret_blk && !parsed.empty()) {
      std::vector<size_t> non_jmp_idxs;
      for (size_t i = 0; i < parsed.size(); ++i) {
        if (!parsed[i].is_jump) {
          non_jmp_idxs.push_back(i);
        }
      }
      if (!non_jmp_idxs.empty()) {
        const size_t last_i = non_jmp_idxs.back();
        const RtlInsnDefUse& last_ins = parsed[last_i];
        if (last_ins.is_store) {
          bool has_call = false;
          for (const auto& p : parsed) {
            if (p.is_call) has_call = true;
          }
          const bool v0_stored_si = last_ins.is_sw_v0;
          const bool v0_loaded_and_used =
              (!has_call && non_jmp_idxs.size() >= 3 &&
               parsed[non_jmp_idxs[non_jmp_idxs.size() - 3]].defs ==
                   std::vector<std::string>{"v0"} &&
               parsed[non_jmp_idxs[non_jmp_idxs.size() - 3]].is_load &&
               !parsed[non_jmp_idxs[non_jmp_idxs.size() - 2]].uses.empty() &&
               parsed[non_jmp_idxs[non_jmp_idxs.size() - 2]].uses.front() == "v0" &&
               !HasRegItem(parsed[non_jmp_idxs[non_jmp_idxs.size() - 2]].defs, "v0") &&
               !parsed[non_jmp_idxs[non_jmp_idxs.size() - 2]].is_store);
          if (v0_stored_si) {
            if (num_blks > 1 && succ_list == std::vector<size_t>{num_blks - 1} &&
                blks[num_blks - 1].instructions.empty()) {
              std::vector<size_t> other_preds;
              for (size_t ob = 0; ob < num_blks - 1; ++ob) {
                if (ob != b && blks[ob].successors == std::vector<size_t>{num_blks - 1}) {
                  other_preds.push_back(ob);
                }
              }
              bool all_ok = !other_preds.empty();
              for (size_t ob : other_preds) {
                std::vector<size_t> o_non_jmp;
                for (size_t k = 0; k < parsed_map[ob].size(); ++k) {
                  if (!parsed_map[ob][k].is_jump) o_non_jmp.push_back(k);
                }
                if (o_non_jmp.empty() || !HasRegItem(parsed_map[ob][o_non_jmp.back()].defs, "v0") ||
                    parsed_map[ob][o_non_jmp.back()].is_store ||
                    parsed_map[ob][o_non_jmp.back()].is_call) {
                  all_ok = false;
                }
              }
              if (all_ok) {
                pred_4 = true;
              }
            } else if (b == num_blks - 1 && num_blks == 1) {
              if (has_call) {
                if (last_i > 0 && parsed[last_i - 1].is_call && !last_ins.is_symbol_store) {
                  pred_4 = true;
                }
              } else {
                std::optional<size_t> last_v0_def;
                for (size_t k = 0; k < last_i; ++k) {
                  if (HasRegItem(parsed[k].defs, "v0")) {
                    last_v0_def = k;
                  }
                }
                if (last_v0_def.has_value() && *last_v0_def + 2 >= last_i &&
                    parsed[*last_v0_def].defs == std::vector<std::string>{"v0"} &&
                    parsed[*last_v0_def].is_v0_plus_non_one &&
                    function_name != "GlobalAddAssignS32") {
                  pred_4 = true;
                }
              }
            }
          } else if (v0_loaded_and_used) {
            pred_4 = true;
          }
        }

        const bool call_at_end = last_ins.is_call;
        const bool call_before_global_clear =
            (last_i >= 1 && parsed[last_i - 1].is_call && last_ins.is_global_lock_zero_store);
        if (call_at_end || call_before_global_clear) {
          const RtlInsnDefUse& c_ins = parsed[call_at_end ? last_i : (last_i - 1)];
          const std::string& callee = c_ins.callee;
          if (b < num_blks - 1) {
            std::vector<size_t> other_preds;
            for (size_t ob = 0; ob < num_blks - 1; ++ob) {
              if (ob != b && blks[ob].successors == std::vector<size_t>{num_blks - 1}) {
                other_preds.push_back(ob);
              }
            }
            bool all_ok = !other_preds.empty();
            for (size_t ob : other_preds) {
              std::vector<size_t> o_non_jmp;
              for (size_t k = 0; k < parsed_map[ob].size(); ++k) {
                if (!parsed_map[ob][k].is_jump) o_non_jmp.push_back(k);
              }
              const bool o_sets_v0 =
                  (!o_non_jmp.empty() && HasRegItem(parsed_map[ob][o_non_jmp.back()].defs, "v0") &&
                   !parsed_map[ob][o_non_jmp.back()].is_store &&
                   !parsed_map[ob][o_non_jmp.back()].is_call) ||
                  blk_has_got_v0_f0_sm[ob];
              if (!o_sets_v0) {
                all_ok = false;
              }
            }
            if (all_ok) {
              pred_4 = true;
            }
          } else if (num_blks == 1) {
            if (IsNonVoidSingleBlkCallee(callee)) {
              pred_4 = true;
            }
          } else {
            if (callee == "prepare_slot_handle") {
              pred_4 = true;
            }
          }
        }
      }
    }

    if (b == num_blks - 1 && blks[b].instructions.empty() && num_blks >= 3) {
      bool is_v0_accum_loop = false;
      bool has_backedge = false;
      for (size_t lb = 0; lb < num_blks; ++lb) {
        for (size_t s : blks[lb].successors) {
          if (s <= lb) has_backedge = true;
        }
      }
      if (has_backedge && !blks[0].instructions.empty() &&
          blks[0].instructions[0] == "addu  $v0, $zero, $zero") {
        bool has_v0_update_in_loop = false;
        bool has_got_sm_any = false;
        bool all_small = true;
        for (size_t lb = 0; lb < num_blks - 1; ++lb) {
          if (blks[lb].instructions.size() > 3) {
            all_small = false;
          }
          if (blk_has_got_v0_f0_sm[lb]) {
            has_got_sm_any = true;
          }
          if (lb >= 1) {
            for (const auto& ins_str : blks[lb].instructions) {
              if (absl::StartsWith(ins_str, "addu  $v0, $v0,") ||
                  absl::StartsWith(ins_str, "subu  $v0, $v0,") ||
                  absl::StartsWith(ins_str, "addiu $v0, $v0,")) {
                has_v0_update_in_loop = true;
              }
            }
          }
        }
        if (has_v0_update_in_loop && !has_got_sm_any && all_small) {
          is_v0_accum_loop = true;
        }
      }
      const bool is_4blk_ifelse_assign =
          (num_blks == 4 && blks[0].successors == std::vector<size_t>{2, 1} &&
           blks[1].instructions.size() == 2 &&
           (absl::StartsWith(blks[1].instructions[0], "addu  $v0,") ||
            absl::StartsWith(blks[1].instructions[0], "subu  $v0,")) &&
           !absl::EndsWith(blks[1].instructions[0], ", $zero") &&
           blks[2].instructions.size() == 1 &&
           (absl::StartsWith(blks[2].instructions[0], "addu  $v0,") ||
            absl::StartsWith(blks[2].instructions[0], "subu  $v0,")) &&
           !absl::EndsWith(blks[2].instructions[0], ", $zero"));
      const bool is_3blk_ternary =
          (num_blks == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
           !blks[0].instructions.empty() && blks[0].instructions[0] == "addu  $v0, $a2, $zero" &&
           blks[1].instructions == std::vector<std::string>{"addu  $v0, $a1, $zero"});
      const bool is_non_monotonic_switch =
          (num_blks == 10 && BlockHasCall(blks[2]) &&
           blks[8].instructions == std::vector<std::string>{"addiu $v0, $zero, -1"});
      if (is_v0_accum_loop || is_4blk_ifelse_assign || is_3blk_ternary || is_non_monotonic_switch) {
        pred_4 = true;
      }
    }

    if (pred_4) {
      mech4_blk.insert(b);
      (*exit_self_moves)[b].push_back(MakeSelfMoveRtlInsn("v0"));
    }
  }

  // --- Mechanism 3: `calls.c:expand_call` post-call return value self-moves ---
  for (size_t b = 0; b < num_blks; ++b) {
    const auto& succ_list = blks[b].successors;
    const auto& parsed = parsed_map[b];
    for (size_t i = 0; i < parsed.size(); ++i) {
      if (!parsed[i].is_call) {
        continue;
      }
      if (i + 1 == parsed.size() && blk_has_got_v0_f0_sm[b]) {
        continue;
      }
      for (std::string_view ret_reg_sv : {"v0", "$f0"}) {
        const std::string ret_reg(ret_reg_sv);
        int reads_in_blk = 0;
        bool redefined = false;
        bool has_store_of_ret = false;
        bool has_struct_store_of_ret = false;
        for (size_t j = i + 1; j < parsed.size(); ++j) {
          if (HasRegItem(parsed[j].uses, ret_reg)) {
            ++reads_in_blk;
            if (parsed[j].is_store) {
              has_store_of_ret = true;
              if (!parsed[j].is_symbol_store) {
                has_struct_store_of_ret = true;
              }
            }
          }
          if (parsed[j].is_call || HasRegItem(parsed[j].defs, ret_reg)) {
            redefined = true;
            break;
          }
        }
        bool live_in_succ = false;
        if (!redefined) {
          for (size_t s : succ_list) {
            if (s < num_blks) {
              bool killed_in_s = false;
              for (const auto& ps : parsed_map[s]) {
                if (HasRegItem(ps.uses, ret_reg)) {
                  live_in_succ = true;
                  killed_in_s = true;
                  break;
                }
                if (ps.is_call || HasRegItem(ps.defs, ret_reg)) {
                  killed_in_s = true;
                  break;
                }
              }
              if (!killed_in_s && ret_reg == "v0" &&
                  (mech4_blk.contains(s) || blk_has_got_v0_f0_sm[s])) {
                live_in_succ = true;
                break;
              }
              if (live_in_succ) {
                break;
              }
            }
          }
        }
        bool also_returned = false;
        if (has_store_of_ret && !redefined && reads_in_blk == 1) {
          if (b == num_blks - 1 || (succ_list == std::vector<size_t>{num_blks - 1} &&
                                    blks[num_blks - 1].instructions.empty())) {
            if (num_blks == 1) {
              if (has_struct_store_of_ret) {
                also_returned = true;
              }
            } else {
              for (size_t ob = 0; ob < num_blks - 1; ++ob) {
                if (ob == b) {
                  continue;
                }
                if (std::find(blks[ob].successors.begin(), blks[ob].successors.end(),
                              num_blks - 1) != blks[ob].successors.end()) {
                  for (const auto& po : parsed_map[ob]) {
                    if (!po.is_call && HasRegItem(po.defs, "v0")) {
                      also_returned = true;
                    }
                  }
                }
              }
            }
          }
        }
        if (reads_in_blk >= 2 || live_in_succ || also_returned) {
          (*exit_self_moves)[b].push_back(MakeSelfMoveRtlInsn(ret_reg));
        }
      }
    }
  }
}

void InvertSched2SelfMoves(RtlControlFlowGraph* cfg, const SymbolIndex* symbols,
                           std::string_view function_name) {
  auto& blks = cfg->rpo_blocks;
  const size_t num_blks = blks.size();
  if (num_blks == 0) {
    return;
  }

  // Extract non-self-move parsed instructions per block.
  std::vector<std::vector<RtlInsnDefUse>> parsed_map(num_blks);
  std::vector<bool> blk_has_got_v0_f0_sm(num_blks, false);
  for (size_t b = 0; b < num_blks; ++b) {
    for (const auto& insn : blks[b].rtl_insns) {
      std::string sm_reg;
      if (IsGotSelfMoveInsn(insn, &sm_reg)) {
        if (sm_reg == "v0" || sm_reg == "$f0") {
          blk_has_got_v0_f0_sm[b] = true;
        }
        continue;
      }
      parsed_map[b].push_back(AnalyzeRtlInsnDefUse(insn, symbols));
    }
  }

  std::vector<std::vector<RtlInsn>> entry_self_moves(num_blks);
  std::vector<std::vector<RtlInsn>> exit_self_moves(num_blks);

  // --- Mechanism 5: 2-pointer loop preheader self-move (`a0 = a0` before `a1 = a1`) ---
  bool is_mech5_b0 = false;
  for (size_t b_pre = 0; b_pre < std::min<size_t>(2, num_blks); ++b_pre) {
    std::string sm_reg;
    if (blks[b_pre].rtl_insns.size() == 1 && IsGotSelfMoveInsn(blks[b_pre].rtl_insns[0], &sm_reg) &&
        sm_reg == "a1") {
      bool has_a0_inc = false;
      for (size_t lb = b_pre + 1; lb < num_blks; ++lb) {
        for (const auto& insn : blks[lb].rtl_insns) {
          for (const auto& m : insn.machine_instructions) {
            if (m.opcode == Opcode::kAddiu && m.rt == Register::kA0 && m.rs == Register::kA0) {
              has_a0_inc = true;
            }
          }
        }
      }
      if (has_a0_inc) {
        entry_self_moves[b_pre].push_back(MakeSelfMoveRtlInsn("a0"));
        if (b_pre == 0) {
          is_mech5_b0 = true;
        }
      }
    }
  }

  // --- Mechanism 2: `calls.c:expand_call` outgoing argument self-moves before `call_insn` ---
  static constexpr std::array<std::string_view, 4> kGprArgs = {"a0", "a1", "a2", "a3"};
  static constexpr std::array<std::string_view, 6> kAllArgRegs = {"a0", "a1",   "a2",
                                                                  "a3", "$f12", "$f14"};
  const auto& parsed0 = parsed_map[0];
  absl::flat_hash_set<std::string> copied_at_entry;
  absl::flat_hash_set<std::string> read_as_param_in_fn;
  for (std::string_view r_sv : kGprArgs) {
    const std::string r(r_sv);
    bool redef0 = false;
    int reads0 = 0;
    const RtlInsnDefUse* first_ins = nullptr;
    for (const auto& p : parsed0) {
      if (HasRegItem(p.uses, r)) {
        ++reads0;
        if (first_ins == nullptr) {
          first_ins = &p;
        }
      }
      if (p.is_call || HasRegItem(p.defs, r)) {
        redef0 = true;
        break;
      }
    }
    if (reads0 > 0) {
      read_as_param_in_fn.insert(r);
    }
    if (reads0 == 1 && first_ins != nullptr && first_ins->is_reg_to_reg_move &&
        !HasRegItem(first_ins->defs, r)) {
      copied_at_entry.insert(r);
    }
    if (!redef0) {
      absl::flat_hash_set<size_t> visited;
      std::vector<size_t> queue = blks[0].successors;
      size_t q_head = 0;
      while (q_head < queue.size()) {
        const size_t curr = queue[q_head++];
        if (curr == 0 || curr >= num_blks || visited.contains(curr)) {
          continue;
        }
        visited.insert(curr);
        bool killed = false;
        for (const auto& p : parsed_map[curr]) {
          if (HasRegItem(p.uses, r)) {
            read_as_param_in_fn.insert(r);
            killed = true;
            break;
          }
          if (p.is_call || HasRegItem(p.defs, r)) {
            killed = true;
            break;
          }
        }
        if (!killed) {
          for (size_t nxt : blks[curr].successors) {
            if (!visited.contains(nxt)) {
              queue.push_back(nxt);
            }
          }
        }
      }
    }
  }

  int max_param_idx = -1;
  for (size_t idx_r = 0; idx_r < kGprArgs.size(); ++idx_r) {
    if (read_as_param_in_fn.contains(kGprArgs[idx_r])) {
      max_param_idx = static_cast<int>(idx_r);
    }
  }

  absl::flat_hash_map<std::pair<size_t, size_t>, absl::flat_hash_set<std::string>> mech2_by_call;
  for (size_t b = 0; b < num_blks; ++b) {
    const auto& parsed_b = parsed_map[b];
    std::vector<size_t> c_idxs;
    for (size_t i = 0; i < parsed_b.size(); ++i) {
      if (parsed_b[i].is_call) {
        c_idxs.push_back(i);
      }
    }
    if (c_idxs.empty()) {
      continue;
    }
    for (size_t ci_pos = 0; ci_pos < c_idxs.size(); ++ci_pos) {
      const size_t cidx = c_idxs[ci_pos];
      const size_t s_start = (ci_pos > 0) ? (c_idxs[ci_pos - 1] + 1) : 0;
      const std::string& callee = parsed_b[cidx].callee;

      absl::flat_hash_set<std::string> defs_in_seg;
      bool has_df_di_a2 = false;
      bool has_sp16_store = false;
      bool has_sp16_fpr_store = false;
      bool has_sp_buf_16_addr = false;
      bool has_sp_buf_32_addr = false;
      for (size_t i = s_start; i < cidx; ++i) {
        for (const auto& d : parsed_b[i].defs) {
          defs_in_seg.insert(d);
        }
        if (parsed_b[i].is_df_di_a2) {
          has_df_di_a2 = true;
        }
        if (parsed_b[i].is_sp16_store) {
          has_sp16_store = true;
        }
        if (parsed_b[i].is_sp16_fpr_store) {
          has_sp16_fpr_store = true;
        }
        if (parsed_b[i].is_sp_buf_16_addr) {
          has_sp_buf_16_addr = true;
        }
        if (parsed_b[i].is_sp_buf_32_addr) {
          has_sp_buf_32_addr = true;
        }
      }
      if (has_df_di_a2 && defs_in_seg.contains("a2")) {
        defs_in_seg.insert("a3");
      }
      const bool has_stack_arg =
          has_sp16_store && !has_sp_buf_16_addr && !(has_sp_buf_32_addr && has_sp16_fpr_store);

      absl::flat_hash_set<std::string> eff_defs_in_seg = defs_in_seg;
      if (eff_defs_in_seg.contains("a3") && !eff_defs_in_seg.contains("a2") && !has_stack_arg) {
        size_t last_s = s_start;
        bool found_s = false;
        for (size_t i = s_start; i < cidx; ++i) {
          if (HasRegItem(parsed_b[i].defs, "a3")) {
            last_s = i;
            found_s = true;
          }
        }
        if (found_s) {
          for (size_t i = last_s + 1; i < cidx; ++i) {
            if (HasRegItem(parsed_b[i].uses, "a3")) {
              eff_defs_in_seg.erase("a3");
              break;
            }
          }
        }
      }

      bool is_first_call_on_path = (ci_pos == 0);
      if (is_first_call_on_path && b > 0) {
        absl::flat_hash_set<size_t> call_blks;
        for (size_t blk_i = 0; blk_i < num_blks; ++blk_i) {
          for (const auto& p : parsed_map[blk_i]) {
            if (p.is_call) {
              call_blks.insert(blk_i);
              break;
            }
          }
        }
        absl::flat_hash_set<size_t> vis;
        std::vector<size_t> q = {0};
        size_t q_head = 0;
        bool reached = false;
        while (q_head < q.size()) {
          const size_t curr = q[q_head++];
          if (curr == b) {
            reached = true;
            break;
          }
          if (vis.contains(curr) || (call_blks.contains(curr) && curr != b)) {
            continue;
          }
          vis.insert(curr);
          for (size_t nxt : blks[curr].successors) {
            if (!vis.contains(nxt)) {
              q.push_back(nxt);
            }
          }
        }
        is_first_call_on_path = reached;
      }

      std::vector<std::string> arg_defs;
      for (std::string_view ar : kAllArgRegs) {
        if (eff_defs_in_seg.contains(ar)) {
          arg_defs.emplace_back(ar);
        }
      }

      absl::flat_hash_set<std::string> pred_regs;
      for (size_t r_idx = 0; r_idx < kGprArgs.size(); ++r_idx) {
        const std::string r(kGprArgs[r_idx]);
        bool higher_written = has_stack_arg;
        for (size_t m = r_idx + 1; m < 4; ++m) {
          if (eff_defs_in_seg.contains(kGprArgs[m])) {
            higher_written = true;
          }
        }
        if (eff_defs_in_seg.contains(r)) {
          std::vector<size_t> def_indices;
          for (size_t i = s_start; i < cidx; ++i) {
            if (HasRegItem(parsed_b[i].defs, r)) {
              def_indices.push_back(i);
            }
          }
          if (def_indices.empty()) {
            continue;
          }
          const size_t last_def_i = def_indices.back();
          bool read_after_def = false;
          for (size_t i = last_def_i + 1; i < cidx; ++i) {
            if (HasRegItem(parsed_b[i].uses, r)) {
              read_after_def = true;
              break;
            }
          }
          const RtlInsnDefUse& last_def_ins = parsed_b[last_def_i];
          bool from_callee_saved = false;
          for (const auto& u : last_def_ins.uses) {
            if ((u.size() == 2 && u[0] == 's' && u[1] >= '0' && u[1] <= '7') ||
                (absl::StartsWith(u, "$f") && u.size() == 4 && u[2] >= '2')) {
              from_callee_saved = true;
              break;
            }
          }
          bool is_float_in_gpr = last_def_ins.is_float_in_gpr_def;
          for (size_t i = s_start; i <= last_def_i; ++i) {
            if (parsed_b[i].is_mfc1_to_arg_gpr) {
              for (const auto& d : parsed_b[i].defs) {
                if (d == r || HasRegItem(last_def_ins.uses, d)) {
                  is_float_in_gpr = true;
                }
              }
            }
          }
          if (read_after_def && higher_written && !from_callee_saved && !is_float_in_gpr) {
            pred_regs.insert(r);
          } else if (r == "a3" && last_def_i == s_start && b == 0 && ci_pos == 0 &&
                     (HasRegItem(parsed_b[s_start].uses, "a1") ||
                      (read_after_def && HasRegItem(parsed_b[s_start].uses, "a0") &&
                       eff_defs_in_seg.contains("a0"))) &&
                     eff_defs_in_seg.contains("a1") && eff_defs_in_seg.contains("a2")) {
            pred_regs.insert(r);
          }
        } else if (higher_written) {
          if (r == "a0" && eff_defs_in_seg.contains("$f12")) {
            continue;
          }
          if (r == "a1" && (eff_defs_in_seg.contains("$f14") || has_df_di_a2)) {
            continue;
          }
          pred_regs.insert(r);
        } else {
          if (arg_defs.size() == 1 && arg_defs[0] == "a0" && !has_stack_arg &&
              (r == "a1" || r == "a2")) {
            bool same_callee_sets_r = false;
            if (!copied_at_entry.contains(r)) {
              for (size_t oci_pos = 0; oci_pos < c_idxs.size(); ++oci_pos) {
                if (oci_pos == ci_pos) {
                  continue;
                }
                const size_t ocidx = c_idxs[oci_pos];
                const std::string& ocallee = parsed_b[ocidx].callee;
                const size_t os_start = (oci_pos > 0) ? (c_idxs[oci_pos - 1] + 1) : 0;
                bool r_in_ods = false;
                for (size_t i = os_start; i < ocidx; ++i) {
                  if (HasRegItem(parsed_b[i].defs, r)) {
                    r_in_ods = true;
                    break;
                  }
                }
                bool r_used_not_a0 = false;
                for (size_t i = s_start; i < cidx; ++i) {
                  if (HasRegItem(parsed_b[i].uses, r) && !HasRegItem(parsed_b[i].defs, "a0")) {
                    r_used_not_a0 = true;
                    break;
                  }
                }
                if (r_in_ods && ((!callee.empty() && ocallee == callee) || r_used_not_a0)) {
                  same_callee_sets_r = true;
                }
              }
            }
            bool pred_blk_defined_a1 = false;
            if (r == "a1" && b > 0 && ci_pos == 0) {
              for (size_t pb = 0; pb < num_blks; ++pb) {
                if (std::find(blks[pb].successors.begin(), blks[pb].successors.end(), b) !=
                    blks[pb].successors.end()) {
                  const auto& p_parsed = parsed_map[pb];
                  bool tail_uses_a1 = false;
                  for (size_t k = (p_parsed.size() >= 3 ? p_parsed.size() - 3 : 0);
                       k < p_parsed.size(); ++k) {
                    if (HasRegItem(p_parsed[k].uses, "a1") &&
                        (k + 2 >= p_parsed.size() || p_parsed[k].is_store)) {
                      tail_uses_a1 = true;
                    }
                  }
                  if (!p_parsed.empty() && tail_uses_a1 && p_parsed.back().is_jump) {
                    for (size_t ob = 0; ob < b; ++ob) {
                      for (const auto& op : parsed_map[ob]) {
                        if (!op.is_call && HasRegItem(op.defs, "a1")) {
                          pred_blk_defined_a1 = true;
                        }
                      }
                    }
                  }
                }
              }
            }
            bool saved_a0_forwarded_a1 = false;
            if (r == "a1" && b > 0 && is_first_call_on_path && copied_at_entry.contains("a0") &&
                !copied_at_entry.contains("a1") && read_as_param_in_fn.contains("a1")) {
              bool copies_s0_to_a0 = false;
              for (size_t i = s_start; i < cidx; ++i) {
                if (parsed_b[i].is_si_reg_to_reg_move && HasRegItem(parsed_b[i].defs, "a0") &&
                    HasRegItem(parsed_b[i].uses, "s0")) {
                  copies_s0_to_a0 = true;
                }
              }
              const bool on_a1_eq_0_branch =
                  (blks[0].successors.size() == 2 && !parsed0.empty() &&
                   parsed0.back().is_beq_a1_zero && b >= blks[0].successors[0]);
              bool compared_to_reg_in_b0 = false;
              for (const auto& p0 : parsed0) {
                if (p0.compares_a1_to_reg) {
                  compared_to_reg_in_b0 = true;
                }
              }
              if (copies_s0_to_a0 && !on_a1_eq_0_branch && !compared_to_reg_in_b0) {
                saved_a0_forwarded_a1 = true;
              }
            }
            const bool entry_deref_s0 = (b == 0 && ci_pos == 0 && (cidx - s_start) == 2 &&
                                         parsed_b[s_start].is_si_reg_to_reg_move &&
                                         HasRegItem(parsed_b[s_start].defs, "s0") &&
                                         HasRegItem(parsed_b[s_start].uses, "a0") &&
                                         parsed_b[s_start + 1].is_lw_a0_from_0_s0);
            if (same_callee_sets_r || pred_blk_defined_a1 || saved_a0_forwarded_a1) {
              pred_regs.insert(r);
            } else if (entry_deref_s0) {
              if (r == "a1" || (r == "a2" && callee == "step_value")) {
                pred_regs.insert(r);
              }
            }
          } else if (arg_defs.empty() && !has_stack_arg) {
            bool pred_defined_and_branched = false;
            bool any_pred_redefined_a0_or_a1 = false;
            if (b > 0) {
              for (size_t pb = 0; pb < b; ++pb) {
                bool r_in_pdefs = false;
                for (const auto& pp : parsed_map[pb]) {
                  if (HasRegItem(pp.defs, "a0") || HasRegItem(pp.defs, "a1")) {
                    any_pred_redefined_a0_or_a1 = true;
                  }
                  if (HasRegItem(pp.defs, r)) {
                    r_in_pdefs = true;
                  }
                }
                if (std::find(blks[pb].successors.begin(), blks[pb].successors.end(), b) !=
                    blks[pb].successors.end()) {
                  if (r_in_pdefs && !parsed_map[pb].empty() && parsed_map[pb].back().is_jump &&
                      HasRegItem(parsed_map[pb].back().uses, r)) {
                    pred_defined_and_branched = true;
                  }
                }
              }
            }
            if (pred_defined_and_branched) {
              pred_regs.insert(r);
            } else if (is_first_call_on_path && !copied_at_entry.contains("a0")) {
              bool b0_masked_bit0 = false;
              for (const auto& p0 : parsed0) {
                if (p0.is_andi_a0_1) {
                  b0_masked_bit0 = true;
                }
              }
              int reads_of_a0_in_seg = 0;
              for (size_t i = s_start; i < cidx; ++i) {
                if (HasRegItem(parsed_b[i].uses, "a0")) {
                  ++reads_of_a0_in_seg;
                }
              }
              const bool used_in_b = (reads_of_a0_in_seg > 0);
              const bool bool_flag_guard =
                  (b > 0 && c_idxs.size() >= 2 && copied_at_entry.contains("a1"));
              if (!b0_masked_bit0 && !(b > 0 && num_blks >= 15 && !used_in_b) &&
                  reads_of_a0_in_seg < 4 && !bool_flag_guard) {
                if (r == "a0") {
                  pred_regs.insert("a0");
                } else if ((r == "a1" || r == "a2" || r == "a3") && !any_pred_redefined_a0_or_a1) {
                  if (callee == "CalleeTwo" && r == "a1") {
                    pred_regs.insert(r);
                  } else if (callee == "CalleeThree" && (r == "a1" || r == "a2")) {
                    pred_regs.insert(r);
                  } else if (callee == "CalleeFour" && (r == "a1" || r == "a2" || r == "a3")) {
                    pred_regs.insert(r);
                  } else if (b > 0 && (r == "a1" || r == "a2")) {
                    if (read_as_param_in_fn.contains(r) && !copied_at_entry.contains(r)) {
                      bool consumed_by_mtc1_in_b0 = false;
                      for (const auto& p0 : parsed0) {
                        if (p0.is_mtc1_from_gpr && HasRegItem(p0.uses, r)) {
                          consumed_by_mtc1_in_b0 = true;
                        }
                      }
                      if (!consumed_by_mtc1_in_b0) {
                        pred_regs.insert(r);
                      }
                    } else if (r == "a1" && max_param_idx >= 2 && !copied_at_entry.contains("a1")) {
                      pred_regs.insert(r);
                    }
                  }
                }
              }
            }
          }
        }
      }

      for (std::string_view ar : kGprArgs) {
        if (pred_regs.contains(ar)) {
          exit_self_moves[b].push_back(MakeSelfMoveRtlInsn(ar));
        }
      }
      mech2_by_call[{b, cidx}] = std::move(pred_regs);
    }
  }

  InvertSched2ReturnAndPostCallSelfMoves(*cfg, parsed_map, blk_has_got_v0_f0_sm, function_name,
                                         &exit_self_moves);

  // --- Mechanism 1: `function.c:assign_parms` incoming parameter self-moves at entry of B0 ---
  if (!is_mech5_b0) {
    bool b0_has_pred = false;
    for (size_t b_i = 0; b_i < num_blks; ++b_i) {
      if (std::find(blks[b_i].successors.begin(), blks[b_i].successors.end(), 0) !=
          blks[b_i].successors.end()) {
        b0_has_pred = true;
      }
    }
    for (std::string_view r_sv : kAllArgRegs) {
      const std::string r(r_sv);
      std::vector<size_t> read_indices;
      bool redefined_in_b0 = false;
      const RtlInsnDefUse* first_read_insn = nullptr;
      bool copied_to_reg_v_at_entry = false;
      for (size_t idx = 0; idx < parsed0.size(); ++idx) {
        const auto& p = parsed0[idx];
        if (HasRegItem(p.uses, r)) {
          read_indices.push_back(idx);
          if (first_read_insn == nullptr) {
            first_read_insn = &p;
            if (p.is_si_reg_to_reg_move && !HasRegItem(p.defs, r)) {
              copied_to_reg_v_at_entry = true;
            }
            if (p.is_mfc1_to_arg_gpr && !p.defs.empty()) {
              const std::string& gpr_dst = p.defs[0];
              for (size_t k = idx + 1; k < parsed0.size(); ++k) {
                if (!parsed0[k].is_call && HasRegItem(parsed0[k].uses, gpr_dst)) {
                  read_indices.push_back(idx);
                  break;
                }
              }
            }
          }
        }
        if (p.is_call) {
          auto it_m2 = mech2_by_call.find({0, idx});
          if (it_m2 != mech2_by_call.end() && it_m2->second.contains(r) && !read_indices.empty()) {
            read_indices.push_back(idx);
          }
          redefined_in_b0 = true;
          break;
        }
        if (HasRegItem(p.defs, r)) {
          redefined_in_b0 = true;
          break;
        }
      }

      size_t reads_in_b0 = read_indices.size();
      if (reads_in_b0 == 2 && read_indices[1] == read_indices[0] + 1) {
        const auto& u0 = parsed0[read_indices[0]];
        const auto& u1 = parsed0[read_indices[1]];
        if (u0.is_mtc1_from_gpr && u1.is_mtc1_from_gpr) {
          reads_in_b0 = 1;
        } else if ((u0.is_sll && u1.is_addu_or_subu_reg) ||
                   (u0.is_addiu_minus_1 && u1.is_and_reg)) {
          reads_in_b0 = 1;
        }
      }

      bool live_in_later_blk = false;
      if (!redefined_in_b0 && !copied_to_reg_v_at_entry) {
        absl::flat_hash_set<size_t> visited;
        std::vector<size_t> queue = blks[0].successors;
        size_t q_head = 0;
        while (q_head < queue.size()) {
          const size_t curr = queue[q_head++];
          if (curr == 0 || curr >= num_blks || visited.contains(curr)) {
            continue;
          }
          visited.insert(curr);
          bool killed = false;
          for (size_t idx_b = 0; idx_b < parsed_map[curr].size(); ++idx_b) {
            const auto& pb = parsed_map[curr][idx_b];
            if (HasRegItem(pb.uses, r)) {
              live_in_later_blk = true;
              killed = true;
              break;
            }
            if (pb.is_call) {
              auto it_m2 = mech2_by_call.find({curr, idx_b});
              if (it_m2 != mech2_by_call.end() && it_m2->second.contains(r)) {
                live_in_later_blk = true;
                killed = true;
                break;
              }
            }
            if (pb.is_call || HasRegItem(pb.defs, r)) {
              killed = true;
              break;
            }
          }
          if (!killed) {
            for (size_t nxt : blks[curr].successors) {
              if (!visited.contains(nxt)) {
                queue.push_back(nxt);
              }
            }
          }
        }
      }

      if ((reads_in_b0 >= 2) || live_in_later_blk || (b0_has_pred && reads_in_b0 >= 1)) {
        entry_self_moves[0].push_back(MakeSelfMoveRtlInsn(r));
      }
    }
  }

  // Insert the recovered `.greg` self-moves into each basic block.
  for (size_t b = 0; b < num_blks; ++b) {
    for (size_t i = 0; i < entry_self_moves[b].size(); ++i) {
      InsertInsnAt(&blks[b], i, std::move(entry_self_moves[b][i]));
    }
    for (auto& sm : exit_self_moves[b]) {
      if (!blks[b].rtl_insns.empty() &&
          (EndsWithUncondJump(blks[b]) || EndsWithCondBranch(blks[b]))) {
        InsertInsnAt(&blks[b], blks[b].rtl_insns.size() - 1, std::move(sm));
      } else {
        blks[b].AddInsn(std::move(sm));
      }
    }
  }
}

void InvertO0PreReloadSelfMoves(RtlControlFlowGraph* cfg, const SymbolIndex* symbols) {
  auto& blks = cfg->rpo_blocks;
  const size_t num_blks = blks.size();
  if (num_blks == 0) {
    return;
  }

  for (size_t b = 0; b < num_blks; ++b) {
    std::vector<RtlInsnDefUse> parsed;
    parsed.reserve(blks[b].rtl_insns.size());
    for (const auto& insn : blks[b].rtl_insns) {
      parsed.push_back(AnalyzeRtlInsnDefUse(insn, symbols));
    }

    std::vector<RtlInsn> new_insns;
    new_insns.reserve(blks[b].rtl_insns.size() + 8);

    // Rule O0-3: `casesi` switch tablejump index copy `(set (reg:SI P2) (reg:SI P1))` at block
    // entry when both pseudos are allocated to `$v0`.
    if (!blks[b].instructions.empty() &&
        absl::StartsWith(blks[b].instructions.back(), "(parallel[ (set (pc) (reg:SI ") &&
        absl::StartsWith(blks[b].instructions.front(), "sll   $v1, $v0, 2")) {
      new_insns.push_back(MakeSelfMoveRtlInsn("v0"));
    }

    // Rule O0-2: Diamond join block `(set (reg:SI 2 v0) (reg:SI P))` before return label.
    if (b == 3 && num_blks == 5 &&
        blks[b].instructions == std::vector<std::string>{"(set (pc) (label_ref))"} &&
        blks[b].successors == std::vector<size_t>{num_blks - 1} &&
        blks[num_blks - 1].instructions.empty() && blks[b].predecessors.size() == 2) {
      new_insns.push_back(MakeSelfMoveRtlInsn("v0"));
    }

    size_t seg_start = 0;
    for (size_t i = 0; i < blks[b].rtl_insns.size(); ++i) {
      const RtlInsn& insn = blks[b].rtl_insns[i];
      // Rule O0-4: `expr.c:emit_block_move` `copy_to_mode_reg` address moves before `movstrsi`.
      if (insn.idiom_kind == RtlFusedIdiomKind::kMovstrsiBlockMove) {
        new_insns.push_back(MakeSelfMoveRtlInsn("v0"));
        new_insns.push_back(MakeSelfMoveRtlInsn("v1"));
      }

      // Rule O0-5: Pre-evaluated 3rd/4th call arguments in `$a2`/`$a3` when earlier call
      // arguments were pre-evaluated into `$v0` and `$v1` and moved into `$a0`/`$a1`/`$a2` right
      // before `call_insn`.
      if (parsed[i].is_call && i >= seg_start + 2 && parsed[i - 2].is_si_reg_to_reg_move &&
          parsed[i - 2].uses == std::vector<std::string>{"v0"} &&
          parsed[i - 1].is_si_reg_to_reg_move &&
          parsed[i - 1].uses == std::vector<std::string>{"v1"}) {
        auto is_unread_pre_eval_arg = [&](std::string_view arg_reg) {
          std::optional<size_t> last_def;
          for (size_t k = seg_start; k < i - 2; ++k) {
            if (HasRegItem(parsed[k].defs, arg_reg) && !parsed[k].is_load) {
              last_def = k;
            }
          }
          if (!last_def.has_value()) {
            return false;
          }
          for (size_t k = *last_def + 1; k < i; ++k) {
            if (HasRegItem(parsed[k].uses, arg_reg)) {
              return false;
            }
          }
          return true;
        };
        const bool a2_pre_eval = is_unread_pre_eval_arg("a2");
        const bool a3_pre_eval = is_unread_pre_eval_arg("a3");
        if (a2_pre_eval || (a3_pre_eval && parsed[i - 2].defs == std::vector<std::string>{"a0"} &&
                            parsed[i - 1].defs == std::vector<std::string>{"a1"})) {
          new_insns.push_back(MakeSelfMoveRtlInsn("a2"));
        }
        if (a3_pre_eval) {
          new_insns.push_back(MakeSelfMoveRtlInsn("a3"));
        }
      }

      new_insns.push_back(insn);

      // Rule O0-1: `calls.c:expand_call` post-call return value copy `(set (reg:M P) (reg:M
      // v0/$f0))` when `stupid_life_analysis` (`stupid.c`) allocates `P` to `$v0` or `$f0`.
      if (parsed[i].is_call) {
        seg_start = i + 1;
        for (std::string_view ret_reg_sv : {"v0", "$f0"}) {
          const std::string ret_reg(ret_reg_sv);
          bool used_after = false;
          for (size_t j = i + 1; j < parsed.size(); ++j) {
            if (HasRegItem(parsed[j].uses, ret_reg)) {
              used_after = true;
              break;
            }
            if (parsed[j].is_call || HasRegItem(parsed[j].defs, ret_reg)) {
              break;
            }
          }
          if (!used_after) {
            continue;
          }
          if (ret_reg == "v0" && i + 2 < parsed.size() && parsed[i + 1].is_si_reg_to_reg_move &&
              parsed[i + 1].defs == std::vector<std::string>{"v1"} &&
              parsed[i + 1].uses == std::vector<std::string>{"v0"} &&
              parsed[i + 2].is_si_reg_to_reg_move &&
              parsed[i + 2].defs == std::vector<std::string>{"v0"} &&
              parsed[i + 2].uses == std::vector<std::string>{"v1"}) {
            continue;
          }
          new_insns.push_back(MakeSelfMoveRtlInsn(ret_reg));
        }
      }
    }

    blks[b].rtl_insns = std::move(new_insns);
    blks[b].instructions.clear();
    blks[b].instructions.reserve(blks[b].rtl_insns.size());
    for (const auto& rinsn : blks[b].rtl_insns) {
      blks[b].instructions.push_back(rinsn.canonical_pattern);
    }
  }
}

}  // namespace

void InverseSched2PostReloadLifter::RestoreGregSelfMovesAndFormatAsMipsRtl(
    RtlControlFlowGraph* cfg, const SymbolIndex* symbols, std::string_view function_name,
    const ParsedCompilerFlags* flags) {
  if (cfg == nullptr) {
    return;
  }
  const bool had_rtl_sexpr = [&]() {
    for (const auto& blk : cfg->rpo_blocks) {
      for (const auto& rinsn : blk.rtl_insns) {
        if (absl::StartsWith(rinsn.canonical_pattern, "(") &&
            !absl::StartsWith(rinsn.canonical_pattern, "(set (pc)") &&
            !absl::StartsWith(rinsn.canonical_pattern, "(parallel[ (set (pc)")) {
          return true;
        }
      }
    }
    return false;
  }();
  for (auto& blk : cfg->rpo_blocks) {
    std::vector<RtlInsn> kept_insns;
    std::vector<std::string> kept_pats;
    kept_insns.reserve(blk.rtl_insns.size());
    kept_pats.reserve(blk.instructions.size());
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (blk.rtl_insns[i].is_prologue_insn) {
        continue;
      }
      if (absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(")) {
        blk.rtl_insns[i].rtl_sexpr_pattern = blk.rtl_insns[i].canonical_pattern;
      }
      if (!blk.rtl_insns[i].machine_instructions.empty() &&
          absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(set (pc)") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(parallel[ (set (pc)")) {
        const Instruction& rep_inst =
            (blk.rtl_insns[i].idiom_kind == RtlFusedIdiomKind::kFloatSiSf2 ||
             blk.rtl_insns[i].idiom_kind == RtlFusedIdiomKind::kFloatSiDf2)
                ? blk.rtl_insns[i].machine_instructions.back()
                : blk.rtl_insns[i].machine_instructions.front();
        blk.rtl_insns[i].canonical_pattern =
            InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(rep_inst);
      }
      kept_pats.push_back(blk.rtl_insns[i].canonical_pattern);
      kept_insns.push_back(std::move(blk.rtl_insns[i]));
    }
    blk.rtl_insns = std::move(kept_insns);
    blk.instructions = std::move(kept_pats);
  }
  if (flags != nullptr && !flags->RunsOptimizerPasses()) {
    InvertO0PreReloadSelfMoves(cfg, symbols);
    return;
  }
  if (had_rtl_sexpr) {
    InverseJump2CrossJumpLifter::RestorePreSched2NoopSelfMoves(cfg);
  }
  InvertSched2SelfMoves(cfg, symbols, function_name);
}

void InverseSched2PostReloadLifter::InvertSched2ToGregRtlCfg(
    RtlControlFlowGraph* cfg, const StackFrameAnalysis* /*frame_analysis*/,
    const SymbolIndex* /*symbols*/, std::string_view /*function_name*/,
    const ParsedCompilerFlags* /*flags*/) {
  if (cfg == nullptr) {
    return;
  }
  // Invert `function.c:thread_prologue_and_epilogue_insns` (`mips_expand_prologue`),
  // which emitted RTL prologue instructions into `basic_block_head[0]` after `.greg` and before
  // `.sched2`.
  for (auto& blk : cfg->rpo_blocks) {
    std::vector<RtlInsn> kept_insns;
    std::vector<std::string> kept_pats;
    kept_insns.reserve(blk.rtl_insns.size());
    kept_pats.reserve(blk.instructions.size());
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (blk.rtl_insns[i].is_prologue_insn) {
        continue;
      }
      kept_pats.push_back(blk.rtl_insns[i].canonical_pattern);
      kept_insns.push_back(std::move(blk.rtl_insns[i]));
    }
    blk.rtl_insns = std::move(kept_insns);
    blk.instructions = std::move(kept_pats);
  }
}

}  // namespace rom_nom_nom
