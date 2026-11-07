#include "gcc_harness/function_hooks.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {

// Core GCC helpers.
void restore_reg_data(rtx_def* first);
void reg_scan(rtx_def* f, int nregs, int repeat);
int get_frame_size();
int compute_frame_size(int size);
extern char regs_ever_live[];
rtx_def* delete_insn(rtx_def* insn);
void init_alias_analysis();
void init_recog_no_volatile();
void allocate_for_life_analysis();
void regclass(rtx_def* f, int nregs);
int gcc_obstack_begin(GccObstack* h, int size, int alignment, void* (*chunkfun)(long),
                      void (*freefun)(void*)) __asm__("_obstack_begin");
void _obstack_newchunk(GccObstack* h, int length);
void* xmalloc(unsigned size);
int num_delay_slots(rtx_def* insn);

// Stage 0: Final & MIPS backend.
void shorten_branches(rtx_def* first);
void final_start_function(rtx_def* first, FILE* file, int optimize);
void final(rtx_def* first, FILE* file, int optimize, int prescan);
void final_end_function(rtx_def* first, FILE* file, int optimize);
rtx_def* final_scan_insn(rtx_def* insn, FILE* file, int optimize, int prescan, int nopeepholes);
void function_prologue(FILE* file, int size);
void function_epilogue(FILE* file, int size);

// Stage 0: Binutils 2.6 GAS.
struct GasMipsOpcode {
  const char* name;
  const char* args;
  unsigned long match;
  unsigned long mask;
  unsigned long pinfo;
};

struct GasMipsClInsn {
  unsigned long insn_opcode;
  const GasMipsOpcode* insn_mo;
};

struct GasExpressionS {
  int X_op;
  void* X_add_symbol;
  void* X_op_symbol;
  long X_add_number;
  unsigned char X_unsigned;
  char pad[23];
};

extern GasFrag* frag_now;
extern int mips_noreorder;
extern int prev_insn_valid;
extern char* insn_error;
extern GasExpressionS imm_expr;
extern GasExpressionS offset_expr;
extern int imm_reloc;
extern int offset_reloc;

void frag_init();
void frag_wane(GasFrag* frag);
void frag_new(int old_frags_var_max_size);
int frag_now_fix();
void subsegs_begin();
void md_assemble(char* str);
void mips_ip(char* str, GasMipsClInsn* ip);
void macro(GasMipsClInsn* ip);
void append_insn(char* place, GasMipsClInsn* ip, GasExpressionS* address_expr, int reloc_type);
void mips_no_prev_insn();
void mips_emit_delays();

// Stage 1: Reorg (`reorg.c`).
void dbr_schedule(rtx_def* first, FILE* file);
void fill_simple_delay_slots(rtx_def* first, int non_jumps_p);
void fill_eager_delay_slots(rtx_def* first);
rtx_def* fill_slots_from_thread(rtx_def* insn, rtx_def* condition, rtx_def* thread,
                                rtx_def* opposite_thread, int likely, int thread_if_true,
                                int own_thread, int own_opposite_thread, int slots_to_fill,
                                int* pslots_filled);
void relax_delay_slots(rtx_def* first);
void make_return_insns(rtx_def* first);
rtx_def* find_end_label();

extern int reorg_max_uid;
extern int* uid_to_ruid;
extern GccObstack unfilled_slots_obstack;
extern rtx_def** unfilled_firstobj;
extern rtx_def* end_of_function_label;
extern void** target_hash_table;
extern int* bb_ticks;
extern int reorg_pass_number;
extern char end_of_function_needs[32];
extern char start_of_epilogue_needs[32];

// Stage 2 & Stage 12: Jump (`jump.c`).
void jump_optimize(rtx_def* f, int cross_jump, int noop_moves, int after_regscan);
void find_cross_jump(rtx_def* e1, rtx_def* e2, int minimum, rtx_def** f1, rtx_def** f2);
void do_cross_jump(rtx_def* insn, rtx_def* newjpos, rtx_def* newlpos);
int redirect_jump(rtx_def* jump, rtx_def* nlabel);
int invert_jump(rtx_def* jump, rtx_def* nlabel);

// Stage 3 & Stage 6: Sched (`sched.c`).
void schedule_insns(FILE* dump_file);
int schedule_block(int b, FILE* file);

extern rtx_def* sched_before_next_call;
extern int* insn_luid;
extern int* insn_priority;
extern int* insn_tick;
extern short* insn_costs;
extern short* insn_units;
extern unsigned int* insn_blockage;
extern int* insn_ref_count;
extern int* sched_reg_n_calls_crossed;
extern int* sched_reg_live_length;
extern int* reg_n_calls_crossed;
extern int* reg_live_length;
extern short* reg_n_refs;
extern short* reg_n_sets;
extern short* reg_n_deaths;
extern char* reg_changes_size;
extern int* reg_basic_block;
extern rtx_def* bb_dead_regs;
extern rtx_def* bb_live_regs;
extern rtx_def** basic_block_live_at_start;
extern int regset_size;
extern int regset_bytes;

// Stage 4 & Stage 5: Register Allocation & Reload (`local-alloc.c`,
// `global.c`, `reload1.c`).
void local_alloc();
void block_alloc(int b);
void update_equiv_regs();
int global_alloc(FILE* file);
int reload(rtx_def* first, int global, FILE* dumpfile);
void alter_reg(int i, int from_reg);

extern int max_scratch;
extern int scratch_list_length;
extern int scratch_index;
extern rom_nom_nom::gcc_harness::Rtx* scratch_list;
extern int* scratch_block;
extern short* qty_phys_reg;
extern void* qty_phys_copy_sugg;
extern short* qty_phys_num_copy_sugg;
extern void* qty_phys_sugg;
extern short* qty_phys_num_sugg;
extern int* qty_birth;
extern int* qty_death;
extern rtx_def** qty_scratch_rtx;
extern int* lra_qty_first_reg;
extern int* qty_size;
extern void* lra_qty_mode;
extern int* qty_n_calls_crossed;
extern void* qty_min_class;
extern void* qty_alternate_class;
extern int* qty_n_refs;
extern char* qty_changes_size;
extern int* lra_reg_qty;
extern char* reg_offset;
extern int* reg_next_in_qty;
extern int lra_next_qty;

// Stage 7: Combine (`combine.c`).
void combine_instructions(rtx_def* f, int nregs);
rtx_def* try_combine(rtx_def* i3, rtx_def* i2, rtx_def* i1);
rtx_def* subst(rtx_def* x, rtx_def* from, rtx_def* to, int in_dest, int unique_copy);
rtx_def* simplify_rtx(rtx_def* x, int op0_mode, int last, int in_dest);
void init_reg_last_arrays();

struct GccUndo {
  int is_int;
  union {
    rtx_def* r;
    int i;
  } old_contents;
  union {
    rtx_def** r;
    int* i;
  } where;
};

