#include "lifter/invert_do_cross_jump.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/post_reload_noop_moves.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_cfg_utils.h"

namespace rom_nom_nom::lifter {
namespace {

bool IsNoopRegMoveExpr(const RtlExpr& expr) {
  return expr.IsSet() && expr.SetDest().op == RtlOp::kReg && expr.SetSrc().op == RtlOp::kReg &&
         expr.SetDest().RegNo() == expr.SetSrc().RegNo() &&
         expr.SetDest().mode == expr.SetSrc().mode;
}

bool ContainsLabelRefInParsedInsn(const RtlPatternAndNotes& parsed) {
  bool found = false;
  auto visit_fn = [&found](const RtlExpr& node) {
    if (node.op == RtlOp::kLabelRef) {
      found = true;
    }
  };
  parsed.pattern.Visit(visit_fn);
  for (const RtlRegNote& note : parsed.reg_notes) {
    note.datum.Visit(visit_fn);
  }
  return found;
}

bool HasNonShareableEqualOrEquivNote(const RtlPatternAndNotes& parsed) {
  for (const RtlRegNote& note : parsed.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEqual || note.kind == RtlMachineMode::kRegEquiv) {
      if (note.datum.op != RtlOp::kConstInt || note.datum.int_val < -kGccMaxSavedConstInt ||
          note.datum.int_val > kGccMaxSavedConstInt) {
        return true;
      }
    }
  }
  return false;
}

// Returns true if `pat` is a non-control, non-use/clobber, non-note instruction
// that `find_cross_jump` (`jump.c`) counts toward `minimum` and can cross-jump
// across basic blocks without altering label reference counts or losing
// non-interned `REG_EQUAL`/`REG_EQUIV` notes under pointer-equality checks.
bool IsShareableCrossJumpInsn(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  if (!parsed.ok()) {
    return false;
  }
  const RtlExpr& expr = parsed->pattern;
  if (IsEpilogueOrControlTopExpr(expr) || expr.op == RtlOp::kNote || expr.op == RtlOp::kAddrVec ||
      expr.op == RtlOp::kAddrDiffVec || expr.op == RtlOp::kConstInt ||
      expr.op == RtlOp::kAsmInput || expr.op == RtlOp::kAsmOperands ||
      expr.op == RtlOp::kSequence) {
    return false;
  }
  if (IsNoopRegMoveExpr(expr) || ContainsLabelRefInParsedInsn(*parsed) ||
      HasNonShareableEqualOrEquivNote(*parsed)) {
    return false;
  }
  return true;
}

bool IsAnyJumpOrReturnInsn(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  return parsed.ok() && IsAnyJumpOrReturnTopExpr(parsed->pattern);
}

bool IsNormalNonJumpNonCallInsn(const RtlExpr& expr) {
  if (expr.op == RtlOp::kNote || expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec ||
      expr.op == RtlOp::kSequence) {
    return false;
  }
  if (IsAnyJumpOrReturnTopExpr(expr) || ContainsCallInExpr(expr)) {
    return false;
  }
  return true;
}

// In `jump.c:jump_optimize` (lines 434-465, `if (noop_moves)`), every normal
// `INSN` whose body is `(set <dest> <src>)` with `sreg = true_regnum(src) >= 0`,
// `dreg = true_regnum(dest) >= 0`, and `sreg != dreg` walks backward via
// `prev_nonnote_insn` up to the nearest `CODE_LABEL` and calls
// `remove_death(dreg, trial)` on the first instruction `trial` that carries a
// `REG_DEAD` note for `dreg`. When a `REG_DEAD` note for `dreg` survived into
// `.11.jump2` (because an intervening `REG_DEAD` note or label in `.10.sched2`
// was consumed during the forward pass), prepending a compensating `REG_DEAD`
// note on `trial` ensures `remove_death` consumes the compensating note and
// preserves `trial`'s surviving `REG_DEAD` note.
void RestoreNoopMovesConsumedDeaths(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                    absl::Span<const size_t> uncross_copied_start_idx = {}) {
  struct FlatInsn {
    size_t b = 0;
    size_t i = 0;
    RtlPatternAndNotes parsed;
    std::vector<int> original_dead_regnos;
    std::vector<RtlRegNote> original_dead_notes;
    std::vector<int> call_arg_dead_regnos;
    bool preceded_by_loop_end_note = false;
    bool from_uncross_jump_tail = false;
    bool modified = false;
  };

  auto insn_reads_hard_reg = [gp64, fp64](const RtlExpr& top, int target_reg) -> bool {
    auto reg_span_matches = [&](const RtlExpr& expr) -> bool {
      const int hr = TrueRegnum(expr);
      if (hr < 0) {
        return false;
      }
      int nw = 1;
      if (expr.mode == RtlMachineMode::kDi) {
        if ((hr < 32 && !gp64) || (hr >= 32 && hr < 64 && !fp64)) {
          nw = 2;
        }
      } else if (expr.mode == RtlMachineMode::kDf) {
        if ((hr < 32 && !gp64) || (hr >= 32 && hr < 64 && !fp64)) {
          nw = 2;
        }
      }
      return target_reg >= hr && target_reg < hr + nw;
    };
    auto visit_rvalue = [&](const auto& self, const RtlExpr& v) -> bool {
      if (v.op == RtlOp::kReg) {
        return reg_span_matches(v);
      }
      if (v.op == RtlOp::kSubreg && !v.children.empty() && v.children[0].op == RtlOp::kReg) {
        return reg_span_matches(v);
      }
      for (const RtlExpr& ch : v.children) {
        if (self(self, ch)) {
          return true;
        }
      }
      return false;
    };
    auto visit_stmt = [&](const auto& self, const RtlExpr& e) -> bool {
      if (e.op == RtlOp::kSequence) {
        for (const RtlExpr& ch : e.children) {
          const RtlExpr& body = (ch.children.size() >= 4) ? ch.children[3] : ch;
          if (self(self, body)) {
            return true;
          }
        }
        return false;
      }
      if (e.op == RtlOp::kParallel) {
        for (const RtlExpr& ch : e.children) {
          if (self(self, ch)) {
            return true;
          }
        }
        return false;
      }
      if (e.IsSet()) {
        if (visit_rvalue(visit_rvalue, e.SetSrc())) {
          return true;
        }
        const RtlExpr& dst = e.SetDest();
        if (dst.op == RtlOp::kMem && !dst.children.empty()) {
          return visit_rvalue(visit_rvalue, dst.children[0]);
        }
        if (dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract ||
            dst.op == RtlOp::kStrictLowPart) {
          for (const RtlExpr& ch : dst.children) {
            if (visit_rvalue(visit_rvalue, ch)) {
              return true;
            }
          }
        }
        return false;
      }
      if (e.op == RtlOp::kClobber) {
        if (!e.children.empty() && e.children[0].op == RtlOp::kMem &&
            !e.children[0].children.empty()) {
          return visit_rvalue(visit_rvalue, e.children[0].children[0]);
        }
        return false;
      }
      return visit_rvalue(visit_rvalue, e);
    };
    return visit_stmt(visit_stmt, top);
  };

  const size_t num_blocks = cfg->rpo_blocks.size();
  size_t run_start_b = 0;
  while (run_start_b < num_blocks) {
    size_t run_end_b = run_start_b + 1;
    while (run_end_b < num_blocks && !cfg->rpo_blocks[run_end_b].has_label) {
      ++run_end_b;
    }

    std::vector<FlatInsn> run_insns;
    bool pending_loop_end_note = false;
    for (size_t b = run_start_b; b < run_end_b; ++b) {
      const auto& blk = cfg->rpo_blocks[b];
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok()) {
          continue;
        }
        if (parsed->pattern.op == RtlOp::kNote) {
          const bool is_loop_end =
              (i < blk.rtl_insns.size() &&
               blk.rtl_insns[i].loop_note_kind == RtlLoopNoteKind::kLoopEnd) ||
              ParseRtlLoopNoteKind(parsed->pattern.str_val) == RtlLoopNoteKind::kLoopEnd ||
              std::any_of(parsed->pattern.generic_items.begin(),
                          parsed->pattern.generic_items.end(), [](const auto& item) {
                            return ParseRtlLoopNoteKind(item.text) == RtlLoopNoteKind::kLoopEnd;
                          });
          if (is_loop_end) {
            pending_loop_end_note = true;
          }
          continue;
        }
        FlatInsn entry;
        entry.b = b;
        entry.i = i;
        entry.parsed = std::move(*parsed);
        entry.preceded_by_loop_end_note = pending_loop_end_note;
        entry.from_uncross_jump_tail =
            (b < uncross_copied_start_idx.size() && i >= uncross_copied_start_idx[b]);
        pending_loop_end_note = false;
        for (const RtlRegNote& note : entry.parsed.reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
            entry.original_dead_regnos.push_back(note.datum.RegNo());
            entry.original_dead_notes.push_back(note);
          }
        }
        // `flow.c:1672-1677` + `jump.c:452-459`: `CALL_INSN`s consume every
        // call-clobbered argument register in `CALL_INSN_FUNCTION_USAGE` and
        // receive `REG_DEAD` on the `CALL_INSN` itself, stopping `remove_death`
        // before earlier blocks even when the arg was inherited from entry.
        if (ContainsCallInExpr(entry.parsed.pattern) && i < blk.rtl_insns.size() &&
            blk.rtl_insns[i].has_explicit_call_fusage) {
          for (const RtlExpr& fu : blk.rtl_insns[i].call_fusage) {
            if (fu.op == RtlOp::kUse && !fu.children.empty() && fu.children[0].op == RtlOp::kReg &&
                !fu.children[0].IsArgPointerReg()) {
              const int arg_regno = fu.children[0].RegNo();
              if (IsMipsO32CallArgReg(arg_regno)) {
                entry.call_arg_dead_regnos.push_back(arg_regno);
              }
            }
          }
        }
        run_insns.push_back(std::move(entry));
      }
    }

    for (size_t idx = 0; idx < run_insns.size(); ++idx) {
      if (!ContainsCallInExpr(run_insns[idx].parsed.pattern)) {
        continue;
      }
      for (size_t prev = idx; prev > 0;) {
        --prev;
        const RtlExpr& prev_expr = run_insns[prev].parsed.pattern;
        if (ContainsCallInExpr(prev_expr) || IsAnyJumpOrReturnTopExpr(prev_expr)) {
          break;
        }
        if (IsNormalNonJumpNonCallInsn(prev_expr) && prev_expr.IsSet() &&
            prev_expr.SetDest().op == RtlOp::kReg) {
          const int dst_regno = prev_expr.SetDest().RegNo();
          if (!IsMipsO32CallArgReg(dst_regno)) {
            continue;
          }
          bool already_added = false;
          for (int existing : run_insns[idx].call_arg_dead_regnos) {
            if (existing == dst_regno) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            run_insns[idx].call_arg_dead_regnos.push_back(dst_regno);
          }
        }
      }
    }

    for (size_t idx = 0; idx < run_insns.size(); ++idx) {
      const RtlExpr& expr = run_insns[idx].parsed.pattern;
      if (!IsNormalNonJumpNonCallInsn(expr) || !expr.IsSet()) {
        continue;
      }
      const int sreg = TrueRegnum(expr.SetSrc());
      const int dreg = TrueRegnum(expr.SetDest());
      if (sreg < 0 || dreg < 0 || sreg == dreg) {
        continue;
      }
      bool saw_intervening_shield = run_insns[idx].preceded_by_loop_end_note;
      bool saw_intervening_call_clobber = false;
      bool saw_live_branch_read = false;
      bool blocked_for_mem_rmw = false;
      bool blocked_for_dying_read = false;
      bool blocked_for_earlyclobber_prod = false;
      std::optional<size_t> last_dying_read_idx;
      RtlExpr last_dying_read_reg;
      std::optional<size_t> earlyclobber_prod_idx;
      RtlExpr earlyclobber_prod_reg;
      std::optional<size_t> mem_rmw_idx;
      RtlExpr mem_rmw_read_reg;
      bool matched_any_dead = false;
      const bool is_di_gpr_reg_move =
          expr.SetDest().IsHardReg() && expr.SetSrc().IsHardReg() &&
          expr.SetDest().mode == RtlMachineMode::kDi && expr.SetSrc().mode == RtlMachineMode::kDi &&
          dreg >= 2 && dreg < 26 && sreg >= 2 && sreg < 26 && (dreg % 2) == 0 && (sreg % 2) == 0;
      for (size_t prev = idx; prev > 0;) {
        --prev;
        bool matched_call_arg = false;
        for (size_t k = 0; k < run_insns[prev].call_arg_dead_regnos.size(); ++k) {
          if (run_insns[prev].call_arg_dead_regnos[k] == dreg) {
            run_insns[prev].call_arg_dead_regnos.erase(
                run_insns[prev].call_arg_dead_regnos.begin() + static_cast<ptrdiff_t>(k));
            matched_call_arg = true;
            break;
          }
        }
        if (matched_call_arg) {
          matched_any_dead = true;
          break;
        }
        bool matched = false;
        // `jump.c:452-459` + `reorg.c:2580-2593`: when `run_insns[idx]` was
        // copied into `pred_b` from behind `join_b`'s `CODE_LABEL` during
        // un-cross-jumping, that label shielded any earlier surviving
        // `REG_DEAD dreg` from `remove_death` in `.11.jump2`, so prepend a
        // compensating `REG_DEAD` note on `trial` unless an intervening
        // `CALL_INSN` already clobbers `dreg` (call-used regs are already
        // killed at the call by `mark_target_live_regs`).
        const bool crossed_uncross_label =
            !saw_intervening_call_clobber && run_insns[idx].from_uncross_jump_tail &&
            (run_insns[prev].b != run_insns[idx].b || !run_insns[prev].from_uncross_jump_tail);
        for (size_t k = 0; k < run_insns[prev].original_dead_regnos.size(); ++k) {
          if (run_insns[prev].original_dead_regnos[k] == dreg) {
            if (saw_intervening_shield || crossed_uncross_label) {
              // Mirrors `flow.c:1670` + `jump.c:452-459`: when an intervening
              // same-block consumer `last_dying_read_idx` read `dreg` right
              // before `idx` overwrote `dreg` (with no intervening jump, call,
              // or def of `dreg`), `jump2` consumed `REG_DEAD dreg` from
              // `last_dying_read_idx` itself rather than from `prev`; restore
              // the single consumed note on `last_dying_read_idx` instead of
              // stacking duplicate `REG_DEAD dreg` notes onto `prev`.
              if (!crossed_uncross_label &&
                  (earlyclobber_prod_idx.has_value() || last_dying_read_idx.has_value())) {
                const size_t restore_idx = earlyclobber_prod_idx.has_value()
                                               ? *earlyclobber_prod_idx
                                               : *last_dying_read_idx;
                const RtlExpr& restore_reg =
                    earlyclobber_prod_idx.has_value() ? earlyclobber_prod_reg : last_dying_read_reg;
                auto& target = run_insns[restore_idx];
                const bool already_has_dead_or_mem_dead =
                    std::any_of(target.parsed.reg_notes.begin(), target.parsed.reg_notes.end(),
                                [dreg](const RtlRegNote& n) {
                                  return n.kind == RtlMachineMode::kRegDead &&
                                         (n.datum.op == RtlOp::kMem ||
                                          (n.datum.op == RtlOp::kReg && n.datum.RegNo() == dreg));
                                });
                if (!already_has_dead_or_mem_dead) {
                  RtlRegNote restored_note;
                  restored_note.kind = RtlMachineMode::kRegDead;
                  restored_note.list_op = RtlOp::kExprList;
                  restored_note.datum = restore_reg;
                  auto insert_pos = target.parsed.reg_notes.begin();
                  if (earlyclobber_prod_idx.has_value()) {
                    // `sched.c:2872-2906` (`attach_deaths_insn`): `REG_DEAD`
                    // notes follow left-to-right operand order ahead of
                    // `REG_UNUSED`. When `dreg` is the second operand of a
                    // binary earlyclobber op, place it after existing
                    // `REG_DEAD` notes (and before `REG_UNUSED`).
                    const RtlExpr& rhs = target.parsed.pattern.children[0].SetSrc();
                    const bool dreg_in_lhs = rhs.children[0].ContainsReg(dreg);
                    if (!dreg_in_lhs) {
                      while (insert_pos != target.parsed.reg_notes.end() &&
                             insert_pos->kind == RtlMachineMode::kRegDead) {
                        ++insert_pos;
                      }
                    }
                  }
                  target.parsed.reg_notes.insert(insert_pos, std::move(restored_note));
                  target.modified = true;
                }
              } else {
                run_insns[prev].parsed.reg_notes.insert(run_insns[prev].parsed.reg_notes.begin(),
                                                        run_insns[prev].original_dead_notes[k]);
                run_insns[prev].modified = true;
              }
            }
            matched = true;
            break;
          }
        }
        if (matched) {
          matched_any_dead = true;
          break;
        }
        const RtlExpr& ppat = run_insns[prev].parsed.pattern;
        if (!blocked_for_dying_read && !last_dying_read_idx.has_value()) {
          if (IsAnyJumpOrReturnTopExpr(ppat) || ContainsCallInExpr(ppat) ||
              run_insns[prev].b != run_insns[idx].b) {
            blocked_for_dying_read = true;
          } else if (IsNormalNonJumpNonCallInsn(ppat) && ppat.IsSet() &&
                     ppat.children.size() == 2 && TrueRegnum(ppat.SetDest()) != dreg &&
                     insn_reads_hard_reg(ppat, dreg)) {
            RtlExpr found_reg;
            bool found_exact = false;
            ppat.SetSrc().Visit([&](const RtlExpr& node) {
              if (!found_exact && node.op == RtlOp::kReg && !node.IsArgPointerReg() &&
                  node.RegNo() == dreg) {
                found_reg = node;
                found_exact = true;
              }
            });
            if (found_exact) {
              last_dying_read_idx = prev;
              last_dying_read_reg = std::move(found_reg);
            } else {
              blocked_for_dying_read = true;
            }
          } else if (ppat.IsSet() && TrueRegnum(ppat.SetDest()) == dreg) {
            blocked_for_dying_read = true;
          }
        }
        if (is_di_gpr_reg_move && !blocked_for_earlyclobber_prod &&
            !earlyclobber_prod_idx.has_value()) {
          if (IsAnyJumpOrReturnTopExpr(ppat) || ContainsCallInExpr(ppat) ||
              run_insns[prev].b != run_insns[idx].b) {
            blocked_for_earlyclobber_prod = true;
          } else if (last_dying_read_idx.has_value() && ppat.op == RtlOp::kParallel &&
                     ppat.children.size() == 2 && ppat.children[0].IsSet() &&
                     ppat.children[0].children.size() == 2 &&
                     ppat.children[1].op == RtlOp::kClobber &&
                     ppat.children[1].children.size() == 1 &&
                     ppat.children[0].SetDest().IsHardReg() &&
                     !ppat.children[0].SetDest().flags.integrated &&
                     ppat.children[0].SetDest().mode == RtlMachineMode::kDi &&
                     ppat.children[0].SetDest().RegNo() == sreg &&
                     ppat.children[1].children[0].IsHardReg()) {
            const int clob_hr = ppat.children[1].children[0].RegNo();
            const RtlExpr& rhs = ppat.children[0].SetSrc();
            const bool is_ec_op = rhs.children.size() == 2 &&
                                  (rhs.op == RtlOp::kPlus ||
                                   ((rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt ||
                                     rhs.op == RtlOp::kLshiftrt) &&
                                    rhs.children[1].IsHardReg()));
            if (is_ec_op && (clob_hr < dreg || clob_hr >= dreg + 2)) {
              RtlExpr found_reg;
              bool found_exact = false;
              rhs.Visit([&](const RtlExpr& node) {
                if (!found_exact && node.op == RtlOp::kReg && !node.IsArgPointerReg() &&
                    node.mode == RtlMachineMode::kDi && node.RegNo() == dreg) {
                  found_reg = node;
                  found_exact = true;
                }
              });
              const bool lhs_is_dreg = rhs.op == RtlOp::kPlus && rhs.children[0].IsHardReg() &&
                                       rhs.children[0].mode == RtlMachineMode::kDi &&
                                       rhs.children[0].RegNo() == dreg;
              if (found_exact && (rhs.op != RtlOp::kPlus || !lhs_is_dreg)) {
                earlyclobber_prod_idx = prev;
                earlyclobber_prod_reg = std::move(found_reg);
              } else {
                blocked_for_earlyclobber_prod = true;
              }
            } else {
              blocked_for_earlyclobber_prod = true;
            }
          } else if (ppat.IsSet() &&
                     (TrueRegnum(ppat.SetDest()) == dreg || TrueRegnum(ppat.SetDest()) == sreg)) {
            blocked_for_earlyclobber_prod = true;
          }
        }
        if (ContainsCallInExpr(ppat) &&
            (dreg < 16 || dreg == 24 || dreg == 25 || (dreg >= 32 && dreg < 52))) {
          saw_intervening_call_clobber = true;
        }
        if (!blocked_for_mem_rmw && !mem_rmw_idx.has_value()) {
          if (saw_live_branch_read && IsNormalNonJumpNonCallInsn(ppat) && ppat.IsSet() &&
              ppat.children.size() == 2 && ppat.SetDest().IsHardReg() &&
              ppat.SetDest().RegNo() == dreg && !ppat.SetDest().flags.integrated &&
              ppat.SetSrc().op == RtlOp::kMem) {
            bool found_any = false;
            RtlExpr found_read;
            ppat.SetSrc().Visit([&](const RtlExpr& node) {
              if (!found_any && node.IsHardReg() && node.RegNo() == dreg) {
                found_read = node;
                found_any = true;
              }
            });
            if (found_any) {
              mem_rmw_idx = prev;
              mem_rmw_read_reg = std::move(found_read);
              break;
            } else {
              blocked_for_mem_rmw = true;
            }
          } else if (ppat.IsSet() && ppat.SetDest().op == RtlOp::kPc &&
                     ppat.SetSrc().op == RtlOp::kIfThenElse && insn_reads_hard_reg(ppat, dreg)) {
            saw_live_branch_read = true;
          } else if (insn_reads_hard_reg(ppat, dreg) ||
                     (ppat.IsSet() && TrueRegnum(ppat.SetDest()) == dreg)) {
            blocked_for_mem_rmw = true;
          }
        }
        if (run_insns[prev].preceded_by_loop_end_note || insn_reads_hard_reg(ppat, dreg)) {
          saw_intervening_shield = true;
        }
      }
      // Mirrors `jump.c:452-459`: if no surviving `REG_DEAD dreg` remained in
      // the label-less run because `jump2` consumed the sole `REG_DEAD dreg` on
      // a pointer-load self-RMW `(set (reg:M dreg) (mem:M <addr reading dreg>))`
      // across a live conditional branch, restore that single `REG_DEAD` note.
      // Mirrors `reload1.c:5599-5665` (`emit_reload_insns`) + `jump.c:452-459`:
      // if `sched2` separated an earlyclobber `DImode` GPR `PARALLEL` from its
      // output-reload move `idx` with an intervening pure reader of `dreg`,
      // `prod` strictly followed that reader before `sched2` and carried the
      // `(REG_DEAD dreg)` consumed by `jump2`.
      if (!matched_any_dead && earlyclobber_prod_idx.has_value()) {
        auto& target = run_insns[*earlyclobber_prod_idx];
        const bool already_has_dead =
            std::any_of(target.parsed.reg_notes.begin(), target.parsed.reg_notes.end(),
                        [dreg](const RtlRegNote& n) {
                          return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                                 n.datum.RegNo() == dreg;
                        });
        if (!already_has_dead) {
          RtlRegNote restored_note;
          restored_note.kind = RtlMachineMode::kRegDead;
          restored_note.list_op = RtlOp::kExprList;
          restored_note.datum = earlyclobber_prod_reg;
          auto insert_pos = target.parsed.reg_notes.begin();
          const RtlExpr& rhs = target.parsed.pattern.children[0].SetSrc();
          if (!rhs.children[0].ContainsReg(dreg)) {
            while (insert_pos != target.parsed.reg_notes.end() &&
                   insert_pos->kind == RtlMachineMode::kRegDead) {
              ++insert_pos;
            }
          }
          target.parsed.reg_notes.insert(insert_pos, restored_note);
          target.original_dead_regnos.push_back(dreg);
          target.original_dead_notes.push_back(std::move(restored_note));
          target.modified = true;
        }
      } else if (!matched_any_dead && mem_rmw_idx.has_value()) {
        RtlRegNote restored_note;
        restored_note.kind = RtlMachineMode::kRegDead;
        restored_note.list_op = RtlOp::kExprList;
        restored_note.datum = mem_rmw_read_reg;
        auto& target = run_insns[*mem_rmw_idx];
        target.parsed.reg_notes.insert(target.parsed.reg_notes.begin(), restored_note);
        target.original_dead_regnos.push_back(dreg);
        target.original_dead_notes.push_back(std::move(restored_note));
        target.modified = true;
      }
    }

    for (const FlatInsn& entry : run_insns) {
      if (entry.modified) {
        cfg->rpo_blocks[entry.b].instructions[entry.i] = FormatInsnWithoutCallFusage(entry.parsed);
      }
    }

    run_start_b = run_end_b;
  }
}

