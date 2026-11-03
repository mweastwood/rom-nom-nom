#include "lifter/rtl_cfg_builder.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "core/mips.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

std::string GprName(Register r) {
  static constexpr std::string_view kGccGprNames[32] = {
      "$0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
      "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
      "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
  };
  return std::string(kGccGprNames[static_cast<int>(r)]);
}

std::string FprName(FpRegister f) {
  return absl::StrFormat("$f%d", static_cast<int>(f));
}

RtlInsn MakeUncondJumpRtlInsn() {
  Instruction inst;
  inst.opcode = Opcode::kJ;
  inst.raw_word = 0x08000000u;
  RtlInsn out;
  out.canonical_pattern = "(set (pc) (label_ref))";
  out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  out.machine_instructions = {inst};
  return out;
}

RtlInsn MakeRegMoveRtlInsn(Register rd, Register rs) {
  Instruction inst;
  inst.opcode = Opcode::kAddu;
  inst.rd = rd;
  inst.rs = rs;
  inst.rt = Register::kZero;
  inst.raw_word = 0x00000021u;
  RtlInsn out;
  out.canonical_pattern = inst.Disassemble();
  out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  out.machine_instructions = {inst};
  return out;
}

RtlInsn MakeSelfMoveRtlInsn(std::string_view reg_name) {
  if (absl::StartsWith(reg_name, "$f")) {
    int fnum = 0;
    (void)absl::SimpleAtoi(reg_name.substr(2), &fnum);
    const FpRegister f = static_cast<FpRegister>(fnum);
    Instruction inst;
    inst.opcode = Opcode::kMovS;
    inst.fd = f;
    inst.fs = f;
    inst.raw_word = 0x46000006u;
    RtlInsn out;
    out.canonical_pattern = inst.Disassemble();
    out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
    out.machine_instructions = {inst};
    return out;
  }
  Register r = Register::kV0;
  if (reg_name == "a0") {
    r = Register::kA0;
  } else if (reg_name == "a1") {
    r = Register::kA1;
  } else if (reg_name == "a2") {
    r = Register::kA2;
  } else if (reg_name == "a3") {
    r = Register::kA3;
  } else if (reg_name == "v0") {
    r = Register::kV0;
  } else if (reg_name == "v1") {
    r = Register::kV1;
  }
  return MakeRegMoveRtlInsn(r, r);
}

bool IsGotSelfMoveInsn(const RtlInsn& insn, std::string* out_reg) {
  if (insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
      insn.machine_instructions.size() != 1) {
    return false;
  }
  const Instruction& m = insn.machine_instructions[0];
  if (m.opcode == Opcode::kAddu && m.rt == Register::kZero && m.rd.has_value() && m.rs == m.rd &&
      *m.rd != Register::kZero) {
    if (out_reg != nullptr) {
      *out_reg = GprName(*m.rd);
    }
    return true;
  }
  if ((m.opcode == Opcode::kMovS || m.opcode == Opcode::kMovD) && m.fd.has_value() &&
      m.fs == m.fd) {
    if (out_reg != nullptr) {
      *out_reg = FprName(*m.fd);
    }
    return true;
  }
  return false;
}

bool EndsWithUncondJump(const RtlBasicBlock& blk) {
  return !blk.instructions.empty() && blk.instructions.back() == "(set (pc) (label_ref))";
}

bool EndsWithCondBranch(const RtlBasicBlock& blk) {
  return !blk.instructions.empty() &&
         absl::StartsWith(blk.instructions.back(), "(set (pc) (if_then_else ");
}

bool IsCallRtlInsn(const RtlInsn& insn) {
  for (const auto& inst : insn.machine_instructions) {
    if (inst.IsCall()) {
      return true;
    }
  }
  return false;
}

bool BlockHasCall(const RtlBasicBlock& blk) {
  for (const auto& insn : blk.rtl_insns) {
    if (IsCallRtlInsn(insn)) {
      return true;
    }
  }
  return false;
}

bool BlockHasStore(const RtlBasicBlock& blk) {
  for (const auto& insn : blk.rtl_insns) {
    for (const auto& inst : insn.machine_instructions) {
      if (inst.IsStore()) {
        return true;
      }
    }
  }
  return false;
}