struct GccUndobuf {
  int num_undo;
  char* storage;
  GccUndo undo[50];
  rtx_def* other_insn;
};

extern int combine_max_regno;
extern GccUndobuf undobuf;
extern int previous_num_undos;
extern uint64_t* reg_nonzero_bits;
extern char* reg_sign_bit_copies;
extern rtx_def** reg_last_death;
extern rtx_def** reg_last_set;
extern rtx_def** reg_last_set_value;
extern int* reg_last_set_table_tick;
extern int* reg_last_set_label;
extern char* reg_last_set_invalid;
extern int* reg_last_set_mode;
extern uint64_t* reg_last_set_nonzero_bits;
extern char* reg_last_set_sign_bit_copies;
extern int* combine_uid_cuid;
extern int max_uid_cuid;
extern int nonzero_bits_mode;
extern int label_tick;
extern rtx_def* subst_prev_insn;
extern int subst_low_cuid;

// Stage 8: Flow (`flow.c`).
void flow_analysis(rtx_def* f, int nregs, FILE* file);
void find_basic_blocks(rtx_def* f, rtx_def* nonlocal_label_list);
void life_analysis(rtx_def* f, int nregs);
void propagate_block(int* old, rtx_def* first, rtx_def* last, int final, int* significant,
                     int bnum);
extern char* basic_block_drops_in;
extern int* uid_block_number;
extern char* uid_volatile;
extern int max_uid_for_flow;
extern short* basic_block_loop_depth;

// Stage 9 & Stage 11: CSE (`cse.c`).
int cse_main(rtx_def* f, int nregs, int after_loop, FILE* file);
rtx_def* cse_basic_block(rtx_def* from, rtx_def* to, void* next_branch, int around_loop);
rtx_def* fold_rtx(rtx_def* x, rtx_def* insn);
void new_basic_block();

extern int cse_max_uid;
extern int* cse_uid_cuid;
extern int max_reg;
extern int max_qty;
extern int next_qty;
extern int* qty_first_reg;
extern int* qty_last_reg;
extern int* qty_mode;
extern rtx_def** qty_const;
extern rtx_def** qty_const_insn;
extern int* qty_comparison_code;
extern rtx_def** qty_comparison_const;
extern int* qty_comparison_qty;
extern int* reg_qty;
extern int* reg_in_table;
extern int* reg_tick;
extern int* reg_next_eqv;
extern int* reg_prev_eqv;
extern int cse_basic_block_start;
extern int cse_basic_block_end;
extern int max_elements_made;
extern int n_elements_made;
extern int* all_minus_one;
extern int* consec_ints;
extern void* cse_table[32];
extern void* free_element_chain;
extern rtx_def* memory_extend_rtx;

// Stage 10: Loop (`loop.c`).
void loop_optimize(rtx_def* f, FILE* dumpfile, int unroll_p);
void scan_loop(rtx_def* loop_start, rtx_def* end, int nregs);
void strength_reduce(rtx_def* scan_start, rtx_def* end, rtx_def* loop_top, int insn_count,
                     rtx_def* loop_start, rtx_def* loop_end);
void count_loop_regs_set(rtx_def* from, rtx_def* to, char* may_not_move, rtx_def** single_usage,
                         int* count_ptr, int nregs);
void find_and_verify_loops(rtx_def* f);
void prescan_loop(rtx_def* start, rtx_def* end);

extern int max_reg_before_loop;
extern char* moved_once;
extern short* n_times_set;
extern short* n_times_used;
extern char* may_not_optimize;
extern rtx_def* regs_may_share;
extern int max_loop_num;
extern int max_uid_for_loop;
extern int* uid_luid;
extern int* uid_loop_num;
extern rtx_def** loop_number_loop_starts;
extern rtx_def** loop_number_loop_ends;
extern int* loop_outer_loop;
extern char* loop_invalid;
extern rtx_def** loop_number_exit_labels;
extern int* loop_number_exit_count;
extern int loop_has_call;
extern int loop_has_volatile;
extern rtx_def* loop_continue;
extern int loops_enclosed;
extern GccObstack temp_obstack;

}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace rom_nom_nom::gcc_harness {
namespace {

constexpr int kFirstPseudoRegister = 67;
constexpr int kBfdRelocUnused = 65;

template <typename T>
T* AllocZeroed(size_t count) {
  static std::vector<void*> arena;
  size_t bytes = std::max<size_t>(1, count) * sizeof(T);
  void* ptr = std::calloc(1, bytes);
  arena.push_back(ptr);
  return static_cast<T*>(ptr);
}

uint32_t ReadBigEndian32(const char* p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  return (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
         (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
}

bool IsBranchMnemonic(std::string_view mnemonic) {
  return mnemonic == "b" || mnemonic == "bal" || mnemonic == "beq" || mnemonic == "bne" ||
         mnemonic == "blez" || mnemonic == "bgtz" || mnemonic == "bltz" || mnemonic == "bgez" ||
         mnemonic == "bltzal" || mnemonic == "bgezal" || mnemonic == "beql" || mnemonic == "bnel" ||
         mnemonic == "blezl" || mnemonic == "bgtzl" || mnemonic == "bltzl" || mnemonic == "bgezl" ||
         mnemonic == "bc1t" || mnemonic == "bc1f" || mnemonic == "bc1tl" || mnemonic == "bc1fl" ||
         mnemonic == "beqz" || mnemonic == "bnez";
}

Rtx FindTargetLabelInPattern(Rtx x) {
  if (x == nullptr) {
    return nullptr;
  }
  const int code = x->code;
  if (code == kLabelRef) {
    return x->fld[0].rtx;
  }
  const char* fmt = rtx_format[code];
  const int len = rtx_length[code];
  for (int i = 0; i < len && fmt[i] != '\0'; ++i) {
    if (fmt[i] == 'e') {
      if (Rtx sub = FindTargetLabelInPattern(x->fld[i].rtx); sub != nullptr) {
        return sub;
      }
    } else if (fmt[i] == 'E' && x->fld[i].rtvec != nullptr) {
      Rtvec vec = x->fld[i].rtvec;
      for (int j = 0; j < vec->num_elem; ++j) {
        if (Rtx sub = FindTargetLabelInPattern(vec->elem[j].rtx); sub != nullptr) {
          return sub;
        }
      }
    }
  }
  return nullptr;
}

// Recomputes `JUMP_LABEL(insn)` (`insn->fld[7].rtx`) and `LABEL_NUSES(label)`
// (`label->fld[5].rtint`) across the instruction chain. Note that
// `flow.c:find_basic_blocks` overwrites `label->fld[5]` (`LABEL_REFS`) with a
// pointer, so any pass that reads `LABEL_NUSES` (`jump.c`, `reorg.c`) requires
// integer reference counts to be restored after `find_basic_blocks`.
void RefreshJumpLabelsAndNuses(Rtx first) {
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kCodeLabel) {
      insn->fld[5].rtint = 0;
    }
  }
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kJumpInsn) {
      Rtx target = FindTargetLabelInPattern(insn->fld[3].rtx);
      insn->fld[7].rtx = target;
      if (target != nullptr && target->code == kCodeLabel) {
        target->fld[5].rtint += 1;
      }
    } else if (insn->code == kInsn && insn->fld[3].rtx != nullptr &&
               insn->fld[3].rtx->code == kSequence) {
      Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
      if (vec != nullptr && vec->num_elem > 0) {
        Rtx first_inner = vec->elem[0].rtx;
        if (first_inner != nullptr && first_inner->code == kJumpInsn) {
          Rtx target = FindTargetLabelInPattern(first_inner->fld[3].rtx);
          first_inner->fld[7].rtx = target;
          if (target != nullptr && target->code == kCodeLabel) {
            target->fld[5].rtint += 1;
          }
        }
      }
    }
  }
}

