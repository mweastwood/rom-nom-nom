#include "lifter/rtl_liveness.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kMipsStackPointerRegno = 29;
constexpr int kMipsFramePointerRegno = 30;
constexpr int kMipsReturnAddressRegno = 31;
constexpr int kMipsArgPointerRegno = 0;

bool IsCallUsedMipsHardReg(int regno) {
  // Mirrors `CALL_USED_REGISTERS` in `config/mips/mips.h:1203-1210`.
  if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
    return false;
  }
  if (regno <= 15) return true;
  if (regno >= 24 && regno <= 29) return true;
  if (regno == 31) return true;
  if (regno >= 32 && regno <= 51) return true;
  if (regno >= 64 && regno <= 67) return true;
  return false;
}

RtlMachineMode RawModeForMipsHardReg(int regno, bool gp64, bool fp64) {
  if (regno >= 32 && regno < 64) {
    return fp64 ? RtlMachineMode::kDf : RtlMachineMode::kSf;
  }
  if (regno == 67) {
    return RtlMachineMode::kCc;
  }
  return gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi;
}

int RegWordsSize(RtlMachineMode mode, bool gp64) {
  const int unit_bytes = gp64 ? 8 : 4;
  return std::max(1, (MipsModeByteSize(mode) + unit_bytes - 1) / unit_bytes);
}

bool IsReturnJumpPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.IsParallel() && !pat.children.empty() && pat.children[0].op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn && !pat.children[0].children.empty()) {
    return IsReturnJumpPattern(pat.children[0].children[0]);
  }
  return false;
}

bool IsStackPopConstPattern(const RtlExpr& pat) {
  // Mirrors `flow.c:1559-1568`:
  // `(set (reg sp) (plus (reg sp) (const_int C)))` has no effect on life.
  if (!pat.IsSet()) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  return dst.IsReg() && dst.RegNo() == kMipsStackPointerRegno && src.op == RtlOp::kPlus &&
         src.children.size() == 2 && src.children[0].IsReg() &&
         src.children[0].RegNo() == kMipsStackPointerRegno && src.children[1].IsConstInt();
}

bool IsVectorFormattedOp(RtlOp op) {
  return op == RtlOp::kParallel || op == RtlOp::kSequence || op == RtlOp::kUnspec ||
         op == RtlOp::kUnspecVolatile || op == RtlOp::kAddrVec || op == RtlOp::kAddrDiffVec ||
         op == RtlOp::kAsmOperands;
}

void ScanMaxRegnoInExpr(const RtlExpr& expr, bool gp64, bool fp64, int* max_seen) {
  expr.Visit([&](const RtlExpr& node) {
    if (node.IsReg()) {
      const int r = node.RegNo();
      if (r >= 0) {
        const int span =
            (r < kMipsFirstPseudoRegister) ? MipsHardRegNregs(r, node.mode, gp64, fp64) : 1;
        *max_seen = std::max(*max_seen, r + span - 1);
      }
    }
  });
}

void ScanMaxRegnoInPn(const RtlPatternAndNotes& pn, bool gp64, bool fp64, int* max_seen) {
  ScanMaxRegnoInExpr(pn.pattern, gp64, fp64, max_seen);
  for (const RtlRegNote& note : pn.reg_notes) {
    ScanMaxRegnoInExpr(note.datum, gp64, fp64, max_seen);
  }
  for (const RtlExpr& fu : pn.call_fusage) {
    ScanMaxRegnoInExpr(fu, gp64, fp64, max_seen);
  }
}

struct ParsedInsnSlot {
  bool valid = false;
  bool is_note = false;
  std::optional<RtlLoopNoteKind> loop_note;
  int forward_loop_depth = 1;
  bool is_sequence = false;
  bool is_call_seq = false;
  bool is_annulled_target_seq = false;
  RtlPatternAndNotes top_pn;
  RtlExpr seq_head_wrap;
  RtlExpr seq_slot_wrap;
  RtlPatternAndNotes head_pn;
  RtlPatternAndNotes slot_pn;
  bool is_call = false;
};

struct RegBitSet {
  std::vector<uint64_t> words;

  explicit RegBitSet(int nregs = 0) : words(static_cast<size_t>((nregs + 63) / 64), 0) {}

  void Clear() { std::fill(words.begin(), words.end(), 0); }

  bool Test(int r) const {
    if (r < 0) return false;
    const size_t w = static_cast<size_t>(r) / 64;
    if (w >= words.size()) return false;
    return ((words[w] >> (r % 64)) & uint64_t{1}) != 0;
  }

  void Set(int r) {
    if (r < 0) return;
    const size_t w = static_cast<size_t>(r) / 64;
    if (w < words.size()) {
      words[w] |= (uint64_t{1} << (r % 64));
    }
  }

  void Reset(int r) {
    if (r < 0) return;
    const size_t w = static_cast<size_t>(r) / 64;
    if (w < words.size()) {
      words[w] &= ~(uint64_t{1} << (r % 64));
    }
  }

  bool OrWith(const RegBitSet& other) {
    bool changed = false;
    const size_t n = std::min(words.size(), other.words.size());
    for (size_t i = 0; i < n; ++i) {
      const uint64_t merged = words[i] | other.words[i];
      if (merged != words[i]) {
        words[i] = merged;
        changed = true;
      }
    }
    return changed;
  }

  void ApplyDeadAndLive(const RegBitSet& dead, const RegBitSet& live) {
    const size_t n = words.size();
    for (size_t i = 0; i < n; ++i) {
      words[i] = (words[i] & ~dead.words[i]) | live.words[i];
    }
  }

  bool operator==(const RegBitSet& other) const { return words == other.words; }
  bool operator!=(const RegBitSet& other) const { return words != other.words; }
};

void StripLivenessNotes(std::vector<RtlRegNote>* notes) {
  notes->erase(std::remove_if(notes->begin(), notes->end(),
                              [](const RtlRegNote& n) {
                                return n.kind == RtlMachineMode::kRegDead ||
                                       n.kind == RtlMachineMode::kRegUnused;
                              }),
               notes->end());
}

void PrependRegNote(std::vector<RtlRegNote>* notes, RtlMachineMode kind, RtlExpr datum) {
  notes->insert(notes->begin(), RtlRegNote{
                                    .kind = kind,
                                    .list_op = RtlOp::kExprList,
                                    .datum = std::move(datum),
                                });
}

bool DeadOrSetRegnoP(const RtlPatternAndNotes& pn, bool is_call, int test_regno, bool gp64,
                     bool fp64, bool reload_completed) {
  // Mirrors `rtlanal.c:1121-1203` (`dead_or_set_regno_p`).
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind != RtlMachineMode::kRegDead || !note.datum.IsReg()) {
      continue;
    }
    const int regno = note.datum.RegNo();
    const int endregno = (regno >= kMipsFirstPseudoRegister)
                             ? (regno + 1)
                             : (regno + MipsHardRegNregs(regno, note.datum.mode, gp64, fp64));
    if (test_regno >= regno && test_regno < endregno) {
      return true;
    }
  }

  if (is_call) {
    for (const RtlExpr& fu : pn.call_fusage) {
      if (fu.op == RtlOp::kClobber && !fu.children.empty() && fu.children[0].IsReg()) {
        const RtlExpr& r = fu.children[0];
        const int regno = r.RegNo();
        const int endregno = (regno >= kMipsFirstPseudoRegister)
                                 ? (regno + 1)
                                 : (regno + MipsHardRegNregs(regno, r.mode, gp64, fp64));
        if (test_regno >= regno && test_regno < endregno) {
          return true;
        }
      }
    }
  }

  auto matches_set_or_clobber = [&](const RtlExpr& body) -> bool {
    if (body.op != RtlOp::kSet && body.op != RtlOp::kClobber) {
      return false;
    }
    if (body.children.empty()) {
      return false;
    }
    const RtlExpr* dest = &body.children[0];
    int word_off = 0;
    RtlMachineMode eff_mode = dest->mode;
    if (dest->IsSubreg() && !dest->children.empty() && dest->children[0].IsReg() &&
        RegWordsSize(dest->mode, gp64) == RegWordsSize(dest->children[0].mode, gp64)) {
      const RtlMachineMode subreg_mode = dest->mode;
      if (reload_completed && dest->children[0].RegNo() < kMipsFirstPseudoRegister) {
        word_off = static_cast<int>(dest->int_val);
        eff_mode = subreg_mode;
      } else {
        eff_mode = dest->children[0].mode;
      }
      dest = &dest->children[0];
    }
    if (!dest->IsReg()) {
      return false;
    }
    const int regno = dest->RegNo() + word_off;
    const int endregno = (regno >= kMipsFirstPseudoRegister)
                             ? (regno + 1)
                             : (regno + MipsHardRegNregs(regno, eff_mode, gp64, fp64));
    return test_regno >= regno && test_regno < endregno;
  };

  if (pn.pattern.op == RtlOp::kSet || pn.pattern.op == RtlOp::kClobber) {
    return matches_set_or_clobber(pn.pattern);
  }
  if (pn.pattern.IsParallel()) {
    for (size_t i = pn.pattern.children.size(); i > 0; --i) {
      if (matches_set_or_clobber(pn.pattern.children[i - 1])) {
        return true;
      }
    }
  }
  return false;
}

bool DeadOrSetP(const RtlPatternAndNotes& pn, bool is_call, const RtlExpr& reg_expr, bool gp64,
                bool fp64, bool reload_completed) {
  // Mirrors `rtlanal.c:1093-1115` (`dead_or_set_p`).
  if (!reg_expr.IsReg()) {
    return false;
  }
  const int regno = reg_expr.RegNo();
  const int last_regno = (regno >= kMipsFirstPseudoRegister)
                             ? regno
                             : (regno + MipsHardRegNregs(regno, reg_expr.mode, gp64, fp64) - 1);
  for (int r = regno; r <= last_regno; ++r) {
    if (!DeadOrSetRegnoP(pn, is_call, r, gp64, fp64, reload_completed)) {
      return false;
    }
  }
  return true;
}

struct FlowEnv {
  int nregs = kMipsFirstPseudoRegister;
  bool gp64 = false;
  bool fp64 = false;
  bool reload_completed = false;
  bool fp_is_fixed = true;
  bool sp_live_at_exit = true;
  bool emit_notes_to_pn = true;
  bool include_call_reg_notes = false;
  std::vector<int> reg_n_refs;
  std::vector<int> reg_n_sets;
  std::vector<int> reg_n_deaths;
  std::vector<int> reg_live_length;
  std::vector<int> reg_n_calls_crossed;
};