std::bitset<68> MipsCallUsedOrGlobalRegsMask() {
  std::bitset<68> mask;
  for (int r = 1; r <= 15; ++r) mask.set(r);
  mask.set(24);
  mask.set(25);
  mask.set(28);
  mask.set(29);
  mask.set(31);
  for (int r = 32; r <= 51; ++r) mask.set(r);
  for (int r = 64; r <= 67; ++r) mask.set(r);
  return mask;
}

void MarkHardRegSpan(int regno, RtlMachineMode mode, bool gp64, bool fp64, std::bitset<68>* mask) {
  if (regno <= 0 || regno >= 68) {
    return;
  }
  const int nregs = MipsHardRegNregs(regno, mode, gp64, fp64);
  for (int k = 0; k < nregs && regno + k < 68; ++k) {
    mask->set(static_cast<size_t>(regno + k));
  }
}

void CollectReadRegsInSubtree(const RtlExpr& expr, bool gp64, bool fp64,
                              std::bitset<68>* read_regs) {
  expr.Visit([gp64, fp64, read_regs](const RtlExpr& node) {
    if (node.op == RtlOp::kReg) {
      MarkHardRegSpan(node.RegNo(), node.mode, gp64, fp64, read_regs);
    }
  });
}

struct InsnSchedSummary {
  bool valid = false;
  bool is_call = false;
  bool touches_mem = false;
  bool is_single_cycle_int_alu = false;
  std::bitset<68> read_regs;
  std::bitset<68> written_regs;
};

InsnSchedSummary SummarizeInsnForSched2(std::string_view pat, bool gp64, bool fp64) {
  InsnSchedSummary sum;
  auto parsed = ParsePatternAndNotes(pat);
  if (!parsed.ok()) {
    return sum;
  }
  const RtlExpr& expr = parsed->pattern;
  if (expr.op == RtlOp::kNote || expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec ||
      expr.op == RtlOp::kSequence) {
    return sum;
  }
  sum.valid = true;
  sum.is_call = ContainsCallInExpr(expr);
  bool has_non_single_cycle_op = false;
  bool has_non_gpr_reg = false;
  expr.Visit([&](const RtlExpr& node) {
    switch (node.op) {
      case RtlOp::kMem:
      case RtlOp::kUnalignedLoad:
      case RtlOp::kUnalignedStore:
        sum.touches_mem = true;
        break;
      case RtlOp::kMult:
      case RtlOp::kDiv:
      case RtlOp::kUdiv:
      case RtlOp::kMod:
      case RtlOp::kUmod:
      case RtlOp::kSqrt:
      case RtlOp::kFloat:
      case RtlOp::kUnsignedFloat:
      case RtlOp::kFix:
      case RtlOp::kUnsignedFix:
      case RtlOp::kFloatExtend:
      case RtlOp::kFloatTruncate:
      case RtlOp::kUnspecVolatile:
      case RtlOp::kAsmInput:
      case RtlOp::kAsmOperands:
        has_non_single_cycle_op = true;
        break;
      case RtlOp::kReg:
        if (node.RegNo() < 0 || node.RegNo() > 31) {
          has_non_gpr_reg = true;
        }
        break;
      default:
        break;
    }
  });

  auto record_element = [&](const RtlExpr& elem) {
    if (elem.IsSet()) {
      const RtlExpr& dst = elem.SetDest();
      const int dreg = TrueRegnum(dst);
      if ((dst.op == RtlOp::kReg || dst.op == RtlOp::kSubreg) && dreg >= 0) {
        MarkHardRegSpan(dreg, dst.mode, gp64, fp64, &sum.written_regs);
      } else {
        CollectReadRegsInSubtree(dst, gp64, fp64, &sum.read_regs);
      }
      CollectReadRegsInSubtree(elem.SetSrc(), gp64, fp64, &sum.read_regs);
    } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
      const RtlExpr& dst = elem.children[0];
      const int dreg = TrueRegnum(dst);
      if ((dst.op == RtlOp::kReg || dst.op == RtlOp::kSubreg) && dreg >= 0) {
        MarkHardRegSpan(dreg, dst.mode, gp64, fp64, &sum.written_regs);
      } else {
        CollectReadRegsInSubtree(dst, gp64, fp64, &sum.read_regs);
      }
    } else {
      CollectReadRegsInSubtree(elem, gp64, fp64, &sum.read_regs);
    }
  };

  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& child : expr.children) {
      record_element(child);
    }
  } else {
    record_element(expr);
  }
  for (const RtlRegNote& note : parsed->reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
      MarkHardRegSpan(note.datum.RegNo(), note.datum.mode, gp64, fp64, &sum.read_regs);
    }
  }
  if (sum.is_call) {
    sum.written_regs |= MipsCallUsedOrGlobalRegsMask();
  }
  sum.is_single_cycle_int_alu = !sum.is_call && !sum.touches_mem && !has_non_single_cycle_op &&
                                !has_non_gpr_reg && !IsEpilogueOrControlTopExpr(expr);
  return sum;
}

bool HasSched2Dependence(const InsnSchedSummary& earlier, const InsnSchedSummary& later) {
  if (!earlier.valid || !later.valid) {
    return false;
  }
  if (later.is_call) {
    if (earlier.is_call || earlier.touches_mem) {
      return true;
    }
    const std::bitset<68> call_mask = MipsCallUsedOrGlobalRegsMask();
    return ((earlier.read_regs | earlier.written_regs) & call_mask).any();
  }
  return (earlier.written_regs & (later.read_regs | later.written_regs)).any() ||
         (earlier.read_regs & later.written_regs).any();
}

int LookupCallerAllocOrderIndex(int hr, bool is_fpr_class) {
  static constexpr std::array<int, 16> kCallerGprOrder = {2,  3,  4,  5,  6,  7,  8,  9,
                                                          10, 11, 12, 13, 14, 15, 24, 25};
  static constexpr std::array<int, 20> kCallerFprOrder = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                                                          42, 43, 44, 45, 46, 47, 48, 49, 50, 51};
  if (!is_fpr_class) {
    auto it = std::find(kCallerGprOrder.begin(), kCallerGprOrder.end(), hr);
    return (it != kCallerGprOrder.end()) ? static_cast<int>(it - kCallerGprOrder.begin()) : -1;
  }
  auto it = std::find(kCallerFprOrder.begin(), kCallerFprOrder.end(), hr);
  return (it != kCallerFprOrder.end()) ? static_cast<int>(it - kCallerFprOrder.begin()) : -1;
}

bool IsFpArithPattern(const RtlExpr& pat) {
  if (!pat.IsSet() || !pat.SetDest().IsHardReg()) {
    return false;
  }
  const int dst_r = pat.SetDest().RegNo();
  if (dst_r < 32 || dst_r >= 64) {
    return false;
  }
  const RtlOp op = pat.SetSrc().op;
  return op == RtlOp::kPlus || op == RtlOp::kMinus || op == RtlOp::kMult || op == RtlOp::kDiv ||
         op == RtlOp::kSqrt;
}

// `sched.c:2362-2407` + `jump.c:1978` (`find_cross_jump`): when a join block
// begins with a cross-jumped prefix ending in a multi-cycle FP arithmetic
// consumer `shared_insns[fp_idx]` killing a caller-saved FPR `dead_fpr`
// produced by a multi-cycle FP op in every predecessor, and at least one
// predecessor scheduled that producer ahead of independent trailing
// instructions (requiring consumer `priority > 1` during `sched2`), return
// `fp_idx + 1` so `sched2` sees the consumer in every predecessor.
size_t SelectSched2FpConsumerCrossJumpPrefixLen(
    const std::vector<size_t>& pred_indices, const RtlControlFlowGraph& cfg,
    const std::vector<std::string>& shared_insns, const std::vector<InsnSchedSummary>& shared_sum,
    const std::vector<std::bitset<68>>& pred_all_written_regs, bool gp64, bool fp64) {
  for (size_t fp_idx = 0; fp_idx < shared_insns.size(); ++fp_idx) {
    if (shared_sum[fp_idx].touches_mem || shared_sum[fp_idx].is_call ||
        shared_sum[fp_idx].is_single_cycle_int_alu) {
      continue;
    }
    auto fp_parsed = ParsePatternAndNotes(shared_insns[fp_idx]);
    if (!fp_parsed.ok() || !IsFpArithPattern(fp_parsed->pattern)) {
      continue;
    }
    bool prefix_ok = true;
    for (size_t k = 0; k < fp_idx; ++k) {
      if (shared_sum[k].touches_mem || shared_sum[k].is_call) {
        prefix_ok = false;
        break;
      }
      const bool feeds_fp = (shared_sum[k].written_regs & shared_sum[fp_idx].read_regs).any();
      bool feeds_from_all_preds_dead_gpr = false;
      if (shared_sum[k].is_single_cycle_int_alu) {
        auto pk = ParsePatternAndNotes(shared_insns[k]);
        if (pk.ok()) {
          for (const RtlRegNote& note : pk->reg_notes) {
            if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
              const int dg = note.datum.RegNo();
              if (dg >= 2 && dg < 32 && shared_sum[k].read_regs.test(static_cast<size_t>(dg))) {
                bool all_def = !pred_indices.empty();
                for (size_t p = 0; p < pred_indices.size(); ++p) {
                  if (!pred_all_written_regs[p].test(static_cast<size_t>(dg))) {
                    all_def = false;
                    break;
                  }
                }
                if (all_def) {
                  feeds_from_all_preds_dead_gpr = true;
                  break;
                }
              }
            }
          }
        }
      }
      if (!feeds_fp && !feeds_from_all_preds_dead_gpr) {
        prefix_ok = false;
        break;
      }
    }
    if (!prefix_ok) {
      continue;
    }
    for (const RtlRegNote& note : fp_parsed->reg_notes) {
      if (note.kind != RtlMachineMode::kRegDead || !note.datum.IsReg()) {
        continue;
      }
      const int dead_fpr = note.datum.RegNo();
      if (dead_fpr < 32 || dead_fpr >= 52 ||
          !shared_sum[fp_idx].read_regs.test(static_cast<size_t>(dead_fpr))) {
        continue;
      }
      bool written_in_prefix = false;
      for (size_t k = 0; k < fp_idx; ++k) {
        if (shared_sum[k].written_regs.test(static_cast<size_t>(dead_fpr))) {
          written_in_prefix = true;
          break;
        }
      }
      if (written_in_prefix) {
        continue;
      }
      bool all_preds_define_multicycle = !pred_indices.empty();
      bool any_pred_hoisted_witness = false;
      for (size_t p = 0; p < pred_indices.size() && all_preds_define_multicycle; ++p) {
        const auto& pred_blk = cfg.rpo_blocks[pred_indices[p]];
        std::optional<size_t> def_pos;
        InsnSchedSummary def_sum;
        std::vector<InsnSchedSummary> active_sums;
        for (const std::string& pat : pred_blk.instructions) {
          auto pp = ParsePatternAndNotes(pat);
          if (!pp.ok() || pp->pattern.op == RtlOp::kNote ||
              IsEpilogueOrControlTopExpr(pp->pattern)) {
            continue;
          }
          const InsnSchedSummary ps = SummarizeInsnForSched2(pat, gp64, fp64);
          if (!ps.valid) {
            continue;
          }
          const size_t cur_idx = active_sums.size();
          active_sums.push_back(ps);
          if (ps.written_regs.test(static_cast<size_t>(dead_fpr))) {
            if (!ps.touches_mem && !ps.is_call && !ps.is_single_cycle_int_alu) {
              def_pos = cur_idx;
              def_sum = ps;
            } else {
              def_pos.reset();
            }
          } else if (HasRegDeadForRegno(*pp, dead_fpr)) {
            def_pos.reset();
          }
        }
        if (!def_pos.has_value()) {
          all_preds_define_multicycle = false;
          break;
        }
        for (size_t u = *def_pos + 1; u < active_sums.size(); ++u) {
          if (active_sums[u].is_single_cycle_int_alu &&
              !HasSched2Dependence(def_sum, active_sums[u])) {
            any_pred_hoisted_witness = true;
            break;
          }
        }
      }
      if (all_preds_define_multicycle && any_pred_hoisted_witness) {
        return fp_idx + 1;
      }
    }
  }
  return 0;
}