void PushInsnToObstack(GccObstack* ob, Rtx insn) {
  if (ob->next_free + sizeof(Rtx) > ob->chunk_limit) {
    _obstack_newchunk(ob, sizeof(Rtx));
  }
  *reinterpret_cast<Rtx*>(ob->next_free) = insn;
  ob->next_free += sizeof(Rtx);
}

// Strips pre-existing flow-generated `LOG_LINKS` (`fld[5]`) and `REG_DEAD` /
// `REG_UNUSED` notes from `REG_NOTES` (`fld[6]`) before running `flow_analysis`
// or `life_analysis`, preventing duplicate `REG_DEAD` notes from being
// prepended when an already-analyzed CFG is re-analyzed.
void StripFlowGeneratedNotes(Rtx first) {
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) {
      continue;
    }
    insn->fld[5].rtx = nullptr;
    Rtx* link = &insn->fld[6].rtx;
    while (*link != nullptr) {
      Rtx cur = *link;
      if (cur->mode == kRegDead || cur->mode == kRegUnused) {
        *link = cur->fld[1].rtx;
      } else {
        link = &cur->fld[1].rtx;
      }
    }
  }
}

}  // namespace

FunctionHooks::FunctionHooks(GccContext* ctx) : ctx_(ctx) {}

// -----------------------------------------------------------------------------
// Stage 0: Final & GAS
// -----------------------------------------------------------------------------

std::string FunctionHooks::RunFinal(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return "";
  }
  reload_completed = 1;
  bool has_sequence = false;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence) {
      has_sequence = true;
      break;
    }
  }
  if (!has_sequence) {
    restore_reg_data(head);
    reg_scan(head, max_reg_num(), 0);
  }
  shorten_branches(head);
  compute_frame_size(get_frame_size());

  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = mem_fp;
  final_start_function(head, mem_fp, optimize);
  final(head, mem_fp, optimize, 0);
  final_end_function(head, mem_fp, optimize);
  asm_out_file = saved_asm;
  std::fclose(mem_fp);

  std::string out(buf != nullptr ? buf : "", len);
  std::free(buf);
  return out;
}

std::string FunctionHooks::RunFinalScanInsn(Rtx insn, int prescan) {
  if (insn == nullptr) {
    return "";
  }
  reload_completed = 1;
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = mem_fp;
  final_scan_insn(insn, mem_fp, optimize, prescan, 0);
  asm_out_file = saved_asm;
  std::fclose(mem_fp);

  std::string out(buf != nullptr ? buf : "", len);
  std::free(buf);
  return out;
}

void FunctionHooks::RunShortenBranches(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head != nullptr) {
    reload_completed = 1;
    shorten_branches(head);
  }
}

std::string FunctionHooks::RunFunctionPrologue(int frame_size, uint32_t saved_gpr_mask) {
  reload_completed = 1;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  for (int r = 0; r < 32; ++r) {
    if ((saved_gpr_mask >> r) & 1u) {
      regs_ever_live[r] = 1;
    }
  }
  compute_frame_size(frame_size);
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = mem_fp;
  function_prologue(mem_fp, frame_size);
  asm_out_file = saved_asm;
  std::fclose(mem_fp);

  std::string out(buf != nullptr ? buf : "", len);
  std::free(buf);
  return out;
}

std::string FunctionHooks::RunFunctionEpilogue(int frame_size, uint32_t saved_gpr_mask) {
  reload_completed = 1;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  for (int r = 0; r < 32; ++r) {
    if ((saved_gpr_mask >> r) & 1u) {
      regs_ever_live[r] = 1;
    }
  }
  compute_frame_size(frame_size);
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = mem_fp;
  function_epilogue(mem_fp, frame_size);
  asm_out_file = saved_asm;
  std::fclose(mem_fp);

  std::string out(buf != nullptr ? buf : "", len);
  std::free(buf);
  return out;
}

std::string FunctionHooks::RunFunctionPrologueEpilogue(int frame_size, uint32_t saved_gpr_mask) {
  reload_completed = 1;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  for (int r = 0; r < 32; ++r) {
    if ((saved_gpr_mask >> r) & 1u) {
      regs_ever_live[r] = 1;
    }
  }
  compute_frame_size(frame_size);
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = mem_fp;
  function_prologue(mem_fp, frame_size);
  function_epilogue(mem_fp, frame_size);
  asm_out_file = saved_asm;
  std::fclose(mem_fp);

  std::string out(buf != nullptr ? buf : "", len);
  std::free(buf);
  return out;
}