void MarkSet1(const FlowEnv& env, const RtlExpr& x, const RegBitSet& needed, RegBitSet* dead,
              bool final_pass, int loop_depth, RtlPatternAndNotes* working_pn,
              FlowEnv* mutable_stats) {
  // Mirrors `flow.c:1933-2137` (`mark_set_1`).
  if (x.children.empty()) {
    return;
  }
  const RtlExpr* reg = &x.children[0];
  int subreg_word_off = 0;
  RtlMachineMode effective_mode = RtlMachineMode::kVoid;
  bool had_outer_subreg = false;
  while (reg->IsSubreg() || reg->op == RtlOp::kZeroExtract || reg->op == RtlOp::kSignExtract ||
         reg->op == RtlOp::kStrictLowPart) {
    if (env.reload_completed && reg->IsSubreg() && !reg->children.empty() &&
        reg->children[0].IsReg() && reg->children[0].RegNo() < kMipsFirstPseudoRegister) {
      subreg_word_off += static_cast<int>(reg->int_val);
      if (!had_outer_subreg) {
        effective_mode = reg->mode;
        had_outer_subreg = true;
      }
    }
    if (reg->children.empty()) {
      return;
    }
    reg = &reg->children[0];
  }

  if (reg->IsReg()) {
    const int regno = reg->RegNo() + (had_outer_subreg ? subreg_word_off : 0);
    const RtlMachineMode reg_mode = had_outer_subreg ? effective_mode : reg->mode;
    const bool ignored_fp = (regno == kMipsFramePointerRegno && env.fp_is_fixed);
    const bool ignored_argp = (regno == kMipsArgPointerRegno);
    if (regno >= 0 && regno < env.nregs && !ignored_fp && !ignored_argp) {
      dead->Set(regno);
      if (regno < kMipsFirstPseudoRegister && regno == kMipsStackPointerRegno) {
        return;
      }

      const int span = (regno < kMipsFirstPseudoRegister)
                           ? MipsHardRegNregs(regno, reg_mode, env.gp64, env.fp64)
                           : 1;
      bool some_needed = needed.Test(regno);
      bool all_needed = needed.Test(regno);
      for (int k = 1; k < span && regno + k < kMipsFirstPseudoRegister; ++k) {
        dead->Set(regno + k);
        const bool bit_needed = needed.Test(regno + k);
        some_needed = some_needed || bit_needed;
        all_needed = all_needed && bit_needed;
      }

      if (final_pass && mutable_stats != nullptr) {
        if (regno < kMipsFirstPseudoRegister) {
          for (int k = 0; k < span && regno + k < kMipsFirstPseudoRegister; ++k) {
            mutable_stats->reg_n_sets[regno + k] += 1;
          }
        } else {
          mutable_stats->reg_n_sets[regno] += 1;
          mutable_stats->reg_n_refs[regno] += loop_depth;
          mutable_stats->reg_live_length[regno] += 1;
        }

        if (!all_needed) {
          if (!some_needed) {
            RtlExpr note_reg = *reg;
            if (had_outer_subreg) {
              note_reg = RtlExpr::MakeReg(reg_mode, regno, reg->flags);
            }
            if (working_pn != nullptr) {
              PrependRegNote(&working_pn->reg_notes, RtlMachineMode::kRegUnused,
                             std::move(note_reg));
            }
            mutable_stats->reg_n_deaths[regno] += 1;
          } else if (working_pn != nullptr) {
            for (int k = span - 1; k >= 0; --k) {
              if (!needed.Test(regno + k)) {
                PrependRegNote(
                    &working_pn->reg_notes, RtlMachineMode::kRegUnused,
                    RtlExpr::MakeReg(RawModeForMipsHardReg(regno + k, env.gp64, env.fp64),
                                     regno + k));
              }
            }
          }
        }
      }
    }
    return;
  }

  if (reg->op == RtlOp::kScratch && final_pass && working_pn != nullptr) {
    PrependRegNote(&working_pn->reg_notes, RtlMachineMode::kRegUnused, *reg);
  }
}

void MarkSetRegs(const FlowEnv& env, const RtlExpr& pat, const RegBitSet& needed, RegBitSet* dead,
                 bool final_pass, int loop_depth, RtlPatternAndNotes* working_pn,
                 FlowEnv* mutable_stats) {
  // Mirrors `flow.c:1907-1928` (`mark_set_regs`).
  if (pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) {
    MarkSet1(env, pat, needed, dead, final_pass, loop_depth, working_pn, mutable_stats);
  } else if (pat.IsParallel()) {
    for (size_t i = pat.children.size(); i > 0; --i) {
      const RtlExpr& elem = pat.children[i - 1];
      if (elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) {
        MarkSet1(env, elem, needed, dead, final_pass, loop_depth, working_pn, mutable_stats);
      }
    }
  }
}

void MarkUsedRegs(const FlowEnv& env, const RtlExpr& expr, const RegBitSet& needed, RegBitSet* live,
                  bool final_pass, int loop_depth, bool is_call, RtlPatternAndNotes* working_pn,
                  FlowEnv* mutable_stats) {
  // Mirrors `flow.c:2338-2676` (`mark_used_regs`).
  const RtlExpr* x = &expr;
  while (true) {
    switch (x->op) {
      case RtlOp::kLabelRef:
      case RtlOp::kSymbolRef:
      case RtlOp::kConstInt:
      case RtlOp::kConst:
      case RtlOp::kConstDouble:
      case RtlOp::kPc:
      case RtlOp::kAddrVec:
      case RtlOp::kAddrDiffVec:
      case RtlOp::kAsmInput:
      case RtlOp::kCc0:
      case RtlOp::kNote:
      case RtlOp::kScratch:
        return;

      case RtlOp::kClobber:
        if (!x->children.empty() && x->children[0].IsMem() && !x->children[0].children.empty()) {
          MarkUsedRegs(env, x->children[0].children[0], needed, live, final_pass, loop_depth,
                       is_call, working_pn, mutable_stats);
        }
        return;

      case RtlOp::kSubreg: {
        if (x->children.empty()) {
          return;
        }
        if (!x->children[0].IsReg()) {
          x = &x->children[0];
          continue;
        }
        // Mirror `flow.c:2387-2406` (`mark_used_regs`, `case SUBREG:` ->
        // `x = SUBREG_REG (x)`) and `sched.c:2937-2942` (`attach_deaths`,
        // `case SUBREG:` -> `attach_deaths (XEXP (x, 0), insn, set_p)`):
        // both pre-reload and post-reload passes unwrap `SUBREG` directly to
        // its inner `REG` without narrowing multi-word hard registers.
        x = &x->children[0];
        [[fallthrough]];
      }

      case RtlOp::kReg: {
        const int regno = x->RegNo();
        if (regno < 0 || regno >= env.nregs) {
          return;
        }
        if (!env.reload_completed || regno != kMipsArgPointerRegno) {
          live->Set(regno);
        }
        if (regno < kMipsFirstPseudoRegister) {
          if (regno == kMipsStackPointerRegno || regno == kMipsArgPointerRegno ||
              (regno == kMipsFramePointerRegno && env.fp_is_fixed)) {
            return;
          }
        }

        const int span = (regno < kMipsFirstPseudoRegister)
                             ? MipsHardRegNregs(regno, x->mode, env.gp64, env.fp64)
                             : 1;
        bool some_needed = needed.Test(regno);
        bool all_needed = needed.Test(regno);
        for (int k = 1; k < span && regno + k < kMipsFirstPseudoRegister; ++k) {
          live->Set(regno + k);
          const bool bit_needed = needed.Test(regno + k);
          some_needed = some_needed || bit_needed;
          all_needed = all_needed && bit_needed;
        }

        if (final_pass && mutable_stats != nullptr) {
          if (regno >= kMipsFirstPseudoRegister) {
            mutable_stats->reg_n_refs[regno] += loop_depth;
          }
          if (!all_needed && working_pn != nullptr &&
              !DeadOrSetP(*working_pn, is_call, *x, env.gp64, env.fp64, env.reload_completed)) {
            if (regno < kMipsFirstPseudoRegister && span > 1) {
              for (int k = 0; k < span; ++k) {
                if (DeadOrSetRegnoP(*working_pn, is_call, regno + k, env.gp64, env.fp64,
                                    env.reload_completed)) {
                  some_needed = true;
                }
              }
            }
            if (!some_needed) {
              PrependRegNote(&working_pn->reg_notes, RtlMachineMode::kRegDead, *x);
              mutable_stats->reg_n_deaths[regno] += 1;
            } else {
              for (int k = span - 1; k >= 0; --k) {
                if (!needed.Test(regno + k) &&
                    !DeadOrSetRegnoP(*working_pn, is_call, regno + k, env.gp64, env.fp64,
                                     env.reload_completed)) {
                  PrependRegNote(
                      &working_pn->reg_notes, RtlMachineMode::kRegDead,
                      RtlExpr::MakeReg(RawModeForMipsHardReg(regno + k, env.gp64, env.fp64),
                                       regno + k));
                }
              }
            }
          }
        }
        return;
      }

      case RtlOp::kSet: {
        if (x->children.size() < 2) {
          return;
        }
        const RtlExpr& test_dst = x->SetDest();
        const RtlExpr& set_src = x->SetSrc();
        if (test_dst.IsMem()) {
          if (!test_dst.children.empty()) {
            MarkUsedRegs(env, test_dst.children[0], needed, live, final_pass, loop_depth, is_call,
                         working_pn, mutable_stats);
          }
          x = &set_src;
          continue;
        }

        const RtlExpr* inner = &test_dst;
        bool mark_dest = false;
        while (inner->op == RtlOp::kStrictLowPart || inner->op == RtlOp::kZeroExtract ||
               inner->op == RtlOp::kSignExtract || inner->IsSubreg()) {
          if (inner->IsSubreg() && !inner->children.empty() &&
              ((env.reload_completed && inner->children[0].IsReg() &&
                inner->children[0].RegNo() < kMipsFirstPseudoRegister) ||
               !(RegWordsSize(inner->children[0].mode, env.gp64) >
                 RegWordsSize(inner->mode, env.gp64)))) {
            // Single-word mode change or post-reload hard-register subword write
            // overwrites whole hard register(s) and does not consume old value.
          } else {
            mark_dest = true;
          }
          if (inner->children.empty()) {
            break;
          }
          inner = &inner->children[0];
        }

        if (inner->IsReg()) {
          const int dst_regno = inner->RegNo();
          const bool ignored_fp = (dst_regno == kMipsFramePointerRegno && env.fp_is_fixed);
          const bool ignored_argp = (dst_regno == kMipsArgPointerRegno);
          if (!ignored_fp && !ignored_argp) {
            if (mark_dest) {
              MarkUsedRegs(env, set_src, needed, live, final_pass, loop_depth, is_call, working_pn,
                           mutable_stats);
              x = &test_dst;
              continue;
            }
            x = &set_src;
            continue;
          }
        }
        break;
      }

      case RtlOp::kReturn:
        if (env.sp_live_at_exit) {
          live->Set(kMipsStackPointerRegno);
        }
        break;

      default:
        break;
    }

    if (x->children.empty()) {
      return;
    }
    if (IsVectorFormattedOp(x->op)) {
      for (const RtlExpr& child : x->children) {
        MarkUsedRegs(env, child, needed, live, final_pass, loop_depth, is_call, working_pn,
                     mutable_stats);
      }
      return;
    }
    for (size_t i = x->children.size(); i > 1; --i) {
      MarkUsedRegs(env, x->children[i - 1], needed, live, final_pass, loop_depth, is_call,
                   working_pn, mutable_stats);
    }
    x = &x->children[0];
  }
}