// Per `sched.c:2362-2407`, once a cross-jumped FP consumer is restored at the
// tail of `pred_blk` (starting at `uncross_start`), any pre-existing multi-cycle
// FP arithmetic chain in `pred_blk` feeding that consumer regains priority > 1.
// Sink any 1-cycle integer ALU instruction that Stage 1 (`StepIntoSched2LatencyShadow`
// or `InvertFillEagerDelaySlots`) placed before such an independent FP arithmetic
// span back down to immediately before its consumer (or `uncross_start`).
void SinkPreExistingGprAluPastFpArithInUncrossedPred(RtlBasicBlock* pred_blk, size_t uncross_start,
                                                     bool gp64, bool fp64) {
  if (uncross_start < 2 || uncross_start >= pred_blk->instructions.size()) {
    return;
  }
  for (size_t k = uncross_start; k > 0; --k) {
    const size_t idx = k - 1;
    const InsnSchedSummary sk = SummarizeInsnForSched2(pred_blk->instructions[idx], gp64, fp64);
    if (!sk.valid || !sk.is_single_cycle_int_alu || sk.touches_mem || sk.is_call ||
        sk.written_regs.none()) {
      continue;
    }
    size_t scan = idx + 1;
    bool saw_fp_arith = false;
    while (scan < uncross_start) {
      auto ps = ParsePatternAndNotes(pred_blk->instructions[scan]);
      const InsnSchedSummary ss = SummarizeInsnForSched2(pred_blk->instructions[scan], gp64, fp64);
      if (!ps.ok() || !ss.valid || ss.touches_mem || ss.is_call || HasSched2Dependence(sk, ss)) {
        break;
      }
      // Only step past independent pure FP instructions (no GPR writes).
      bool writes_gpr = false;
      for (int r = 1; r < 32; ++r) {
        if (ss.written_regs.test(static_cast<size_t>(r))) {
          writes_gpr = true;
          break;
        }
      }
      if (writes_gpr) {
        break;
      }
      if (IsFpArithPattern(ps->pattern)) {
        saw_fp_arith = true;
      }
      ++scan;
    }
    if (!saw_fp_arith || scan <= idx + 1) {
      continue;
    }
    // Only relocate instructions that Stage 1 could have placed prematurely
    // ahead of the FP span: either `idx == 0` (restored at block head by
    // `InvertFillEagerDelaySlots`) or `scan == uncross_start` (unpacked into
    // the latency shadow by `StepIntoSched2LatencyShadow` before its consumer
    // at `uncross_start` was un-cross-jumped back into `pred_blk`).
    if (idx > 0 && scan < uncross_start) {
      continue;
    }
    // Verify the instruction at `scan` (or `uncross_start`) actually consumes `sk`.
    bool consumed_at_or_after = false;
    for (size_t c = scan; c < pred_blk->instructions.size(); ++c) {
      const InsnSchedSummary sc = SummarizeInsnForSched2(pred_blk->instructions[c], gp64, fp64);
      if (sc.valid && (sk.written_regs & sc.read_regs).any()) {
        consumed_at_or_after = true;
        break;
      }
    }
    if (!consumed_at_or_after) {
      continue;
    }
    std::string moved_str = std::move(pred_blk->instructions[idx]);
    RtlInsn moved_rtl = std::move(pred_blk->rtl_insns[idx]);
    pred_blk->instructions.erase(pred_blk->instructions.begin() + static_cast<ptrdiff_t>(idx));
    pred_blk->rtl_insns.erase(pred_blk->rtl_insns.begin() + static_cast<ptrdiff_t>(idx));
    pred_blk->instructions.insert(pred_blk->instructions.begin() + static_cast<ptrdiff_t>(scan - 1),
                                  std::move(moved_str));
    pred_blk->rtl_insns.insert(pred_blk->rtl_insns.begin() + static_cast<ptrdiff_t>(scan - 1),
                               std::move(moved_rtl));
  }
}

size_t SelectSched2SafeSharedPrefixLen(const std::vector<size_t>& pred_indices,
                                       const RtlControlFlowGraph& cfg,
                                       const std::vector<std::string>& shared_insns,
                                       const std::vector<std::string>& epilogue_insns, bool gp64,
                                       bool fp64, bool runs_sched2,
                                       bool* out_fp_consumer_prefix = nullptr) {
  if (shared_insns.empty() || epilogue_insns.empty()) {
    return 0;
  }
  const std::string& first_epilogue_insn = epilogue_insns.front();
  std::vector<InsnSchedSummary> shared_sum;
  shared_sum.reserve(shared_insns.size());
  std::optional<size_t> last_call_idx;
  for (size_t k = 0; k < shared_insns.size(); ++k) {
    InsnSchedSummary s = SummarizeInsnForSched2(shared_insns[k], gp64, fp64);
    if (!s.valid) {
      return 0;
    }
    if (s.is_call) {
      last_call_idx = k;
    }
    shared_sum.push_back(s);
  }

  std::vector<InsnSchedSummary> pred_last_active;
  std::vector<bool> pred_all_single_cycle;
  std::vector<std::bitset<68>> pred_mem_load_written_regs;
  std::vector<std::bitset<68>> pred_all_written_regs;
  pred_last_active.reserve(pred_indices.size());
  pred_all_single_cycle.reserve(pred_indices.size());
  pred_mem_load_written_regs.reserve(pred_indices.size());
  pred_all_written_regs.reserve(pred_indices.size());
  for (size_t pred_b : pred_indices) {
    const auto& pred_blk = cfg.rpo_blocks[pred_b];
    bool all_sc = true;
    std::optional<InsnSchedSummary> last_act;
    std::bitset<68> mem_load_defs;
    std::bitset<68> all_defs;
    for (const std::string& pat : pred_blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok() || parsed->pattern.op == RtlOp::kNote ||
          IsEpilogueOrControlTopExpr(parsed->pattern)) {
        continue;
      }
      InsnSchedSummary s = SummarizeInsnForSched2(pat, gp64, fp64);
      if (!s.valid) {
        return 0;
      }
      if (!s.is_single_cycle_int_alu) {
        all_sc = false;
      }
      if (s.touches_mem && !s.is_call && s.written_regs.any()) {
        mem_load_defs |= s.written_regs;
      }
      all_defs |= s.written_regs;
      last_act = s;
    }
    if (!last_act.has_value()) {
      return 0;
    }
    pred_last_active.push_back(*last_act);
    pred_all_single_cycle.push_back(all_sc);
    pred_mem_load_written_regs.push_back(mem_load_defs);
    pred_all_written_regs.push_back(all_defs);
  }

  if (last_call_idx.has_value()) {
    const size_t c_idx = *last_call_idx;
    // `jump.c:1978` (`find_cross_jump`): when `c_idx > 0` and no predecessor
    // supplies any live MIPS ABI call-argument register (`$4..$7`, `$f12..$f15`)
    // that reaches the call without being overwritten by `shared_insns[0..c_idx-1]`,
    // require every predecessor to directly feed `shared_insns[0]`; otherwise
    // the call and its setup are simply the head of the post-join continuation
    // block rather than a cross-jumped branch tail.
    if (c_idx > 0 && !runs_sched2) {
      std::bitset<68> call_arg_mask;
      for (int r : {4, 5, 6, 7, 44, 45, 46, 47}) {
        call_arg_mask.set(static_cast<size_t>(r));
      }
      std::bitset<68> shared_written;
      for (size_t k = 0; k < c_idx; ++k) {
        shared_written |= shared_sum[k].written_regs;
      }
      bool any_pred_supplies_call_arg = false;
      for (size_t p = 0; p < pred_indices.size(); ++p) {
        if ((pred_all_written_regs[p] & call_arg_mask & ~shared_written).any()) {
          any_pred_supplies_call_arg = true;
          break;
        }
      }
      if (!any_pred_supplies_call_arg) {
        bool all_preds_feed_first = true;
        for (size_t p = 0; p < pred_indices.size(); ++p) {
          if ((pred_all_written_regs[p] & shared_sum[0].read_regs).none()) {
            all_preds_feed_first = false;
            break;
          }
        }
        if (!all_preds_feed_first) {
          return 0;
        }
      }
    }
    for (size_t k = 0; k < c_idx; ++k) {
      if (shared_sum[k].touches_mem || !HasSched2Dependence(shared_sum[k], shared_sum[c_idx])) {
        return 0;
      }
      // `sched.c:2362-2407` (`priority` / `rank_for_schedule`): when `sched2`
      // runs, a multi-cycle pre-call op (`!is_single_cycle_int_alu`, e.g. FP
      // `mtc1`/`add.s`) independent of a multi-cycle predecessor's definitions
      // would be hoisted/interleaved ahead of `pred_b`'s body by `sched2`
      // rather than remaining contiguous at the tail of `pred_b`.
      if (runs_sched2 && !shared_sum[k].is_single_cycle_int_alu) {
        for (size_t p = 0; p < pred_indices.size(); ++p) {
          if (!pred_all_single_cycle[p] &&
              (pred_all_written_regs[p] & shared_sum[k].read_regs).none() &&
              (pred_all_written_regs[p] & shared_sum[k].written_regs).none()) {
            return 0;
          }
        }
      }
    }
    for (size_t p = 0; p < pred_last_active.size(); ++p) {
      if (!HasSched2Dependence(pred_last_active[p], shared_sum[c_idx]) &&
          !pred_all_single_cycle[p]) {
        return 0;
      }
    }
    size_t len = c_idx + 1;
    while (len < shared_sum.size() && shared_sum[len].is_single_cycle_int_alu &&
           HasSched2Dependence(shared_sum[len - 1], shared_sum[len])) {
      ++len;
    }
    return len;
  }

  // Shared tail of memory stores followed by a 1-cycle integer ALU instruction
  // feeding `first_epilogue_insn`: if `join_b` had existed before `sched2`,
  // `sched2` would have hoisted the higher-priority ALU instruction ahead of
  // `shared_sum[0]`. When every predecessor loads the ALU operand from memory
  // (pinning the ALU instruction behind the stores via multi-cycle load
  // latency) and defines a register read by `shared_sum[0]`, the entire
  // `shared_sum` was un-merged before `jump2`.
  if (shared_sum.size() >= 2 && shared_sum[0].touches_mem && shared_sum[0].written_regs.none()) {
    size_t alu_idx = 0;
    while (alu_idx < shared_sum.size() && shared_sum[alu_idx].touches_mem &&
           shared_sum[alu_idx].written_regs.none()) {
      ++alu_idx;
    }
    const InsnSchedSummary epi_sum = SummarizeInsnForSched2(first_epilogue_insn, gp64, fp64);
    if (alu_idx >= 1 && alu_idx + 1 == shared_sum.size() &&
        shared_sum[alu_idx].is_single_cycle_int_alu && epi_sum.valid &&
        HasSched2Dependence(shared_sum[alu_idx], epi_sum)) {
      bool stores_independent_of_alu = true;
      for (size_t k = 0; k < alu_idx; ++k) {
        if (HasSched2Dependence(shared_sum[k], shared_sum[alu_idx])) {
          stores_independent_of_alu = false;
          break;
        }
      }
      bool all_preds_pin_stores_and_alu = stores_independent_of_alu;
      for (size_t p = 0; p < pred_indices.size() && all_preds_pin_stores_and_alu; ++p) {
        if ((pred_mem_load_written_regs[p] & shared_sum[alu_idx].read_regs).none() ||
            (pred_all_written_regs[p] & shared_sum[0].read_regs).none()) {
          all_preds_pin_stores_and_alu = false;
        }
      }
      if (all_preds_pin_stores_and_alu) {
        return shared_sum.size();
      }
    }
  }

  // At `-O1` (`!runs_sched2`), `sched2` does not run after `reload`, so a single
  // memory store consuming a dying GPR defined by every predecessor's trailing
  // 1-cycle ALU instruction was merged by `jump2` without `sched2` interference.
  if (!runs_sched2 && shared_sum[0].touches_mem && !shared_sum[0].is_call &&
      shared_sum[0].written_regs.none()) {
    auto s0_parsed = ParsePatternAndNotes(shared_insns[0]);
    bool has_vol_mem = false;
    if (s0_parsed.ok()) {
      s0_parsed->pattern.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kMem && n.flags.volatil) {
          has_vol_mem = true;
        }
      });
    }
    if (s0_parsed.ok() && !has_vol_mem) {
      for (const RtlRegNote& note : s0_parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
          const int dead_r = note.datum.RegNo();
          if (dead_r >= 2 && dead_r < 32 &&
              shared_sum[0].read_regs.test(static_cast<size_t>(dead_r))) {
            bool all_preds_define_dead_r = !pred_last_active.empty();
            for (const InsnSchedSummary& last_act : pred_last_active) {
              if (!last_act.is_single_cycle_int_alu ||
                  !last_act.written_regs.test(static_cast<size_t>(dead_r))) {
                all_preds_define_dead_r = false;
                break;
              }
            }
            if (all_preds_define_dead_r) {
              return 1;
            }
          }
        }
      }
    }
  }

  if (runs_sched2) {
    const size_t fp_prefix = SelectSched2FpConsumerCrossJumpPrefixLen(
        pred_indices, cfg, shared_insns, shared_sum, pred_all_written_regs, gp64, fp64);
    if (fp_prefix > 0) {
      if (out_fp_consumer_prefix != nullptr) {
        *out_fp_consumer_prefix = true;
      }
      return fp_prefix;
    }
  }

  if (!shared_sum[0].is_single_cycle_int_alu) {
    return 0;
  }

  // When `shared_insns[0]` is a 1-cycle integer ALU instruction killing a
  // caller-saved GPR `dead_r` that every predecessor defines and at least one
  // predecessor defines via a `fix` `PARALLEL` whose GPR scratch clobber has a
  // higher `REG_ALLOC_ORDER` index than `dead_r`, `dead_r` was a single-block
  // local pseudo inside that predecessor during `local_alloc` and `shared_insns[0]`
  // was merged into `join_b` by `jump2`.
  if (auto s0_parsed = ParsePatternAndNotes(shared_insns[0]); s0_parsed.ok()) {
    for (const RtlRegNote& note : s0_parsed->reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
        const int dead_r = note.datum.RegNo();
        const int dead_ord = LookupCallerAllocOrderIndex(dead_r, /*is_fpr_class=*/false);
        if (dead_r >= 2 && dead_r <= 25 && dead_ord >= 0 &&
            shared_sum[0].read_regs.test(static_cast<size_t>(dead_r))) {
          bool all_preds_define = !pred_indices.empty();
          bool any_pred_fix_scratch_witness = false;
          for (size_t p = 0; p < pred_indices.size() && all_preds_define; ++p) {
            if (!pred_all_written_regs[p].test(static_cast<size_t>(dead_r))) {
              all_preds_define = false;
              break;
            }
            const auto& pred_blk = cfg.rpo_blocks[pred_indices[p]];
            bool live_from_fix = false;
            bool live_from_any_def = false;
            for (const std::string& pat : pred_blk.instructions) {
              auto pp = ParsePatternAndNotes(pat);
              if (!pp.ok() || pp->pattern.op == RtlOp::kNote ||
                  IsEpilogueOrControlTopExpr(pp->pattern)) {
                continue;
              }
              const InsnSchedSummary ps = SummarizeInsnForSched2(pat, gp64, fp64);
              if (!ps.valid) {
                continue;
              }
              if (ps.written_regs.test(static_cast<size_t>(dead_r))) {
                live_from_any_def = true;
                live_from_fix = false;
                if (pp->pattern.op == RtlOp::kParallel && !pp->pattern.children.empty() &&
                    pp->pattern.children[0].IsSet() &&
                    pp->pattern.children[0].SetDest().IsHardReg() &&
                    pp->pattern.children[0].SetDest().RegNo() == dead_r &&
                    pp->pattern.children[0].SetSrc().op == RtlOp::kFix) {
                  for (size_t ci = 1; ci < pp->pattern.children.size(); ++ci) {
                    const RtlExpr& ch = pp->pattern.children[ci];
                    if (ch.op == RtlOp::kClobber && !ch.children.empty() &&
                        ch.children[0].IsHardReg() && ch.children[0].mode == RtlMachineMode::kSi) {
                      const int scr_ord = LookupCallerAllocOrderIndex(ch.children[0].RegNo(),
                                                                      /*is_fpr_class=*/false);
                      if (scr_ord > dead_ord) {
                        live_from_fix = true;
                      }
                    }
                  }
                }
              } else if (HasRegDeadForRegno(*pp, dead_r)) {
                live_from_any_def = false;
                live_from_fix = false;
              }
            }
            if (!live_from_any_def) {
              all_preds_define = false;
            } else if (live_from_fix) {
              any_pred_fix_scratch_witness = true;
            }
          }
          if (all_preds_define && any_pred_fix_scratch_witness) {
            return 1;
          }
        }
      }
    }
  }

  for (const InsnSchedSummary& last_act : pred_last_active) {
    if (!last_act.is_single_cycle_int_alu || !HasSched2Dependence(last_act, shared_sum[0])) {
      return 0;
    }
  }
  size_t len = 1;
  while (len < shared_sum.size() && shared_sum[len].is_single_cycle_int_alu &&
         HasSched2Dependence(shared_sum[len - 1], shared_sum[len])) {
    ++len;
  }
  while (len > 0) {
    std::bitset<68> prefix_written;
    for (size_t k = 0; k < len; ++k) {
      prefix_written |= shared_sum[k].written_regs;
    }
    auto first_rem_ref_is_nondead_rmw = [&](int r) -> bool {
      auto check_insn = [&](const std::string& pat) -> int {
        auto parsed = ParsePatternAndNotes(pat);
        if (!parsed.ok() || parsed->pattern.op == RtlOp::kNote ||
            IsEpilogueOrControlTopExpr(parsed->pattern)) {
          return 0;
        }
        const InsnSchedSummary s = SummarizeInsnForSched2(pat, gp64, fp64);
        if (!s.valid) {
          return 0;
        }
        const bool reads_r = s.read_regs.test(static_cast<size_t>(r));
        const bool writes_r = s.written_regs.test(static_cast<size_t>(r));
        if (!reads_r && !writes_r) {
          return 0;
        }
        const bool is_integrated_dst = parsed->pattern.IsSet() &&
                                       parsed->pattern.SetDest().IsReg() &&
                                       parsed->pattern.SetDest().flags.integrated;
        if (reads_r && writes_r && !is_integrated_dst && !HasRegDeadForRegno(*parsed, r)) {
          return 1;
        }
        return -1;
      };
      for (size_t k = len; k < shared_insns.size(); ++k) {
        const int res = check_insn(shared_insns[k]);
        if (res != 0) {
          return res > 0;
        }
      }
      for (const std::string& pat : epilogue_insns) {
        const int res = check_insn(pat);
        if (res != 0) {
          return res > 0;
        }
      }
      return false;
    };
    bool conflicts_with_local_rmw = false;
    for (int r = 2; r <= 30; ++r) {
      if ((r > 25 && (r != 30 || cfg.sideband.frame_pointer_needed)) ||
          !prefix_written.test(static_cast<size_t>(r))) {
        continue;
      }
      if (first_rem_ref_is_nondead_rmw(r)) {
        conflicts_with_local_rmw = true;
        break;
      }
    }
    if (!conflicts_with_local_rmw) {
      break;
    }
    --len;
  }
  return len;
}

