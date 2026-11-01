#ifndef LIFTER_RTL_CFG_BUILDER_H_
#define LIFTER_RTL_CFG_BUILDER_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "core/mips.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {

// Returns the GCC 2.7.2 RTL name of GPR `r` (e.g. `"$0"`, `"v0"`, `"a0"`, `"s0"`, `"sp"`).
std::string GprName(Register r);

// Returns the GCC 2.7.2 RTL name of FPR `f` (e.g. `"$f0"`, `"$f12"`).
std::string FprName(FpRegister f);

// Constructs a synthetic unconditional jump `RtlInsn` (`(set (pc) (label_ref))`).
RtlInsn MakeUncondJumpRtlInsn();

// Constructs a synthetic GPR-to-GPR move `RtlInsn` (`addu rd, rs, $zero`).
RtlInsn MakeRegMoveRtlInsn(Register rd, Register rs);

// Constructs a synthetic self-move `RtlInsn` (`addu r, r, $zero` or `mov.s $fX, $fX`).
RtlInsn MakeSelfMoveRtlInsn(std::string_view reg_name);

// Returns true if `insn` is a single-instruction GPR or FPR self-move (`r = r`).
bool IsGotSelfMoveInsn(const RtlInsn& insn, std::string* out_reg = nullptr);

// Basic-block terminator & instruction predicates.
bool EndsWithUncondJump(const RtlBasicBlock& blk);
bool EndsWithCondBranch(const RtlBasicBlock& blk);
bool IsCallRtlInsn(const RtlInsn& insn);
bool BlockHasCall(const RtlBasicBlock& blk);
bool BlockHasStore(const RtlBasicBlock& blk);

// Returns the GPR defined by `inst`, or `std::nullopt` if `inst` does not write a non-`$zero` GPR.
std::optional<Register> DefinedGprOfInst(const Instruction& inst);

// Returns true if `insn` defines a hardware return register (`$v0` or `$f0`).
bool RtlInsnDefinesReturnReg(const RtlInsn& insn);

// Basic-block instruction list mutators that keep `instructions` and `rtl_insns` synchronized.
void InsertInsnAt(RtlBasicBlock* blk, size_t idx, RtlInsn insn);
void EraseInsnAt(RtlBasicBlock* blk, size_t idx);
void PopBackInsn(RtlBasicBlock* blk);
void AppendBlockInsns(RtlBasicBlock* dst, const RtlBasicBlock& src);
void InsertBlockInsnsBeforeJump(RtlBasicBlock* dst, const RtlBasicBlock& src);

// Recomputes Reverse Post-Order (RPO) from entry block 0 and updates `successors` / `predecessors`.
void RecomputeRtlCfgRpo(RtlControlFlowGraph* cfg);

}  // namespace rom_nom_nom

#endif  // LIFTER_RTL_CFG_BUILDER_H_