void StepSingleInsnBackward(const FlowEnv& env, RtlPatternAndNotes* pn, bool is_call,
                            bool final_pass, int loop_depth, RegBitSet* old_live,
                            RegBitSet* maxlive, FlowEnv* mutable_stats) {
  if (pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kAddrVec ||
      pn->pattern.op == RtlOp::kAddrDiffVec) {
    return;
  }

  // Always work on a clean note list during `final_pass` so `dead_or_set_p` and
  // `reg_n_deaths` reflect freshly synthesized notes, then commit `reg_notes`
  // back to `*pn` only when `env.emit_notes_to_pn && !is_call`.
  RtlPatternAndNotes working_pn = *pn;
  if (final_pass) {
    StripLivenessNotes(&working_pn.reg_notes);
  }

  RegBitSet dead(env.nregs);
  RegBitSet live(env.nregs);

  if (!IsStackPopConstPattern(pn->pattern)) {
    bool is_reload_libcall_clobber = false;
    if (env.reload_completed && pn->pattern.op == RtlOp::kClobber) {
      // Pre-reload `emit_no_conflict_block` (`optabs.c:2488,2521`) emits a standalone
      // `(clobber (reg:M target))` tagged with `REG_LIBCALL`. When `global_alloc` coalesces
      // a `REG_NO_CONFLICT` input register into one subword of `target`, that input subword
      // remains live across the `CLOBBER` in GCC's pre-reload `flow` live sets, and `flow.c`
      // never attaches a partial-word `REG_UNUSED` note to the pre-reload pseudo `CLOBBER`.
      for (const RtlRegNote& note : pn->reg_notes) {
        if (note.kind == RtlMachineMode::kRegLibcall) {
          is_reload_libcall_clobber = true;
          break;
        }
      }
    }
    MarkSetRegs(env, pn->pattern, *old_live, &dead, final_pass, loop_depth,
                (final_pass && !is_reload_libcall_clobber) ? &working_pn : nullptr, mutable_stats);
    if (is_reload_libcall_clobber) {
      for (size_t w = 0; w < dead.words.size(); ++w) {
        dead.words[w] &= ~old_live->words[w];
      }
    }
    MarkUsedRegs(env, pn->pattern, *old_live, &live, final_pass, loop_depth, is_call,
                 final_pass ? &working_pn : nullptr, mutable_stats);

    if (is_call) {
      for (const RtlExpr& fu : pn->call_fusage) {
        if (fu.op == RtlOp::kUse && !fu.children.empty()) {
          MarkUsedRegs(env, fu.children[0], *old_live, &live, final_pass, loop_depth,
                       /*is_call=*/true, final_pass ? &working_pn : nullptr, mutable_stats);
        }
      }
      for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
        if (IsCallUsedMipsHardReg(r)) {
          dead.Set(r);
        }
      }
      live.Set(kMipsStackPointerRegno);
    }

    old_live->ApplyDeadAndLive(dead, live);
    if (env.reload_completed) {
      old_live->Reset(kMipsArgPointerRegno);
    }

    if (is_call && final_pass && maxlive != nullptr && mutable_stats != nullptr) {
      const size_t n_words = old_live->words.size();
      for (size_t w = 0; w < n_words; ++w) {
        uint64_t m = old_live->words[w] & maxlive->words[w];
        while (m != 0) {
          const int bit = __builtin_ctzll(m);
          const int regno = static_cast<int>(w * 64) + bit;
          if (regno < env.nregs) {
            mutable_stats->reg_n_calls_crossed[regno] += 1;
          }
          m &= (m - 1);
        }
      }
    }
  }

  if (final_pass && maxlive != nullptr && mutable_stats != nullptr) {
    maxlive->OrWith(live);
    const size_t n_words = old_live->words.size();
    for (size_t w = 0; w < n_words; ++w) {
      uint64_t m = old_live->words[w] & maxlive->words[w];
      while (m != 0) {
        const int bit = __builtin_ctzll(m);
        const int regno = static_cast<int>(w * 64) + bit;
        if (regno < env.nregs) {
          mutable_stats->reg_live_length[regno] += 1;
        }
        m &= (m - 1);
      }
    }
    if (env.emit_notes_to_pn && (!is_call || env.include_call_reg_notes)) {
      pn->reg_notes = std::move(working_pn.reg_notes);
    }
  }
}

void StupidMarkRefs(const RtlExpr& x, int suid, int last_call_suid, bool gp64,
                    std::vector<int>* reg_where_born, std::vector<int>* reg_where_dead,
                    std::vector<uint8_t>* regs_live, std::vector<int>* reg_n_refs,
                    std::vector<int>* reg_n_calls_crossed) {
  // Mirrors `stupid.c:412-558` (`stupid_mark_refs`) for pseudo-register stats.
  if (x.op == RtlOp::kNote || x.op == RtlOp::kAddrVec || x.op == RtlOp::kAddrDiffVec) {
    return;
  }
  if (x.op == RtlOp::kSet || x.op == RtlOp::kClobber) {
    if (!x.children.empty()) {
      const RtlExpr& dst = x.children[0];
      int regno = -1;
      if (dst.IsReg()) {
        regno = dst.RegNo();
      } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg() &&
                 dst.children[0].RegNo() >= kMipsFirstPseudoRegister) {
        regno = dst.children[0].RegNo();
      }
      if (regno >= kMipsFirstPseudoRegister &&
          static_cast<size_t>(regno) < reg_where_born->size()) {
        const int where_born = suid - (x.op == RtlOp::kClobber ? 1 : 0);
        (*reg_where_born)[regno] = where_born;
        if ((*reg_where_dead)[regno] < where_born + 2) {
          (*reg_where_dead)[regno] = where_born + 2;
          (*regs_live)[regno] = 1;
        }
        (*reg_n_refs)[regno] += 1;
        if (last_call_suid < (*reg_where_dead)[regno]) {
          (*reg_n_calls_crossed)[regno] += 1;
        }
      }
    }
    if (x.op == RtlOp::kSet && x.children.size() >= 2) {
      StupidMarkRefs(x.SetSrc(), suid, last_call_suid, gp64, reg_where_born, reg_where_dead,
                     regs_live, reg_n_refs, reg_n_calls_crossed);
      if (!x.SetDest().IsReg()) {
        StupidMarkRefs(x.SetDest(), suid, last_call_suid, gp64, reg_where_born, reg_where_dead,
                       regs_live, reg_n_refs, reg_n_calls_crossed);
      }
    }
    return;
  }
  if (x.IsReg()) {
    const int regno = x.RegNo();
    if (regno >= kMipsFirstPseudoRegister && static_cast<size_t>(regno) < reg_where_born->size()) {
      (*reg_where_born)[regno] = suid;
      (*reg_n_refs)[regno] += 1;
      if ((*regs_live)[regno] == 0) {
        (*regs_live)[regno] = 1;
        (*reg_where_dead)[regno] = suid;
      }
    }
    return;
  }
  for (size_t i = x.children.size(); i > 0; --i) {
    StupidMarkRefs(x.children[i - 1], suid, last_call_suid, gp64, reg_where_born, reg_where_dead,
                   regs_live, reg_n_refs, reg_n_calls_crossed);
  }
}