bool IsMipsCalleeSavedGpr(int regno) {
  return (regno >= 16 && regno <= 23) || regno == 30;
}

// Reverses `jump.c:jump_optimize` (`noop_moves = 1`, lines 434-465) deletion of
// `(set (reg:M arg_reg) (reg:M saved_reg))` before the first `CALL_INSN` in the
// entry label-less fallthrough run when `saved_reg` (`$16..$23` or `$30`) still
// holds incoming `arg_reg` (`$4..$7`) from an earlier
// `(set (reg:M saved_reg) (reg:M arg_reg))`. Per `reload.c:find_equiv_reg`
// (lines 5413-5590) and `jump.c:452-463`, both equivalence discovery and
// `remove_death(dreg, trial)` scan backward across fallthrough conditional
// branches, stopping only at `CODE_LABEL`, `CALL_INSN`, or register clobber.
void RestoreDeletedNoopCallArgMoves(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                    bool runs_sched2) {
  if (cfg->rpo_blocks.empty()) {
    return;
  }

  struct FlatInsn {
    size_t b = 0;
    size_t i = 0;
    RtlPatternAndNotes parsed;
    InsnSchedSummary sum;
    bool is_true_prologue_save = false;
  };
  struct InterveningBranch {
    size_t flat_idx = 0;
    std::vector<size_t> taken_targets;
  };

  std::vector<FlatInsn> prefix;
  std::vector<InterveningBranch> intervening_branches;
  std::optional<size_t> first_call_flat;
  size_t call_b = 0;
  size_t call_i = 0;
  std::optional<size_t> ra_save_flat;
  int64_t min_prologue_save_off = std::numeric_limits<int64_t>::max();
  absl::flat_hash_map<int, size_t> gpr_save_flat;
  std::bitset<68> written_so_far;

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    if (b > 0) {
      if (cfg->rpo_blocks[b].has_label) {
        break;
      }
      const auto& prev_succs = cfg->rpo_blocks[b - 1].successors;
      if (std::find(prev_succs.begin(), prev_succs.end(), b) == prev_succs.end()) {
        break;
      }
    }
    const RtlBasicBlock& blk = cfg->rpo_blocks[b];
    bool fell_through_cond_jump = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return;
      }
      InsnSchedSummary s = SummarizeInsnForSched2(blk.instructions[i], gp64, fp64);
      bool is_prologue_save = false;
      if (!first_call_flat.has_value()) {
        if (IsAnyJumpOrReturnTopExpr(parsed->pattern)) {
          const bool is_cond_jump = parsed->pattern.IsSet() &&
                                    parsed->pattern.SetDest().op == RtlOp::kPc &&
                                    parsed->pattern.SetSrc().op == RtlOp::kIfThenElse;
          if (!is_cond_jump) {
            return;
          }
          InterveningBranch br;
          br.flat_idx = prefix.size();
          for (size_t succ : blk.successors) {
            if (succ != b + 1) {
              br.taken_targets.push_back(succ);
            }
          }
          intervening_branches.push_back(std::move(br));
          written_so_far |= s.written_regs;
          prefix.push_back(FlatInsn{b, i, std::move(*parsed), s, false});
          fell_through_cond_jump = true;
          break;
        }
        if (ContainsCallInExpr(parsed->pattern)) {
          first_call_flat = prefix.size();
          call_b = b;
          call_i = i;
        } else {
          if (b == 0) {
            int saved_reg = -1;
            int64_t sp_off = 0;
            if (IsPrologueStackRegSave(parsed->pattern, &saved_reg, &sp_off)) {
              const bool is_saved_reg = saved_reg == 31 || IsMipsCalleeSavedGpr(saved_reg) ||
                                        (saved_reg >= 52 && saved_reg < 64);
              if (is_saved_reg &&
                  (saved_reg == 31 || !written_so_far.test(static_cast<size_t>(saved_reg)))) {
                is_prologue_save = true;
                min_prologue_save_off = std::min(min_prologue_save_off, sp_off);
                if (saved_reg == 31 && !ra_save_flat.has_value()) {
                  ra_save_flat = prefix.size();
                } else if (IsMipsCalleeSavedGpr(saved_reg) || (saved_reg >= 52 && saved_reg < 64)) {
                  const int save_span =
                      MipsHardRegNregs(saved_reg, parsed->pattern.SetSrc().mode, gp64, fp64);
                  for (int off = 0; off < save_span; ++off) {
                    if (!gpr_save_flat.contains(saved_reg + off)) {
                      gpr_save_flat[saved_reg + off] = prefix.size();
                    }
                  }
                }
              }
            }
          }
          written_so_far |= s.written_regs;
        }
      }
      prefix.push_back(FlatInsn{b, i, std::move(*parsed), s, is_prologue_save});
    }
    if (first_call_flat.has_value()) {
      break;
    }
    if (!fell_through_cond_jump) {
      // Straight fallthrough into a label-less block without an intervening
      // jump still stays inside the label-less run; if there is no next block
      // we exit on loop termination.
      continue;
    }
  }

  if (!first_call_flat.has_value() || !ra_save_flat.has_value() || prefix.size() < 4) {
    return;
  }
  const size_t call_flat = *first_call_flat;

  std::bitset<68> read_after_first_call;
  for (size_t i = call_flat + 1; i < prefix.size(); ++i) {
    read_after_first_call |= prefix[i].sum.read_regs;
  }
  if (call_b == 0) {
    for (size_t b = 1; b < cfg->rpo_blocks.size(); ++b) {
      for (const std::string& pat : cfg->rpo_blocks[b].instructions) {
        InsnSchedSummary s = SummarizeInsnForSched2(pat, gp64, fp64);
        if (s.valid) {
          read_after_first_call |= s.read_regs;
        }
      }
    }
  } else {
    std::vector<bool> visited(cfg->rpo_blocks.size(), false);
    std::vector<size_t> worklist;
    for (size_t succ : cfg->rpo_blocks[call_b].successors) {
      if (succ < cfg->rpo_blocks.size() && !visited[succ]) {
        visited[succ] = true;
        worklist.push_back(succ);
      }
    }
    while (!worklist.empty()) {
      const size_t cur = worklist.back();
      worklist.pop_back();
      for (const std::string& pat : cfg->rpo_blocks[cur].instructions) {
        InsnSchedSummary s = SummarizeInsnForSched2(pat, gp64, fp64);
        if (s.valid) {
          read_after_first_call |= s.read_regs;
        }
      }
      for (size_t succ : cfg->rpo_blocks[cur].successors) {
        if (succ < cfg->rpo_blocks.size() && !visited[succ]) {
          visited[succ] = true;
          worklist.push_back(succ);
        }
      }
    }
  }

  std::bitset<8> arg_gpr_live_into_call;
  for (int r = 4; r <= 7; ++r) {
    std::optional<size_t> last_def;
    for (size_t i = 0; i < call_flat; ++i) {
      if (prefix[i].sum.written_regs.test(static_cast<size_t>(r))) {
        last_def = i;
      }
    }
    if (last_def.has_value()) {
      bool killed_before_call = false;
      for (size_t k = *last_def + 1; k < call_flat; ++k) {
        if (HasRegDeadForRegno(prefix[k].parsed, r)) {
          killed_before_call = true;
          break;
        }
      }
      if (!killed_before_call) {
        arg_gpr_live_into_call.set(static_cast<size_t>(r));
      }
    }
  }

  // Track call argument word slots (`$4..$7`) already claimed by floating-point
  // argument registers `$f12` (`44`) and `$f14` (`46`) per `mips.c:function_arg`
  // (`lines 2871-2902`): when `$f12` holds argument 0, `$4` (and `$5` for `!gp64`
  // `DFmode`) cannot also be passed as a GPR argument to the same call.
  const bool is_gp64 = gp64;
  auto fpr_live_into_call = [&](int regno, bool* out_is_df) -> bool {
    std::optional<size_t> last_def;
    for (size_t i = 0; i < call_flat; ++i) {
      if (prefix[i].sum.written_regs.test(static_cast<size_t>(regno))) {
        last_def = i;
      }
    }
    if (!last_def.has_value()) {
      return false;
    }
    for (size_t k = *last_def + 1; k < call_flat; ++k) {
      if (HasRegDeadForRegno(prefix[k].parsed, regno)) {
        return false;
      }
    }
    if (out_is_df != nullptr) {
      const RtlExpr& def_pat = prefix[*last_def].parsed.pattern;
      *out_is_df = def_pat.IsSet() && def_pat.SetDest().mode == RtlMachineMode::kDf;
    }
    return true;
  };
  std::bitset<8> arg_word_occupied_by_fpr;
  bool f12_is_df = false;
  if (fpr_live_into_call(44, &f12_is_df)) {
    arg_word_occupied_by_fpr.set(4);
    if (!is_gp64 && f12_is_df) {
      arg_word_occupied_by_fpr.set(5);
    }
    bool f14_is_df = false;
    if (fpr_live_into_call(46, &f14_is_df)) {
      if (is_gp64) {
        arg_word_occupied_by_fpr.set(5);
      } else if (f12_is_df) {
        arg_word_occupied_by_fpr.set(6);
        if (f14_is_df) {
          arg_word_occupied_by_fpr.set(7);
        }
      } else {
        arg_word_occupied_by_fpr.set(5);
        if (f14_is_df) {
          arg_word_occupied_by_fpr.set(6);
          arg_word_occupied_by_fpr.set(7);
        }
      }
    }
  }

  auto reg_live_at_block_start = [&](size_t target_b, int regno) -> bool {
    auto it = cfg->sideband.basic_block_live_at_start.find(static_cast<int>(target_b));
    if (it == cfg->sideband.basic_block_live_at_start.end()) {
      return false;
    }
    return std::find(it->second.begin(), it->second.end(), regno) != it->second.end();
  };

  struct CandidateCopy {
    size_t copy_flat = 0;
    size_t copy_b = 0;
    size_t copy_i = 0;
    int saved_reg = -1;
    int arg_reg = -1;
    int saved_span = 1;
    int arg_span = 1;
    RtlMachineMode mode = RtlMachineMode::kSi;
    bool saved_reg_read_before_call = false;
  };
  std::vector<CandidateCopy> candidates;
  std::bitset<8> arg_gpr_saved_in_callee_reg;
  auto is_callee_saved_reg = [](int r) -> bool {
    return IsMipsCalleeSavedGpr(r) || (r >= 52 && r < 64);
  };

  for (size_t i = 0; i < call_flat; ++i) {
    const RtlExpr& pat = prefix[i].parsed.pattern;
    if (!IsNormalNonJumpNonCallInsn(pat) || !pat.IsSet() || pat.SetDest().op != RtlOp::kReg ||
        pat.SetSrc().op != RtlOp::kReg) {
      continue;
    }
    const RtlMachineMode mode = pat.SetDest().mode;
    if (mode != pat.SetSrc().mode || (mode != RtlMachineMode::kSi && mode != RtlMachineMode::kSf &&
                                      mode != RtlMachineMode::kDi && mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int saved_reg = pat.SetDest().RegNo();
    const int arg_reg = pat.SetSrc().RegNo();
    const int saved_span = MipsHardRegNregs(saved_reg, mode, gp64, fp64);
    const int arg_span = MipsHardRegNregs(arg_reg, mode, gp64, fp64);
    if (!is_callee_saved_reg(saved_reg) || arg_reg < 4 || arg_reg + arg_span - 1 > 7) {
      continue;
    }
    if (saved_span == 2 && !is_callee_saved_reg(saved_reg + 1)) {
      continue;
    }

    if (HasRegDeadForRegno(prefix[i].parsed, arg_reg)) {
      continue;
    }

    bool valid = true;
    bool saved_reg_read_before_call = false;
    for (int off = 0; off < saved_span; ++off) {
      const size_t sr = static_cast<size_t>(saved_reg + off);
      if (!read_after_first_call.test(sr)) {
        valid = false;
      }
      // Per `jump.c:434-465` (`reload.c:find_equiv_reg`), reading `saved_reg`
      // before the call preserves equivalence `saved_reg == arg_reg`; only a
      // write to `saved_reg` invalidates the noop-move deletion.
      for (size_t k = i + 1; k < call_flat; ++k) {
        if (prefix[k].sum.written_regs.test(sr)) {
          valid = false;
        }
        if (prefix[k].sum.read_regs.test(sr)) {
          saved_reg_read_before_call = true;
        }
      }
    }
    for (int off = 0; off < arg_span; ++off) {
      const size_t ar = static_cast<size_t>(arg_reg + off);
      if (arg_word_occupied_by_fpr.test(ar)) {
        valid = false;
      }
      for (size_t k = 0; k < i; ++k) {
        if (prefix[k].sum.written_regs.test(ar)) {
          valid = false;
        }
      }
      for (size_t k = i + 1; k < call_flat; ++k) {
        if (prefix[k].sum.written_regs.test(ar) || prefix[k].sum.read_regs.test(ar)) {
          valid = false;
        }
      }
      for (const InterveningBranch& br : intervening_branches) {
        if (br.flat_idx > i && br.flat_idx < call_flat) {
          for (size_t target_b : br.taken_targets) {
            if (reg_live_at_block_start(target_b, arg_reg + off)) {
              valid = false;
            }
          }
        }
      }
    }
    if (!valid) {
      continue;
    }
    candidates.push_back(CandidateCopy{i, prefix[i].b, prefix[i].i, saved_reg, arg_reg, saved_span,
                                       arg_span, mode, saved_reg_read_before_call});
    for (int off = 0; off < arg_span; ++off) {
      arg_gpr_saved_in_callee_reg.set(static_cast<size_t>(arg_reg + off));
    }
  }

  if (candidates.empty()) {
    return;
  }

  RtlBasicBlock& call_blk = cfg->rpo_blocks[call_b];
  if (call_blk.rtl_insns.size() != call_blk.instructions.size()) {
    call_blk.SyncRtlInsnsFromInstructions();
  }

  bool ra_sank_into_indirect_target_load_shadow = false;
  if (runs_sched2 && call_b == 0) {
    int indirect_target_reg = -1;
    prefix[call_flat].parsed.pattern.Visit([&indirect_target_reg](const RtlExpr& n) {
      if (n.op == RtlOp::kCall && !n.children.empty() && n.children[0].op == RtlOp::kMem &&
          !n.children[0].children.empty() && n.children[0].children[0].op == RtlOp::kReg) {
        indirect_target_reg = n.children[0].children[0].RegNo();
      }
    });
    if (indirect_target_reg >= 1 && indirect_target_reg < 32 &&
        (indirect_target_reg < 4 || indirect_target_reg > 7)) {
      std::optional<size_t> target_def_idx;
      bool target_def_is_mem_load = false;
      for (size_t i = 0; i < call_flat; ++i) {
        if (prefix[i].sum.written_regs.test(static_cast<size_t>(indirect_target_reg))) {
          target_def_idx = i;
          const RtlExpr& dpat = prefix[i].parsed.pattern;
          bool has_mem = false;
          if (dpat.IsSet()) {
            dpat.SetSrc().Visit([&has_mem](const RtlExpr& n) {
              if (n.op == RtlOp::kMem) {
                has_mem = true;
              }
            });
          }
          target_def_is_mem_load = has_mem;
        }
      }
      if (target_def_idx.has_value() && target_def_is_mem_load && *target_def_idx < *ra_save_flat) {
        ra_sank_into_indirect_target_load_shadow = true;
      }
    }
  }

  std::vector<std::string> restored_moves;
  std::vector<std::string> restored_fpr_moves;
  std::vector<std::string> post_ra_stalled_moves;
  bool any_restored_fpr_src = false;
  size_t max_cand_copy_flat = 0;
  std::optional<size_t> max_same_blk_copy_i;
  int min_restored_arg_reg = 8;
  std::bitset<68> restored_touched_regs;
  std::bitset<68> restored_fpr_touched_regs;
  for (const CandidateCopy& cand : candidates) {
    const bool has_explicit_fusage = call_blk.rtl_insns[call_i].has_explicit_call_fusage;
    if (has_explicit_fusage) {
      bool in_call_fusage = false;
      for (const RtlExpr& existing : call_blk.rtl_insns[call_i].call_fusage) {
        if (existing.op == RtlOp::kUse && !existing.children.empty() &&
            existing.children[0].op == RtlOp::kReg &&
            existing.children[0].RegNo() == cand.arg_reg) {
          in_call_fusage = true;
          break;
        }
      }
      if (!in_call_fusage) {
        continue;
      }
    }

    int64_t call_arg_bytes = 0;
    prefix[call_flat].parsed.pattern.Visit([&call_arg_bytes](const RtlExpr& n) {
      if (n.op == RtlOp::kCall && n.children.size() >= 2 && n.children[1].op == RtlOp::kConstInt) {
        call_arg_bytes = std::max(call_arg_bytes, n.children[1].int_val);
      }
    });

    bool save_hoisted_before_ra = false;
    if (call_b == 0 && !ra_sank_into_indirect_target_load_shadow &&
        !cand.saved_reg_read_before_call) {
      for (int off = 0; off < cand.saved_span; ++off) {
        auto it = gpr_save_flat.find(cand.saved_reg + off);
        if (it != gpr_save_flat.end() && it->second < *ra_save_flat &&
            it->second < cand.copy_flat) {
          // Per `sched.c:priority`: an FPR arg copy (`mfc1`, latency 2) would
          // give `s.d $f2x -> mtc1` priority >= 3, so `sched2` could not have
          // scheduled `s.d $f2x` AFTER an independent non-copy GPR ALU op
          // (unless stack args > 16 bytes prove all O32 arg regs are passed).
          bool preceded_by_non_copy_gpr_alu = false;
          if (runs_sched2 && cand.saved_reg >= 32 && call_arg_bytes <= 16) {
            for (size_t k = 0; k < it->second; ++k) {
              const RtlExpr& kp = prefix[k].parsed.pattern;
              if (prefix[k].sum.is_single_cycle_int_alu && !prefix[k].is_true_prologue_save &&
                  kp.IsSet() && kp.SetDest().IsHardReg() && kp.SetDest().RegNo() >= 2 &&
                  kp.SetDest().RegNo() < 29 && !kp.SetSrc().IsHardReg()) {
                preceded_by_non_copy_gpr_alu = true;
                break;
              }
            }
          }
          if (!preceded_by_non_copy_gpr_alu) {
            save_hoisted_before_ra = true;
          }
        }
      }
    }

    bool arg6_written_as_8byte = false;
    if (!is_gp64 && arg_gpr_live_into_call.test(6) && arg_gpr_live_into_call.test(7)) {
      for (size_t k = call_flat; k > 0; --k) {
        const RtlExpr& kp = prefix[k - 1].parsed.pattern;
        if (prefix[k - 1].sum.written_regs.test(6) || prefix[k - 1].sum.written_regs.test(7)) {
          if (kp.IsSet() && kp.SetDest().IsHardReg() && kp.SetDest().RegNo() == 6 &&
              (kp.SetDest().mode == RtlMachineMode::kDf ||
               kp.SetDest().mode == RtlMachineMode::kDi)) {
            arg6_written_as_8byte = true;
          }
          break;
        }
      }
    }

    bool higher_arg_written = false;
    for (int r = cand.arg_reg + cand.arg_span; r <= 7; ++r) {
      // Under MIPS O32 (`config/mips/mips.c:function_arg`), an 8-byte aligned
      // `DImode`/`DFmode` argument in `$a2..$a3` skips `$a1` (`reg 5`) as
      // alignment padding and does not prove `$a1` was passed.
      if (cand.arg_reg == 5 && cand.arg_span == 1 && arg6_written_as_8byte) {
        break;
      }
      if (arg_gpr_live_into_call.test(static_cast<size_t>(r))) {
        higher_arg_written = true;
      }
    }
    bool all_lower_args_supplied = true;
    for (int r = 4; r < cand.arg_reg; ++r) {
      // Under MIPS O32, an 8-byte aligned DImode/DFmode argument in $6..$7 skips
      // $5 when $4 holds a 4-byte argument.
      if (cand.arg_span == 2 && cand.arg_reg == 6 && r == 5) {
        continue;
      }
      if (!arg_gpr_live_into_call.test(static_cast<size_t>(r)) &&
          !arg_gpr_saved_in_callee_reg.test(static_cast<size_t>(r)) &&
          !arg_word_occupied_by_fpr.test(static_cast<size_t>(r))) {
        all_lower_args_supplied = false;
      }
    }

    const bool call_requires_arg = (call_b == 0) ? (save_hoisted_before_ra || higher_arg_written)
                                                 : (has_explicit_fusage || higher_arg_written);
    if (!all_lower_args_supplied || !call_requires_arg) {
      continue;
    }

    RtlBasicBlock& copy_blk = cfg->rpo_blocks[cand.copy_b];
    if (copy_blk.rtl_insns.size() != copy_blk.instructions.size()) {
      copy_blk.SyncRtlInsnsFromInstructions();
    }
    bool already_has_dead = false;
    for (const RtlRegNote& existing : prefix[cand.copy_flat].parsed.reg_notes) {
      if (existing.kind == RtlMachineMode::kRegDead && existing.datum.op == RtlOp::kReg &&
          existing.datum.RegNo() == cand.arg_reg) {
        already_has_dead = true;
        break;
      }
    }
    if (!already_has_dead) {
      RtlRegNote dead_note;
      dead_note.kind = RtlMachineMode::kRegDead;
      dead_note.datum = MakeGccHardReg(cand.mode, cand.arg_reg);
      prefix[cand.copy_flat].parsed.reg_notes.insert(
          prefix[cand.copy_flat].parsed.reg_notes.begin(), std::move(dead_note));
      copy_blk.instructions[cand.copy_i] =
          FormatInsnWithoutCallFusage(prefix[cand.copy_flat].parsed);
      copy_blk.rtl_insns[cand.copy_i].canonical_pattern = copy_blk.instructions[cand.copy_i];
    }

    RtlPatternAndNotes move_pat;
    move_pat.pattern = RtlExpr::MakeSet(MakeGccHardReg(cand.mode, cand.arg_reg),
                                        MakeGccHardReg(cand.mode, cand.saved_reg));
    std::string formatted = FormatInsnWithoutCallFusage(move_pat);
    // Per `mips.md:define_function_unit` (`xfer` latency 2) and
    // `sched.c:insn_cost`, an FPR->GPR arg copy (`saved_reg >= 32`) whose
    // GPR->FPR producer (`copy_flat`) is within 2 insns of `sw $ra` stalls
    // behind the `mtc1` latency while `sw $ra` and trailing prologue saves
    // execute; place it right after those prologue saves rather than before
    // `sw $ra`.
    const bool fpr_stalled_past_ra =
        runs_sched2 && call_b == 0 && cand.saved_reg >= 32 && *ra_save_flat == cand.copy_flat + 1;
    if (fpr_stalled_past_ra) {
      post_ra_stalled_moves.push_back(std::move(formatted));
    } else {
      if (cand.saved_reg >= 32) {
        restored_fpr_moves.push_back(std::move(formatted));
        any_restored_fpr_src = true;
        for (int off = 0; off < cand.arg_span; ++off) {
          restored_fpr_touched_regs.set(static_cast<size_t>(cand.arg_reg + off));
        }
        for (int off = 0; off < cand.saved_span; ++off) {
          restored_fpr_touched_regs.set(static_cast<size_t>(cand.saved_reg + off));
        }
      } else {
        restored_moves.push_back(std::move(formatted));
      }
      max_cand_copy_flat = std::max(max_cand_copy_flat, cand.copy_flat);
      if (cand.copy_b == call_b) {
        max_same_blk_copy_i = max_same_blk_copy_i.has_value()
                                  ? std::max(*max_same_blk_copy_i, cand.copy_i)
                                  : cand.copy_i;
      }
      min_restored_arg_reg = std::min(min_restored_arg_reg, cand.arg_reg);
      for (int off = 0; off < cand.arg_span; ++off) {
        restored_touched_regs.set(static_cast<size_t>(cand.arg_reg + off));
      }
      for (int off = 0; off < cand.saved_span; ++off) {
        restored_touched_regs.set(static_cast<size_t>(cand.saved_reg + off));
      }
    }
  }

  if (restored_moves.empty() && restored_fpr_moves.empty() && post_ra_stalled_moves.empty()) {
    return;
  }

  auto is_sp_stack_store = [](const RtlExpr& pat, int64_t* out_off) -> bool {
    if (!pat.IsSet() || pat.SetDest().op != RtlOp::kMem || pat.SetDest().flags.volatil ||
        pat.SetDest().children.size() != 1) {
      return false;
    }
    const RtlExpr& addr = pat.SetDest().children[0];
    if (addr.op == RtlOp::kReg && addr.RegNo() == 29) {
      *out_off = 0;
      return true;
    }
    if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
        addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == 29 &&
        addr.children[1].op == RtlOp::kConstInt) {
      *out_off = addr.children[1].int_val;
      return true;
    }
    return false;
  };
  auto is_non_unchanging_mem_load = [](const RtlExpr& pat) -> bool {
    if (!pat.IsSet() || pat.SetDest().op == RtlOp::kMem) {
      return false;
    }
    bool has_varying_load = false;
    pat.SetSrc().Visit([&](const RtlExpr& n) {
      if ((n.op == RtlOp::kMem && !n.flags.unchanging) || n.op == RtlOp::kUnalignedLoad) {
        has_varying_load = true;
      }
    });
    return has_varying_load;
  };

  auto insert_moves_at = [&](size_t pos, const std::vector<std::string>& moves) {
    if (moves.empty()) {
      return;
    }
    std::vector<RtlInsn> rtl_insns;
    rtl_insns.reserve(moves.size());
    for (const std::string& pat_str : moves) {
      RtlInsn insn;
      insn.canonical_pattern = pat_str;
      insn.is_synthetic_rtl_expansion = true;
      rtl_insns.push_back(std::move(insn));
    }
    call_blk.instructions.insert(call_blk.instructions.begin() + static_cast<ptrdiff_t>(pos),
                                 moves.begin(), moves.end());
    call_blk.rtl_insns.insert(call_blk.rtl_insns.begin() + static_cast<ptrdiff_t>(pos),
                              std::make_move_iterator(rtl_insns.begin()),
                              std::make_move_iterator(rtl_insns.end()));
  };

  if (!post_ra_stalled_moves.empty()) {
    size_t post_ra_pos = *ra_save_flat + 1;
    for (size_t k = *ra_save_flat + 1; k < call_i; ++k) {
      if (prefix[k].is_true_prologue_save) {
        post_ra_pos = k + 1;
      }
    }
    insert_moves_at(post_ra_pos, post_ra_stalled_moves);
  }

  if (restored_moves.empty() && restored_fpr_moves.empty()) {
    return;
  }

  size_t insert_pos = call_i;
  std::optional<size_t> post_ra_xfer_stall_pos;
  if (call_b == 0) {
    while (insert_pos > max_cand_copy_flat + 1) {
      int64_t sp_off = 0;
      if (!prefix[insert_pos - 1].is_true_prologue_save &&
          is_sp_stack_store(prefix[insert_pos - 1].parsed.pattern, &sp_off) && sp_off >= 16 &&
          sp_off < min_prologue_save_off) {
        --insert_pos;
      } else {
        break;
      }
    }
    if (runs_sched2 && *ra_save_flat > max_cand_copy_flat && *ra_save_flat < insert_pos) {
      // Per `sched.c:schedule_block` and `mips.md:define_function_unit`, when a
      // ready FPR->GPR arg copy (`xfer`, priority 2) is restored in a block
      // whose post-`sw $ra` pre-call body contains an adjacent `const_double`
      // (`li.s`/`li.d`, `xfer` latency 2) -> FP-arithmetic consumer pair
      // independent of `restored_fpr_touched_regs`, `sched2` schedules that
      // deleted FPR->GPR copy into the 1-cycle `xfer` stall slot while GPR arg
      // copies still sit ahead of `sw $ra`.
      if (!restored_fpr_moves.empty()) {
        for (size_t k = *ra_save_flat + 1; k + 1 < insert_pos; ++k) {
          const RtlExpr& p0 = prefix[k].parsed.pattern;
          const RtlExpr& p1 = prefix[k + 1].parsed.pattern;
          if (p0.IsSet() && p0.SetDest().IsHardReg() && p0.SetDest().RegNo() >= 32 &&
              p0.SetDest().RegNo() < 64 && p0.SetSrc().op == RtlOp::kConstDouble &&
              IsFpArithPattern(p1) &&
              prefix[k + 1].sum.read_regs.test(static_cast<size_t>(p0.SetDest().RegNo())) &&
              (prefix[k].sum.read_regs & restored_fpr_touched_regs).none() &&
              (prefix[k].sum.written_regs & restored_fpr_touched_regs).none() &&
              (prefix[k + 1].sum.read_regs & restored_fpr_touched_regs).none() &&
              (prefix[k + 1].sum.written_regs & restored_fpr_touched_regs).none()) {
            post_ra_xfer_stall_pos = k + 1;
            break;
          }
        }
      }
      if (!post_ra_xfer_stall_pos.has_value() && !restored_fpr_moves.empty()) {
        for (std::string& m : restored_fpr_moves) {
          restored_moves.push_back(std::move(m));
        }
        restored_fpr_moves.clear();
      }
      if (post_ra_xfer_stall_pos.has_value()) {
        insert_moves_at(*post_ra_xfer_stall_pos, restored_fpr_moves);
        restored_fpr_moves.clear();
      }
      if (restored_moves.empty()) {
        return;
      }
      const bool fpr_still_in_restored =
          any_restored_fpr_src && !post_ra_xfer_stall_pos.has_value();
      bool saw_varying_load_after_ra = false;
      for (size_t k = *ra_save_flat + 1; k < insert_pos; ++k) {
        if (is_non_unchanging_mem_load(prefix[k].parsed.pattern)) {
          saw_varying_load_after_ra = true;
          break;
        }
      }
      if (!saw_varying_load_after_ra) {
        insert_pos = *ra_save_flat;
        while (insert_pos > max_cand_copy_flat + 1) {
          const FlatInsn& prev_entry = prefix[insert_pos - 1];
          if (prev_entry.is_true_prologue_save) {
            --insert_pos;
            continue;
          }
          // Per `sched.c:priority` and `mips.md:define_function_unit`, a ready
          // multi-cycle FPR->GPR arg copy (`xfer` latency 2) has higher
          // scheduling priority than an independent single-cycle GPR arg setup
          // feeding the same call.
          const RtlExpr& ppat = prev_entry.parsed.pattern;
          if (fpr_still_in_restored && prev_entry.sum.is_single_cycle_int_alu && ppat.IsSet() &&
              ppat.SetDest().op == RtlOp::kReg && ppat.SetDest().RegNo() >= 4 &&
              ppat.SetDest().RegNo() <= 7 &&
              (prev_entry.sum.read_regs & restored_touched_regs).none() &&
              (prev_entry.sum.written_regs & restored_touched_regs).none()) {
            --insert_pos;
            continue;
          }
          break;
        }
      }
    } else if (!restored_fpr_moves.empty()) {
      for (std::string& m : restored_fpr_moves) {
        restored_moves.push_back(std::move(m));
      }
      restored_fpr_moves.clear();
    }
  } else {
    if (!restored_fpr_moves.empty()) {
      for (std::string& m : restored_fpr_moves) {
        restored_moves.push_back(std::move(m));
      }
      restored_fpr_moves.clear();
    }
    const size_t call_blk_flat_base = call_flat - call_i;
    const size_t min_insert_pos = max_same_blk_copy_i.has_value() ? (*max_same_blk_copy_i + 1) : 0;
    while (insert_pos > min_insert_pos) {
      const FlatInsn& prev_entry = prefix[call_blk_flat_base + insert_pos - 1];
      int64_t sp_off = 0;
      if (is_sp_stack_store(prev_entry.parsed.pattern, &sp_off) && sp_off >= 16 &&
          sp_off < min_prologue_save_off) {
        --insert_pos;
        continue;
      }
      // `calls.c:expand_call` loads register arguments in ascending order
      // (`$4`, `$5`, `$6`, `$7`); place the restored lower-numbered arg move
      // ahead of independent single-cycle higher-numbered arg setups.
      const RtlExpr& ppat = prev_entry.parsed.pattern;
      if (prev_entry.sum.is_single_cycle_int_alu && ppat.IsSet() &&
          ppat.SetDest().op == RtlOp::kReg && ppat.SetDest().RegNo() > min_restored_arg_reg &&
          ppat.SetDest().RegNo() <= 7 &&
          (prev_entry.sum.read_regs & restored_touched_regs).none() &&
          (prev_entry.sum.written_regs & restored_touched_regs).none()) {
        --insert_pos;
        continue;
      }
      break;
    }
  }
  insert_moves_at(insert_pos, restored_moves);
}

