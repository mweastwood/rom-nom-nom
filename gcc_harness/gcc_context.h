#ifndef GCC_HARNESS_GCC_CONTEXT_H_
#define GCC_HARNESS_GCC_CONTEXT_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "gcc_harness/rtl_cfg.h"

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {

struct rtx_def;
struct rtvec_def;

union rtunion_def {
  int rtwint;
  int rtint;
  char* rtstr;
  rtx_def* rtx;
  rtvec_def* rtvec;
  int rttype;
};

struct rtx_def {
  unsigned short code;
  unsigned int mode : 8;
  unsigned int jump : 1;
  unsigned int call : 1;
  unsigned int unchanging : 1;
  unsigned int volatil : 1;
  unsigned int in_struct : 1;
  unsigned int used : 1;
  unsigned int integrated : 1;
  rtunion_def fld[1];
};

struct rtvec_def {
  int num_elem;
  rtunion_def elem[1];
};

struct GccObstack {
  long chunk_size;
  void* chunk;
  char* object_base;
  char* next_free;
  char* chunk_limit;
  long temp;
  int alignment_mask;
  void* (*chunkfun)(void*, long);
  void (*freefun)(void*, void*);
  void* extra_arg;
  unsigned use_extra_arg : 1;
  unsigned maybe_empty_object : 1;
  unsigned alloc_failed : 1;
};

struct GasFrag {
  uint64_t fr_address;
  GasFrag* fr_next;
  long fr_fix;
  long fr_var;
  void* fr_symbol;
  long fr_offset;
  char* fr_opcode;
  void* line;
  int fr_type;
  unsigned int fr_subtype;
  unsigned short align_mask;
  unsigned short align_offset;
  char fr_pcrel_adjust;
  char fr_bsr;
  char fr_literal[1];
};

enum GccRtxCode {
  kUnknown = 0,
  kNil = 1,
  kExprList = 2,
  kInsnList = 3,
  kSequence = 19,
  kInsn = 27,
  kJumpInsn = 28,
  kCallInsn = 29,
  kBarrier = 30,
  kCodeLabel = 31,
  kNote = 32,
  kParallel = 34,
  kAddrVec = 39,
  kAddrDiffVec = 40,
  kSet = 41,
  kUse = 42,
  kClobber = 43,
  kCall = 44,
  kReturn = 45,
  kConstInt = 47,
  kConstDouble = 48,
  kPc = 51,
  kReg = 52,
  kSubreg = 54,
  kMem = 57,
  kLabelRef = 58,
  kSymbolRef = 59,
  kCc0 = 60,
  kIfThenElse = 62,
  kNumRtxCode = 116,
};

enum GccMachineMode {
  kVoidMode = 0,
  kQiMode = 1,
  kHiMode = 2,
  kSiMode = 4,
  kDiMode = 6,
  kSfMode = 12,
  kDfMode = 13,
  kCcMode = 27,
  kNumMachineModes = 32,
};

enum GccRegNote {
  kRegDead = 1,
  kRegInc = 2,
  kRegEquiv = 3,
  kRegWas0 = 4,
  kRegEqual = 5,
  kRegRetval = 6,
  kRegLibcall = 7,
  kRegNonneg = 8,
  kRegNoConflict = 9,
  kRegUnused = 10,
  kRegCcSetter = 11,
  kRegCcUser = 12,
  kRegLabel = 13,
  kRegDepAnti = 14,
  kRegDepOutput = 15,
};

enum GccInsnNote {
  kNoteInsnDeleted = -1,
  kNoteInsnBlockBeg = -2,
  kNoteInsnBlockEnd = -3,
  kNoteInsnLoopBeg = -4,
  kNoteInsnLoopEnd = -5,
  kNoteInsnFunctionEnd = -6,
  kNoteInsnSetjmp = -7,
  kNoteInsnLoopCont = -8,
  kNoteInsnLoopVtop = -9,
  kNoteInsnPrologueEnd = -10,
  kNoteInsnEpilogueBeg = -11,
  kNoteInsnDeletedLabel = -12,
  kNoteInsnFunctionBeg = -13,
};

// GCC 2.7.2 core tables and allocators.
extern char* rtx_name[];
extern char* rtx_format[];
extern int rtx_length[];
extern char rtx_class[];
extern char* mode_name[];
extern char* reg_note_name[];
extern char* note_insn_name[];
extern GccObstack* rtl_obstack;
extern rtx_def** regno_reg_rtx;
extern short* reg_renumber;
extern int max_regno;
extern int n_basic_blocks;
extern rtx_def** basic_block_head;
extern rtx_def** basic_block_end;
extern int optimize;
extern int target_flags;
extern int reload_completed;
extern void* current_function_decl;
extern FILE* asm_out_file;

rtx_def* rtx_alloc(int code);
rtvec_def* rtvec_alloc(int n);
rtx_def* gen_rtx(int code, int mode, ...);
rtx_def* gen_reg_rtx(int mode);
rtx_def* gen_label_rtx();
rtx_def* emit_insn(rtx_def* pattern);
rtx_def* emit_jump_insn(rtx_def* pattern);
rtx_def* emit_call_insn(rtx_def* pattern);
rtx_def* emit_label(rtx_def* label);
rtx_def* emit_barrier();
rtx_def* emit_note(char* file, int line);
rtx_def* make_insn_raw(rtx_def* pattern);
void add_insn(rtx_def* insn);
rtx_def* get_insns();
rtx_def* get_last_insn();
int max_reg_num();
int max_label_num();
int get_max_uid();
void print_rtl(FILE* outf, rtx_def* rtx_first);
void print_rtx(rtx_def* in_rtx);

}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace rom_nom_nom::gcc_harness {

using Rtx = rtx_def*;
using Rtvec = rtvec_def*;

// Manages in-process KMC GCC 2.7.2 (`cc1`) and GNU Binutils 2.6 (`as`) global
// state, RTL parsing/formatting, and lossless bidirectional conversion between
// `RtlControlFlowGraph` and GCC's in-memory `rtx` instruction chain.
class GccContext {
 public:
  GccContext();
  ~GccContext();

  GccContext(const GccContext&) = delete;
  GccContext& operator=(const GccContext&) = delete;

  // Resets the per-function RTL chain, pseudo-register tables, and synthetic
  // `FUNCTION_DECL` state so a new function can be materialized and passes run.
  void BeginFunction(std::string_view func_name = "harness_func");

  // Parses a single RTL S-expression (supporting `/v`, `/i`, `/s`, `/u` flags,
  // `:REG_*` note modes, `(label_ref L<k>)`, and pseudo-register canonicalization)
  // into a live GCC `rtx_def*`.
  Rtx ParseRtx(std::string_view rtl_text);

  // Formats a single `rtx_def*` to its canonical single-line S-expression string
  // using GCC's `print_rtx`.
  std::string FormatRtx(Rtx rtx) const;

  // Formats an entire doubly-linked instruction chain starting at `first_insn`
  // using GCC's `print_rtl`.
  std::string DumpInsnChain(Rtx first_insn = nullptr) const;

  // Emits a single RTL S-expression into the active function's instruction chain.
  // Correctly handles container nodes (`insn`, `jump_insn`, `call_insn`),
  // `(sequence [...])` delay-slot bundles, `(note ...)`, `(code_label ...)`,
  // `(barrier)`, and bare patterns `(set ...)`.
  Rtx EmitPattern(std::string_view pattern_rtl);

  // Materializes a `lifter::RtlControlFlowGraph` into GCC's in-memory doubly-linked
  // `rtx` instruction chain (`get_insns()`), preserving all `/v`, `/i`, `/s`, `/u`
  // flags, `SEQUENCE` delay-slot wrappers, `REG_NOTES`, `NOTE_INSN_LOOP_*` notes,
  // `CODE_LABEL` targets, and `BARRIER`s. Returns `get_insns()`.
  Rtx MaterializeFromCfg(const RtlControlFlowGraph& cfg);

  // Converts an in-memory GCC `rtx` instruction chain back into a canonical
  // `RtlControlFlowGraph` via `print_rtl` + `GccRtlDumpInspector`.
  RtlControlFlowGraph DematerializeToCfg(Rtx first_insn = nullptr,
                                         RtlPassKind pass = RtlPassKind::kDbr) const;

  // Returns the head of the active function's instruction chain (`get_insns()`).
  Rtx GetInsns() const;

  // Ensures pseudo-registers up to `min_max_regno` are allocated in `regno_reg_rtx`.
  void EnsurePseudoRegsUpTo(int min_max_regno, int default_mode = kSiMode);

  // Resolves or creates a `CODE_LABEL` `rtx` for a symbolic label name (`"L0"`, etc.).
  Rtx GetOrCreateLabel(std::string_view label_token);

 private:
  static void EnsureGlobalInit();

  Rtx ParseRtxImpl(std::string_view text, size_t* pos);
  Rtx BuildInsnFromContainerOrPattern(std::string_view item_text, bool emit_to_chain);

  std::string current_func_name_;
  absl::flat_hash_map<std::string, Rtx> label_map_;
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_GCC_CONTEXT_H_