void ApplyPostSchedAndEquivLiveLength(const RtlControlFlowGraph& cfg,
                                      const ParsedCompilerFlags& parsed_flags, bool gp64, bool fp64,
                                      int nregs,
                                      const std::vector<std::vector<ParsedInsnSlot>>& block_slots,
                                      const std::vector<bool>& block_has_non_fallthrough_pred,
                                      const std::vector<RegBitSet>& live_at_start,
                                      const std::vector<RegBitSet>& live_at_end, FlowEnv* env) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  if (parsed_flags.RunsSched1()) {
    std::vector<int> sched_live_length(static_cast<size_t>(nregs), 0);
    auto sched_kill_elem = [&](const RtlExpr& elem, RegBitSet* bb_live) {
      if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
        return;
      }
      const RtlExpr* reg = &elem.children[0];
      bool subreg_p = false;
      while (reg->IsSubreg() || reg->op == RtlOp::kStrictLowPart ||
             reg->op == RtlOp::kSignExtract || reg->op == RtlOp::kZeroExtract) {
        if (!reg->IsSubreg() ||
            (!reg->children.empty() &&
             RegWordsSize(reg->children[0].mode, gp64) > RegWordsSize(reg->mode, gp64))) {
          subreg_p = true;
        }
        if (reg->children.empty()) {
          return;
        }
        reg = &reg->children[0];
      }
      if (!subreg_p && reg->IsReg()) {
        const int regno = reg->RegNo();
        if (regno >= kMipsFirstPseudoRegister && regno < nregs) {
          bb_live->Reset(regno);
        }
      }
    };

    auto sched_use_expr = [&](auto& self, const RtlExpr& x, bool set_p,
                              RegBitSet* bb_live) -> void {
      switch (x.op) {
        case RtlOp::kConstInt:
        case RtlOp::kConstDouble:
        case RtlOp::kLabelRef:
        case RtlOp::kSymbolRef:
        case RtlOp::kConst:
        case RtlOp::kPc:
        case RtlOp::kCc0:
        case RtlOp::kAddrVec:
        case RtlOp::kAddrDiffVec:
        case RtlOp::kNote:
        case RtlOp::kScratch:
          return;
        case RtlOp::kReg: {
          if (!set_p) {
            const int regno = x.RegNo();
            if (regno >= kMipsFirstPseudoRegister && regno < nregs) {
              bb_live->Set(regno);
            }
          }
          return;
        }
        case RtlOp::kMem:
          if (!x.children.empty()) {
            self(self, x.children[0], false, bb_live);
          }
          return;
        case RtlOp::kSubreg:
        case RtlOp::kStrictLowPart:
          if (!x.children.empty()) {
            self(self, x.children[0], set_p, bb_live);
          }
          return;
        case RtlOp::kZeroExtract:
        case RtlOp::kSignExtract:
          if (!x.children.empty()) {
            self(self, x.children[0], set_p, bb_live);
            for (size_t k = 1; k < x.children.size(); ++k) {
              self(self, x.children[k], false, bb_live);
            }
          }
          return;
        default:
          for (const RtlExpr& child : x.children) {
            self(self, child, false, bb_live);
          }
          return;
      }
    };

    auto is_jump_pattern = [](const RtlExpr& pat) {
      if (IsReturnJumpPattern(pat)) {
        return true;
      }
      if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
        return true;
      }
      if (pat.IsParallel() && !pat.children.empty() && pat.children[0].IsSet() &&
          pat.children[0].SetDest().op == RtlOp::kPc) {
        return true;
      }
      return false;
    };

    std::vector<int> sometimes_len(static_cast<size_t>(nregs), -1);
    std::vector<int> active_regs;

    for (size_t b = 0; b < n_blocks; ++b) {
      bool block_has_non_note = false;
      for (const auto& p : block_slots[b]) {
        if (p.valid && !p.is_note) {
          block_has_non_note = true;
          break;
        }
      }
      if (b > 0 && !block_has_non_note && cfg.rpo_blocks[b].successors.empty() &&
          !cfg.rpo_blocks[b].has_label && !block_has_non_fallthrough_pred[b]) {
        continue;
      }

      if (!block_slots[b].empty()) {
        size_t head_idx = 0;
        size_t tail_idx = block_slots[b].size() - 1;
        if (b == 0) {
          while (head_idx < tail_idx &&
                 (!block_slots[b][head_idx].valid || block_slots[b][head_idx].is_note)) {
            ++head_idx;
          }
          while (head_idx < tail_idx) {
            const ParsedInsnSlot& s = block_slots[b][head_idx];
            if (!s.valid || s.is_note || s.is_call || s.is_sequence ||
                is_jump_pattern(s.top_pn.pattern) || !s.top_pn.pattern.IsSet()) {
              break;
            }
            const RtlExpr* src = &s.top_pn.pattern.SetSrc();
            while ((src->IsSubreg() || src->op == RtlOp::kSignExtend ||
                    src->op == RtlOp::kZeroExtend || src->op == RtlOp::kSignExtract ||
                    src->op == RtlOp::kZeroExtract) &&
                   !src->children.empty()) {
              src = &src->children[0];
            }
            if (!src->IsReg() || src->RegNo() >= kMipsFirstPseudoRegister) {
              break;
            }
            ++head_idx;
          }
        }
        while (head_idx < tail_idx) {
          if (!block_slots[b][head_idx].valid || block_slots[b][head_idx].is_note) {
            ++head_idx;
          } else if (!block_slots[b][tail_idx].valid || block_slots[b][tail_idx].is_note) {
            --tail_idx;
          } else {
            break;
          }
        }
        const bool only_note = (head_idx == tail_idx && (!block_slots[b][head_idx].valid ||
                                                         block_slots[b][head_idx].is_note));
        if (!only_note) {
          RegBitSet bb_live = live_at_end[b];
          RegBitSet old_live = bb_live;
          active_regs.clear();
          for (size_t w = 0; w < bb_live.words.size(); ++w) {
            uint64_t mask = bb_live.words[w];
            while (mask != 0) {
              const int bit = __builtin_ctzll(mask);
              const int r = static_cast<int>(w * 64) + bit;
              if (r >= kMipsFirstPseudoRegister && r < nregs) {
                sometimes_len[static_cast<size_t>(r)] = 0;
                active_regs.push_back(r);
              }
              mask &= (mask - 1);
            }
          }

          for (size_t rev = tail_idx + 1; rev > head_idx; --rev) {
            const ParsedInsnSlot& s = block_slots[b][rev - 1];
            if (!s.valid || s.is_note) {
              continue;
            }
            const RtlPatternAndNotes& pn = s.top_pn;
            if (pn.pattern.op == RtlOp::kSet || pn.pattern.op == RtlOp::kClobber) {
              sched_kill_elem(pn.pattern, &bb_live);
            } else if (pn.pattern.IsParallel()) {
              for (size_t j = pn.pattern.children.size(); j > 0; --j) {
                const RtlExpr& sub = pn.pattern.children[j - 1];
                if (sub.op == RtlOp::kSet || sub.op == RtlOp::kClobber) {
                  sched_kill_elem(sub, &bb_live);
                }
              }
            }

            if (pn.pattern.IsSet()) {
              sched_use_expr(sched_use_expr, pn.pattern.SetSrc(), false, &bb_live);
              sched_use_expr(sched_use_expr, pn.pattern.SetDest(), true, &bb_live);
            } else if (pn.pattern.IsParallel()) {
              for (size_t j = pn.pattern.children.size(); j > 0; --j) {
                const RtlExpr& sub = pn.pattern.children[j - 1];
                if (sub.IsSet()) {
                  sched_use_expr(sched_use_expr, sub.SetSrc(), false, &bb_live);
                  sched_use_expr(sched_use_expr, sub.SetDest(), true, &bb_live);
                } else if (sub.op != RtlOp::kClobber) {
                  sched_use_expr(sched_use_expr, sub, false, &bb_live);
                }
              }
            } else if (pn.pattern.op == RtlOp::kClobber) {
              if (!pn.pattern.children.empty() && pn.pattern.children[0].IsMem() &&
                  !pn.pattern.children[0].children.empty()) {
                sched_use_expr(sched_use_expr, pn.pattern.children[0].children[0], false, &bb_live);
              }
            } else {
              sched_use_expr(sched_use_expr, pn.pattern, false, &bb_live);
            }

            if (s.is_call) {
              for (const RtlExpr& fu : pn.call_fusage) {
                if (!fu.children.empty()) {
                  sched_use_expr(sched_use_expr, fu.children[0], fu.op == RtlOp::kClobber,
                                 &bb_live);
                }
              }
            }

            for (size_t w = 0; w < bb_live.words.size(); ++w) {
              uint64_t diff = bb_live.words[w] & ~old_live.words[w];
              if (diff != 0) {
                old_live.words[w] |= diff;
                while (diff != 0) {
                  const int bit = __builtin_ctzll(diff);
                  const int r = static_cast<int>(w * 64) + bit;
                  if (r >= kMipsFirstPseudoRegister && r < nregs &&
                      sometimes_len[static_cast<size_t>(r)] < 0) {
                    sometimes_len[static_cast<size_t>(r)] = 0;
                    active_regs.push_back(r);
                  }
                  diff &= (diff - 1);
                }
              }
            }

            for (size_t ai = 0; ai < active_regs.size(); ++ai) {
              const int r = active_regs[ai];
              sometimes_len[static_cast<size_t>(r)] += 1;
              if (!bb_live.Test(r)) {
                sched_live_length[static_cast<size_t>(r)] += sometimes_len[static_cast<size_t>(r)];
                sometimes_len[static_cast<size_t>(r)] = -1;
                old_live.Reset(r);
                active_regs[ai] = active_regs.back();
                active_regs.pop_back();
                --ai;
              }
            }
          }

          for (int r : active_regs) {
            sched_live_length[static_cast<size_t>(r)] += sometimes_len[static_cast<size_t>(r)];
            sometimes_len[static_cast<size_t>(r)] = -1;
          }
        }
      }

      if (!cfg.rpo_blocks[b].addr_vec_targets.empty()) {
        RegBitSet tbl_live(nregs);
        for (size_t t : cfg.rpo_blocks[b].addr_vec_targets) {
          if (t < n_blocks) {
            tbl_live.OrWith(live_at_start[t]);
          }
        }
        for (size_t w = 0; w < tbl_live.words.size(); ++w) {
          uint64_t mask = tbl_live.words[w];
          while (mask != 0) {
            const int bit = __builtin_ctzll(mask);
            const int r = static_cast<int>(w * 64) + bit;
            if (r >= kMipsFirstPseudoRegister && r < nregs) {
              sched_live_length[static_cast<size_t>(r)] += 1;
            }
            mask &= (mask - 1);
          }
        }
      }
    }

    for (int r = kMipsFirstPseudoRegister; r < nregs; ++r) {
      if (sched_live_length[static_cast<size_t>(r)] > 0 &&
          env->reg_live_length[static_cast<size_t>(r)] >= 0) {
        env->reg_live_length[static_cast<size_t>(r)] = sched_live_length[static_cast<size_t>(r)];
      }
    }
  }

  // Mirrors `local-alloc.c:1019-1065` (`update_equiv_regs`): for any pseudo
  // `r >= 68` with `reg_n_sets[r] == 1` whose single-set initializing insn
  // carries a `REG_EQUIV` note at that point in the scan (excluding notes
  // attached retroactively by a later `(set (mem ...) (reg r))` store in the
  // same single basic block at lines 990-1000), `reg_live_length[r] *= 2`.
  auto extract_single_set = [](const RtlPatternAndNotes& pn, bool is_call, bool gp64_flag,
                               bool fp64_flag) -> const RtlExpr* {
    if (pn.pattern.IsSet()) {
      return &pn.pattern;
    }
    if (!pn.pattern.IsParallel()) {
      return nullptr;
    }
    const RtlExpr* found = nullptr;
    for (const RtlExpr& sub : pn.pattern.children) {
      if (sub.IsSet()) {
        if (found != nullptr) {
          if (sub.SetDest().IsReg() &&
              DeadOrSetP(pn, is_call, sub.SetDest(), gp64_flag, fp64_flag, false)) {
            continue;
          }
          if (found->SetDest().IsReg() &&
              DeadOrSetP(pn, is_call, found->SetDest(), gp64_flag, fp64_flag, false)) {
            found = &sub;
            continue;
          }
          return nullptr;
        }
        found = &sub;
      }
    }
    return found;
  };

  for (size_t b = 0; b < n_blocks; ++b) {
    for (size_t i = 0; i < block_slots[b].size(); ++i) {
      const ParsedInsnSlot& s = block_slots[b][i];
      if (!s.valid || s.is_note) {
        continue;
      }
      const RtlExpr* set = extract_single_set(s.top_pn, s.is_call, gp64, fp64);
      if (set == nullptr || !set->SetDest().IsReg()) {
        continue;
      }
      const int r = set->SetDest().RegNo();
      if (r < kMipsFirstPseudoRegister || r >= nregs ||
          env->reg_n_sets[static_cast<size_t>(r)] != 1 ||
          env->reg_live_length[static_cast<size_t>(r)] < 0) {
        continue;
      }
      const RtlExpr* equiv_datum = nullptr;
      for (const RtlRegNote& note : s.top_pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegEquiv) {
          equiv_datum = &note.datum;
          break;
        }
      }
      if (equiv_datum == nullptr) {
        continue;
      }
      bool added_by_later_store = false;
      if (equiv_datum->IsMem() && *equiv_datum != set->SetSrc() && !live_at_start[b].Test(r) &&
          !live_at_end[b].Test(r)) {
        for (size_t k = i + 1; k < block_slots[b].size(); ++k) {
          const ParsedInsnSlot& sk = block_slots[b][k];
          if (!sk.valid || sk.is_note) {
            continue;
          }
          const RtlExpr* sk_set = extract_single_set(sk.top_pn, sk.is_call, gp64, fp64);
          if (sk_set != nullptr && sk_set->SetDest().IsMem() && sk_set->SetSrc().IsReg() &&
              sk_set->SetSrc().RegNo() == r && sk_set->SetDest() == *equiv_datum) {
            added_by_later_store = true;
            break;
          }
        }
      }
      if (!added_by_later_store) {
        env->reg_live_length[static_cast<size_t>(r)] *= 2;
      }
    }
  }
}

}  // namespace