bool HasMultiCycleReadyLatency(const RtlExpr& expr) {
  bool multi_cycle = false;
  if (expr.IsSet() && expr.SetDest().op == RtlOp::kMem && !expr.SetDest().flags.volatil) {
    // Store (`memory` unit ready-delay = 1 in `mips.md:161`); only check address/src for loads.
    expr.SetSrc().Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem || node.op == RtlOp::kUnalignedLoad) {
        multi_cycle = true;
      }
    });
  } else {
    expr.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem || node.op == RtlOp::kUnalignedLoad ||
          node.op == RtlOp::kUnalignedStore) {
        multi_cycle = true;
      }
    });
  }
  expr.Visit([&](const RtlExpr& node) {
    switch (node.op) {
      case RtlOp::kMult:
      case RtlOp::kDiv:
      case RtlOp::kUdiv:
      case RtlOp::kMod:
      case RtlOp::kUmod:
      case RtlOp::kSqrt:
      case RtlOp::kFloat:
      case RtlOp::kUnsignedFloat:
      case RtlOp::kFix:
      case RtlOp::kUnsignedFix:
      case RtlOp::kFloatExtend:
      case RtlOp::kFloatTruncate:
        multi_cycle = true;
        break;
      case RtlOp::kReg:
        if (node.RegNo() >= 32 && node.RegNo() <= 63) {
          multi_cycle = true;
        }
        break;
      default:
        break;
    }
  });
  return multi_cycle;
}