std::vector<uint32_t> FunctionHooks::RunGasMdAssemble(std::string_view asm_text) {
  struct PendingBranch {
    size_t word_index = 0;
    std::string target_label;
  };

  absl::flat_hash_map<std::string, size_t> label_word_index;
  std::vector<PendingBranch> pending_branches;
  std::vector<uint32_t> words;

  mips_noreorder = 1;
  prev_insn_valid = 0;

  for (std::string_view raw_line : absl::StrSplit(asm_text, '\n')) {
    size_t comment_pos = raw_line.find('#');
    if (comment_pos != std::string_view::npos) {
      raw_line = raw_line.substr(0, comment_pos);
    }
    std::string_view line = absl::StripAsciiWhitespace(raw_line);
    if (line.empty()) {
      continue;
    }

    // Extract any leading `<label>:` definition on this line.
    while (true) {
      size_t colon_pos = line.find(':');
      if (colon_pos == std::string_view::npos) {
        break;
      }
      std::string_view before = line.substr(0, colon_pos);
      if (before.find_first_of(" \t,()") != std::string_view::npos) {
        break;
      }
      label_word_index[std::string(before)] = words.size();
      line = absl::StripAsciiWhitespace(line.substr(colon_pos + 1));
    }
    if (line.empty()) {
      continue;
    }

    // Handle assembler directives.
    if (line.front() == '.') {
      continue;
    }

    // Check if this is a branch to a symbolic label.
    size_t sp = 0;
    while (sp < line.size() && !absl::ascii_isspace(line[sp])) {
      ++sp;
    }
    std::string_view mnemonic = line.substr(0, sp);
    std::string target_lbl;
    if (IsBranchMnemonic(mnemonic)) {
      size_t last_comma = line.rfind(',');
      std::string_view last_arg = absl::StripAsciiWhitespace(
          last_comma != std::string_view::npos ? line.substr(last_comma + 1) : line.substr(sp));
      if (!last_arg.empty() &&
          !(last_arg.front() == '$' && last_arg.size() > 1 && last_arg[1] != 'L') &&
          !absl::ascii_isdigit(last_arg.front()) && last_arg.front() != '-') {
        target_lbl = std::string(last_arg);
      }
    }

    frag_wane(frag_now);
    frag_new(0);
    mips_noreorder = 1;
    prev_insn_valid = 0;

    std::string mutable_line(line);
    std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
    md_assemble(mutable_line.data());

    const int nbytes = frag_now_fix();
    if (frag_now != nullptr && nbytes >= 4) {
      for (int off = 0; off + 4 <= nbytes; off += 4) {
        if (!target_lbl.empty() && off == 0) {
          pending_branches.push_back(PendingBranch{
              .word_index = words.size(),
              .target_label = target_lbl,
          });
        }
        words.push_back(ReadBigEndian32(frag_now->fr_literal + off));
      }
    }
  }

  // Apply PC-relative branch offsets (`BFD_RELOC_16_PCREL_S2`).
  for (const auto& br : pending_branches) {
    auto it = label_word_index.find(br.target_label);
    if (it != label_word_index.end() && br.word_index < words.size()) {
      const int32_t offset_words =
          static_cast<int32_t>(it->second) - static_cast<int32_t>(br.word_index + 1);
      words[br.word_index] =
          (words[br.word_index] & 0xffff0000u) | (static_cast<uint32_t>(offset_words) & 0xffffu);
    }
  }

  return words;
}

uint32_t FunctionHooks::RunGasMipsIp(std::string_view insn_asm_line) {
  imm_expr.X_op = 0;
  imm_expr.X_add_number = 0;
  offset_expr.X_op = 0;
  offset_expr.X_add_number = 0;
  std::string mutable_line(absl::StripAsciiWhitespace(insn_asm_line));
  std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
  GasMipsClInsn insn{};
  mips_ip(mutable_line.data(), &insn);
  uint32_t word = static_cast<uint32_t>(insn.insn_opcode);
  if (imm_expr.X_op != 0) {
    word |= static_cast<uint32_t>(imm_expr.X_add_number) & 0xffffu;
  } else if (offset_expr.X_op != 0) {
    word |= static_cast<uint32_t>(offset_expr.X_add_number) & 0xffffu;
  }
  return word;
}

std::vector<uint32_t> FunctionHooks::RunGasMacro(std::string_view macro_asm_line) {
  frag_wane(frag_now);
  frag_new(0);
  mips_noreorder = 1;
  prev_insn_valid = 0;
  imm_expr.X_op = 0;
  offset_expr.X_op = 0;

  std::string mutable_line(absl::StripAsciiWhitespace(macro_asm_line));
  std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
  GasMipsClInsn insn{};
  mips_ip(mutable_line.data(), &insn);
  if (insn.insn_mo != nullptr) {
    macro(&insn);
  }

  std::vector<uint32_t> words;
  const int nbytes = frag_now_fix();
  if (frag_now != nullptr && nbytes >= 4) {
    for (int off = 0; off + 4 <= nbytes; off += 4) {
      words.push_back(ReadBigEndian32(frag_now->fr_literal + off));
    }
  }
  return words;
}

uint32_t FunctionHooks::RunGasAppendInsn(std::string_view insn_asm_line) {
  frag_wane(frag_now);
  frag_new(0);
  mips_noreorder = 1;
  prev_insn_valid = 0;
  imm_expr.X_op = 0;
  offset_expr.X_op = 0;

  std::string mutable_line(absl::StripAsciiWhitespace(insn_asm_line));
  std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
  GasMipsClInsn insn{};
  mips_ip(mutable_line.data(), &insn);
  if (imm_expr.X_op != 0) {
    append_insn(nullptr, &insn, &imm_expr, imm_reloc);
  } else if (offset_expr.X_op != 0) {
    append_insn(nullptr, &insn, &offset_expr, offset_reloc);
  } else {
    append_insn(nullptr, &insn, nullptr, kBfdRelocUnused);
  }

  if (frag_now != nullptr && frag_now_fix() >= 4) {
    return ReadBigEndian32(frag_now->fr_literal);
  }
  return 0;
}

std::vector<uint32_t> FunctionHooks::RunGasAppendInsnReorder(
    absl::Span<const std::string> insn_asm_lines) {
  frag_wane(frag_now);
  frag_new(0);
  mips_noreorder = 0;
  mips_no_prev_insn();

  for (const std::string& raw_line : insn_asm_lines) {
    imm_expr.X_op = 0;
    offset_expr.X_op = 0;
    std::string mutable_line(absl::StripAsciiWhitespace(raw_line));
    std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
    GasMipsClInsn insn{};
    mips_ip(mutable_line.data(), &insn);
    if (imm_expr.X_op != 0) {
      append_insn(nullptr, &insn, &imm_expr, imm_reloc);
    } else if (offset_expr.X_op != 0) {
      append_insn(nullptr, &insn, &offset_expr, offset_reloc);
    } else {
      append_insn(nullptr, &insn, nullptr, kBfdRelocUnused);
    }
  }
  mips_emit_delays();

  std::vector<uint32_t> words;
  const int nbytes = frag_now_fix();
  if (frag_now != nullptr && nbytes >= 4) {
    for (int off = 0; off + 4 <= nbytes; off += 4) {
      words.push_back(ReadBigEndian32(frag_now->fr_literal + off));
    }
  }
  return words;
}

// -----------------------------------------------------------------------------
// Stage 1: Reorg (`reorg.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::PrepareReorgState(Rtx first) {
  reload_completed = 1;
  init_recog_no_volatile();
  RefreshJumpLabelsAndNuses(first);
  // Preserve `CODE_LABEL`s (`LABEL_PRESERVE_P(insn)` == `insn->in_struct`)
  // during `find_basic_blocks` so labels that are only targeted from inside
  // already-formed `SEQUENCE`s are not deleted as unreachable before `reorg.c`
  // inspects them.
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kCodeLabel) {
      insn->in_struct = 1;
    }
  }
  RunFindBasicBlocks(first, nullptr);
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kCodeLabel) {
      insn->in_struct = 0;
    }
  }
  RefreshJumpLabelsAndNuses(first);
  const int max_uid = get_max_uid() + 64;
  reorg_max_uid = max_uid;
  uid_to_ruid = AllocZeroed<int>(max_uid + 1);
  gcc_obstack_begin(&unfilled_slots_obstack, 0, 0, reinterpret_cast<void* (*)(long)>(xmalloc),
                    std::free);
  unfilled_firstobj = reinterpret_cast<Rtx*>(unfilled_slots_obstack.object_base);
  int ruid = 0;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->fld[0].rtint <= max_uid) {
      uid_to_ruid[insn->fld[0].rtint] = ruid++;
    }
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code != kSequence &&
        num_delay_slots(insn) > 0) {
      PushInsnToObstack(&unfilled_slots_obstack, insn);
    }
  }
  std::memset(end_of_function_needs, 0, sizeof(end_of_function_needs));
  std::memset(start_of_epilogue_needs, 0, sizeof(start_of_epilogue_needs));
  end_of_function_label = nullptr;
  target_hash_table = AllocZeroed<void*>(257);
  bb_ticks = AllocZeroed<int>(std::max(1, n_basic_blocks) + 4);
  reorg_pass_number = 0;
}