void ReconstructRtlLiveness(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                            const RtlLivenessOptions& options) {
  if (cfg == nullptr) {
    return;
  }

  struct SavedPhantomPseudo {
    int regno = 0;
    int refs = 0;
    int deaths = 0;
    int live_length = 0;
  };
  std::vector<SavedPhantomPseudo> saved_phantoms;
  if (options.preserve_phantom_pseudos) {
    for (const auto& [r, refs] : cfg->sideband.reg_n_refs) {
      if (r < kMipsFirstPseudoRegister || refs <= 0) {
        continue;
      }
      if (auto ren_it = cfg->sideband.reg_renumber.find(r);
          ren_it != cfg->sideband.reg_renumber.end() && ren_it->second >= 0) {
        continue;
      }
      if (auto set_it = cfg->sideband.reg_n_sets.find(r);
          set_it != cfg->sideband.reg_n_sets.end() && set_it->second != 0) {
        continue;
      }
      const auto d_it = cfg->sideband.reg_n_deaths.find(r);
      const auto l_it = cfg->sideband.reg_live_length.find(r);
      saved_phantoms.push_back(SavedPhantomPseudo{
          .regno = r,
          .refs = refs,
          .deaths = (d_it != cfg->sideband.reg_n_deaths.end()) ? d_it->second : 0,
          .live_length = (l_it != cfg->sideband.reg_live_length.end()) ? l_it->second : 0,
      });
    }
    std::sort(
        saved_phantoms.begin(), saved_phantoms.end(),
        [](const SavedPhantomPseudo& a, const SavedPhantomPseudo& b) { return a.regno < b.regno; });
  }

  cfg->sideband.basic_block_live_at_start.clear();
  cfg->sideband.reg_n_refs.clear();
  cfg->sideband.reg_n_sets.clear();
  cfg->sideband.reg_n_deaths.clear();
  cfg->sideband.reg_live_length.clear();
  cfg->sideband.reg_n_calls_crossed.clear();

  if (cfg->rpo_blocks.empty()) {
    cfg->sideband.max_regno = std::max(cfg->sideband.max_regno, kMipsFirstPseudoRegister);
    return;
  }

  ReconstructMissingCallFusage(cfg, profile);

  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  const ParsedCompilerFlags parsed_flags = ParseCompilationProfileFlags(profile);

  const size_t n_blocks = cfg->rpo_blocks.size();
  std::vector<std::vector<ParsedInsnSlot>> block_slots(n_blocks);
  int max_seen_regno = kMipsFirstPseudoRegister - 1;
  int running_loop_depth = 1;

  for (size_t b = 0; b < n_blocks; ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    block_slots[b].resize(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      ParsedInsnSlot& slot = block_slots[b][i];
      if (auto lk = ParseRtlLoopNoteKind(blk.instructions[i]); lk.has_value()) {
        slot.valid = true;
        slot.is_note = true;
        slot.loop_note = lk;
        if (*lk == RtlLoopNoteKind::kLoopBeg) {
          ++running_loop_depth;
        } else if (*lk == RtlLoopNoteKind::kLoopEnd) {
          running_loop_depth = std::max(1, running_loop_depth - 1);
        }
        slot.forward_loop_depth = running_loop_depth;
        continue;
      }

      auto pn = ParsePatternAndNotes(blk.instructions[i]);
      if (!pn.ok()) {
        continue;
      }
      slot.valid = true;
      slot.forward_loop_depth = running_loop_depth;
      slot.top_pn = std::move(*pn);
      if (slot.top_pn.pattern.op == RtlOp::kNote) {
        slot.is_note = true;
        continue;
      }

      const RtlInsn& src_rinsn = blk.rtl_insns[i];
      if (slot.top_pn.pattern.op == RtlOp::kSequence && slot.top_pn.pattern.children.size() == 2) {
        auto h_pn = RtlPatternAndNotes::FromSequenceSubInsn(slot.top_pn.pattern.children[0]);
        auto s_pn = RtlPatternAndNotes::FromSequenceSubInsn(slot.top_pn.pattern.children[1]);
        if (h_pn.ok() && s_pn.ok()) {
          slot.is_sequence = true;
          slot.seq_head_wrap = slot.top_pn.pattern.children[0];
          slot.seq_slot_wrap = slot.top_pn.pattern.children[1];
          slot.head_pn = std::move(*h_pn);
          slot.slot_pn = std::move(*s_pn);
          slot.is_call_seq = (slot.seq_head_wrap.op == RtlOp::kCallInsn);
          if (slot.is_call_seq && !slot.head_pn.has_explicit_call_fusage &&
              src_rinsn.has_explicit_call_fusage) {
            slot.head_pn.has_explicit_call_fusage = true;
            slot.head_pn.call_fusage = src_rinsn.call_fusage;
          }
          slot.is_annulled_target_seq =
              (slot.seq_head_wrap.op == RtlOp::kJumpInsn && slot.seq_head_wrap.flags.unchanging &&
               slot.seq_slot_wrap.flags.in_struct && i + 1 == blk.instructions.size());
          ScanMaxRegnoInPn(slot.head_pn, gp64, fp64, &max_seen_regno);
          ScanMaxRegnoInPn(slot.slot_pn, gp64, fp64, &max_seen_regno);
          continue;
        }
      }

      if (!slot.top_pn.has_explicit_call_fusage && src_rinsn.has_explicit_call_fusage) {
        slot.top_pn.has_explicit_call_fusage = true;
        slot.top_pn.call_fusage = src_rinsn.call_fusage;
      }
      slot.is_call = (src_rinsn.has_explicit_call_fusage || IsCallPattern(slot.top_pn.pattern));
      ScanMaxRegnoInPn(slot.top_pn, gp64, fp64, &max_seen_regno);
    }
  }

  std::optional<RtlPatternAndNotes> epilogue_delay_pn;
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern); ep.ok()) {
      ScanMaxRegnoInPn(*ep, gp64, fp64, &max_seen_regno);
      epilogue_delay_pn = std::move(*ep);
    }
  }

  const int nregs =
      std::max({cfg->sideband.max_regno, kMipsFirstPseudoRegister, max_seen_regno + 1});
  cfg->sideband.max_regno = nregs;

  auto restore_saved_phantoms = [&]() {
    for (const SavedPhantomPseudo& ph : saved_phantoms) {
      if (cfg->sideband.reg_n_refs.contains(ph.regno) ||
          cfg->sideband.reg_n_sets.contains(ph.regno)) {
        continue;
      }
      cfg->sideband.reg_n_refs[ph.regno] = ph.refs;
      if (ph.deaths != 0) {
        cfg->sideband.reg_n_deaths[ph.regno] = ph.deaths;
      } else {
        cfg->sideband.reg_n_deaths.erase(ph.regno);
      }
      if (ph.live_length != 0) {
        cfg->sideband.reg_live_length[ph.regno] = ph.live_length;
      }
      cfg->sideband.max_regno = std::max(cfg->sideband.max_regno, ph.regno + 1);
    }
  };

  // `-O0`: `stupid.c:stupid_life_analysis`.
  if (parsed_flags.opt_level == 0) {
    for (int r = 0; r < nregs; ++r) {
      cfg->sideband.reg_n_deaths[r] = 1;
    }

    struct FlatStupidInsn {
      const RtlPatternAndNotes* pn = nullptr;
      bool is_call = false;
    };
    std::vector<FlatStupidInsn> flat_insns;
    for (size_t b = 0; b < n_blocks; ++b) {
      bool block_mutated = false;
      for (size_t i = 0; i < block_slots[b].size(); ++i) {
        ParsedInsnSlot& slot = block_slots[b][i];
        if (!slot.valid || slot.is_note) {
          continue;
        }
        if (options.update_reg_notes) {
          if (slot.is_sequence) {
            const size_t prev_h = slot.head_pn.reg_notes.size();
            const size_t prev_s = slot.slot_pn.reg_notes.size();
            StripLivenessNotes(&slot.head_pn.reg_notes);
            StripLivenessNotes(&slot.slot_pn.reg_notes);
            if (slot.head_pn.reg_notes.size() != prev_h ||
                slot.slot_pn.reg_notes.size() != prev_s) {
              cfg->rpo_blocks[b].instructions[i] =
                  RtlExpr::MakeSequence(
                      {
                          RtlExpr::MakeInsnWrapper(slot.seq_head_wrap.op, slot.seq_head_wrap.flags,
                                                   slot.head_pn),
                          RtlExpr::MakeInsnWrapper(slot.seq_slot_wrap.op, slot.seq_slot_wrap.flags,
                                                   slot.slot_pn),
                      })
                      .ToGccPrintRtlString();
              block_mutated = true;
            }
          } else if (!slot.is_call) {
            const size_t prev_n = slot.top_pn.reg_notes.size();
            StripLivenessNotes(&slot.top_pn.reg_notes);
            if (slot.top_pn.reg_notes.size() != prev_n) {
              cfg->rpo_blocks[b].instructions[i] = slot.top_pn.ToGccPrintRtlString();
              block_mutated = true;
            }
          }
        }
        if (slot.is_sequence) {
          flat_insns.push_back(FlatStupidInsn{.pn = &slot.head_pn, .is_call = slot.is_call_seq});
          flat_insns.push_back(FlatStupidInsn{.pn = &slot.slot_pn, .is_call = false});
        } else {
          flat_insns.push_back(FlatStupidInsn{.pn = &slot.top_pn, .is_call = slot.is_call});
        }
      }
      if (block_mutated) {
        cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
      }
    }

    int last_call_suid = static_cast<int>(flat_insns.size()) + 1;
    std::vector<int> reg_where_born(static_cast<size_t>(nregs), 0);
    std::vector<int> reg_where_dead(static_cast<size_t>(nregs), 0);
    std::vector<uint8_t> regs_live(static_cast<size_t>(nregs), 0);
    std::vector<int> refs(static_cast<size_t>(nregs), 0);
    std::vector<int> calls_crossed(static_cast<size_t>(nregs), 0);

    for (size_t idx = flat_insns.size(); idx > 0; --idx) {
      const int suid = static_cast<int>(idx);
      const FlatStupidInsn& item = flat_insns[idx - 1];
      StupidMarkRefs(item.pn->pattern, suid, last_call_suid, gp64, &reg_where_born, &reg_where_dead,
                     &regs_live, &refs, &calls_crossed);
      if (item.is_call) {
        last_call_suid = suid;
        for (size_t fu_i = item.pn->call_fusage.size(); fu_i > 0; --fu_i) {
          StupidMarkRefs(item.pn->call_fusage[fu_i - 1], suid, last_call_suid, gp64,
                         &reg_where_born, &reg_where_dead, &regs_live, &refs, &calls_crossed);
        }
      }
    }

    for (int r = 0; r < nregs; ++r) {
      if (refs[r] != 0) {
        cfg->sideband.reg_n_refs[r] = refs[r];
      }
      if (calls_crossed[r] > 0) {
        cfg->sideband.reg_n_calls_crossed[r] = calls_crossed[r];
      }
    }
    restore_saved_phantoms();
    return;
  }

  // `-O1/-O2/-O3`: `flow.c:life_analysis` + `propagate_block`.
  FlowEnv env;
  env.nregs = nregs;
  env.gp64 = gp64;
  env.fp64 = fp64;
  env.reload_completed = options.reload_completed;
  env.fp_is_fixed = !options.reload_completed || cfg->sideband.frame_pointer_needed;
  env.sp_live_at_exit = true;
  env.emit_notes_to_pn = options.update_reg_notes;
  env.include_call_reg_notes = options.include_call_reg_notes;
  env.reg_n_refs.assign(static_cast<size_t>(nregs), 0);
  env.reg_n_sets.assign(static_cast<size_t>(nregs), 0);
  env.reg_n_deaths.assign(static_cast<size_t>(nregs), 0);
  env.reg_live_length.assign(static_cast<size_t>(nregs), 0);
  env.reg_n_calls_crossed.assign(static_cast<size_t>(nregs), 0);

  RegBitSet post_reload_exit_ret(nregs);
  if (options.reload_completed) {
    auto collect_integrated_ret = [&](const RtlExpr& expr) {
      expr.Visit([&](const RtlExpr& node) {
        if (node.IsReg() && node.flags.integrated) {
          const int r = node.RegNo();
          if (r == 2 || r == 3 || r == 32 || r == 33) {
            const int span = MipsHardRegNregs(r, node.mode, gp64, fp64);
            for (int k = 0; k < span && r + k < kMipsFirstPseudoRegister; ++k) {
              post_reload_exit_ret.Set(r + k);
            }
          }
        }
      });
    };
    for (size_t b = 0; b < n_blocks; ++b) {
      for (const ParsedInsnSlot& s : block_slots[b]) {
        if (!s.valid || s.is_note) continue;
        if (s.is_sequence) {
          collect_integrated_ret(s.head_pn.pattern);
          collect_integrated_ret(s.slot_pn.pattern);
        } else {
          collect_integrated_ret(s.top_pn.pattern);
        }
      }
    }
    if (epilogue_delay_pn.has_value()) {
      collect_integrated_ret(epilogue_delay_pn->pattern);
    }
  }

  std::vector<bool> block_can_exit(n_blocks, false);
  std::vector<bool> block_uses_epilogue_delay(n_blocks, false);
  for (size_t b = 0; b < n_blocks; ++b) {
    bool ends_with_ret = false;
    if (!block_slots[b].empty()) {
      for (size_t k = block_slots[b].size(); k > 0; --k) {
        const ParsedInsnSlot& s = block_slots[b][k - 1];
        if (!s.valid || s.is_note) continue;
        ends_with_ret = IsReturnJumpPattern(s.top_pn.pattern);
        break;
      }
    }
    if (!options.reload_completed) {
      block_can_exit[b] = (b + 1 == n_blocks);
    } else {
      block_can_exit[b] = cfg->rpo_blocks[b].successors.empty() || ends_with_ret;
      block_uses_epilogue_delay[b] =
          block_can_exit[b] && !ends_with_ret && epilogue_delay_pn.has_value();
    }
  }

  auto build_block_exit_live = [&](size_t b) -> RegBitSet {
    RegBitSet out(nregs);
    if (block_can_exit[b]) {
      if (env.sp_live_at_exit) {
        out.Set(kMipsStackPointerRegno);
      }
      if (!options.reload_completed || cfg->sideband.frame_pointer_needed) {
        out.Set(kMipsFramePointerRegno);
      }
      if (options.reload_completed) {
        out.Set(kMipsReturnAddressRegno);
        out.OrWith(post_reload_exit_ret);
        if (block_uses_epilogue_delay[b]) {
          RtlPatternAndNotes ep_copy = *epilogue_delay_pn;
          StepSingleInsnBackward(env, &ep_copy, /*is_call=*/false, /*final_pass=*/false,
                                 /*loop_depth=*/1, &out, /*maxlive=*/nullptr,
                                 /*mutable_stats=*/nullptr);
        }
      }
    }
    return out;
  };

  auto has_fallthrough_edge = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) return false;
    const auto& succs = cfg->rpo_blocks[b].successors;
    return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
  };

  std::vector<RegBitSet> live_at_start(n_blocks, RegBitSet(nregs));
  std::vector<RegBitSet> live_at_end(n_blocks, RegBitSet(nregs));

  auto propagate_slot_backward = [&](size_t b, ParsedInsnSlot* slot, bool final_pass,
                                     RegBitSet* cur_live, RegBitSet* maxlive) {
    if (!slot->valid || slot->is_note) {
      return;
    }
    if (!slot->is_sequence) {
      StepSingleInsnBackward(env, &slot->top_pn, slot->is_call, final_pass,
                             slot->forward_loop_depth, cur_live, maxlive,
                             final_pass ? &env : nullptr);
      return;
    }
    if (slot->is_call_seq) {
      StepSingleInsnBackward(env, &slot->head_pn, /*is_call=*/true, final_pass,
                             slot->forward_loop_depth, cur_live, maxlive,
                             final_pass ? &env : nullptr);
      StepSingleInsnBackward(env, &slot->slot_pn, /*is_call=*/false, final_pass,
                             slot->forward_loop_depth, cur_live, maxlive,
                             final_pass ? &env : nullptr);
      return;
    }
    if (slot->is_annulled_target_seq && has_fallthrough_edge(b)) {
      RegBitSet taken_live = build_block_exit_live(b);
      for (size_t sb : cfg->rpo_blocks[b].successors) {
        if (sb != b + 1 && sb < n_blocks) {
          taken_live.OrWith(live_at_start[sb]);
        }
      }
      StepSingleInsnBackward(env, &slot->slot_pn, /*is_call=*/false, final_pass,
                             slot->forward_loop_depth, &taken_live, maxlive,
                             final_pass ? &env : nullptr);
      *cur_live = taken_live;
      cur_live->OrWith(live_at_start[b + 1]);
      StepSingleInsnBackward(env, &slot->head_pn, /*is_call=*/false, final_pass,
                             slot->forward_loop_depth, cur_live, maxlive,
                             final_pass ? &env : nullptr);
      return;
    }
    StepSingleInsnBackward(env, &slot->slot_pn, /*is_call=*/false, final_pass,
                           slot->forward_loop_depth, cur_live, maxlive,
                           final_pass ? &env : nullptr);
    StepSingleInsnBackward(env, &slot->head_pn, /*is_call=*/false, final_pass,
                           slot->forward_loop_depth, cur_live, maxlive,
                           final_pass ? &env : nullptr);
  };

  // Fixed-point backward relaxation across RPO basic blocks (`flow.c:1086-1193`).
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t rev = n_blocks; rev > 0; --rev) {
      const size_t b = rev - 1;
      RegBitSet out = build_block_exit_live(b);
      for (size_t sb : cfg->rpo_blocks[b].successors) {
        if (sb < n_blocks) {
          out.OrWith(live_at_start[sb]);
        }
      }
      live_at_end[b] = out;
      RegBitSet cur = std::move(out);
      for (size_t i = block_slots[b].size(); i > 0; --i) {
        propagate_slot_backward(b, &block_slots[b][i - 1], /*final_pass=*/false, &cur,
                                /*maxlive=*/nullptr);
      }
      if (cur != live_at_start[b]) {
        live_at_start[b] = std::move(cur);
        changed = true;
      }
    }
  }

  // Final pass (`flow.c:1218-1226`): synthesize `REG_DEAD` / `REG_UNUSED` notes
  // and accumulate `reg_n_refs`, `reg_n_sets`, `reg_n_deaths`, `reg_live_length`,
  // and `reg_n_calls_crossed`.
  std::vector<bool> block_has_non_fallthrough_pred(n_blocks, false);
  for (size_t p = 0; p < n_blocks; ++p) {
    bool ends_with_uncond = false;
    for (size_t k = block_slots[p].size(); k > 0; --k) {
      const ParsedInsnSlot& s = block_slots[p][k - 1];
      if (!s.valid || s.is_note) {
        continue;
      }
      const RtlExpr& jpat = s.is_sequence ? s.head_pn.pattern : s.top_pn.pattern;
      if (jpat.IsSet() && jpat.SetDest().op == RtlOp::kPc && jpat.SetSrc().op == RtlOp::kLabelRef) {
        ends_with_uncond = true;
      }
      break;
    }
    for (size_t sb : cfg->rpo_blocks[p].successors) {
      if (sb < n_blocks && (sb != p + 1 || ends_with_uncond)) {
        block_has_non_fallthrough_pred[sb] = true;
      }
    }
    for (size_t tb : cfg->rpo_blocks[p].addr_vec_targets) {
      if (tb < n_blocks) {
        block_has_non_fallthrough_pred[tb] = true;
      }
    }
  }

  for (size_t b = 0; b < n_blocks; ++b) {
    RegBitSet cur = live_at_end[b];
    RegBitSet maxlive = cur;
    bool block_mutated = false;
    for (size_t i = block_slots[b].size(); i > 0; --i) {
      ParsedInsnSlot& slot = block_slots[b][i - 1];
      propagate_slot_backward(b, &slot, /*final_pass=*/true, &cur, &maxlive);
      if (options.update_reg_notes && slot.valid && !slot.is_note) {
        std::string formatted;
        if (slot.is_sequence) {
          RtlPatternAndNotes seq_head_pn = slot.head_pn;
          if (slot.seq_head_wrap.op == RtlOp::kCallInsn) {
            seq_head_pn.reg_notes.clear();
            seq_head_pn.has_explicit_call_fusage = false;
            seq_head_pn.call_fusage.clear();
          }
          formatted = RtlExpr::MakeSequence(
                          {
                              RtlExpr::MakeInsnWrapper(slot.seq_head_wrap.op,
                                                       slot.seq_head_wrap.flags, seq_head_pn),
                              RtlExpr::MakeInsnWrapper(slot.seq_slot_wrap.op,
                                                       slot.seq_slot_wrap.flags, slot.slot_pn),
                          })
                          .ToGccPrintRtlString();
        } else if (!slot.is_call || options.include_call_reg_notes) {
          formatted = FormatGccPrintRtlPatternAndNotes(slot.top_pn.pattern, slot.top_pn.reg_notes,
                                                       slot.top_pn.bracketed_notes,
                                                       /*call_fusage=*/{});
        }
        if (!formatted.empty() && formatted != cfg->rpo_blocks[b].instructions[i - 1]) {
          cfg->rpo_blocks[b].instructions[i - 1] = std::move(formatted);
          block_mutated = true;
        }
      }
    }
    if (block_mutated) {
      cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
      for (size_t k = 0; k < block_slots[b].size() && k < cfg->rpo_blocks[b].rtl_insns.size();
           ++k) {
        if (block_slots[b][k].top_pn.has_explicit_call_fusage &&
            !cfg->rpo_blocks[b].rtl_insns[k].has_explicit_call_fusage) {
          cfg->rpo_blocks[b].rtl_insns[k].has_explicit_call_fusage = true;
          cfg->rpo_blocks[b].rtl_insns[k].call_fusage = block_slots[b][k].top_pn.call_fusage;
        }
      }
    }

    // In `flow.c:find_basic_blocks`, each switch jump-table `(jump_insn (addr_vec [...]))`
    // forms a 1-instruction basic block whose live set is the union of its target blocks,
    // incrementing `reg_live_length` once for each register live across the tablejump.
    if (!cfg->rpo_blocks[b].addr_vec_targets.empty()) {
      RegBitSet tbl_live(nregs);
      for (size_t t : cfg->rpo_blocks[b].addr_vec_targets) {
        if (t < n_blocks) {
          tbl_live.OrWith(live_at_start[t]);
        }
      }
      for (size_t w = 0; w < tbl_live.words.size(); ++w) {
        uint64_t mask = tbl_live.words[w];
        while (mask != 0) {
          const int bit = __builtin_ctzll(mask);
          const int regno = static_cast<int>(w * 64) + bit;
          if (regno < nregs) {
            env.reg_live_length[regno] += 1;
          }
          mask &= (mask - 1);
        }
      }
    }

    // In `flow.c:find_basic_blocks`, a trailing RPO block containing only `NOTE_INSN_LOOP_*`
    // notes (`!block_has_non_note`) with no `CODE_LABEL` (`!has_label` and no non-fallthrough
    // incoming jump edge) does not form a basic block in `n_basic_blocks`.
    bool block_has_non_note = false;
    for (const auto& p : block_slots[b]) {
      if (p.valid && !p.is_note) {
        block_has_non_note = true;
        break;
      }
    }
    if (!options.reload_completed && b > 0 && !block_has_non_note &&
        cfg->rpo_blocks[b].successors.empty() && !cfg->rpo_blocks[b].has_label &&
        !block_has_non_fallthrough_pred[b]) {
      continue;
    }

    absl::flat_hash_set<int> live_set;
    for (size_t w = 0; w < live_at_start[b].words.size(); ++w) {
      uint64_t mask = live_at_start[b].words[w];
      while (mask != 0) {
        const int bit = __builtin_ctzll(mask);
        const int regno = static_cast<int>(w * 64) + bit;
        if (regno < nregs) {
          live_set.insert(regno);
        }
        mask &= (mask - 1);
      }
    }
    if (!live_set.empty()) {
      cfg->sideband.basic_block_live_at_start[static_cast<int>(b)] = std::move(live_set);
    }
  }

  if (options.apply_post_sched_and_equiv_live_length && !options.reload_completed) {
    ApplyPostSchedAndEquivLiveLength(*cfg, parsed_flags, gp64, fp64, nregs, block_slots,
                                     block_has_non_fallthrough_pred, live_at_start, live_at_end,
                                     &env);
  }

  for (int r = 0; r < nregs; ++r) {
    if (env.reg_n_refs[r] != 0) {
      cfg->sideband.reg_n_refs[r] = env.reg_n_refs[r];
    }
    if (env.reg_n_sets[r] != 0) {
      cfg->sideband.reg_n_sets[r] = env.reg_n_sets[r];
    }
    if (env.reg_n_deaths[r] != 0) {
      cfg->sideband.reg_n_deaths[r] = env.reg_n_deaths[r];
    }
    if (env.reg_live_length[r] > 0) {
      cfg->sideband.reg_live_length[r] = env.reg_live_length[r];
    }
    if (env.reg_n_calls_crossed[r] > 0) {
      cfg->sideband.reg_n_calls_crossed[r] = env.reg_n_calls_crossed[r];
    }
  }

  restore_saved_phantoms();
}