bool ExprContainsMulOrDiv(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kMult || node.op == RtlOp::kDiv || node.op == RtlOp::kUdiv ||
        node.op == RtlOp::kMod || node.op == RtlOp::kUmod) {
      found = true;
    }
  });
  return found;
}

void CollectPatternReadRegs(const RtlExpr& expr, bool gp64, bool fp64, std::bitset<68>* read_regs) {
  auto record_elem = [&](const RtlExpr& elem) {
    if (elem.IsSet()) {
      const RtlExpr& dst = elem.SetDest();
      const int dreg = TrueRegnum(dst);
      if ((dst.op != RtlOp::kReg && dst.op != RtlOp::kSubreg) || dreg < 0) {
        CollectReadRegsInSubtree(dst, gp64, fp64, read_regs);
      }
      CollectReadRegsInSubtree(elem.SetSrc(), gp64, fp64, read_regs);
    } else if (elem.op == RtlOp::kClobber) {
      if (!elem.children.empty()) {
        const RtlExpr& dst = elem.children[0];
        const int dreg = TrueRegnum(dst);
        if ((dst.op != RtlOp::kReg && dst.op != RtlOp::kSubreg) || dreg < 0) {
          CollectReadRegsInSubtree(dst, gp64, fp64, read_regs);
        }
      }
    } else {
      CollectReadRegsInSubtree(elem, gp64, fp64, read_regs);
    }
  };
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : expr.children) {
      record_elem(ch);
    }
  } else {
    record_elem(expr);
  }
}

// Reverses `jump.c:jump_optimize` (`noop_moves = 1`, lines 434-465) deletion of
// dead output reloads `(set (reg:DI 64) (reg:DI dst))` emitted when `reload`
// reloads a `REG_RETVAL` self-move on `MD_REGS` (`movdi_internal` has `*d <- *x`
// and `*x <- *d` alternatives, no `x <- x`) and reload inheritance substitutes
// `dst` into the consumer `I_cons` while leaving `REG_DEAD (reg:DI 64)` on
// `I_cons`. During `sched2` (`sched.c:2099-2101`, `2640-2672`), that output
// reload occupies `imuldiv` (`issue-delay = 3`) immediately ahead of `I_cons`,
// allowing 1-2 ready single-cycle GPR instructions to schedule between
// `(set (reg:DI dst) (reg:DI 64))` and `I_cons` before `jump2` deletes the
// redundant `(set (reg:DI 64) (reg:DI dst))` via `find_equiv_reg`.
void RestoreDeletedNoopMdOutputReloads(RtlControlFlowGraph* cfg, bool gp64, bool fp64) {
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    const size_t n = blk.instructions.size();
    if (n < 4) {
      continue;
    }

    std::vector<RtlPatternAndNotes> parsed;
    std::vector<InsnSchedSummary> sum;
    std::vector<std::bitset<68>> pat_reads(n);
    parsed.reserve(n);
    sum.reserve(n);
    bool parse_ok = true;
    for (size_t i = 0; i < n; ++i) {
      auto p = ParsePatternAndNotes(blk.instructions[i]);
      if (!p.ok()) {
        parse_ok = false;
        break;
      }
      sum.push_back(SummarizeInsnForSched2(blk.instructions[i], gp64, fp64));
      if (p->pattern.op != RtlOp::kNote && p->pattern.op != RtlOp::kAddrVec &&
          p->pattern.op != RtlOp::kAddrDiffVec && p->pattern.op != RtlOp::kSequence) {
        CollectPatternReadRegs(p->pattern, gp64, fp64, &pat_reads[i]);
      }
      parsed.push_back(std::move(*p));
    }
    if (!parse_ok) {
      continue;
    }

    std::bitset<68> cur_live;
    if (blk.successors.empty()) {
      cur_live.set(28);
      cur_live.set(29);
      cur_live.set(31);
      if (cfg->sideband.frame_pointer_needed) {
        cur_live.set(30);
      }
      if (cfg->sideband.epilogue_delay_pattern.has_value() &&
          !cfg->sideband.epilogue_delay_pattern->empty()) {
        InsnSchedSummary ep_sum =
            SummarizeInsnForSched2(*cfg->sideband.epilogue_delay_pattern, gp64, fp64);
        std::bitset<68> ep_reads;
        auto ep_parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
        if (ep_parsed.ok()) {
          CollectPatternReadRegs(ep_parsed->pattern, gp64, fp64, &ep_reads);
        }
        cur_live &= ~ep_sum.written_regs;
        cur_live |= ep_reads;
      }
    } else {
      for (size_t succ : blk.successors) {
        auto it = cfg->sideband.basic_block_live_at_start.find(static_cast<int>(succ));
        if (it != cfg->sideband.basic_block_live_at_start.end()) {
          for (int r : it->second) {
            if (r > 0 && r < 68) {
              cur_live.set(static_cast<size_t>(r));
            }
          }
        }
      }
    }

    std::vector<std::bitset<68>> live_after(n);
    for (size_t rev = n; rev > 0; --rev) {
      const size_t i = rev - 1;
      live_after[i] = cur_live;
      if (parsed[i].pattern.op == RtlOp::kNote) {
        continue;
      }
      cur_live &= ~sum[i].written_regs;
      cur_live |= pat_reads[i];
      cur_live.reset(0);
    }

    std::vector<bool> multi_cycle_priority_tainted(n, false);
    std::bitset<68> tainted_regs;
    std::vector<bool> md_from_mul_div_at(n, false);
    bool md_from_mul_div = false;
    for (size_t i = 0; i < n; ++i) {
      md_from_mul_div_at[i] = md_from_mul_div;
      if (parsed[i].pattern.op == RtlOp::kNote) {
        continue;
      }
      if (sum[i].is_call) {
        tainted_regs.reset();
        md_from_mul_div = false;
        continue;
      }
      const bool cur_tainted =
          HasMultiCycleReadyLatency(parsed[i].pattern) || (pat_reads[i] & tainted_regs).any();
      multi_cycle_priority_tainted[i] = cur_tainted;
      if (cur_tainted) {
        tainted_regs |= sum[i].written_regs;
      } else {
        tainted_regs &= ~sum[i].written_regs;
      }
      if (sum[i].written_regs.test(64) || sum[i].written_regs.test(65)) {
        md_from_mul_div = ExprContainsMulOrDiv(parsed[i].pattern);
      }
    }

    std::vector<std::pair<size_t, int>> insertions;
    for (size_t idx_mf = 0; idx_mf < n; ++idx_mf) {
      const RtlExpr& pat_mf = parsed[idx_mf].pattern;
      if (!IsNormalNonJumpNonCallInsn(pat_mf) || !pat_mf.IsSet() ||
          pat_mf.SetDest().op != RtlOp::kReg || pat_mf.SetDest().mode != RtlMachineMode::kDi ||
          pat_mf.SetSrc().op != RtlOp::kReg || pat_mf.SetSrc().mode != RtlMachineMode::kDi ||
          pat_mf.SetSrc().RegNo() != 64) {
        continue;
      }
      const int dst = pat_mf.SetDest().RegNo();
      if (dst < 2 || dst > 24 || (dst % 2) != 0 || md_from_mul_div_at[idx_mf] ||
          live_after[idx_mf].test(64) || live_after[idx_mf].test(65)) {
        continue;
      }
      bool has_reg_unused = false;
      for (const RtlRegNote& note : parsed[idx_mf].reg_notes) {
        if (note.kind == RtlMachineMode::kRegUnused) {
          has_reg_unused = true;
          break;
        }
      }
      if (has_reg_unused) {
        continue;
      }

      size_t active_intervening = 0;
      std::optional<size_t> idx_cons;
      bool span_ok = true;
      for (size_t k = idx_mf + 1; k < n; ++k) {
        const RtlExpr& pk = parsed[k].pattern;
        if (pk.op == RtlOp::kNote) {
          continue;
        }
        if (pat_reads[k].test(static_cast<size_t>(dst)) ||
            pat_reads[k].test(static_cast<size_t>(dst + 1))) {
          idx_cons = k;
          break;
        }
        if (pk.op == RtlOp::kClobber || pk.op == RtlOp::kUse) {
          if (sum[k].written_regs.test(static_cast<size_t>(dst)) ||
              sum[k].written_regs.test(static_cast<size_t>(dst + 1)) ||
              sum[k].written_regs.test(64) || sum[k].written_regs.test(65) ||
              pat_reads[k].test(64) || pat_reads[k].test(65)) {
            span_ok = false;
            break;
          }
          continue;
        }
        ++active_intervening;
        int saved_reg = -1;
        int64_t sp_off = 0;
        const bool is_b0_prologue_save_store =
            (b == 0 && !pk.SetDest().flags.volatil &&
             IsPrologueStackRegSave(pk, &saved_reg, &sp_off) && sp_off >= 0 &&
             ((saved_reg >= 16 && saved_reg <= 23) || saved_reg == 30 || saved_reg == 31 ||
              (saved_reg >= 52 && saved_reg <= 63)));
        if (active_intervening > 2 ||
            !(sum[k].is_single_cycle_int_alu || is_b0_prologue_save_store) ||
            multi_cycle_priority_tainted[k] || sum[k].written_regs.test(static_cast<size_t>(dst)) ||
            sum[k].written_regs.test(static_cast<size_t>(dst + 1)) ||
            sum[k].written_regs.test(64) || sum[k].written_regs.test(65)) {
          span_ok = false;
          break;
        }
      }
      if (!span_ok || !idx_cons.has_value() || active_intervening < 1 || active_intervening > 2) {
        continue;
      }
      const size_t c_idx = *idx_cons;
      const RtlExpr& pat_cons = parsed[c_idx].pattern;
      if (!IsNormalNonJumpNonCallInsn(pat_cons) || pat_cons.op == RtlOp::kUse ||
          pat_cons.op == RtlOp::kClobber || live_after[c_idx].test(static_cast<size_t>(dst)) ||
          live_after[c_idx].test(static_cast<size_t>(dst + 1))) {
        continue;
      }
      bool nearby_hilo_after = false;
      size_t checked_after = 0;
      for (size_t k = c_idx; k < n && checked_after < 3; ++k) {
        const RtlExpr& pk = parsed[k].pattern;
        if (pk.op == RtlOp::kNote || pk.op == RtlOp::kClobber || pk.op == RtlOp::kUse) {
          continue;
        }
        if (pat_reads[k].test(64) || pat_reads[k].test(65) || sum[k].written_regs.test(64) ||
            sum[k].written_regs.test(65)) {
          nearby_hilo_after = true;
          break;
        }
        ++checked_after;
      }
      if (nearby_hilo_after) {
        continue;
      }

      if (!HasRegDeadForRegno(parsed[idx_mf], 64)) {
        parsed[idx_mf].reg_notes.insert(parsed[idx_mf].reg_notes.begin(),
                                        RtlRegNote{
                                            .kind = RtlMachineMode::kRegDead,
                                            .list_op = RtlOp::kExprList,
                                            .datum = RtlExpr::MakeReg(RtlMachineMode::kDi, 64),
                                        });
        blk.instructions[idx_mf] = FormatInsnWithoutCallFusage(parsed[idx_mf]);
      }
      if (!HasRegDeadForRegno(parsed[c_idx], 64)) {
        auto ins_it =
            std::find_if(parsed[c_idx].reg_notes.begin(), parsed[c_idx].reg_notes.end(),
                         [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegUnused; });
        parsed[c_idx].reg_notes.insert(ins_it,
                                       RtlRegNote{
                                           .kind = RtlMachineMode::kRegDead,
                                           .list_op = RtlOp::kExprList,
                                           .datum = RtlExpr::MakeReg(RtlMachineMode::kDi, 64),
                                       });
        blk.instructions[c_idx] = FormatInsnWithoutCallFusage(parsed[c_idx]);
      }
      insertions.push_back({c_idx, dst});
      idx_mf = c_idx;
    }

    if (insertions.empty()) {
      continue;
    }
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    for (auto it = insertions.rbegin(); it != insertions.rend(); ++it) {
      RtlPatternAndNotes wb_pn;
      wb_pn.pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, 64),
                                       RtlExpr::MakeReg(RtlMachineMode::kDi, it->second));
      const std::string wb_str = wb_pn.ToGccPrintRtlString();
      RtlInsn wb_rinsn;
      wb_rinsn.canonical_pattern = wb_str;
      wb_rinsn.is_synthetic_rtl_expansion = true;
      blk.instructions.insert(blk.instructions.begin() + static_cast<ptrdiff_t>(it->first), wb_str);
      blk.rtl_insns.insert(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(it->first),
                           std::move(wb_rinsn));
    }
  }
}

constexpr int kReturnAddressRegno = 31;

bool IsPostReloadReturnInternalPattern(const RtlExpr& pat) {
  return pat.op == RtlOp::kParallel && pat.children.size() == 2 &&
         pat.children[0].op == RtlOp::kReturn && pat.children[1].op == RtlOp::kUse &&
         pat.children[1].children.size() == 1 && pat.children[1].children[0].IsHardReg() &&
         pat.children[1].children[0].RegNo() == kReturnAddressRegno;
}

bool IsUnconditionalJumpOrTablejumpPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kPc) {
    return pat.children[1].op == RtlOp::kLabelRef || pat.children[1].op == RtlOp::kReg;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    return IsUnconditionalJumpOrTablejumpPattern(pat.children[0]);
  }
  return false;
}

bool IsMipsCalleeSavedOrRaReg(int regno) {
  return (regno >= 16 && regno <= 23) || regno == 30 || regno == 31 || (regno >= 52 && regno <= 63);
}

bool SubtreeContainsCallOrCalleeSavedReg(const RtlExpr& expr, bool gp64, bool fp64) {
  bool found = false;
  expr.Visit([&](const RtlExpr& node) {
    if (found) {
      return;
    }
    if (node.op == RtlOp::kCall) {
      found = true;
      return;
    }
    if (node.IsHardReg()) {
      const int r = node.RegNo();
      const int nw = std::max(1, MipsHardRegNregs(r, node.mode, gp64, fp64));
      for (int k = 0; k < nw; ++k) {
        if (IsMipsCalleeSavedOrRaReg(r + k)) {
          found = true;
          return;
        }
      }
    }
  });
  return found;
}