void CleanupReorgBlockMarkers(Rtx first) {
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kInsn && insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kUse &&
        insn->fld[3].rtx->fld[0].rtx != nullptr &&
        rtx_class[insn->fld[3].rtx->fld[0].rtx->code] == 'i') {
      insn->code = kNote;
      insn->fld[3].rtstr = nullptr;
      insn->fld[4].rtint = kNoteInsnDeleted;
    }
  }
}

void FunctionHooks::RunDbrSchedule(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  reload_completed = 1;
  restore_reg_data(head);
  reg_scan(head, max_reg_num(), 0);
  RunFindBasicBlocks(head, nullptr);
  RefreshJumpLabelsAndNuses(head);
  dbr_schedule(head, nullptr);
}

void FunctionHooks::RunFillSimpleDelaySlots(int non_jumps_p, Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  fill_simple_delay_slots(head, non_jumps_p);
  CleanupReorgBlockMarkers(head);
}

void FunctionHooks::RunFillEagerDelaySlots(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  fill_eager_delay_slots(head);
  CleanupReorgBlockMarkers(head);
}

Rtx FunctionHooks::RunFillSlotsFromThread(Rtx insn, Rtx condition, Rtx thread, Rtx opposite_thread,
                                          int likely, int thread_if_true, int own_thread,
                                          int own_opposite_thread, int slots_to_fill,
                                          int* pslots_filled) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    PrepareReorgState(head);
  }
  Rtx res = fill_slots_from_thread(insn, condition, thread, opposite_thread, likely, thread_if_true,
                                   own_thread, own_opposite_thread, slots_to_fill, pslots_filled);
  if (head != nullptr) {
    CleanupReorgBlockMarkers(head);
  }
  return res;
}

void FunctionHooks::RunRelaxDelaySlots(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  relax_delay_slots(head);
  CleanupReorgBlockMarkers(head);
}

void FunctionHooks::RunMakeReturnInsns(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  end_of_function_label = find_end_label();
  if (end_of_function_label != nullptr && --end_of_function_label->fld[5].rtint == 0) {
    delete_insn(end_of_function_label);
  }
  if (end_of_function_label != nullptr) {
    make_return_insns(head);
  }
  CleanupReorgBlockMarkers(head);
}

Rtx FunctionHooks::RunFindEndLabel(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return nullptr;
  }
  PrepareReorgState(head);
  return find_end_label();
}

// -----------------------------------------------------------------------------
// Stage 2 & Stage 12: Jump (`jump.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::RunJumpOptimize(int cross_jump, int noop_moves, int after_regscan, Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  reload_completed = (cross_jump != 0) ? 1 : 0;
  restore_reg_data(head);
  const int nregs = max_reg_num();
  reg_scan(head, nregs, 0);
  const bool temp_renumber = (noop_moves != 0 && reg_renumber == nullptr);
  if (temp_renumber) {
    reg_renumber = AllocZeroed<short>(nregs + 16);
    for (int i = 0; i < nregs + 16; ++i) {
      reg_renumber[i] = -1;
    }
  }
  jump_optimize(head, cross_jump, noop_moves, after_regscan);
  if (temp_renumber) {
    reg_renumber = nullptr;
  }
}

CrossJumpSearchResult FunctionHooks::RunFindCrossJump(Rtx e1, Rtx e2, int minimum) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    RefreshJumpLabelsAndNuses(head);
  }
  CrossJumpSearchResult res;
  find_cross_jump(e1, e2, minimum, &res.e1, &res.e2);
  return res;
}

void FunctionHooks::RunDoCrossJump(Rtx insn, Rtx newjpos, Rtx newlpos) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    RefreshJumpLabelsAndNuses(head);
  }
  do_cross_jump(insn, newjpos, newlpos);
}

int FunctionHooks::RunRedirectJump(Rtx jump, Rtx nlabel) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    RefreshJumpLabelsAndNuses(head);
  }
  return redirect_jump(jump, nlabel);
}

int FunctionHooks::RunInvertJump(Rtx jump, Rtx nlabel) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    RefreshJumpLabelsAndNuses(head);
  }
  return invert_jump(jump, nlabel);
}

// -----------------------------------------------------------------------------
// Stage 3 & Stage 6: Sched (`sched.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::PrepareSchedState(Rtx first) {
  StripFlowGeneratedNotes(first);
  restore_reg_data(first);
  max_regno = max_reg_num();
  reg_scan(first, max_regno, 0);
  flow_analysis(first, max_regno, nullptr);
  init_alias_analysis();

  // `schedule_block` expects a trailing `NOTE_INSN_DELETED` after
  // `basic_block_end[n_basic_blocks - 1]`.
  if (n_basic_blocks > 0 && basic_block_end[n_basic_blocks - 1] != nullptr &&
      basic_block_end[n_basic_blocks - 1]->fld[2].rtx == nullptr) {
    emit_note(nullptr, kNoteInsnDeleted);
  }

  const int max_uid = get_max_uid() + 64;
  sched_before_next_call = make_insn_raw(nullptr);
  insn_luid = AllocZeroed<int>(max_uid + 1);
  insn_priority = AllocZeroed<int>(max_uid + 1);
  insn_tick = AllocZeroed<int>(max_uid + 1);
  insn_costs = AllocZeroed<short>(max_uid + 1);
  insn_units = AllocZeroed<short>(max_uid + 1);
  insn_blockage = AllocZeroed<unsigned int>(max_uid + 1);
  insn_ref_count = AllocZeroed<int>(max_uid + 1);

  sched_reg_n_calls_crossed = AllocZeroed<int>(max_regno + 16);
  sched_reg_live_length = AllocZeroed<int>(max_regno + 16);
  if (reg_n_calls_crossed != nullptr) {
    std::memcpy(sched_reg_n_calls_crossed, reg_n_calls_crossed, max_regno * sizeof(int));
  }
  if (reg_live_length != nullptr) {
    std::memcpy(sched_reg_live_length, reg_live_length, max_regno * sizeof(int));
  }
  bb_dead_regs = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_size + 4));
  bb_live_regs = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_size + 4));
}