bool CanonicalizeInsnLivenessNoteOrder(RtlPatternAndNotes* pn, bool sched_note_order, bool gp64,
                                       bool fp64, bool reload_completed) {
  if (pn == nullptr || pn->reg_notes.empty()) {
    return false;
  }

  std::optional<size_t> first_liveness_pos;
  for (size_t i = 0; i < pn->reg_notes.size(); ++i) {
    const RtlMachineMode kind = pn->reg_notes[i].kind;
    if (kind == RtlMachineMode::kRegDead || kind == RtlMachineMode::kRegUnused) {
      first_liveness_pos = i;
      break;
    }
  }
  if (!first_liveness_pos.has_value()) {
    return false;
  }

  std::vector<RtlRegNote> prefix_non_liveness;
  std::vector<RtlRegNote> existing_dead;
  std::vector<RtlRegNote> existing_unused;
  std::vector<RtlRegNote> suffix_non_liveness;
  for (size_t i = 0; i < pn->reg_notes.size(); ++i) {
    const RtlRegNote& note = pn->reg_notes[i];
    if (note.kind == RtlMachineMode::kRegDead) {
      if (note.datum.IsReg() || note.datum.IsMem()) {
        existing_dead.push_back(note);
      } else if (i < *first_liveness_pos) {
        prefix_non_liveness.push_back(note);
      } else {
        suffix_non_liveness.push_back(note);
      }
    } else if (note.kind == RtlMachineMode::kRegUnused) {
      existing_unused.push_back(note);
    } else if (i < *first_liveness_pos) {
      prefix_non_liveness.push_back(note);
    } else {
      suffix_non_liveness.push_back(note);
    }
  }

  auto reg_span_len = [&](int regno, RtlMachineMode mode) -> int {
    if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
      return 1;
    }
    return std::max(1, MipsHardRegNregs(regno, mode, gp64, fp64));
  };

  // `sched.c:3573` ("Only unlink REG_DEAD notes; leave REG_UNUSED notes alone")
  // preserves the incoming relative order of `REG_UNUSED` notes verbatim.
  std::vector<RtlRegNote> reordered_unused = std::move(existing_unused);

  std::vector<RtlRegNote> reordered_dead;
  if (existing_dead.size() >= 2) {
    std::vector<bool> dead_matched(existing_dead.size(), false);
    absl::flat_hash_set<int> attached_regnos;

    auto visit_reg_read = [&](int regno, RtlMachineMode eff_mode) {
      if (regno < 0) {
        return;
      }
      const int span = reg_span_len(regno, eff_mode);
      bool all_attached = true;
      for (int j = 0; j < span; ++j) {
        if (!attached_regnos.contains(regno + j)) {
          all_attached = false;
          break;
        }
      }
      if (all_attached) {
        return;
      }
      for (size_t k = 0; k < existing_dead.size(); ++k) {
        if (!dead_matched[k] && existing_dead[k].datum.IsReg() &&
            existing_dead[k].datum.RegNo() == regno &&
            reg_span_len(regno, existing_dead[k].datum.mode) == span) {
          dead_matched[k] = true;
          for (int j = 0; j < span; ++j) {
            attached_regnos.insert(regno + j);
          }
          reordered_dead.insert(reordered_dead.begin(), existing_dead[k]);
          return;
        }
      }
      if (regno < kMipsFirstPseudoRegister && span > 1) {
        for (int j = span - 1; j >= 0; --j) {
          if (attached_regnos.contains(regno + j)) {
            continue;
          }
          for (size_t k = 0; k < existing_dead.size(); ++k) {
            if (!dead_matched[k] && existing_dead[k].datum.IsReg() &&
                existing_dead[k].datum.RegNo() == regno + j) {
              dead_matched[k] = true;
              const int nspan = reg_span_len(regno + j, existing_dead[k].datum.mode);
              for (int m = 0; m < nspan; ++m) {
                attached_regnos.insert(regno + j + m);
              }
              reordered_dead.insert(reordered_dead.begin(), existing_dead[k]);
              break;
            }
          }
        }
      }
    };

    if (sched_note_order) {
      // Mirrors `sched.c:2779-3018` (`attach_deaths` / `attach_deaths_insn`).
      auto attach_sched = [&](const auto& self, const RtlExpr& x, bool set_p) -> void {
        switch (x.op) {
          case RtlOp::kLabelRef:
          case RtlOp::kSymbolRef:
          case RtlOp::kConstInt:
          case RtlOp::kConst:
          case RtlOp::kConstDouble:
          case RtlOp::kPc:
          case RtlOp::kAddrVec:
          case RtlOp::kAddrDiffVec:
          case RtlOp::kAsmInput:
          case RtlOp::kCc0:
          case RtlOp::kNote:
          case RtlOp::kScratch:
          case RtlOp::kClobber:
            return;
          case RtlOp::kReg:
            if (!set_p) {
              visit_reg_read(x.RegNo(), x.mode);
            }
            return;
          case RtlOp::kMem:
            if (!x.children.empty()) {
              self(self, x.children[0], false);
            }
            return;
          case RtlOp::kSubreg:
            if (!x.children.empty()) {
              // Mirrors `sched.c:2937-2942` (`attach_deaths`, `case SUBREG:`).
              self(self, x.children[0], set_p);
            }
            return;
          case RtlOp::kStrictLowPart:
            if (!x.children.empty()) {
              self(self, x.children[0], set_p);
            }
            return;
          case RtlOp::kZeroExtract:
          case RtlOp::kSignExtract:
            if (x.children.size() >= 1) {
              self(self, x.children[0], set_p);
            }
            if (x.children.size() >= 2) {
              self(self, x.children[1], false);
            }
            if (x.children.size() >= 3) {
              self(self, x.children[2], false);
            }
            return;
          default:
            if (IsVectorFormattedOp(x.op)) {
              for (const RtlExpr& ch : x.children) {
                self(self, ch, false);
              }
            } else {
              for (size_t i = x.children.size(); i > 0; --i) {
                self(self, x.children[i - 1], false);
              }
            }
            return;
        }
      };

      const RtlExpr& pat = pn->pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() >= 2) {
        attach_sched(attach_sched, pat.SetSrc(), false);
        attach_sched(attach_sched, pat.SetDest(), true);
      } else if (pat.IsParallel()) {
        for (size_t i = pat.children.size(); i > 0; --i) {
          const RtlExpr& elem = pat.children[i - 1];
          if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
            attach_sched(attach_sched, elem.SetSrc(), false);
            attach_sched(attach_sched, elem.SetDest(), true);
          } else if (elem.op != RtlOp::kClobber) {
            attach_sched(attach_sched, elem, false);
          }
        }
      } else if (pat.op == RtlOp::kClobber) {
        if (!pat.children.empty() && pat.children[0].IsMem() && !pat.children[0].children.empty()) {
          attach_sched(attach_sched, pat.children[0].children[0], false);
        }
      } else {
        attach_sched(attach_sched, pat, false);
      }
      for (const RtlExpr& fu : pn->call_fusage) {
        if (!fu.children.empty()) {
          attach_sched(attach_sched, fu.children[0], fu.op == RtlOp::kClobber);
        }
      }
    } else {
      // Mirrors `flow.c:2338-2676` (`mark_used_regs`).
      auto visit_flow = [&](const auto& self, const RtlExpr& expr) -> void {
        const RtlExpr* x = &expr;
        while (true) {
          switch (x->op) {
            case RtlOp::kLabelRef:
            case RtlOp::kSymbolRef:
            case RtlOp::kConstInt:
            case RtlOp::kConst:
            case RtlOp::kConstDouble:
            case RtlOp::kPc:
            case RtlOp::kAddrVec:
            case RtlOp::kAddrDiffVec:
            case RtlOp::kAsmInput:
            case RtlOp::kCc0:
            case RtlOp::kNote:
            case RtlOp::kScratch:
              return;
            case RtlOp::kClobber:
              if (!x->children.empty() && x->children[0].IsMem() &&
                  !x->children[0].children.empty()) {
                self(self, x->children[0].children[0]);
              }
              return;
            case RtlOp::kSubreg:
              if (x->children.empty()) {
                return;
              }
              if (!x->children[0].IsReg()) {
                x = &x->children[0];
                continue;
              }
              // Mirrors `flow.c:2387-2405` (`mark_used_regs`, `case SUBREG:`).
              visit_reg_read(x->children[0].RegNo(), x->children[0].mode);
              return;
            case RtlOp::kReg:
              visit_reg_read(x->RegNo(), x->mode);
              return;
            case RtlOp::kSet: {
              if (x->children.size() < 2) {
                return;
              }
              const RtlExpr& test_dst = x->SetDest();
              const RtlExpr& set_src = x->SetSrc();
              if (test_dst.IsMem()) {
                if (!test_dst.children.empty()) {
                  self(self, test_dst.children[0]);
                }
                x = &set_src;
                continue;
              }
              const RtlExpr* inner = &test_dst;
              bool mark_dest = false;
              while (inner->op == RtlOp::kStrictLowPart || inner->op == RtlOp::kZeroExtract ||
                     inner->op == RtlOp::kSignExtract || inner->IsSubreg()) {
                if (inner->IsSubreg() && !inner->children.empty() &&
                    ((reload_completed && inner->children[0].IsReg() &&
                      inner->children[0].RegNo() < kMipsFirstPseudoRegister) ||
                     !(RegWordsSize(inner->children[0].mode, gp64) >
                       RegWordsSize(inner->mode, gp64)))) {
                } else {
                  mark_dest = true;
                }
                if (inner->children.empty()) {
                  break;
                }
                inner = &inner->children[0];
              }
              if (inner->IsReg() && inner->RegNo() != kMipsArgPointerRegno) {
                if (mark_dest) {
                  self(self, set_src);
                  x = &test_dst;
                  continue;
                }
                x = &set_src;
                continue;
              }
              break;
            }
            default:
              break;
          }
          if (x->children.empty()) {
            return;
          }
          if (IsVectorFormattedOp(x->op)) {
            for (const RtlExpr& ch : x->children) {
              self(self, ch);
            }
            return;
          }
          for (size_t i = x->children.size(); i > 1; --i) {
            self(self, x->children[i - 1]);
          }
          x = &x->children[0];
        }
      };

      visit_flow(visit_flow, pn->pattern);
      for (const RtlExpr& fu : pn->call_fusage) {
        if (fu.op == RtlOp::kUse && !fu.children.empty()) {
          visit_flow(visit_flow, fu.children[0]);
        }
      }
    }

    std::vector<RtlRegNote> merged_dead;
    merged_dead.reserve(existing_dead.size());
    size_t matched_cursor = 0;
    for (size_t k = 0; k < existing_dead.size(); ++k) {
      if (dead_matched[k] && matched_cursor < reordered_dead.size()) {
        merged_dead.push_back(std::move(reordered_dead[matched_cursor++]));
      } else {
        merged_dead.push_back(std::move(existing_dead[k]));
      }
    }
    while (matched_cursor < reordered_dead.size()) {
      merged_dead.push_back(std::move(reordered_dead[matched_cursor++]));
    }
    reordered_dead = std::move(merged_dead);
  } else {
    reordered_dead = std::move(existing_dead);
  }

  std::vector<RtlRegNote> new_notes;
  new_notes.reserve(pn->reg_notes.size());
  for (RtlRegNote& n : prefix_non_liveness) {
    new_notes.push_back(std::move(n));
  }
  for (RtlRegNote& n : reordered_dead) {
    new_notes.push_back(std::move(n));
  }
  for (RtlRegNote& n : reordered_unused) {
    new_notes.push_back(std::move(n));
  }
  for (RtlRegNote& n : suffix_non_liveness) {
    new_notes.push_back(std::move(n));
  }

  if (new_notes.size() == pn->reg_notes.size()) {
    bool identical = true;
    for (size_t i = 0; i < new_notes.size(); ++i) {
      if (new_notes[i].kind != pn->reg_notes[i].kind ||
          new_notes[i].list_op != pn->reg_notes[i].list_op ||
          new_notes[i].datum != pn->reg_notes[i].datum) {
        identical = false;
        break;
      }
    }
    if (identical) {
      return false;
    }
  }

  pn->reg_notes = std::move(new_notes);
  return true;
}