std::optional<Register> DefinedGprOfInst(const Instruction& inst) {
  if (inst.IsBranch() || inst.IsStore() || inst.IsReturn() || inst.opcode == Opcode::kJ ||
      inst.opcode == Opcode::kJr || inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr ||
      inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kCtc1 ||
      inst.opcode == Opcode::kMult || inst.opcode == Opcode::kMultu ||
      inst.opcode == Opcode::kDiv || inst.opcode == Opcode::kDivu || inst.IsNop()) {
    return std::nullopt;
  }
  if (inst.rd.has_value() && *inst.rd != Register::kZero) {
    return inst.rd;
  }
  if ((inst.IsLoad() || inst.opcode == Opcode::kLui || inst.opcode == Opcode::kAddiu ||
       inst.opcode == Opcode::kAddi || inst.opcode == Opcode::kOri ||
       inst.opcode == Opcode::kAndi || inst.opcode == Opcode::kXori ||
       inst.opcode == Opcode::kSlti || inst.opcode == Opcode::kSltiu ||
       inst.opcode == Opcode::kMfc1 || inst.opcode == Opcode::kCfc1) &&
      inst.rt.has_value() && *inst.rt != Register::kZero) {
    return inst.rt;
  }
  return std::nullopt;
}

bool RtlInsnDefinesReturnReg(const RtlInsn& insn) {
  for (const auto& inst : insn.machine_instructions) {
    if (DefinedGprOfInst(inst) == Register::kV0) {
      return true;
    }
    if (inst.fd == FpRegister::kF0 ||
        ((inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kLwc1 ||
          inst.opcode == Opcode::kLdc1) &&
         (inst.fs == FpRegister::kF0 || inst.ft == FpRegister::kF0))) {
      return true;
    }
  }
  return false;
}

void InsertInsnAt(RtlBasicBlock* blk, size_t idx, RtlInsn insn) {
  blk->instructions.insert(blk->instructions.begin() + static_cast<ptrdiff_t>(idx),
                           insn.canonical_pattern);
  blk->rtl_insns.insert(blk->rtl_insns.begin() + static_cast<ptrdiff_t>(idx), std::move(insn));
}

void EraseInsnAt(RtlBasicBlock* blk, size_t idx) {
  if (idx < blk->instructions.size()) {
    blk->instructions.erase(blk->instructions.begin() + static_cast<ptrdiff_t>(idx));
  }
  if (idx < blk->rtl_insns.size()) {
    blk->rtl_insns.erase(blk->rtl_insns.begin() + static_cast<ptrdiff_t>(idx));
  }
}

void PopBackInsn(RtlBasicBlock* blk) {
  if (!blk->instructions.empty()) {
    blk->instructions.pop_back();
  }
  if (!blk->rtl_insns.empty()) {
    blk->rtl_insns.pop_back();
  }
}

void AppendBlockInsns(RtlBasicBlock* dst, const RtlBasicBlock& src) {
  for (const auto& insn : src.rtl_insns) {
    dst->AddInsn(insn);
  }
}

void InsertBlockInsnsBeforeJump(RtlBasicBlock* dst, const RtlBasicBlock& src) {
  if (EndsWithUncondJump(*dst)) {
    RtlInsn jmp = dst->rtl_insns.back();
    PopBackInsn(dst);
    for (const auto& insn : src.rtl_insns) {
      if (insn.canonical_pattern != "(set (pc) (label_ref))") {
        dst->AddInsn(insn);
      }
    }
    dst->AddInsn(std::move(jmp));
  } else {
    AppendBlockInsns(dst, src);
  }
}

void RecomputeRtlCfgRpo(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const size_t n = cfg->rpo_blocks.size();
  std::vector<bool> visited(n, false);
  std::vector<size_t> post_order;
  post_order.reserve(n);
  auto dfs = [&](auto& self, size_t u) -> void {
    visited[u] = true;
    for (size_t v : cfg->rpo_blocks[u].successors) {
      if (v < n && !visited[v]) {
        self(self, v);
      }
    }
    post_order.push_back(u);
  };
  dfs(dfs, 0);
  std::vector<size_t> rpo(post_order.rbegin(), post_order.rend());
  absl::flat_hash_map<size_t, size_t> old_to_new;
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    old_to_new[rpo[idx]] = idx;
  }
  std::vector<RtlBasicBlock> new_blocks;
  new_blocks.reserve(rpo.size());
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    RtlBasicBlock blk = std::move(cfg->rpo_blocks[rpo[idx]]);
    blk.id = static_cast<uint32_t>(idx);
    std::vector<size_t> new_succs;
    new_succs.reserve(blk.successors.size());
    for (size_t s : blk.successors) {
      if (auto it = old_to_new.find(s); it != old_to_new.end()) {
        new_succs.push_back(it->second);
      }
    }
    blk.successors = std::move(new_succs);
    blk.predecessors.clear();
    new_blocks.push_back(std::move(blk));
  }
  for (size_t b = 0; b < new_blocks.size(); ++b) {
    for (size_t s : new_blocks[b].successors) {
      if (s < new_blocks.size()) {
        new_blocks[s].predecessors.push_back(b);
      }
    }
  }
  cfg->rpo_blocks = std::move(new_blocks);
}

}  // namespace rom_nom_nom