void FunctionHooks::RunScheduleInsns(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  StripFlowGeneratedNotes(head);
  restore_reg_data(head);
  max_regno = max_reg_num();
  reg_scan(head, max_regno, 0);
  flow_analysis(head, max_regno, nullptr);
  schedule_insns(nullptr);
}

void FunctionHooks::RunScheduleBlock(int bb_index, Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareSchedState(head);
  if (bb_index >= 0 && bb_index < n_basic_blocks) {
    schedule_block(bb_index, nullptr);
  }
}

// -----------------------------------------------------------------------------
// Stage 4 & Stage 5: Local Alloc, Global Alloc, Reload
// -----------------------------------------------------------------------------

void FunctionHooks::PrepareLocalAllocState(Rtx first) {
  StripFlowGeneratedNotes(first);
  restore_reg_data(first);
  max_regno = max_reg_num();
  reg_scan(first, max_regno, 1);
  flow_analysis(first, max_regno, nullptr);
  regclass(first, max_regno);

  const int max_qty_val = std::max(32, (max_regno - kFirstPseudoRegister) + max_scratch + 16);
  scratch_list_length = max_qty_val;
  scratch_index = 0;
  scratch_list = AllocZeroed<Rtx>(max_qty_val);
  scratch_block = AllocZeroed<int>(max_qty_val);
  qty_phys_reg = AllocZeroed<short>(max_qty_val);
  qty_phys_copy_sugg = AllocZeroed<uint64_t>(max_qty_val * 2);
  qty_phys_num_copy_sugg = AllocZeroed<short>(max_qty_val);
  qty_phys_sugg = AllocZeroed<uint64_t>(max_qty_val * 2);
  qty_phys_num_sugg = AllocZeroed<short>(max_qty_val);
  qty_birth = AllocZeroed<int>(max_qty_val);
  qty_death = AllocZeroed<int>(max_qty_val);
  qty_scratch_rtx = AllocZeroed<Rtx>(max_qty_val);
  lra_qty_first_reg = AllocZeroed<int>(max_qty_val);
  qty_size = AllocZeroed<int>(max_qty_val);
  lra_qty_mode = AllocZeroed<int>(max_qty_val);
  qty_n_calls_crossed = AllocZeroed<int>(max_qty_val);
  qty_min_class = AllocZeroed<int>(max_qty_val);
  qty_alternate_class = AllocZeroed<int>(max_qty_val);
  qty_n_refs = AllocZeroed<int>(max_qty_val);
  qty_changes_size = AllocZeroed<char>(max_qty_val);

  lra_reg_qty = AllocZeroed<int>(max_regno + 16);
  reg_offset = AllocZeroed<char>(max_regno + 16);
  reg_next_in_qty = AllocZeroed<int>(max_regno + 16);
  reg_renumber = AllocZeroed<short>(max_regno + 16);
  for (int i = 0; i < max_regno; ++i) {
    if (i >= kFirstPseudoRegister && reg_basic_block != nullptr && reg_n_deaths != nullptr &&
        reg_basic_block[i] >= 0 && reg_n_deaths[i] == 1) {
      lra_reg_qty[i] = -2;
    } else {
      lra_reg_qty[i] = -1;
    }
    reg_renumber[i] = -1;
  }
  lra_next_qty = 0;
}

void FunctionHooks::RunLocalAlloc(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  StripFlowGeneratedNotes(head);
  restore_reg_data(head);
  max_regno = max_reg_num();
  reg_scan(head, max_regno, 1);
  flow_analysis(head, max_regno, nullptr);
  regclass(head, max_regno);
  local_alloc();
}

void FunctionHooks::RunBlockAlloc(int bb_index, Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareLocalAllocState(head);
  if (bb_index >= 0 && bb_index < n_basic_blocks) {
    block_alloc(bb_index);
  }
}

void FunctionHooks::RunUpdateEquivRegs(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareLocalAllocState(head);
  update_equiv_regs();
}

int FunctionHooks::RunGlobalAlloc(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return 0;
  }
  RunLocalAlloc(head);
  return global_alloc(nullptr);
}

int FunctionHooks::RunReload(Rtx first, int global) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return 0;
  }
  if (reg_renumber == nullptr) {
    PrepareLocalAllocState(head);
  }
  return reload(head, global, nullptr);
}

void FunctionHooks::RunAlterReg(int regno, int from_reg) {
  Rtx head = ctx_->GetInsns();
  if (reg_renumber == nullptr && head != nullptr) {
    PrepareLocalAllocState(head);
  }
  alter_reg(regno, from_reg);
}

// -----------------------------------------------------------------------------
// Stage 7: Combine (`combine.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::PrepareCombineState(Rtx first) {
  restore_reg_data(first);
  const int nregs = max_reg_num() + 16;
  max_regno = max_reg_num();
  reg_scan(first, max_regno, 0);

  bool has_existing_log_links = false;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[5].rtx != nullptr) {
      has_existing_log_links = true;
      break;
    }
  }

  if (!has_existing_log_links && reg_n_deaths == nullptr) {
    flow_analysis(first, max_regno, nullptr);
  } else {
    RunFindBasicBlocks(first, nullptr);
    reg_n_refs = AllocZeroed<short>(nregs);
    reg_n_sets = AllocZeroed<short>(nregs);
    reg_n_deaths = AllocZeroed<short>(nregs);
    reg_n_calls_crossed = AllocZeroed<int>(nregs);
    reg_live_length = AllocZeroed<int>(nregs);
    reg_changes_size = AllocZeroed<char>(nregs);
    reg_basic_block = AllocZeroed<int>(nregs);
    const int n_blocks = n_basic_blocks > 0 ? n_basic_blocks : 1;
    const int regset_words = (nregs / 32) + 2;
    basic_block_live_at_start = AllocZeroed<Rtx>(n_blocks);
    for (int b = 0; b < n_blocks; ++b) {
      basic_block_live_at_start[b] = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_words));
    }
  }

  combine_max_regno = nregs;
  undobuf.num_undo = 0;
  undobuf.other_insn = nullptr;
  previous_num_undos = 0;

  reg_nonzero_bits = AllocZeroed<uint64_t>(nregs);
  reg_sign_bit_copies = AllocZeroed<char>(nregs);
  reg_last_death = AllocZeroed<Rtx>(nregs);
  reg_last_set = AllocZeroed<Rtx>(nregs);
  reg_last_set_value = AllocZeroed<Rtx>(nregs);
  reg_last_set_table_tick = AllocZeroed<int>(nregs);
  reg_last_set_label = AllocZeroed<int>(nregs);
  reg_last_set_invalid = AllocZeroed<char>(nregs);
  reg_last_set_mode = AllocZeroed<int>(nregs);
  reg_last_set_nonzero_bits = AllocZeroed<uint64_t>(nregs);
  reg_last_set_sign_bit_copies = AllocZeroed<char>(nregs);
  init_reg_last_arrays();
  init_recog_no_volatile();

  const int max_uid = get_max_uid() + 64;
  max_uid_cuid = max_uid;
  combine_uid_cuid = AllocZeroed<int>(max_uid + 1);
  int cuid = 0;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->fld[0].rtint <= max_uid) {
      combine_uid_cuid[insn->fld[0].rtint] = ++cuid;
    }
  }
  nonzero_bits_mode = kSiMode;
  label_tick = 1;
  subst_prev_insn = nullptr;
  subst_low_cuid = 1;
}