void CanonicalizeCfgLivenessNoteOrder(RtlControlFlowGraph* cfg, bool sched_note_order, bool gp64,
                                      bool fp64, bool reload_completed) {
  if (cfg == nullptr) {
    return;
  }
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    bool block_changed = false;
    std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage) {
        saved_fusage[i] = blk.rtl_insns[i].call_fusage;
      }
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      if (saved_fusage[i].has_value() && !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = *saved_fusage[i];
      }
      if (parsed->pattern.op == RtlOp::kSequence && parsed->pattern.children.size() == 2) {
        auto h = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[0]);
        auto s = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[1]);
        if (h.ok() && s.ok()) {
          const RtlExpr h_wrap = parsed->pattern.children[0];
          const RtlExpr s_wrap = parsed->pattern.children[1];
          const bool ch_h = CanonicalizeInsnLivenessNoteOrder(&(*h), sched_note_order, gp64, fp64,
                                                              reload_completed);
          const bool ch_s = CanonicalizeInsnLivenessNoteOrder(&(*s), sched_note_order, gp64, fp64,
                                                              reload_completed);
          if (ch_h || ch_s) {
            if (h_wrap.op == RtlOp::kCallInsn) {
              h->reg_notes.clear();
              h->has_explicit_call_fusage = false;
              h->call_fusage.clear();
            }
            blk.instructions[i] =
                RtlExpr::MakeSequence({
                                          RtlExpr::MakeInsnWrapper(h_wrap.op, h_wrap.flags, *h),
                                          RtlExpr::MakeInsnWrapper(s_wrap.op, s_wrap.flags, *s),
                                      })
                    .ToGccPrintRtlString();
            block_changed = true;
          }
        }
        continue;
      }
      if (CanonicalizeInsnLivenessNoteOrder(&(*parsed), sched_note_order, gp64, fp64,
                                            reload_completed)) {
        blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
            parsed->pattern, parsed->reg_notes, parsed->bracketed_notes, /*call_fusage=*/{});
        block_changed = true;
      }
    }
    if (block_changed) {
      blk.SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < blk.rtl_insns.size() && i < saved_fusage.size(); ++i) {
        if (saved_fusage[i].has_value() && !blk.rtl_insns[i].has_explicit_call_fusage) {
          blk.rtl_insns[i].has_explicit_call_fusage = true;
          blk.rtl_insns[i].call_fusage = std::move(*saved_fusage[i]);
        }
      }
    }
  }
}

void CanonicalizeCfgLivenessNoteOrder(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                                      bool sched_note_order, bool reload_completed) {
  if (cfg == nullptr) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  CanonicalizeCfgLivenessNoteOrder(cfg, sched_note_order, gp64, fp64, reload_completed);
}

}  // namespace rom_nom_nom::lifter