// Reverses `jump2` (`jump.c:289-306, 2065-2081`) insertion of trailing
// `return_internal` (`(parallel [(return) (use (reg:SI 31 ra))])`) on zero-frame
// leaf functions (`HAVE_return` == `reload_completed && simple_epilogue_p()`,
// `mips.md:5928-5941`, `mips.c:5398-5407`).
//
// In KMC GCC 2.7.2 MIPS, `define_expand "epilogue"` (`mips.md:6056`) is
// commented out, so `.09.greg` and `.10.sched2` never contain `return_internal`;
// `jump2` appends `emit_jump_insn(gen_return()); emit_barrier();` at
// `get_last_insn()` when `simple_epilogue_p()` holds (`total_size == 0`) and
// `get_last_insn()` is a non-`BARRIER` non-`NOTE` item. Note that `jump2`
// never converts early jumps `(set (pc) (label_ref L_exit))` in `b < n - 1`
// into `return_internal` because `redirect_exp` (`jump.c:3801`) attempts bare
// `(return)`, which `validate_change` rejects on MIPS.
bool UnexpandPostReloadTrailingReturnInsn(RtlControlFlowGraph* cfg,
                                          const CompilationProfile& profile) {
  if (profile.optimize_level <= 0 || cfg->rpo_blocks.empty() ||
      cfg->sideband.frame_size_bytes != 0 || cfg->sideband.frame_pointer_needed ||
      cfg->sideband.outgoing_args_size != 0) {
    return false;
  }

  const size_t n = cfg->rpo_blocks.size();
  if (n == 1 && cfg->rpo_blocks[0].instructions.size() == 1 && !cfg->rpo_blocks[0].has_label) {
    // Removing `return_internal` from a 1-block unlabeled 1-insn CFG would leave
    // zero non-`NOTE` items in the RTL chain (`jump.c:296-300` `insn == 0`).
    return false;
  }

  std::vector<size_t> ret_blocks;
  for (size_t b = 0; b < n; ++b) {
    const auto& insns = cfg->rpo_blocks[b].instructions;
    for (size_t i = 0; i < insns.size(); ++i) {
      const auto parsed = ParsePatternAndNotes(insns[i]);
      if (!parsed.ok()) {
        return false;
      }
      if (IsPostReloadReturnInternalPattern(parsed->pattern)) {
        if (i + 1 != insns.size() || !parsed->reg_notes.empty()) {
          return false;
        }
        ret_blocks.push_back(b);
      }
    }
  }
  // In KMC GCC 2.7.2 MIPS, `gen_return()` is emitted at most once per zero-frame
  // leaf function (at `get_last_insn()` in `jump.c:303, 2079` or `reorg.c:833`),
  // because `redirect_exp` (`jump.c:3801`) attempts bare `(return)` which fails
  // `validate_change`. If hand-written assembly contains 2+ `jr $ra` exits
  // (`ret_blocks.size() > 1`), `jump2` + `dbr_schedule` cannot synthesize the
  // non-trailing `return_internal`s from a `return_internal`-free `.10.sched2`,
  // so preserving them is required for $f(g(x)) = x$.
  if (ret_blocks.size() != 1) {
    return false;
  }

  const size_t ret_b = ret_blocks[0];
  {
    const RtlBasicBlock& ret_blk = cfg->rpo_blocks[ret_b];
    if (!ret_blk.successors.empty() || !ret_blk.addr_vec_targets.empty()) {
      return false;
    }
    if (ret_blk.instructions.size() >= 2) {
      const auto prev_parsed =
          ParsePatternAndNotes(ret_blk.instructions[ret_blk.instructions.size() - 2]);
      if (!prev_parsed.ok() || IsUnconditionalJumpOrTablejumpPattern(prev_parsed->pattern)) {
        return false;
      }
    } else if (!ret_blk.has_label && ret_b >= 1) {
      const RtlBasicBlock& pred_blk = cfg->rpo_blocks[ret_b - 1];
      const bool pred_falls_through =
          std::find(pred_blk.successors.begin(), pred_blk.successors.end(), ret_b) !=
          pred_blk.successors.end();
      if (!pred_falls_through || pred_blk.instructions.empty()) {
        return false;
      }
      const auto pred_last = ParsePatternAndNotes(pred_blk.instructions.back());
      if (!pred_last.ok() || IsUnconditionalJumpOrTablejumpPattern(pred_last->pattern)) {
        return false;
      }
    }
  }

  if (ret_b != n - 1) {
    const RtlBasicBlock& tail_blk = cfg->rpo_blocks[n - 1];
    if (tail_blk.instructions.empty() || !tail_blk.addr_vec_targets.empty()) {
      return false;
    }
    const auto tail_last = ParsePatternAndNotes(tail_blk.instructions.back());
    if (!tail_last.ok() || !IsUnconditionalJumpOrTablejumpPattern(tail_last->pattern)) {
      return false;
    }
  }

  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  for (size_t b = 0; b < n; ++b) {
    const auto& insns = cfg->rpo_blocks[b].instructions;
    const size_t limit = (b == ret_b) ? (insns.size() - 1) : insns.size();
    for (size_t i = 0; i < limit; ++i) {
      const auto parsed = ParsePatternAndNotes(insns[i]);
      if (!parsed.ok() || SubtreeContainsCallOrCalleeSavedReg(parsed->pattern, gp64, fp64)) {
        return false;
      }
    }
  }
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    const auto ep_parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (!ep_parsed.ok() || SubtreeContainsCallOrCalleeSavedReg(ep_parsed->pattern, gp64, fp64)) {
      return false;
    }
  }

  if (ret_b == n - 1) {
    RtlBasicBlock& mut_last = cfg->rpo_blocks[n - 1];
    mut_last.instructions.pop_back();
    if (!mut_last.rtl_insns.empty()) {
      mut_last.rtl_insns.pop_back();
    }
    mut_last.has_trailing_barrier = false;
    mut_last.successors.clear();

    if (n > 1 && mut_last.instructions.empty() && !mut_last.has_label &&
        mut_last.addr_vec_targets.empty()) {
      auto& pred_succs = cfg->rpo_blocks[n - 2].successors;
      pred_succs.erase(std::remove(pred_succs.begin(), pred_succs.end(), n - 1), pred_succs.end());
      cfg->rpo_blocks.pop_back();
    }
  } else {
    size_t total_labels = 0;
    for (size_t b = 0; b < n; ++b) {
      if (cfg->rpo_blocks[b].has_label) {
        ++total_labels;
      }
      if (!cfg->rpo_blocks[b].addr_vec_targets.empty()) {
        ++total_labels;
      }
    }

    const size_t exit_idx = n;
    const std::string exit_lbl = absl::StrCat("L", total_labels);
    RtlBasicBlock exit_blk;
    exit_blk.id = static_cast<uint32_t>(exit_idx);
    exit_blk.has_label = true;
    cfg->rpo_blocks.push_back(std::move(exit_blk));

    RtlBasicBlock& mut_b = cfg->rpo_blocks[ret_b];
    if (mut_b.rtl_insns.size() != mut_b.instructions.size()) {
      mut_b.SyncRtlInsnsFromInstructions();
    }
    const RtlExpr jump_expr =
        RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef(exit_lbl));
    mut_b.rtl_insns.back().SetFromExpr(jump_expr);
    mut_b.instructions.back() = mut_b.rtl_insns.back().canonical_pattern;
    mut_b.has_trailing_barrier = true;
    mut_b.successors = {exit_idx};
  }

  // When `.12.dbr` ended in a backward jump and interior `return_internal`
  // blocks were un-expanded (either in Stage 1 `make_return_insns` or above in
  // `jump2`), `jump2` had deleted the trailing `end_of_function_label` and its
  // `(use (reg/i ...))` once all interior jumps became `RETURN`. Restore
  // `(use (reg/i:<mode> <regno>))` in the trailing exit block if `cfg` marks a
  // return register with `REG_FUNCTION_VALUE_P` (`reg/i`) and has no existing
  // return `USE`.
  if (!cfg->rpo_blocks.empty() && cfg->rpo_blocks.back().successors.empty()) {
    bool has_existing_ret_use = false;
    bool saw_r2 = false;
    bool saw_r3 = false;
    bool saw_r2_di = false;
    bool saw_f0 = false;
    bool saw_f1 = false;
    bool saw_f0_df = false;
    for (const RtlBasicBlock& blk : cfg->rpo_blocks) {
      for (const std::string& pat_s : blk.instructions) {
        const auto parsed = ParsePatternAndNotes(pat_s);
        if (!parsed.ok()) {
          continue;
        }
        if (parsed->pattern.op == RtlOp::kUse) {
          parsed->pattern.Visit([&](const RtlExpr& node) {
            if (node.IsHardReg()) {
              const int r = node.RegNo();
              if (r == 2 || r == 3 || r == 32 || r == 33) {
                has_existing_ret_use = true;
              }
            }
          });
        }
        parsed->pattern.Visit([&](const RtlExpr& node) {
          if (!node.IsHardReg() || !node.flags.integrated) {
            return;
          }
          const int r = node.RegNo();
          if (r == 2) {
            saw_r2 = true;
            if (node.mode == RtlMachineMode::kDi) {
              saw_r2_di = true;
            }
          } else if (r == 3) {
            saw_r3 = true;
          } else if (r == 32) {
            saw_f0 = true;
            if (node.mode == RtlMachineMode::kDf) {
              saw_f0_df = true;
            }
          } else if (r == 33) {
            saw_f1 = true;
          }
        });
      }
    }
    if (!has_existing_ret_use && (saw_r2 || saw_r3 || saw_f0 || saw_f1)) {
      const int ret_regno = (saw_r2 || saw_r3) ? 2 : 32;
      const RtlMachineMode ret_mode =
          (ret_regno == 2) ? ((saw_r3 || saw_r2_di) ? RtlMachineMode::kDi : RtlMachineMode::kSi)
                           : ((saw_f1 || saw_f0_df) ? RtlMachineMode::kDf : RtlMachineMode::kSf);
      RtlBasicBlock& exit_blk = cfg->rpo_blocks.back();
      RtlInsn ret_use_insn;
      ret_use_insn.SetFromExpr(
          RtlExpr::MakeUse(RtlExpr::MakeReg(ret_mode, ret_regno, RtlFlags{.integrated = true})));
      exit_blk.AddInsn(std::move(ret_use_insn));
    }
  }

  cfg->sideband.saved_gpr_regnos.erase(
      std::remove(cfg->sideband.saved_gpr_regnos.begin(), cfg->sideband.saved_gpr_regnos.end(),
                  kReturnAddressRegno),
      cfg->sideband.saved_gpr_regnos.end());
  RecomputePostReloadLiveAtStart(cfg, profile);
  return true;
}

// Inverse of chained `jump.c:1978, 2371-2560` (`find_cross_jump(insn,
// JUMP_LABEL(insn), 1, ...)` + `do_cross_jump`, where `jump.c:2392` skips
// `CODE_LABEL`s in `i2` and `jump.c:2469-2470` compares `PATTERN`s regardless
// of `REG_EQUIV` notes): when a pure fallthrough block `join_b` (no jump or
// return, single fallthrough successor `next_b` with a label and non-bare-return
// active head) starts with a memory store `(set (mem ...) (reg hr))` consuming
// a live-in hard register `hr` defined in every predecessor, duplicates `join_b`'s
// entire body into all predecessors, redirects jumping predecessors to `next_b`,
// and absorbs `join_b` in reverse RPO order so chained shared store tails unwind
// completely.
void UnCrossJumpChainedPureFallthroughStoreTails(RtlControlFlowGraph* cfg, bool gp64, bool fp64,
                                                 bool runs_sched2) {
  const size_t num_blocks = cfg->rpo_blocks.size();
  if (num_blocks < 3) {
    return;
  }

  auto is_cross_jumpable_body_insn = [](std::string_view pat) {
    auto parsed = ParsePatternAndNotes(pat);
    if (!parsed.ok()) {
      return false;
    }
    const RtlExpr& expr = parsed->pattern;
    if (IsEpilogueOrControlTopExpr(expr) || expr.op == RtlOp::kNote || expr.op == RtlOp::kAddrVec ||
        expr.op == RtlOp::kAddrDiffVec || expr.op == RtlOp::kConstInt ||
        expr.op == RtlOp::kAsmInput || expr.op == RtlOp::kAsmOperands ||
        expr.op == RtlOp::kSequence) {
      return false;
    }
    return !IsNoopRegMoveExpr(expr) && !ContainsLabelRefInParsedInsn(*parsed);
  };

  auto block_defines_hard_reg = [](const RtlBasicBlock& blk, int hr) {
    for (size_t rev = blk.instructions.size(); rev > 0; --rev) {
      auto parsed = ParsePatternAndNotes(blk.instructions[rev - 1]);
      if (!parsed.ok()) {
        continue;
      }
      const RtlExpr& pat = parsed->pattern;
      if (IsSimpleUncondJumpExpr(pat)) {
        continue;
      }
      if (pat.IsSet() && pat.SetDest().IsHardReg() && pat.SetDest().RegNo() == hr) {
        return true;
      }
      if (pat.IsParallel()) {
        for (const RtlExpr& ch : pat.children) {
          if (ch.IsSet() && ch.SetDest().IsHardReg() && ch.SetDest().RegNo() == hr) {
            return true;
          }
        }
      }
    }
    return false;
  };

  std::vector<bool> is_addr_vec_target(num_blocks, false);
  for (size_t b = 0; b < num_blocks; ++b) {
    for (size_t t : cfg->rpo_blocks[b].addr_vec_targets) {
      if (t < num_blocks) {
        is_addr_vec_target[t] = true;
      }
    }
  }

  std::vector<bool> absorbed(num_blocks, false);
  bool any_absorbed = false;

  for (size_t join_b = num_blocks - 1; join_b > 0; --join_b) {
    if (absorbed[join_b] || is_addr_vec_target[join_b]) {
      continue;
    }
    const auto& join_blk = cfg->rpo_blocks[join_b];
    if (!join_blk.has_label || join_blk.instructions.empty() ||
        !join_blk.addr_vec_targets.empty() || join_blk.successors.size() != 1) {
      continue;
    }
    const size_t next_b = join_blk.successors[0];
    // `next_b` must be the immediate next non-absorbed block in layout order
    // (fallthrough successor) and a labeled block whose head is neither a bare
    // `(return)` nor an unconditional jump (so `jump_optimize` does not inline
    // `(return)` or thread past `next_b` before `jump.c:1978` `find_cross_jump`).
    size_t layout_next = join_b + 1;
    while (layout_next < num_blocks && absorbed[layout_next]) {
      ++layout_next;
    }
    if (next_b != layout_next || next_b >= num_blocks || !cfg->rpo_blocks[next_b].has_label ||
        cfg->rpo_blocks[next_b].instructions.empty()) {
      continue;
    }
    {
      auto next_head = ParsePatternAndNotes(cfg->rpo_blocks[next_b].instructions.front());
      if (!next_head.ok() || next_head->pattern.op == RtlOp::kReturn ||
          IsSimpleUncondJumpExpr(next_head->pattern)) {
        continue;
      }
    }

    // Verify all instructions in `join_b` are cross-jumpable body insns and that
    // `join_b[0]` consumes a dying live-in hard register `consumed_hr` defined
    // in every predecessor — either a memory store `(set (mem ...) (reg hr))` or
    // a non-copy register update `(set (reg dst_hr) <expr reading hr>)` where
    // `dst_hr` is preserved across predecessors (`jump.c:1978, 2371-2470`).
    bool all_body_ok = true;
    for (const std::string& pat : join_blk.instructions) {
      if (!is_cross_jumpable_body_insn(pat)) {
        all_body_ok = false;
        break;
      }
    }
    if (!all_body_ok) {
      continue;
    }
    auto head_parsed = ParsePatternAndNotes(join_blk.instructions.front());
    if (!head_parsed.ok() || !head_parsed->pattern.IsSet()) {
      continue;
    }
    int consumed_hr = -1;
    int preserved_dst_hr = -1;
    if (head_parsed->pattern.SetDest().op == RtlOp::kMem &&
        head_parsed->pattern.SetSrc().IsHardReg()) {
      consumed_hr = head_parsed->pattern.SetSrc().RegNo();
    } else if (head_parsed->pattern.SetDest().IsHardReg() &&
               !head_parsed->pattern.SetDest().flags.integrated &&
               head_parsed->pattern.SetSrc().op != RtlOp::kReg) {
      preserved_dst_hr = head_parsed->pattern.SetDest().RegNo();
      int dead_cand = -1;
      int dead_count = 0;
      for (const RtlRegNote& n : head_parsed->reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg()) {
          const int dr = n.datum.RegNo();
          if (dr != preserved_dst_hr && dr >= 2 && dr < 64 && dr != 28 && dr != 29) {
            dead_cand = dr;
            ++dead_count;
          }
        }
      }
      if (dead_count == 1) {
        bool src_reads_dead = false;
        head_parsed->pattern.SetSrc().Visit([&](const RtlExpr& node) {
          if (node.IsHardReg() && node.RegNo() == dead_cand) {
            src_reads_dead = true;
          }
        });
        if (src_reads_dead) {
          consumed_hr = dead_cand;
        }
      }
    }
    if (consumed_hr < 2 || consumed_hr >= 64 || consumed_hr == 28 || consumed_hr == 29) {
      continue;
    }
    const bool head_kills_consumed_hr = std::any_of(
        head_parsed->reg_notes.begin(), head_parsed->reg_notes.end(), [&](const RtlRegNote& n) {
          return n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg() &&
                 n.datum.RegNo() == consumed_hr;
        });
    if (!head_kills_consumed_hr) {
      continue;
    }

    // Collect active predecessors of `join_b` and verify exact `jump.c:1978`
    // fallthrough + forward simple-unconditional-jump preconditions.
    std::vector<size_t> preds;
    for (size_t b = 0; b < num_blocks; ++b) {
      if (absorbed[b]) {
        continue;
      }
      for (size_t s : cfg->rpo_blocks[b].successors) {
        if (s == join_b) {
          preds.push_back(b);
          break;
        }
      }
    }
    if (preds.size() < 2) {
      continue;
    }
    size_t layout_prev = join_b;
    while (layout_prev > 0 && absorbed[layout_prev - 1]) {
      --layout_prev;
    }
    if (layout_prev == 0) {
      continue;
    }
    const size_t fallthrough_pred = layout_prev - 1;
    bool has_fallthrough_pred = false;
    bool all_preds_valid = true;
    for (size_t pred_b : preds) {
      if (pred_b >= join_b) {
        all_preds_valid = false;
        break;
      }
      const auto& pred_blk = cfg->rpo_blocks[pred_b];
      if (pred_blk.instructions.empty() || pred_blk.successors.size() != 1 ||
          pred_blk.successors[0] != join_b || !pred_blk.addr_vec_targets.empty() ||
          !block_defines_hard_reg(pred_blk, consumed_hr) ||
          (preserved_dst_hr >= 0 && block_defines_hard_reg(pred_blk, preserved_dst_hr))) {
        all_preds_valid = false;
        break;
      }
      for (size_t i = 0; i + 1 < pred_blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(pred_blk.instructions[i]);
        if (!parsed.ok() || ContainsLabelRefInParsedInsn(*parsed)) {
          all_preds_valid = false;
          break;
        }
      }
      if (!all_preds_valid) {
        break;
      }
      if (pred_b == fallthrough_pred && !IsAnyJumpOrReturnInsn(pred_blk.instructions.back())) {
        auto parsed_last = ParsePatternAndNotes(pred_blk.instructions.back());
        if (!parsed_last.ok() || ContainsLabelRefInParsedInsn(*parsed_last)) {
          all_preds_valid = false;
          break;
        }
        has_fallthrough_pred = true;
      } else if (!IsSimpleUncondJumpInsn(pred_blk.instructions.back())) {
        all_preds_valid = false;
        break;
      }
    }
    if (!all_preds_valid || !has_fallthrough_pred) {
      continue;
    }

    if (cfg->rpo_blocks[join_b].rtl_insns.size() != cfg->rpo_blocks[join_b].instructions.size()) {
      cfg->rpo_blocks[join_b].SyncRtlInsnsFromInstructions();
    }
    const std::vector<std::string> tail_insns = cfg->rpo_blocks[join_b].instructions;
    const std::vector<RtlInsn> tail_rtl_insns = cfg->rpo_blocks[join_b].rtl_insns;
    // Per `sched.c:1488, 2428` and `config/mips/mips.md:161` (`store` ready
    // delay = 1): when every active insn in `tail_insns` and in each predecessor
    // is a 1-cycle integer ALU/const op or a memory store (zero multi-cycle
    // loads/mul/div/FP ops), all instructions in every expanded predecessor have
    // `priority == 1`, so `sched2` preserves order identically via `INSN_LUID`.
    auto is_unit_latency_alu_or_store = [&](std::string_view pat) {
      const InsnSchedSummary s = SummarizeInsnForSched2(pat, gp64, fp64);
      return s.valid && !s.is_call &&
             (s.is_single_cycle_int_alu || (s.touches_mem && s.written_regs.none()));
    };
    bool sched2_safe = true;
    for (const std::string& pat : tail_insns) {
      if (!is_unit_latency_alu_or_store(pat)) {
        sched2_safe = false;
        break;
      }
    }
    if (sched2_safe && runs_sched2) {
      for (size_t pred_b : preds) {
        for (const std::string& pat : cfg->rpo_blocks[pred_b].instructions) {
          auto parsed = ParsePatternAndNotes(pat);
          if (!parsed.ok() || parsed->pattern.op == RtlOp::kNote ||
              IsEpilogueOrControlTopExpr(parsed->pattern)) {
            continue;
          }
          if (!is_unit_latency_alu_or_store(pat)) {
            sched2_safe = false;
            break;
          }
        }
        if (!sched2_safe) {
          break;
        }
      }
    }
    if (!sched2_safe) {
      continue;
    }

    for (size_t pred_b : preds) {
      auto& pred_blk = cfg->rpo_blocks[pred_b];
      if (pred_blk.rtl_insns.size() != pred_blk.instructions.size()) {
        pred_blk.SyncRtlInsnsFromInstructions();
      }
      if (IsSimpleUncondJumpInsn(pred_blk.instructions.back())) {
        std::string jmp = std::move(pred_blk.instructions.back());
        RtlInsn jmp_rinsn = std::move(pred_blk.rtl_insns.back());
        pred_blk.instructions.pop_back();
        pred_blk.rtl_insns.pop_back();
        for (size_t k = 0; k < tail_insns.size(); ++k) {
          pred_blk.instructions.push_back(tail_insns[k]);
          pred_blk.rtl_insns.push_back(tail_rtl_insns[k]);
        }
        pred_blk.instructions.push_back(std::move(jmp));
        pred_blk.rtl_insns.push_back(std::move(jmp_rinsn));
      } else {
        for (size_t k = 0; k < tail_insns.size(); ++k) {
          pred_blk.instructions.push_back(tail_insns[k]);
          pred_blk.rtl_insns.push_back(tail_rtl_insns[k]);
        }
      }
      pred_blk.successors[0] = next_b;
    }
    absorbed[join_b] = true;
    any_absorbed = true;
  }

  if (!any_absorbed) {
    return;
  }

  std::vector<size_t> old_to_new(num_blocks, num_blocks);
  std::vector<RtlBasicBlock> compacted;
  compacted.reserve(num_blocks);
  for (size_t b = 0; b < num_blocks; ++b) {
    if (absorbed[b]) {
      continue;
    }
    old_to_new[b] = compacted.size();
    compacted.push_back(std::move(cfg->rpo_blocks[b]));
  }
  for (size_t nb = 0; nb < compacted.size(); ++nb) {
    auto& blk = compacted[nb];
    blk.id = static_cast<int>(nb);
    blk.predecessors.clear();
    for (size_t& s : blk.successors) {
      if (s < num_blocks && old_to_new[s] < compacted.size()) {
        s = old_to_new[s];
      }
    }
    for (size_t& t : blk.addr_vec_targets) {
      if (t < num_blocks && old_to_new[t] < compacted.size()) {
        t = old_to_new[t];
      }
    }
  }
  for (size_t nb = 0; nb < compacted.size(); ++nb) {
    for (size_t s : compacted[nb].successors) {
      if (s < compacted.size()) {
        compacted[s].predecessors.push_back(nb);
      }
    }
  }
  cfg->rpo_blocks = std::move(compacted);
}

}  // namespace

RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) {
  return InvertDoCrossJump(jump2_cfg_x, CompilationProfile{.optimize_level = 2});
}

RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                      const CompilationProfile& profile) {
  if (!IsValidCfg(jump2_cfg_x, CfgEmptyBlockPolicy::kAllowTrailingTerminalEmptyBlockOnly)) {
    return RtlControlFlowGraph{};
  }

  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  const bool is_optimized = flags.opt_level > 0;
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(jump2_cfg_x, &profile, &gp64, &fp64);

  RtlControlFlowGraph out = jump2_cfg_x;
  out.pass = RtlPassKind::kSched2;
  std::vector<size_t> uncross_copied_start_idx;

  if (is_optimized) {
    UnCrossJumpChainedPureFallthroughStoreTails(&out, gp64, fp64, flags.RunsSched2());
    const size_t num_blocks = out.rpo_blocks.size();
    uncross_copied_start_idx.assign(num_blocks, std::string::npos);
    std::vector<std::vector<size_t>> preds(num_blocks);
    std::vector<bool> is_addr_vec_target(num_blocks, false);
    std::vector<bool> modified_blocks(num_blocks, false);
    for (size_t b = 0; b < num_blocks; ++b) {
      for (size_t s : out.rpo_blocks[b].successors) {
        preds[s].push_back(b);
      }
      for (size_t t : out.rpo_blocks[b].addr_vec_targets) {
        is_addr_vec_target[t] = true;
      }
    }

    for (size_t join_b = 1; join_b < num_blocks; ++join_b) {
      if (preds[join_b].size() < 2 || is_addr_vec_target[join_b] || modified_blocks[join_b]) {
        continue;
      }
      bool pred_already_modified = false;
      for (size_t pred_b : preds[join_b]) {
        if (modified_blocks[pred_b]) {
          pred_already_modified = true;
          break;
        }
      }
      if (pred_already_modified) {
        continue;
      }
      const auto& join_blk = out.rpo_blocks[join_b];
      if (join_blk.instructions.empty()) {
        continue;
      }

      // For `jump_optimize` (`find_cross_jump` + `do_cross_jump`) to merge
      // duplicated instructions from all predecessors back into `join_b` and
      // delete the split label, `join_b - 1` must be a single-successor
      // fallthrough predecessor of `join_b`, and every other predecessor of
      // `join_b` must be a forward (`pred_b < join_b`) single-successor block
      // ending with a simple unconditional jump `(set (pc) (label_ref ...))`
      // to `join_b`.
      bool has_fallthrough_pred = false;
      bool all_preds_valid = true;
      for (size_t pred_b : preds[join_b]) {
        if (pred_b >= join_b) {
          all_preds_valid = false;
          break;
        }
        const auto& pred_blk = out.rpo_blocks[pred_b];
        if (pred_blk.instructions.empty() || pred_blk.successors.size() != 1 ||
            pred_blk.successors[0] != join_b || !pred_blk.addr_vec_targets.empty()) {
          all_preds_valid = false;
          break;
        }
        // Ensure no non-terminator instruction in `pred_blk` references a label.
        for (size_t i = 0; i + 1 < pred_blk.instructions.size(); ++i) {
          auto parsed = ParsePatternAndNotes(pred_blk.instructions[i]);
          if (!parsed.ok() || ContainsLabelRefInParsedInsn(*parsed)) {
            all_preds_valid = false;
            break;
          }
        }
        if (!all_preds_valid) {
          break;
        }
        if (pred_b + 1 == join_b && !IsAnyJumpOrReturnInsn(pred_blk.instructions.back())) {
          auto parsed_last = ParsePatternAndNotes(pred_blk.instructions.back());
          if (!parsed_last.ok() || ContainsLabelRefInParsedInsn(*parsed_last)) {
            all_preds_valid = false;
            break;
          }
          has_fallthrough_pred = true;
        } else if (!IsSimpleUncondJumpInsn(pred_blk.instructions.back())) {
          all_preds_valid = false;
          break;
        }
      }
      if (!all_preds_valid || !has_fallthrough_pred) {
        continue;
      }

      // Partition `join_blk.instructions` into leading shareable cross-jump
      // instructions and trailing epilogue/control instructions. Require at
      // least 2 shareable instructions so that duplicating all but the last
      // into predecessors still leaves at least 1 active non-jump, non-return
      // instruction at the head of `join_b` (preventing `jump_optimize` from
      // redirecting jumps to `RETURN` or threading them past `join_b` before
      // `find_cross_jump` runs).
      if (out.rpo_blocks[join_b].rtl_insns.size() != out.rpo_blocks[join_b].instructions.size()) {
        out.rpo_blocks[join_b].SyncRtlInsnsFromInstructions();
      }
      std::vector<std::string> shared_insns;
      std::vector<RtlInsn> shared_rtl_insns;
      std::vector<std::string> epilogue_insns;
      std::vector<RtlInsn> epilogue_rtl_insns;
      for (size_t idx = 0; idx < join_blk.instructions.size(); ++idx) {
        const std::string& pat = join_blk.instructions[idx];
        const RtlInsn& rinsn = join_blk.rtl_insns[idx];
        if (epilogue_insns.empty() && IsShareableCrossJumpInsn(pat)) {
          shared_insns.push_back(pat);
          shared_rtl_insns.push_back(rinsn);
        } else {
          epilogue_insns.push_back(pat);
          epilogue_rtl_insns.push_back(rinsn);
        }
      }
      // When `preds[join_b].size() == 2` (1 unconditional jump + 1 fallthrough),
      // `jump.c:1978` calls `find_cross_jump(insn, JUMP_LABEL(insn), 1, ...)`
      // (`minimum = 1`). If `epilogue_insns` already begins with an active
      // non-control instruction stopped solely by `HasNonShareableEqualOrEquivNote`,
      // duplicating even 1 shared instruction still leaves an active non-control
      // instruction at the head of `join_b`.
      bool epilogue_has_active_head = false;
      if (preds[join_b].size() == 2 && !epilogue_insns.empty()) {
        if (auto ep_parsed = ParsePatternAndNotes(epilogue_insns.front()); ep_parsed.ok()) {
          const RtlExpr& expr = ep_parsed->pattern;
          epilogue_has_active_head = !IsEpilogueOrControlTopExpr(expr) && expr.op != RtlOp::kNote &&
                                     expr.op != RtlOp::kAddrVec && expr.op != RtlOp::kAddrDiffVec &&
                                     expr.op != RtlOp::kConstInt && expr.op != RtlOp::kAsmInput &&
                                     expr.op != RtlOp::kAsmOperands &&
                                     expr.op != RtlOp::kSequence && !IsNoopRegMoveExpr(expr) &&
                                     !ContainsLabelRefInParsedInsn(*ep_parsed) &&
                                     HasNonShareableEqualOrEquivNote(*ep_parsed);
        }
      }
      if (epilogue_has_active_head && shared_insns.size() == 1) {
        // When the single cross-jumped instruction is a memory store `(set (mem ...) (reg hr))`,
        // `local-alloc.c:update_equiv_regs` attaches `REG_EQUIV (mem ...)` to each predecessor's
        // defining instruction of `hr` only if that store originally lived in each predecessor.
        if (auto sh_parsed = ParsePatternAndNotes(shared_insns[0]);
            sh_parsed.ok() && sh_parsed->pattern.IsSet() &&
            sh_parsed->pattern.SetDest().op == RtlOp::kMem &&
            sh_parsed->pattern.SetSrc().IsHardReg()) {
          const int stored_hr = sh_parsed->pattern.SetSrc().RegNo();
          const RtlExpr& stored_mem = sh_parsed->pattern.SetDest();
          for (size_t pred_b : preds[join_b]) {
            const auto& pred_blk = out.rpo_blocks[pred_b];
            bool found_matching_equiv = false;
            for (size_t rev = pred_blk.instructions.size(); rev > 0; --rev) {
              auto pp = ParsePatternAndNotes(pred_blk.instructions[rev - 1]);
              if (!pp.ok() || !pp->pattern.IsSet() || !pp->pattern.SetDest().IsHardReg() ||
                  pp->pattern.SetDest().RegNo() != stored_hr) {
                continue;
              }
              for (const RtlRegNote& note : pp->reg_notes) {
                if (note.kind == RtlMachineMode::kRegEquiv && note.datum == stored_mem) {
                  found_matching_equiv = true;
                  break;
                }
              }
              break;
            }
            if (!found_matching_equiv) {
              epilogue_has_active_head = false;
              break;
            }
          }
        }
      }
      if (epilogue_has_active_head) {
        if (shared_insns.empty()) {
          continue;
        }
      } else {
        if (shared_insns.size() < 2) {
          continue;
        }
        epilogue_insns.insert(epilogue_insns.begin(), shared_insns.back());
        epilogue_rtl_insns.insert(epilogue_rtl_insns.begin(), shared_rtl_insns.back());
        shared_insns.pop_back();
        shared_rtl_insns.pop_back();
      }
      bool is_fp_consumer_prefix = false;
      const size_t safe_len =
          SelectSched2SafeSharedPrefixLen(preds[join_b], out, shared_insns, epilogue_insns, gp64,
                                          fp64, flags.RunsSched2(), &is_fp_consumer_prefix);
      if (safe_len == 0) {
        continue;
      }
      if (safe_len < shared_insns.size()) {
        epilogue_insns.insert(epilogue_insns.begin(),
                              shared_insns.begin() + static_cast<ptrdiff_t>(safe_len),
                              shared_insns.end());
        epilogue_rtl_insns.insert(epilogue_rtl_insns.begin(),
                                  shared_rtl_insns.begin() + static_cast<ptrdiff_t>(safe_len),
                                  shared_rtl_insns.end());
        shared_insns.resize(safe_len);
        shared_rtl_insns.resize(safe_len);
      }
      if (shared_insns.empty() || epilogue_insns.empty()) {
        continue;
      }

      for (size_t pred_b : preds[join_b]) {
        auto& pred_blk = out.rpo_blocks[pred_b];
        if (pred_blk.rtl_insns.size() != pred_blk.instructions.size()) {
          pred_blk.SyncRtlInsnsFromInstructions();
        }
        size_t pred_uncross_start = 0;
        if (IsSimpleUncondJumpInsn(pred_blk.instructions.back())) {
          std::string jmp = std::move(pred_blk.instructions.back());
          RtlInsn jmp_rinsn = std::move(pred_blk.rtl_insns.back());
          pred_blk.instructions.pop_back();
          pred_blk.rtl_insns.pop_back();
          pred_uncross_start = pred_blk.instructions.size();
          uncross_copied_start_idx[pred_b] =
              std::min(uncross_copied_start_idx[pred_b], pred_uncross_start);
          for (size_t k = 0; k < shared_insns.size(); ++k) {
            pred_blk.instructions.push_back(shared_insns[k]);
            pred_blk.rtl_insns.push_back(shared_rtl_insns[k]);
          }
          pred_blk.instructions.push_back(std::move(jmp));
          pred_blk.rtl_insns.push_back(std::move(jmp_rinsn));
        } else {
          pred_uncross_start = pred_blk.instructions.size();
          uncross_copied_start_idx[pred_b] =
              std::min(uncross_copied_start_idx[pred_b], pred_uncross_start);
          for (size_t k = 0; k < shared_insns.size(); ++k) {
            pred_blk.instructions.push_back(shared_insns[k]);
            pred_blk.rtl_insns.push_back(shared_rtl_insns[k]);
          }
        }
        if (is_fp_consumer_prefix) {
          SinkPreExistingGprAluPastFpArithInUncrossedPred(&pred_blk, pred_uncross_start, gp64,
                                                          fp64);
        }
        modified_blocks[pred_b] = true;
      }
      out.rpo_blocks[join_b].instructions = std::move(epilogue_insns);
      out.rpo_blocks[join_b].rtl_insns = std::move(epilogue_rtl_insns);
      modified_blocks[join_b] = true;
    }
  }

  SynchronizeCanonicalLabels(&out, CanonicalLabelSyncOptions{.strip_call_fusage_on_rewrite = true});
  for (auto& blk : out.rpo_blocks) {
    blk.SyncRtlInsnsFromInstructions();
  }
  RecomputePostReloadLiveAtStart(&out, profile);
  if (is_optimized) {
    RestoreNoopMovesConsumedDeaths(&out, gp64, fp64, uncross_copied_start_idx);
    RestoreDeletedNoopCallArgMoves(&out, gp64, fp64, flags.RunsSched2());
    if (!gp64 && flags.RunsSched2()) {
      RestoreDeletedNoopMdOutputReloads(&out, gp64, fp64);
    }
    for (auto& blk : out.rpo_blocks) {
      blk.SyncRtlInsnsFromInstructions();
    }
    UnexpandPostReloadTrailingReturnInsn(&out, profile);
    RestoreJump2DeletedNoopMoves(&out, profile);
  }
  return out;
}

}  // namespace rom_nom_nom::lifter