void FunctionHooks::RunCombineInstructions(Rtx first, int nregs) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  restore_reg_data(head);
  const int num_regs = nregs > 0 ? nregs : max_reg_num();
  max_regno = num_regs;
  reg_scan(head, num_regs, 0);

  bool has_existing_log_links = false;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[5].rtx != nullptr) {
      has_existing_log_links = true;
      break;
    }
  }
  if (!has_existing_log_links) {
    flow_analysis(head, num_regs, nullptr);
  } else {
    PrepareCombineState(head);
  }
  combine_instructions(head, num_regs);
}

Rtx FunctionHooks::RunTryCombine(Rtx i3, Rtx i2, Rtx i1) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    PrepareCombineState(head);
  }
  return try_combine(i3, i2, i1);
}

Rtx FunctionHooks::RunSubst(Rtx x, Rtx from, Rtx to, int in_dest, int unique_copy) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    PrepareCombineState(head);
  }
  return subst(x, from, to, in_dest, unique_copy);
}

Rtx FunctionHooks::RunSimplifyRtx(Rtx x, int op0_mode, int last, int in_dest) {
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    PrepareCombineState(head);
  }
  return simplify_rtx(x, op0_mode, last, in_dest);
}

// -----------------------------------------------------------------------------
// Stage 8: Flow (`flow.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::RunFlowAnalysis(Rtx first, int nregs) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  StripFlowGeneratedNotes(head);
  restore_reg_data(head);
  const int num_regs = nregs > 0 ? nregs : max_reg_num();
  max_regno = num_regs;
  reg_scan(head, num_regs, 0);
  flow_analysis(head, num_regs, nullptr);
}

void FunctionHooks::RunFindBasicBlocks(Rtx first, Rtx nonlocal_label_list) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  int prev_code = kJumpInsn;
  int block_count = 0;
  int max_uid = get_max_uid();
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    const int code = insn->code;
    if (insn->fld[0].rtint > max_uid) {
      max_uid = insn->fld[0].rtint;
    }
    if (code == kCodeLabel ||
        (rtx_class[code] == 'i' &&
         (prev_code == kJumpInsn || (prev_code == kCallInsn && nonlocal_label_list != nullptr) ||
          prev_code == kBarrier))) {
      ++block_count;
    }
    if (code != kNote) {
      prev_code = code;
    }
  }
  max_uid_for_flow = max_uid + 64;
  n_basic_blocks = block_count;
  const int alloc_blocks = std::max(1, block_count) + 4;
  basic_block_head = AllocZeroed<Rtx>(alloc_blocks);
  basic_block_end = AllocZeroed<Rtx>(alloc_blocks);
  basic_block_drops_in = AllocZeroed<char>(alloc_blocks);
  basic_block_loop_depth = AllocZeroed<short>(alloc_blocks);
  uid_block_number = AllocZeroed<int>(max_uid_for_flow + 1);
  uid_volatile = AllocZeroed<char>(max_uid_for_flow + 1);
  const int nregs = std::max(max_reg_num(), 67) + 16;
  const int regset_words = (nregs / 32) + 2;
  regset_size = regset_words;
  regset_bytes = regset_words * static_cast<int>(sizeof(int));
  basic_block_live_at_start = AllocZeroed<Rtx>(alloc_blocks);
  for (int b = 0; b < alloc_blocks; ++b) {
    basic_block_live_at_start[b] = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_words));
  }
  find_basic_blocks(head, nonlocal_label_list);
}

void FunctionHooks::RunLifeAnalysis(Rtx first, int nregs) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  StripFlowGeneratedNotes(head);
  restore_reg_data(head);
  const int num_regs = nregs > 0 ? nregs : max_reg_num();
  max_regno = num_regs;
  reg_scan(head, num_regs, 0);
  RunFindBasicBlocks(head, nullptr);
  life_analysis(head, num_regs);
}

void FunctionHooks::RunPropagateBlock(Rtx old_live, Rtx first_insn, Rtx last_insn, int final_pass,
                                      Rtx significant, int bnum) {
  propagate_block(reinterpret_cast<int*>(old_live), first_insn, last_insn, final_pass,
                  reinterpret_cast<int*>(significant), bnum);
}

// -----------------------------------------------------------------------------
// Stage 9 & Stage 11: CSE (`cse.c`)
// -----------------------------------------------------------------------------

int FunctionHooks::RunCseMain(Rtx first, int nregs, int after_loop) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return 0;
  }
  restore_reg_data(head);
  const int num_regs = nregs > 0 ? nregs : max_reg_num();
  reg_scan(head, num_regs, 1);
  return cse_main(head, num_regs, after_loop, nullptr);
}

Rtx FunctionHooks::RunCseBasicBlock(Rtx from, Rtx to, void* next_branch, int around_loop) {
  Rtx head = ctx_->GetInsns();
  if (from == nullptr) {
    from = head;
  }
  struct DummyBranchPath {
    Rtx branch = nullptr;
    int status = 0;
  } empty_branch_path{};
  if (head != nullptr) {
    restore_reg_data(head);
    max_reg = std::max(256, max_reg_num() + 16);
    reg_scan(head, max_reg, 1);
    init_recog_no_volatile();
    init_alias_analysis();

    all_minus_one = AllocZeroed<int>(max_reg);
    consec_ints = AllocZeroed<int>(max_reg + 500);
    for (int i = 0; i < max_reg; ++i) {
      all_minus_one[i] = -1;
    }
    for (int i = 0; i < max_reg + 500; ++i) {
      consec_ints[i] = i;
    }
    std::memset(cse_table, 0, sizeof(cse_table));
    free_element_chain = nullptr;
    n_elements_made = 0;
    memory_extend_rtx = ctx_->ParseRtx("(zero_extend (nil))");

    const int max_uid = get_max_uid() + 64;
    cse_max_uid = max_uid;
    cse_uid_cuid = AllocZeroed<int>(max_uid + 1);
    int cuid = 0;
    for (Rtx p = head; p != nullptr; p = p->fld[2].rtx) {
      if (p->fld[0].rtint <= max_uid) {
        cse_uid_cuid[p->fld[0].rtint] = ++cuid;
      }
    }
    cse_basic_block_start = 1;
    cse_basic_block_end = cuid + 16;
    max_qty = std::max(max_reg + 500, cuid * 2 + max_reg * 2);
    reg_qty = AllocZeroed<int>(max_reg);
    reg_in_table = AllocZeroed<int>(max_reg);
    reg_tick = AllocZeroed<int>(max_reg);
    reg_next_eqv = AllocZeroed<int>(max_reg);
    reg_prev_eqv = AllocZeroed<int>(max_reg);
  }
  return cse_basic_block(from, to, next_branch != nullptr ? next_branch : &empty_branch_path,
                         around_loop);
}

Rtx FunctionHooks::RunFoldRtx(Rtx x, Rtx insn) {
  Rtx head = ctx_->GetInsns();
  if (insn == nullptr) {
    max_reg = std::max(256, (head != nullptr ? max_reg_num() : 128) + 16);
    max_qty = max_reg + 500;
    all_minus_one = AllocZeroed<int>(max_reg);
    consec_ints = AllocZeroed<int>(max_qty);
    reg_qty = AllocZeroed<int>(max_reg);
    reg_in_table = AllocZeroed<int>(max_reg);
    reg_tick = AllocZeroed<int>(max_reg);
    reg_next_eqv = AllocZeroed<int>(max_reg);
    reg_prev_eqv = AllocZeroed<int>(max_reg);
    for (int i = 0; i < max_reg; ++i) {
      all_minus_one[i] = -1;
      consec_ints[i] = i;
      reg_in_table[i] = -1;
      reg_qty[i] = i;
    }
    std::memset(cse_table, 0, sizeof(cse_table));
    free_element_chain = nullptr;
    n_elements_made = 0;
  }
  return fold_rtx(x, insn);
}

// -----------------------------------------------------------------------------
// Stage 10: Loop (`loop.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::PrepareLoopState(Rtx first) {
  restore_reg_data(first);
  init_recog_no_volatile();
  init_alias_analysis();

  max_reg_before_loop = max_reg_num();
  moved_once = AllocZeroed<char>(max_reg_before_loop + 64);
  regs_may_share = nullptr;

  max_loop_num = 0;
  for (Rtx p = first; p != nullptr; p = p->fld[2].rtx) {
    if (p->code == kNote && p->fld[4].rtint == kNoteInsnLoopBeg) {
      ++max_loop_num;
    }
  }
  const int loop_cap = std::max(8, max_loop_num + 4);
  max_uid_for_loop = get_max_uid() + 1 + loop_cap * 32 + 64;
  uid_luid = AllocZeroed<int>(max_uid_for_loop + 1);
  uid_loop_num = AllocZeroed<int>(max_uid_for_loop + 1);
  loop_number_loop_starts = AllocZeroed<Rtx>(loop_cap);
  loop_number_loop_ends = AllocZeroed<Rtx>(loop_cap);
  loop_outer_loop = AllocZeroed<int>(loop_cap);
  loop_invalid = AllocZeroed<char>(loop_cap);
  loop_has_call = 0;
  loop_has_volatile = 0;
  loop_continue = nullptr;
  loops_enclosed = 1;
  loop_number_exit_labels = AllocZeroed<Rtx>(loop_cap);
  loop_number_exit_count = AllocZeroed<int>(loop_cap);

  gcc_obstack_begin(&temp_obstack, 0, 0, reinterpret_cast<void* (*)(long)>(xmalloc), std::free);
  find_and_verify_loops(first);
  reg_scan(first, max_reg_num(), 1);
  int luid = 0;
  for (Rtx p = first; p != nullptr; p = p->fld[2].rtx) {
    if (p->fld[0].rtint < max_uid_for_loop) {
      if (p->code != kNote || p->fld[4].rtint <= 0) {
        uid_luid[p->fld[0].rtint] = ++luid;
      } else {
        uid_luid[p->fld[0].rtint] = luid;
      }
    }
  }
  for (int i = 0; i < max_uid_for_loop; ++i) {
    uid_luid[0] = uid_luid[i];
    if (uid_luid[0] != 0) {
      break;
    }
  }
  for (int i = 1; i < max_uid_for_loop; ++i) {
    if (uid_luid[i] == 0) {
      uid_luid[i] = uid_luid[i - 1];
    }
  }
}

void FunctionHooks::RunLoopOptimize(Rtx first, int unroll_p) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  restore_reg_data(head);
  reg_scan(head, max_reg_num(), 1);
  loop_optimize(head, nullptr, unroll_p);
}

void FunctionHooks::RunScanLoop(Rtx loop_start, Rtx end, int nregs) {
  Rtx head = ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareLoopState(head);
  if ((loop_start == nullptr || end == nullptr) && max_loop_num > 0) {
    if (loop_start == nullptr) {
      loop_start = loop_number_loop_starts[0];
    }
    if (end == nullptr) {
      end = loop_number_loop_ends[0];
    }
  }
  if (loop_start == nullptr || end == nullptr) {
    return;
  }
  scan_loop(loop_start, end, nregs > 0 ? nregs : max_reg_num());
}

void FunctionHooks::RunStrengthReduce(Rtx scan_start, Rtx end, Rtx loop_top, int insn_count,
                                      Rtx loop_start, Rtx loop_end) {
  Rtx head = ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareLoopState(head);
  if ((loop_start == nullptr || loop_end == nullptr) && max_loop_num > 0) {
    if (loop_start == nullptr) {
      loop_start = loop_number_loop_starts[0];
    }
    if (loop_end == nullptr) {
      loop_end = loop_number_loop_ends[0];
    }
  }
  if (loop_start == nullptr || loop_end == nullptr) {
    return;
  }
  if (end == nullptr) {
    end = loop_end;
  }
  if (scan_start == nullptr) {
    for (Rtx p = loop_start->fld[2].rtx; p != end && p != nullptr; p = p->fld[2].rtx) {
      if (p->code == kCodeLabel || p->code == kInsn || p->code == kJumpInsn ||
          p->code == kCallInsn) {
        scan_start = p;
        break;
      }
    }
  }
  if (scan_start == nullptr) {
    return;
  }
  prescan_loop(loop_start, end);
  const int nregs = max_reg_num() + 16;
  n_times_set = AllocZeroed<short>(nregs);
  n_times_used = AllocZeroed<short>(nregs);
  may_not_optimize = AllocZeroed<char>(nregs);
  int computed_count = 0;
  count_loop_regs_set(loop_top != nullptr ? loop_top : loop_start, end, may_not_optimize, nullptr,
                      &computed_count, nregs);
  for (int i = 0; i < kFirstPseudoRegister && i < nregs; ++i) {
    may_not_optimize[i] = 1;
    n_times_set[i] = 1;
  }
  std::memcpy(n_times_used, n_times_set, nregs * sizeof(short));
  strength_reduce(scan_start, end, loop_top, insn_count > 0 ? insn_count : computed_count,
                  loop_start, loop_end);
}

}  // namespace rom_nom_nom::gcc_harness
