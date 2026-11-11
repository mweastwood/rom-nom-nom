#include "gcc_harness/function_hooks.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "core/rtl_sexpr_utils.h"

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {

// Core GCC helpers & C frontend.
int yyparse();
void permanent_allocation(int function_end);
void* getdecls();
void* gettags();
void unshare_all_rtl(rtx_def* insn);
void instantiate_virtual_regs(void* fndecl, rtx_def* insns);
extern FILE* finput;
extern int lineno;
extern int errorcount;
extern int sorrycount;
extern int quiet_flag;
extern int rtl_dump;
extern int rtl_dump_and_exit;
extern int jump_opt_dump;
extern int cse_dump;
extern int loop_dump;
extern int cse2_dump;
extern int flow_dump;
extern int combine_dump;
extern int sched_dump;
extern int local_reg_dump;
extern int global_reg_dump;
extern int sched2_dump;
extern int jump2_opt_dump;
extern int dbr_sched_dump;
extern FILE* rtl_dump_file;
extern FILE* jump_opt_dump_file;
extern FILE* cse_dump_file;
extern FILE* loop_dump_file;
extern FILE* cse2_dump_file;
extern FILE* flow_dump_file;
extern FILE* combine_dump_file;
extern FILE* sched_dump_file;
extern FILE* local_reg_dump_file;
extern FILE* global_reg_dump_file;
extern FILE* sched2_dump_file;
extern FILE* jump2_opt_dump_file;
extern FILE* dbr_sched_dump_file;
extern int frame_offset;
extern int current_function_outgoing_args_size;
extern int obey_regdecls;
extern int flag_signed_char;
extern int flag_defer_pop;
extern int flag_thread_jumps;
extern int flag_cse_follow_jumps;
extern int flag_cse_skip_blocks;
extern int flag_force_mem;
extern int flag_inline_functions;
extern int flag_caller_saves;
extern int flag_expensive_optimizations;
extern int flag_strength_reduce;
extern int flag_schedule_insns;
extern int flag_schedule_insns_after_reload;
extern int flag_delayed_branch;
extern int flag_omit_frame_pointer;
extern int frame_pointer_needed;
extern int cse_not_expected;
extern const char* mips_cpu_string;
extern const char* mips_isa_string;
extern int g_switch_set;
extern int g_switch_value;
void set_target_switch(char* name);
void override_options();

struct GccLangIndepOption {
  const char* string;
  int* variable;
  int on_value;
};
extern GccLangIndepOption f_options[52];

extern rtx_def* pc_rtx;
extern rtx_def* stack_pointer_rtx;
extern rtx_def* frame_pointer_rtx;
extern rtx_def* arg_pointer_rtx;
void restore_reg_data(rtx_def* first);
void reg_scan(rtx_def* f, int nregs, int repeat);
int get_frame_size();
int compute_frame_size(int size);
extern char regs_ever_live[];
extern char call_used_regs[];
extern char global_regs[];
extern int reg_class_size[];
extern rtx_def* current_function_epilogue_delay_list;
rtx_def* delete_insn(rtx_def* insn);
void init_alias_analysis();
void init_recog();
void init_recog_no_volatile();
void clear_const_double_mem();
int recog(rtx_def* x0, rtx_def* insn, int* pnum_clobbers);
void allocate_for_life_analysis();
void regclass_init();
void regclass(rtx_def* f, int nregs);
int reg_preferred_class(int regno);
int reg_alternate_class(int regno);
int simplejump_p(rtx_def* insn);
rtx_def* next_label(rtx_def* insn);
rtx_def* single_set(rtx_def* insn);
int rtx_equal_p(rtx_def* x, rtx_def* y);
rtx_def* find_reg_note(rtx_def* insn, int kind, rtx_def* datum);
int volatile_refs_p(rtx_def* x);
void note_stores(rtx_def* x, void (*fun)(rtx_def*, rtx_def*));
rtx_def* try_split(rtx_def* pat, rtx_def* trial, int last);
void reposition_prologue_and_epilogue_notes(rtx_def* f);
int gcc_obstack_begin(GccObstack* h, int size, int alignment, void* (*chunkfun)(long),
                      void (*freefun)(void*)) __asm__("_obstack_begin");
void _obstack_newchunk(GccObstack* h, int length);
void* xmalloc(unsigned size);
int num_delay_slots(rtx_def* insn);

// Stage 0: Final & MIPS backend.
void thread_prologue_and_epilogue_insns(rtx_def* f);
void init_insn_lengths();
void shorten_branches(rtx_def* first);
void final_start_function(rtx_def* first, FILE* file, int optimize);
void final(rtx_def* first, FILE* file, int optimize, int prescan);
void final_end_function(rtx_def* first, FILE* file, int optimize);
rtx_def* final_scan_insn(rtx_def* insn, FILE* file, int optimize, int prescan, int nopeepholes);
void function_prologue(FILE* file, int size);
void function_epilogue(FILE* file, int size);
int simple_epilogue_p();
extern char* line_note_exists;
extern rtx_def* last_ignored_compare;
extern rtx_def* final_sequence;
extern int set_noreorder;
extern int set_nomacro;
extern int mips_branch_likely;
extern int const_labelno;
int get_pool_size();
void output_constant_pool(char* fnname, void* fndecl);
void text_section();
void function_section(void* decl);
void save_varasm_status(void* p);
void restore_varasm_status(void* p);

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

struct GasRealExpressionS {
  int X_op;
  void* X_add_symbol;
  void* X_op_symbol;
  long X_add_number;
  int X_unsigned;
};

struct GasSymbol {
  void* bsym;
  GasRealExpressionS sy_value;
  GasSymbol* sy_next;
  GasSymbol* sy_previous;
  GasFrag* sy_frag;
  unsigned int flags;
};

struct GasFix {
  char fx_pcrel_adjust;
  unsigned char fx_size;
  unsigned fx_pcrel : 1;
  unsigned fx_plt : 1;
  unsigned fx_im_disp : 2;
  unsigned fx_tcbit : 1;
  unsigned fx_done : 1;
  unsigned fx_no_overflow : 1;
  GasFrag* fx_frag;
  long fx_where;
  GasSymbol* fx_addsy;
  GasSymbol* fx_subsy;
  unsigned long fx_offset;
  GasFix* fx_next;
  void* fx_bit_fixP;
  int fx_r_type;
  unsigned long fx_addnumber;
  char* fx_file;
  unsigned fx_line;
};

struct GasFrchain {
  GasFrag* frch_root;
  GasFrag* frch_last;
  GasFrchain* frch_next;
  void* frch_seg;
  int frch_subseg;
  GasFix* fix_root;
  GasFix* fix_tail;
};

extern void* gas_text_section;
extern GasFrchain* frchain_now;
extern GasFrag* frag_now;
extern int mips_noreorder;
extern int mips_optimize;
extern int mips_debug;
extern int mips_isa;
extern int file_mips_isa;
extern int mips_cpu;
extern int prev_insn_valid;
extern char* insn_error;
extern GasExpressionS imm_expr;
extern GasExpressionS offset_expr;
extern int imm_reloc;
extern int offset_reloc;

void* subseg_new(const char* name, int subseg);
void subseg_set(void* seg, int subseg);
const char* S_GET_NAME(GasSymbol* s);
void* S_GET_SEGMENT(GasSymbol* s);
unsigned long S_GET_VALUE(GasSymbol* s);
void frag_init();
void frag_wane(GasFrag* frag);
void frag_new(int old_frags_var_max_size);
void frag_grow(unsigned int nchars);
int frag_now_fix();
void subsegs_begin();
void md_assemble(char* str);
void mips_ip(char* str, GasMipsClInsn* ip);
void macro(GasMipsClInsn* ip);
void append_insn(char* place, GasMipsClInsn* ip, GasExpressionS* address_expr, int reloc_type);
void mips_no_prev_insn();
void mips_emit_delays();
void mips_define_label(void* sym);

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
void mark_target_live_regs(rtx_def* target, void* res);
rtx_def* next_nonnote_insn(rtx_def* insn);
rtx_def* emit_note_before(int subtype, rtx_def* before);

extern int reorg_max_uid;
extern int* uid_to_ruid;
extern GccObstack unfilled_slots_obstack;
extern rtx_def** unfilled_firstobj;
extern rtx_def* end_of_function_label;
extern void** target_hash_table;
extern int* bb_ticks;
extern int reorg_pass_number;
extern char end_of_function_needs[16];
extern char start_of_epilogue_needs[16];

// Stage 2 & Stage 12: Jump (`jump.c`).
void jump_optimize(rtx_def* f, int cross_jump, int noop_moves, int after_regscan);
void thread_jumps(rtx_def* f, int max_reg, int flag_before_loop);
void delete_dead_from_cse(rtx_def* f, int nregs);
void find_cross_jump(rtx_def* e1, rtx_def* e2, int minimum, rtx_def** f1, rtx_def** f2);
void do_cross_jump(rtx_def* insn, rtx_def* newjpos, rtx_def* newlpos);
int redirect_jump(rtx_def* jump, rtx_def* nlabel);
int invert_jump(rtx_def* jump, rtx_def* nlabel);
rtx_def* prev_real_insn(rtx_def* insn);
rtx_def* next_real_insn(rtx_def* insn);
rtx_def* condjump_label(rtx_def* insn);

// Stage 3 & Stage 6: Sched (`sched.c`).
void schedule_insns(FILE* dump_file);
int schedule_block(int b, FILE* file);
void update_flow_info(rtx_def* notes, rtx_def* first, rtx_def* last, rtx_def* orig_insn);

extern rtx_def* sched_before_next_call;
extern rtx_def* unused_insn_list;
extern rtx_def* unused_expr_list;
extern rtx_def* note_list;
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
extern int* reg_n_refs;
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
void global_conflicts();
void expand_preferences();
void prune_preferences();
int allocno_compare(const void* v1, const void* v2);
int reload(rtx_def* first, int global, FILE* dumpfile);
void alter_reg(int i, int from_reg);
rtx_def* assign_stack_local(int mode, int size, int align);
extern int mode_size[];
extern int current_function_calls_alloca;
extern int current_function_has_nonlocal_label;
extern int max_parallel;

using GccHardRegSet = uint32_t[3];
extern int max_allocno;
extern int* reg_allocno;
extern int* allocno_reg;
extern int* allocno_order;
extern int* allocno_size;
extern int* reg_may_share;
extern int* conflicts;
extern int allocno_row_words;
extern GccHardRegSet* hard_reg_conflicts;
extern GccHardRegSet* hard_reg_preferences;
extern GccHardRegSet* hard_reg_copy_preferences;
extern GccHardRegSet* hard_reg_full_preferences;
extern GccHardRegSet* regs_someone_prefers;
extern GccHardRegSet no_global_alloc_regs;
extern GccHardRegSet regs_used_so_far;
extern GccHardRegSet eliminable_regset;
extern int* allocno_calls_crossed;
extern int* allocno_n_refs;
extern int* allocno_live_length;
extern int local_reg_n_refs[68];
extern int local_reg_live_length[68];
extern int* allocnos_live;

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
void setup_incoming_promotions();
void set_nonzero_bits_and_sign_copies(rtx_def* x, rtx_def* set);
void record_dead_and_set_regs(rtx_def* insn);

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
extern int nonzero_sign_valid;
extern int label_tick;
extern rtx_def* subst_prev_insn;
extern int subst_low_cuid;
extern rtx_def* subst_insn;
extern int this_basic_block;
extern int last_call_cuid;
extern int mem_last_set;

// Stage 8: Flow (`flow.c`, `stupid.c`).
void flow_analysis(rtx_def* f, int nregs, FILE* file);
void find_basic_blocks(rtx_def* f, rtx_def* nonlocal_label_list);
void life_analysis(rtx_def* f, int nregs);
void propagate_block(int* old, rtx_def* first, rtx_def* last, int final, int* significant,
                     int bnum);
void stupid_life_analysis(rtx_def* f, int nregs, FILE* file);
extern char* basic_block_drops_in;
extern int* uid_block_number;
extern char* uid_volatile;
extern int max_uid_for_flow;
extern short* basic_block_loop_depth;
extern rtx_def** reg_next_use;
extern uint32_t* regs_live_at_setjmp;
extern uint64_t elim_reg_set[2];

// Stage 9 & Stage 11: CSE (`cse.c`).
struct CseBranchPathEntry {
  rtx_def* branch;
  int status;
};

struct CseBasicBlockData {
  int low_cuid;
  int high_cuid;
  int nsets;
  rtx_def* last;
  int path_size;
  CseBranchPathEntry path[10];
};

int cse_main(rtx_def* f, int nregs, int after_loop, FILE* file);
void cse_end_of_basic_block(rtx_def* insn, CseBasicBlockData* data, int follow_jumps,
                            int after_loop, int skip_blocks);
rtx_def* cse_basic_block(rtx_def* from, rtx_def* to, void* next_branch, int around_loop);
rtx_def* fold_rtx(rtx_def* x, rtx_def* insn);
extern char fixed_regs[];
extern int flag_pic;
extern uint32_t regs_invalidated_by_call[4];
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
extern int cse_jumps_altered;
extern int recorded_label_ref;
extern int constant_pool_entries_cost;
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
void move_movables(void* movables, int threshold, int insn_count, rtx_def* loop_start, rtx_def* end,
                   int nregs);
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
extern FILE* loop_dump_stream;

// Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`).
void expand_function_end(char* filename, int line, int end_bindings);
rtx_def* expand_expr(void* exp, rtx_def* target, int tmode, int modifier);
rtx_def* expand_assignment(void* to, void* from, int want_value, int suggest_reg);
rtx_def* do_store_flag(void* exp, rtx_def* target, int mode, int only_cheap);
void do_jump(void* exp, rtx_def* if_false_label, rtx_def* if_true_label);
rtx_def* expand_call(void* exp, rtx_def* target, int ignore);
rtx_def* emit_move_insn(rtx_def* x, rtx_def* y);
void* convert(void* type, void* expr);
extern int virtuals_instantiated;
extern rtx_def** jump_chain;
extern int max_jump_chain;

struct GccOptabHandler {
  int insn_code;
  rtx_def* libfunc;
};
struct GccOptab {
  int code;
  GccOptabHandler handlers[28];
};
extern GccOptab* sdiv_optab;
extern GccOptab* sdivmod_optab;
extern GccOptab* udiv_optab;
extern GccOptab* udivmod_optab;
extern GccOptab* smod_optab;
extern GccOptab* umod_optab;
void init_all_optabs();

}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace rom_nom_nom::gcc_harness {
namespace {

constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
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

void PopulateInsnCodesWithVolatileOk(Rtx first) {
  init_recog();
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) {
      continue;
    }
    if (insn->fld[4].rtint >= 0) {
      continue;
    }
    Rtx pat = insn->fld[3].rtx;
    if (pat == nullptr || pat->code == kSequence || pat->code == kUse || pat->code == kClobber ||
        pat->code == kAddrVec || pat->code == kAddrDiffVec) {
      continue;
    }
    int clobbers = 0;
    const int code = recog(pat, insn, &clobbers);
    if (code >= 0) {
      insn->fld[4].rtint = code;
    }
  }
}

bool IsBranchMnemonic(std::string_view mnemonic) {
  return mnemonic == "b" || mnemonic == "bal" || mnemonic == "beq" || mnemonic == "bne" ||
         mnemonic == "blez" || mnemonic == "bgtz" || mnemonic == "bltz" || mnemonic == "bgez" ||
         mnemonic == "bltzal" || mnemonic == "bgezal" || mnemonic == "bltzall" ||
         mnemonic == "bgezall" || mnemonic == "beql" || mnemonic == "bnel" || mnemonic == "blezl" ||
         mnemonic == "bgtzl" || mnemonic == "bltzl" || mnemonic == "bgezl" || mnemonic == "bc0t" ||
         mnemonic == "bc0f" || mnemonic == "bc0tl" || mnemonic == "bc0fl" || mnemonic == "bc1t" ||
         mnemonic == "bc1f" || mnemonic == "bc1tl" || mnemonic == "bc1fl" || mnemonic == "beqz" ||
         mnemonic == "bnez" || mnemonic == "beqzl" || mnemonic == "bnezl";
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

void IncrementAllLabelRefUsesInRtx(Rtx x) {
  if (x == nullptr) {
    return;
  }
  const int code = x->code;
  if (code == kLabelRef) {
    Rtx lbl = x->fld[0].rtx;
    if (lbl != nullptr && lbl->code == kCodeLabel) {
      lbl->fld[5].rtint += 1;
    }
    return;
  }
  if (code == kAddrVec || code == kAddrDiffVec) {
    // Match `jump.c:mark_jump_label` (`case ADDR_VEC` / `case ADDR_DIFF_VEC`):
    // walk the target vector elements (`eltnum = 0` for `ADDR_VEC`, `1` for
    // `ADDR_DIFF_VEC`), not operand 0 of `ADDR_DIFF_VEC`.
    const int elt_idx = (code == kAddrDiffVec) ? 1 : 0;
    if (x->fld[elt_idx].rtvec != nullptr) {
      Rtvec vec = x->fld[elt_idx].rtvec;
      for (int j = 0; j < vec->num_elem; ++j) {
        IncrementAllLabelRefUsesInRtx(vec->elem[j].rtx);
      }
    }
    return;
  }
  const char* fmt = rtx_format[code];
  const int len = rtx_length[code];
  for (int i = 0; i < len && fmt[i] != '\0'; ++i) {
    if (fmt[i] == 'e') {
      IncrementAllLabelRefUsesInRtx(x->fld[i].rtx);
    } else if (fmt[i] == 'E' && x->fld[i].rtvec != nullptr) {
      Rtvec vec = x->fld[i].rtvec;
      for (int j = 0; j < vec->num_elem; ++j) {
        IncrementAllLabelRefUsesInRtx(vec->elem[j].rtx);
      }
    }
  }
}

// Recomputes `JUMP_LABEL(insn)` (`insn->fld[7].rtx`) and `LABEL_NUSES(label)`
// (`label->fld[5].rtint`) across the instruction chain. Note that
// `flow.c:find_basic_blocks` overwrites `label->fld[5]` (`LABEL_REFS`) with a
// pointer, so any pass that reads `LABEL_NUSES` (`jump.c`, `cse.c`, `reorg.c`)
// requires integer reference counts to be restored after `find_basic_blocks`.
void RefreshJumpLabelsAndNuses(Rtx first) {
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kCodeLabel) {
      insn->fld[5].rtint = 0;
    }
  }
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kJumpInsn) {
      Rtx pat = insn->fld[3].rtx;
      if (pat != nullptr && (pat->code == kAddrVec || pat->code == kAddrDiffVec)) {
        // `jump.c:3230`: never set `JUMP_LABEL` on an `ADDR_VEC` / `ADDR_DIFF_VEC`,
        // and increment `LABEL_NUSES` for every element in the vector.
        insn->fld[7].rtx = nullptr;
        IncrementAllLabelRefUsesInRtx(pat);
      } else {
        Rtx target = FindTargetLabelInPattern(pat);
        insn->fld[7].rtx = target;
        IncrementAllLabelRefUsesInRtx(pat);
      }
    } else if (insn->code == kInsn && insn->fld[3].rtx != nullptr &&
               insn->fld[3].rtx->code == kSequence) {
      Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
      if (vec != nullptr) {
        for (int i = 0; i < vec->num_elem; ++i) {
          Rtx inner = vec->elem[i].rtx;
          if (inner == nullptr) {
            continue;
          }
          if (inner->code == kJumpInsn) {
            Rtx pat = inner->fld[3].rtx;
            if (pat != nullptr && (pat->code == kAddrVec || pat->code == kAddrDiffVec)) {
              inner->fld[7].rtx = nullptr;
            } else {
              inner->fld[7].rtx = FindTargetLabelInPattern(pat);
            }
          }
          if (inner->code == kInsn || inner->code == kJumpInsn || inner->code == kCallInsn) {
            IncrementAllLabelRefUsesInRtx(inner->fld[3].rtx);
          }
        }
      }
    } else if (insn->code == kInsn || insn->code == kCallInsn) {
      IncrementAllLabelRefUsesInRtx(insn->fld[3].rtx);
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

void DeduplicateRegEquivNotes(Rtx first) {
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) {
      continue;
    }
    std::vector<Rtx> seen_equiv_datums;
    Rtx* link = &insn->fld[6].rtx;
    while (*link != nullptr) {
      Rtx cur = *link;
      if (cur->mode == kRegEquiv) {
        bool duplicate = false;
        for (Rtx prev_datum : seen_equiv_datums) {
          if (rtx_equal_p(cur->fld[0].rtx, prev_datum) != 0) {
            duplicate = true;
            break;
          }
        }
        if (duplicate) {
          *link = cur->fld[1].rtx;
          continue;
        }
        seen_equiv_datums.push_back(cur->fld[0].rtx);
      }
      link = &cur->fld[1].rtx;
    }
  }
}

}  // namespace

FunctionHooks::FunctionHooks(GccContext* ctx) : ctx_(ctx) {}

// -----------------------------------------------------------------------------
// Stage 0: Final & GAS
// -----------------------------------------------------------------------------

std::string FunctionHooks::RunFinal(Rtx first, bool include_prologue_epilogue) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return "";
  }
  reload_completed = 1;
  if (frame_pointer_needed == 0 && flag_omit_frame_pointer == 0) {
    frame_pointer_needed = 1;
  }
  bool has_sequence = false;
  int max_line = 0;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence) {
      has_sequence = true;
    }
    if (insn->code == kNote && insn->fld[4].rtint > max_line) {
      max_line = insn->fld[4].rtint;
    }
  }
  if (!has_sequence) {
    restore_reg_data(head);
    reg_scan(head, max_reg_num(), 0);
  }
  init_recog();
  auto memoize_with_opt_fallback = [](Rtx r) {
    if (r == nullptr || (r->code != kInsn && r->code != kJumpInsn && r->code != kCallInsn)) {
      return;
    }
    if (r->fld[4].rtint < 0 && r->fld[3].rtx != nullptr && r->fld[3].rtx->code != kSequence) {
      int code = recog(r->fld[3].rtx, r, nullptr);
      if (code < 0) {
        const int saved_opt = optimize;
        optimize = (saved_opt == 0) ? 2 : 0;
        code = recog(r->fld[3].rtx, r, nullptr);
        optimize = saved_opt;
      }
      if (code >= 0) {
        r->fld[4].rtint = code;
      }
    }
  };
  std::vector<std::pair<Rtx, Rtx>> saved_split_hilo_pats;
  auto stash_if_split_hilo = [&](Rtx r) {
    if (r == nullptr || r->code != kInsn || r->fld[3].rtx == nullptr) {
      return;
    }
    Rtx pat = r->fld[3].rtx;
    if (pat->code != kSet || pat->fld[0].rtx == nullptr || pat->fld[1].rtx == nullptr) {
      return;
    }
    auto has_high_or_losum = [](Rtx e) {
      if (e == nullptr || rtx_name[e->code] == nullptr) {
        return false;
      }
      const std::string_view cname = rtx_name[e->code];
      if (cname == "high" || cname == "lo_sum") {
        return true;
      }
      if ((cname == "sign_extend" || cname == "zero_extend") && e->fld[0].rtx != nullptr) {
        e = e->fld[0].rtx;
      }
      if (e->code == kMem && e->fld[0].rtx != nullptr && rtx_name[e->fld[0].rtx->code] != nullptr) {
        return std::string_view(rtx_name[e->fld[0].rtx->code]) == "lo_sum";
      }
      return false;
    };
    if (has_high_or_losum(pat->fld[0].rtx) || has_high_or_losum(pat->fld[1].rtx)) {
      saved_split_hilo_pats.emplace_back(r, pat);
      r->fld[3].rtx = gen_rtx(kUse, kVoidMode, gen_rtx(kConstInt, kVoidMode, 0));
      r->fld[4].rtint = -1;
    }
  };
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    stash_if_split_hilo(insn);
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence &&
        insn->fld[3].rtx->fld[0].rtvec != nullptr) {
      Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
      for (int i = 0; i < vec->num_elem; ++i) {
        stash_if_split_hilo(vec->elem[i].rtx);
      }
    }
  }

  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    memoize_with_opt_fallback(insn);
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence &&
        insn->fld[3].rtx->fld[0].rtvec != nullptr) {
      Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
      for (int i = 0; i < vec->num_elem; ++i) {
        memoize_with_opt_fallback(vec->elem[i].rtx);
      }
    }
  }
  shorten_branches(head);
  for (const auto& [r, orig_pat] : saved_split_hilo_pats) {
    r->fld[3].rtx = orig_pat;
    r->fld[4].rtint = -1;
  }
  compute_frame_size(get_frame_size());

  last_ignored_compare = nullptr;
  line_note_exists = AllocZeroed<char>(max_line + 16);
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kNote && insn->fld[4].rtint > 0) {
      line_note_exists[insn->fld[4].rtint] = 1;
    }
  }

  char* start_buf = nullptr;
  size_t start_len = 0;
  FILE* start_fp = open_memstream(&start_buf, &start_len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = start_fp;
  alignas(16) uint8_t saved_fn_status[2048] = {};
  const bool emit_pool = include_prologue_epilogue && get_pool_size() > 0;
  if (emit_pool) {
    save_varasm_status(saved_fn_status);
    text_section();
    output_constant_pool(const_cast<char*>("harness_func"), current_function_decl);
    function_section(current_function_decl);
  }
  final_start_function(head, start_fp, optimize);
  if (emit_pool) {
    restore_varasm_status(saved_fn_status);
  }
  asm_out_file = saved_asm;
  std::fclose(start_fp);
  std::string out;
  if (include_prologue_epilogue && start_buf != nullptr && start_len > 0) {
    out.assign(start_buf, start_len);
  }
  std::free(start_buf);

  for (Rtx insn = head->fld[2].rtx; insn != nullptr;) {
    out.append(RunFinalScanInsn(insn, /*prescan=*/0, &insn));
  }

  char* end_buf = nullptr;
  size_t end_len = 0;
  FILE* end_fp = open_memstream(&end_buf, &end_len);
  asm_out_file = end_fp;
  final_end_function(head, end_fp, optimize);
  asm_out_file = saved_asm;
  std::fclose(end_fp);
  if (include_prologue_epilogue && end_buf != nullptr && end_len > 0) {
    out.append(end_buf, end_len);
  }
  std::free(end_buf);
  return out;
}

std::string FunctionHooks::RunFinalScanInsn(Rtx insn, int prescan, Rtx* next_insn_out) {
  if (insn == nullptr) {
    if (next_insn_out != nullptr) {
      *next_insn_out = nullptr;
    }
    return "";
  }
  reload_completed = 1;
  if (line_note_exists == nullptr) {
    line_note_exists = AllocZeroed<char>(1024);
  }

  auto format_split_sym_expr = [](Rtx e, auto& self) -> std::string {
    if (e == nullptr || rtx_name[e->code] == nullptr) {
      return "";
    }
    const std::string_view cname = rtx_name[e->code];
    if (cname == "symbol_ref" && e->fld[0].rtstr != nullptr) {
      std::string sym(e->fld[0].rtstr);
      if (!sym.empty() && sym.front() == '*') {
        sym.erase(0, 1);
      }
      return sym;
    }
    if (cname == "label_ref" && e->fld[0].rtx != nullptr) {
      return absl::StrFormat("$L%d", e->fld[0].rtx->fld[3].rtint);
    }
    if (cname == "const" && e->fld[0].rtx != nullptr && rtx_name[e->fld[0].rtx->code] != nullptr &&
        std::string_view(rtx_name[e->fld[0].rtx->code]) == "plus") {
      Rtx p = e->fld[0].rtx;
      const std::string base = self(p->fld[0].rtx, self);
      if (!base.empty() && p->fld[1].rtx != nullptr && p->fld[1].rtx->code == kConstInt) {
        const int off = p->fld[1].rtx->fld[0].rtint;
        if (off == 0) return base;
        if (off > 0) return absl::StrFormat("%s+%d", base, off);
        return absl::StrFormat("%s%d", base, off);
      }
    }
    return "";
  };

  auto format_split_hilo_pat = [&](Rtx pat) -> std::string {
    if (pat == nullptr || pat->code != kSet || pat->fld[0].rtx == nullptr ||
        pat->fld[1].rtx == nullptr) {
      return "";
    }
    Rtx dst = pat->fld[0].rtx;
    Rtx src = pat->fld[1].rtx;
    auto unwrap_reg = [](Rtx r) -> int {
      if (r == nullptr) return -1;
      if (r->code == kSubreg && r->fld[0].rtx != nullptr) r = r->fld[0].rtx;
      if (r->code == kReg) return r->fld[0].rtint;
      return -1;
    };
    const std::string_view src_code = (rtx_name[src->code] != nullptr) ? rtx_name[src->code] : "";
    if (dst->code == kReg && src_code == "high" && src->fld[0].rtx != nullptr) {
      const std::string sym = format_split_sym_expr(src->fld[0].rtx, format_split_sym_expr);
      if (!sym.empty()) {
        return absl::StrFormat("lui\t$%d,%%hi(%s)", dst->fld[0].rtint, sym);
      }
    }
    if (dst->code == kReg && src_code == "lo_sum" && src->fld[0].rtx != nullptr &&
        src->fld[0].rtx->code == kReg && src->fld[1].rtx != nullptr) {
      const std::string sym = format_split_sym_expr(src->fld[1].rtx, format_split_sym_expr);
      if (!sym.empty()) {
        const char* mnem = (dst->mode == kDiMode) ? "daddiu" : "addiu";
        return absl::StrFormat("%s\t$%d,$%d,%%lo(%s)", mnem, dst->fld[0].rtint,
                               src->fld[0].rtx->fld[0].rtint, sym);
      }
    }
    bool is_sign_ext = (src_code == "sign_extend");
    bool is_zero_ext = (src_code == "zero_extend");
    Rtx load_mem = (is_sign_ext || is_zero_ext) ? src->fld[0].rtx : src;
    if (dst->code == kReg && load_mem != nullptr && load_mem->code == kMem &&
        load_mem->fld[0].rtx != nullptr && rtx_name[load_mem->fld[0].rtx->code] != nullptr &&
        std::string_view(rtx_name[load_mem->fld[0].rtx->code]) == "lo_sum") {
      Rtx losum = load_mem->fld[0].rtx;
      const int breg = unwrap_reg(losum->fld[0].rtx);
      const std::string sym = format_split_sym_expr(losum->fld[1].rtx, format_split_sym_expr);
      const int dreg = dst->fld[0].rtint;
      if (breg >= 0 && !sym.empty()) {
        const std::string_view mname =
            (mode_name[load_mem->mode] != nullptr) ? mode_name[load_mem->mode] : "";
        if (dreg >= 32 && dreg < 64) {
          return absl::StrFormat("%s\t$f%d,%%lo(%s)($%d)", mname == "DF" ? "l.d" : "l.s", dreg - 32,
                                 sym, breg);
        }
        const char* mnem = "lw";
        if (mname == "QI") {
          mnem = is_sign_ext ? "lb" : "lbu";
        } else if (mname == "HI") {
          mnem = is_sign_ext ? "lh" : "lhu";
        } else if (mname == "SI") {
          mnem = (is_zero_ext && dst->mode == kDiMode) ? "lwu" : "lw";
        } else if (mname == "DI") {
          mnem = "ld";
        }
        return absl::StrFormat("%s\t$%d,%%lo(%s)($%d)", mnem, dreg, sym, breg);
      }
    }
    if (dst->code == kMem && dst->fld[0].rtx != nullptr &&
        rtx_name[dst->fld[0].rtx->code] != nullptr &&
        std::string_view(rtx_name[dst->fld[0].rtx->code]) == "lo_sum") {
      Rtx losum = dst->fld[0].rtx;
      const int breg = unwrap_reg(losum->fld[0].rtx);
      const std::string sym = format_split_sym_expr(losum->fld[1].rtx, format_split_sym_expr);
      int sreg = unwrap_reg(src);
      if (sreg < 0 && src->code == kConstInt && src->fld[0].rtint == 0) {
        sreg = 0;
      }
      if (breg >= 0 && sreg >= 0 && !sym.empty()) {
        const std::string_view mname =
            (mode_name[dst->mode] != nullptr) ? mode_name[dst->mode] : "";
        if (sreg >= 32 && sreg < 64) {
          return absl::StrFormat("%s\t$f%d,%%lo(%s)($%d)", mname == "DF" ? "s.d" : "s.s", sreg - 32,
                                 sym, breg);
        }
        const char* mnem = "sw";
        if (mname == "QI") {
          mnem = "sb";
        } else if (mname == "HI") {
          mnem = "sh";
        } else if (mname == "DI" || mname == "DF") {
          mnem = "sd";
        }
        return absl::StrFormat("%s\t$%d,%%lo(%s)($%d)", mnem, sreg, sym, breg);
      }
    }
    return "";
  };

  if (insn->code == kInsn && insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code != kSequence) {
    const std::string split_asm = format_split_hilo_pat(insn->fld[3].rtx);
    if (!split_asm.empty()) {
      if (next_insn_out != nullptr) {
        *next_insn_out = insn->fld[2].rtx;
      }
      return (prescan > 0) ? "" : absl::StrCat("\t", split_asm, "\n");
    }
  }

  Rtx seq_split_slot_insn = nullptr;
  Rtx seq_split_slot_orig_pat = nullptr;
  std::string seq_split_slot_asm;
  if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
      insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence &&
      insn->fld[3].rtx->fld[0].rtvec != nullptr && insn->fld[3].rtx->fld[0].rtvec->num_elem == 2) {
    Rtx slot = insn->fld[3].rtx->fld[0].rtvec->elem[1].rtx;
    if (slot != nullptr && slot->code == kInsn && slot->fld[3].rtx != nullptr) {
      seq_split_slot_asm = format_split_hilo_pat(slot->fld[3].rtx);
      if (!seq_split_slot_asm.empty()) {
        seq_split_slot_insn = slot;
        seq_split_slot_orig_pat = slot->fld[3].rtx;
        slot->fld[3].rtx = gen_rtx(kUse, kVoidMode, gen_rtx(kConstInt, kVoidMode, 0));
        slot->fld[4].rtint = -1;
      }
    }
  }

  auto memoize_with_opt_fallback = [](Rtx r) {
    if (r == nullptr || (r->code != kInsn && r->code != kJumpInsn && r->code != kCallInsn)) {
      return;
    }
    if (r->fld[4].rtint < 0 && r->fld[3].rtx != nullptr && r->fld[3].rtx->code != kSequence) {
      int code = recog(r->fld[3].rtx, r, nullptr);
      if (code < 0) {
        const int saved_opt = optimize;
        optimize = (saved_opt == 0) ? 2 : 0;
        code = recog(r->fld[3].rtx, r, nullptr);
        optimize = saved_opt;
      }
      if (code >= 0) {
        r->fld[4].rtint = code;
      }
    }
  };
  memoize_with_opt_fallback(insn);
  if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
      insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence &&
      insn->fld[3].rtx->fld[0].rtvec != nullptr) {
    Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
    for (int i = 0; i < vec->num_elem; ++i) {
      memoize_with_opt_fallback(vec->elem[i].rtx);
    }
  }

  const bool standalone_annulled_branch =
      (insn->code == kJumpInsn && insn->unchanging == 1 &&
       (insn->fld[3].rtx == nullptr || insn->fld[3].rtx->code != kSequence) &&
       final_sequence == nullptr);
  if (standalone_annulled_branch) {
    final_sequence = insn;
  }

  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = mem_fp;
  Rtx next = final_scan_insn(insn, mem_fp, optimize, prescan, 0);
  if (next_insn_out != nullptr) {
    *next_insn_out = next;
  }
  asm_out_file = saved_asm;
  std::fclose(mem_fp);

  if (standalone_annulled_branch) {
    final_sequence = nullptr;
    set_noreorder = 0;
    set_nomacro = 0;
    mips_branch_likely = 0;
  }

  std::string out(buf != nullptr ? buf : "", len);
  std::free(buf);
  if (seq_split_slot_insn != nullptr) {
    seq_split_slot_insn->fld[3].rtx = seq_split_slot_orig_pat;
    seq_split_slot_insn->fld[4].rtint = -1;
    const size_t nop_pos = out.rfind("\tnop\n");
    if (nop_pos != std::string::npos) {
      out.replace(nop_pos, 5, absl::StrCat("\t", seq_split_slot_asm, "\n"));
    }
  }
  return out;
}

void FunctionHooks::RunShortenBranches(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head != nullptr) {
    reload_completed = 1;
    shorten_branches(head);
  }
}

std::string FunctionHooks::RunFunctionPrologue(int frame_size, uint32_t saved_gpr_mask,
                                               uint32_t saved_fpr_mask) {
  reload_completed = 1;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  for (int r = 0; r < 32; ++r) {
    if ((saved_gpr_mask >> r) & 1u) {
      regs_ever_live[r] = 1;
    }
    if ((saved_fpr_mask >> r) & 1u) {
      regs_ever_live[32 + r] = 1;
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

std::string FunctionHooks::RunFunctionEpilogue(int frame_size, uint32_t saved_gpr_mask,
                                               uint32_t saved_fpr_mask) {
  reload_completed = 1;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  for (int r = 0; r < 32; ++r) {
    if ((saved_gpr_mask >> r) & 1u) {
      regs_ever_live[r] = 1;
    }
    if ((saved_fpr_mask >> r) & 1u) {
      regs_ever_live[32 + r] = 1;
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

std::string FunctionHooks::RunFunctionPrologueEpilogue(int frame_size, uint32_t saved_gpr_mask,
                                                       uint32_t saved_fpr_mask) {
  reload_completed = 1;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  for (int r = 0; r < 32; ++r) {
    if ((saved_gpr_mask >> r) & 1u) {
      regs_ever_live[r] = 1;
    }
    if ((saved_fpr_mask >> r) & 1u) {
      regs_ever_live[32 + r] = 1;
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
  return RunGasMdAssembleWithMetadata(asm_text).machine_words;
}

Stage0FunctionInput FunctionHooks::RunGasMdAssembleWithMetadata(std::string_view asm_text) {
  struct ScopedGasIsaGuard {
    int saved = mips_isa;
    ScopedGasIsaGuard() {
      if (mips_isa < 2) {
        mips_isa = 3;
      }
    }
    ~ScopedGasIsaGuard() { mips_isa = saved; }
  } gas_isa_guard;
  constexpr int kGasOpAbsent = 1;
  constexpr int kBfdReloc32 = 2;
  constexpr int kBfdReloc16PcrelS2 = 44;
  constexpr int kBfdRelocGpRel16 = 48;
  constexpr int kBfdRelocMipsJmp = 86;
  constexpr int kBfdRelocHi16 = 87;
  constexpr int kBfdRelocHi16S = 88;
  constexpr int kBfdRelocLo16 = 89;
  constexpr int kBfdRelocMipsLiteral = 92;

  struct PendingBranch {
    size_t word_index = 0;
    std::string target_label;
    bool is_jump_26 = false;
  };
  struct PendingNumericFixup {
    size_t word_index = 0;
    uint32_t mask = 0;
    uint32_t value = 0;
  };
  struct RawDataBlock {
    std::string label_name;
    std::string section_name;
    std::vector<std::string> symbol_words;
    std::vector<uint32_t> int_words;
  };

  Stage0FunctionInput out;
  absl::flat_hash_map<std::string, size_t> label_word_index;
  std::vector<PendingBranch> pending_branches;
  std::vector<PendingNumericFixup> pending_numeric_fixups;
  std::vector<RawDataBlock> raw_data_blocks;

  subseg_set(gas_text_section, 0);
  frag_wane(frag_now);
  frag_new(0);
  frag_grow(8192);
  GasFrchain* const text_frchain = frchain_now;
  if (text_frchain != nullptr) {
    text_frchain->fix_root = nullptr;
    text_frchain->fix_tail = nullptr;
  }
  GasFrag* const first_frag = frag_now;
  auto count_emitted_words = [first_frag]() -> size_t {
    size_t total = 0;
    for (GasFrag* f = first_frag; f != nullptr; f = f->fr_next) {
      const long nbytes = (f == frag_now) ? frag_now_fix() : f->fr_fix;
      if (nbytes >= 4) {
        total += static_cast<size_t>(nbytes / 4);
      }
      if (f == frag_now) {
        break;
      }
    }
    return total;
  };
  const bool is_bare_reorder_lines = !absl::StrContains(asm_text, ".ent") &&
                                     !absl::StartsWith(asm_text, ".set reorder") &&
                                     !absl::StartsWith(asm_text, "\t");
  if (is_bare_reorder_lines) {
    mips_optimize = 2;
    mips_noreorder = 0;
  } else {
    const bool has_directives =
        absl::StrContains(asm_text, ".set") || absl::StrContains(asm_text, ".ent");
    mips_noreorder = has_directives ? 0 : 1;
  }
  mips_no_prev_insn();

  uint64_t dummy_sym[8] = {};
  std::string current_section = ".text";
  size_t active_data_idx = static_cast<size_t>(-1);

  auto parse_int64_or_uint64 = [](std::string_view tok, uint64_t* out_val) -> bool {
    std::string s(absl::StripAsciiWhitespace(tok));
    if (s.empty()) {
      return false;
    }
    char* endptr = nullptr;
    errno = 0;
    if (s.front() == '-') {
      const long long val = std::strtoll(s.c_str(), &endptr, 0);
      if (errno != 0 || endptr == s.c_str() || *endptr != '\0') {
        return false;
      }
      *out_val = static_cast<uint64_t>(static_cast<int64_t>(val));
      return true;
    }
    const unsigned long long val = std::strtoull(s.c_str(), &endptr, 0);
    if (errno != 0 || endptr == s.c_str() || *endptr != '\0') {
      return false;
    }
    *out_val = static_cast<uint64_t>(val);
    return true;
  };
  auto parse_int_word = [&](std::string_view tok, uint32_t* out_word) -> bool {
    uint64_t val = 0;
    if (!parse_int64_or_uint64(tok, &val)) {
      return false;
    }
    *out_word = static_cast<uint32_t>(val);
    return true;
  };

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
      if (current_section == ".text") {
        subseg_set(gas_text_section, 0);
        mips_emit_delays();
        label_word_index[std::string(before)] = count_emitted_words();
        mips_define_label(dummy_sym);
      } else {
        raw_data_blocks.push_back(RawDataBlock{
            .label_name = std::string(before),
            .section_name = current_section,
        });
        active_data_idx = raw_data_blocks.size() - 1;
      }
      line = absl::StripAsciiWhitespace(line.substr(colon_pos + 1));
    }
    if (line.empty()) {
      continue;
    }

    // Handle assembler directives (including section switches and `.word`/`.float`/`.double`).
    if (line.front() == '.') {
      std::string_view rest = absl::StripAsciiWhitespace(line.substr(1));
      auto match_dir = [&](std::string_view name) {
        return absl::StartsWith(rest, name) &&
               (rest.size() == name.size() ||
                absl::ascii_isspace(static_cast<unsigned char>(rest[name.size()])) ||
                rest[name.size()] == ',');
      };
      if (match_dir("text")) {
        current_section = ".text";
        active_data_idx = static_cast<size_t>(-1);
        subseg_set(gas_text_section, 0);
      } else if (match_dir("rdata")) {
        mips_emit_delays();
        current_section = ".rdata";
        active_data_idx = static_cast<size_t>(-1);
      } else if (match_dir("rodata")) {
        mips_emit_delays();
        current_section = ".rodata";
        active_data_idx = static_cast<size_t>(-1);
      } else if (match_dir("sdata")) {
        mips_emit_delays();
        current_section = ".sdata";
        active_data_idx = static_cast<size_t>(-1);
      } else if (match_dir("lit4")) {
        mips_emit_delays();
        current_section = ".lit4";
        active_data_idx = static_cast<size_t>(-1);
      } else if (match_dir("lit8")) {
        mips_emit_delays();
        current_section = ".lit8";
        active_data_idx = static_cast<size_t>(-1);
      } else if (match_dir("data") || match_dir("bss") || match_dir("sbss")) {
        mips_emit_delays();
        current_section = std::string(line.substr(0, line.find_first_of(" \t")));
        active_data_idx = static_cast<size_t>(-1);
      } else if (match_dir("section")) {
        mips_emit_delays();
        std::string_view sec_arg = absl::StripAsciiWhitespace(rest.substr(7));
        const size_t comma = sec_arg.find_first_of(", \t");
        std::string_view sec_tok =
            (comma == std::string_view::npos) ? sec_arg : sec_arg.substr(0, comma);
        if (!sec_tok.empty() && sec_tok.front() == '"' && sec_tok.back() == '"' &&
            sec_tok.size() >= 2) {
          sec_tok = sec_tok.substr(1, sec_tok.size() - 2);
        }
        current_section = sec_tok.empty() ? ".data" : std::string(sec_tok);
        active_data_idx = static_cast<size_t>(-1);
        if (current_section == ".text") {
          subseg_set(gas_text_section, 0);
        }
      } else if (current_section != ".text" && active_data_idx < raw_data_blocks.size()) {
        if (match_dir("word")) {
          std::string_view args = absl::StripAsciiWhitespace(rest.substr(4));
          for (std::string_view item : absl::StrSplit(args, ',')) {
            item = absl::StripAsciiWhitespace(item);
            if (item.empty()) {
              continue;
            }
            uint32_t w = 0;
            if (parse_int_word(item, &w)) {
              raw_data_blocks[active_data_idx].int_words.push_back(w);
            } else {
              raw_data_blocks[active_data_idx].symbol_words.emplace_back(item);
            }
          }
        } else if (match_dir("dword")) {
          std::string_view args = absl::StripAsciiWhitespace(rest.substr(5));
          for (std::string_view item : absl::StrSplit(args, ',')) {
            item = absl::StripAsciiWhitespace(item);
            if (item.empty()) {
              continue;
            }
            uint64_t dw = 0;
            if (parse_int64_or_uint64(item, &dw)) {
              raw_data_blocks[active_data_idx].int_words.push_back(static_cast<uint32_t>(dw >> 32));
              raw_data_blocks[active_data_idx].int_words.push_back(
                  static_cast<uint32_t>(dw & 0xffffffffu));
            }
          }
        } else if (match_dir("float") || match_dir("single")) {
          const size_t kw_len = match_dir("float") ? 5 : 6;
          std::string_view args = absl::StripAsciiWhitespace(rest.substr(kw_len));
          for (std::string_view item : absl::StrSplit(args, ',')) {
            std::string s(absl::StripAsciiWhitespace(item));
            if (s.empty()) {
              continue;
            }
            char* endptr = nullptr;
            const float fv = std::strtof(s.c_str(), &endptr);
            if (endptr != s.c_str() && *endptr == '\0') {
              raw_data_blocks[active_data_idx].int_words.push_back(absl::bit_cast<uint32_t>(fv));
            }
          }
        } else if (match_dir("double")) {
          std::string_view args = absl::StripAsciiWhitespace(rest.substr(6));
          for (std::string_view item : absl::StrSplit(args, ',')) {
            std::string s(absl::StripAsciiWhitespace(item));
            if (s.empty()) {
              continue;
            }
            char* endptr = nullptr;
            const double dv = std::strtod(s.c_str(), &endptr);
            if (endptr != s.c_str() && *endptr == '\0') {
              const uint64_t bits = absl::bit_cast<uint64_t>(dv);
              raw_data_blocks[active_data_idx].int_words.push_back(
                  static_cast<uint32_t>(bits >> 32));
              raw_data_blocks[active_data_idx].int_words.push_back(
                  static_cast<uint32_t>(bits & 0xffffffffu));
            }
          }
        }
      } else if (match_dir("set")) {
        std::string_view opt = absl::StripAsciiWhitespace(rest.substr(3));
        if (opt == "noreorder") {
          subseg_set(gas_text_section, 0);
          mips_emit_delays();
          mips_noreorder = 1;
        } else if (opt == "reorder") {
          if (mips_noreorder != 0) {
            mips_no_prev_insn();
          }
          mips_noreorder = 0;
        }
      }
      continue;
    }

    if (current_section != ".text") {
      continue;
    }

    subseg_set(gas_text_section, 0);

    // Check if this is a branch or jump to a symbolic label or numeric offset.
    size_t sp = 0;
    while (sp < line.size() && !absl::ascii_isspace(line[sp])) {
      ++sp;
    }
    std::string_view mnemonic = line.substr(0, sp);
    std::string target_lbl;
    const bool is_branch_or_jump =
        IsBranchMnemonic(mnemonic) || mnemonic == "j" || mnemonic == "jal";
    const bool is_jump_26 = (mnemonic == "j" || mnemonic == "jal");
    if (is_branch_or_jump && mnemonic != "jal") {
      size_t last_comma = line.rfind(',');
      std::string_view last_arg = absl::StripAsciiWhitespace(
          last_comma != std::string_view::npos ? line.substr(last_comma + 1) : line.substr(sp));
      if (!last_arg.empty() &&
          !(last_arg.front() == '$' && last_arg.size() > 1 && last_arg[1] != 'L') &&
          !absl::ascii_isdigit(last_arg.front()) && last_arg.front() != '-') {
        target_lbl = std::string(last_arg);
      }
    }

    const size_t pre_words = count_emitted_words();
    std::string mutable_line(line);
    std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
    md_assemble(mutable_line.data());
    subseg_set(gas_text_section, 0);
    const size_t post_words = count_emitted_words();

    if (post_words > pre_words) {
      size_t br_word = pre_words;
      if (mips_noreorder == 0 && post_words >= 2) {
        br_word = post_words - 2;
      } else if (mips_noreorder != 0 && post_words > pre_words) {
        br_word = post_words - 1;
      }
      if (!target_lbl.empty()) {
        pending_branches.push_back(PendingBranch{
            .word_index = br_word,
            .target_label = target_lbl,
            .is_jump_26 = is_jump_26,
        });
      } else if (is_branch_or_jump && offset_expr.X_op != kGasOpAbsent &&
                 offset_expr.X_add_symbol == nullptr) {
        if (is_jump_26) {
          pending_numeric_fixups.push_back(PendingNumericFixup{
              .word_index = br_word,
              .mask = 0x03ffffffu,
              .value = (static_cast<uint32_t>(offset_expr.X_add_number) >> 2) & 0x03ffffffu,
          });
        } else {
          pending_numeric_fixups.push_back(PendingNumericFixup{
              .word_index = br_word,
              .mask = 0x0000ffffu,
              .value = static_cast<uint32_t>(offset_expr.X_add_number) & 0xffffu,
          });
        }
      }
    }
  }

  subseg_set(gas_text_section, 0);
  mips_emit_delays();
  absl::flat_hash_map<const GasFrag*, size_t> frag_word_base;
  for (GasFrag* f = first_frag; f != nullptr; f = f->fr_next) {
    frag_word_base[f] = out.machine_words.size();
    const long nbytes = (f == frag_now) ? frag_now_fix() : f->fr_fix;
    for (long off = 0; off + 4 <= nbytes; off += 4) {
      out.machine_words.push_back(ReadBigEndian32(f->fr_literal + off));
    }
    if (f == frag_now) {
      break;
    }
  }

  for (const auto& fix : pending_numeric_fixups) {
    if (fix.word_index < out.machine_words.size()) {
      out.machine_words[fix.word_index] =
          (out.machine_words[fix.word_index] & ~fix.mask) | (fix.value & fix.mask);
    }
  }

  auto find_local_or_exact_label = [&](std::string_view sym) -> std::optional<size_t> {
    if (auto it = label_word_index.find(sym); it != label_word_index.end()) {
      return it->second;
    }
    if (absl::StartsWith(sym, "$L")) {
      const std::string alt = absl::StrCat(".L", sym.substr(2));
      if (auto it = label_word_index.find(alt); it != label_word_index.end()) {
        return it->second;
      }
    } else if (absl::StartsWith(sym, ".L")) {
      const std::string alt = absl::StrCat("$L", sym.substr(2));
      if (auto it = label_word_index.find(alt); it != label_word_index.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  };

  // Apply PC-relative branch offsets (`BFD_RELOC_16_PCREL_S2`) and local jump
  // target offsets (`BFD_RELOC_MIPS_JMP` for intra-function `j`).
  for (const auto& br : pending_branches) {
    const auto target_idx = find_local_or_exact_label(br.target_label);
    if (target_idx.has_value() && br.word_index < out.machine_words.size()) {
      if (br.is_jump_26) {
        out.machine_words[br.word_index] = (out.machine_words[br.word_index] & 0xfc000000u) |
                                           (static_cast<uint32_t>(*target_idx) & 0x03ffffffu);
      } else {
        const int32_t offset_words =
            static_cast<int32_t>(*target_idx) - static_cast<int32_t>(br.word_index + 1);
        out.machine_words[br.word_index] = (out.machine_words[br.word_index] & 0xffff0000u) |
                                           (static_cast<uint32_t>(offset_words) & 0xffffu);
      }
    }
  }

  // Resolve `.rdata`/`.rodata`/`.sdata` jump tables and explicit constant-pool entries.
  for (const RawDataBlock& block : raw_data_blocks) {
    if (!block.symbol_words.empty()) {
      Stage0JumpTable jt;
      jt.label_name = block.label_name;
      bool all_resolved = true;
      for (const std::string& sym : block.symbol_words) {
        const auto target_idx = find_local_or_exact_label(sym);
        if (!target_idx.has_value()) {
          all_resolved = false;
          break;
        }
        jt.target_word_indices.push_back(*target_idx);
      }
      if (all_resolved && !jt.target_word_indices.empty()) {
        out.jump_tables.push_back(std::move(jt));
      }
    } else if (!block.int_words.empty()) {
      out.constant_pool.push_back(Stage0ConstantPoolEntry{
          .label_name = block.label_name,
          .section_name = block.section_name,
          .byte_size = block.int_words.size() * 4,
          .words = block.int_words,
      });
    }
  }

  // Extract GAS `fixS` relocations from `.text` (and anonymous `li.s`/`li.d` literals).
  absl::flat_hash_map<const GasSymbol*, std::string> anon_literal_labels;
  if (text_frchain != nullptr) {
    for (GasFix* fx = text_frchain->fix_root; fx != nullptr; fx = fx->fx_next) {
      if (fx->fx_addsy == nullptr || fx->fx_frag == nullptr || fx->fx_where < 0) {
        continue;
      }
      auto frag_it = frag_word_base.find(fx->fx_frag);
      if (frag_it == frag_word_base.end()) {
        continue;
      }
      const size_t word_index = frag_it->second + static_cast<size_t>(fx->fx_where) / 4;
      if (word_index >= out.machine_words.size()) {
        continue;
      }
      if (fx->fx_r_type == kBfdReloc16PcrelS2) {
        continue;
      }

      const char* raw_name = S_GET_NAME(fx->fx_addsy);
      std::string sym_name = raw_name != nullptr ? raw_name : "";
      if (sym_name.empty()) {
        continue;
      }

      if (fx->fx_r_type == kBfdRelocMipsJmp) {
        const uint32_t opcode = (out.machine_words[word_index] >> 26) & 0x3fu;
        if (opcode == 0x02u && find_local_or_exact_label(sym_name).has_value()) {
          continue;
        }
      }

      // Capture anonymous GAS `li.s`/`li.d` literal pool entries (`"L0\001"`).
      if (sym_name.find('\001') != std::string::npos) {
        auto lit_it = anon_literal_labels.find(fx->fx_addsy);
        if (lit_it == anon_literal_labels.end()) {
          std::string synth_label = absl::StrCat(".Llit", anon_literal_labels.size());
          void* seg = S_GET_SEGMENT(fx->fx_addsy);
          const char* raw_sec =
              seg != nullptr ? *reinterpret_cast<const char* const*>(seg) : ".rdata";
          std::string sec_name = raw_sec != nullptr ? raw_sec : ".rdata";
          const size_t byte_size = (sec_name == ".lit4") ? 4u : 8u;
          std::vector<uint32_t> lit_words;
          if (fx->fx_addsy->sy_frag != nullptr) {
            const unsigned long lit_off = S_GET_VALUE(fx->fx_addsy);
            for (size_t off = 0; off + 4 <= byte_size; off += 4) {
              lit_words.push_back(
                  ReadBigEndian32(fx->fx_addsy->sy_frag->fr_literal + lit_off + off));
            }
          }
          out.constant_pool.push_back(Stage0ConstantPoolEntry{
              .label_name = synth_label,
              .section_name = std::move(sec_name),
              .byte_size = byte_size,
              .words = std::move(lit_words),
          });
          lit_it = anon_literal_labels.emplace(fx->fx_addsy, std::move(synth_label)).first;
        }
        sym_name = lit_it->second;
      }

      Stage0RelocKind reloc_kind = Stage0RelocKind::kMips32;
      switch (fx->fx_r_type) {
        case kBfdRelocMipsJmp:
          reloc_kind = Stage0RelocKind::kMips26;
          break;
        case kBfdRelocHi16:
        case kBfdRelocHi16S:
          reloc_kind = Stage0RelocKind::kMipsHi16;
          break;
        case kBfdRelocLo16:
          reloc_kind = Stage0RelocKind::kMipsLo16;
          break;
        case kBfdRelocGpRel16:
          reloc_kind = Stage0RelocKind::kMipsGpRel16;
          break;
        case kBfdReloc32:
          reloc_kind = Stage0RelocKind::kMips32;
          break;
        case kBfdRelocMipsLiteral:
          reloc_kind = Stage0RelocKind::kMipsLiteral;
          break;
        default:
          continue;
      }

      out.relocations.push_back(Stage0Relocation{
          .word_index = word_index,
          .kind = reloc_kind,
          .symbol_name = std::move(sym_name),
          .addend = static_cast<int64_t>(static_cast<long>(fx->fx_offset)),
      });
    }
  }

  std::stable_sort(out.relocations.begin(), out.relocations.end(),
                   [](const Stage0Relocation& a, const Stage0Relocation& b) {
                     return a.word_index < b.word_index;
                   });

  return out;
}

uint32_t FunctionHooks::RunGasMipsIp(std::string_view insn_asm_line) {
  constexpr int kGasOpAbsent = 1;
  imm_expr.X_op = kGasOpAbsent;
  imm_expr.X_add_number = 0;
  offset_expr.X_op = kGasOpAbsent;
  offset_expr.X_add_number = 0;
  insn_error = nullptr;
  std::string mutable_line(absl::StripAsciiWhitespace(insn_asm_line));
  if (mutable_line.empty()) {
    return 0xffffffffu;
  }
  std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
  GasMipsClInsn insn{};
  mips_ip(mutable_line.data(), &insn);
  if (insn_error != nullptr || insn.insn_mo == nullptr) {
    return 0xffffffffu;
  }
  if (insn.insn_mo->pinfo == 0xffffffffUL) {
    if (insn.insn_mo->args != nullptr && std::strcmp(insn.insn_mo->args, "a") == 0) {
      uint32_t word = static_cast<uint32_t>(insn.insn_mo[1].match);
      if (offset_expr.X_op != kGasOpAbsent) {
        word |= (static_cast<uint32_t>(offset_expr.X_add_number) >> 2) & 0x03ffffffu;
      }
      return word;
    }
    return 0xffffffffu;
  }
  uint32_t word = static_cast<uint32_t>(insn.insn_opcode);
  if (imm_expr.X_op != kGasOpAbsent) {
    word |= static_cast<uint32_t>(imm_expr.X_add_number) & 0xffffu;
  } else if (offset_expr.X_op != kGasOpAbsent) {
    word |= static_cast<uint32_t>(offset_expr.X_add_number) & 0xffffu;
  }
  return word;
}

std::vector<uint32_t> FunctionHooks::RunGasMacro(std::string_view macro_asm_line) {
  constexpr int kGasOpAbsent = 1;
  frag_wane(frag_now);
  frag_new(0);
  frag_grow(256);
  GasFrag* const first_frag = frag_now;
  mips_noreorder = 1;
  prev_insn_valid = 0;
  imm_expr.X_op = kGasOpAbsent;
  imm_expr.X_add_number = 0;
  offset_expr.X_op = kGasOpAbsent;
  offset_expr.X_add_number = 0;
  insn_error = nullptr;

  std::string mutable_line(absl::StripAsciiWhitespace(macro_asm_line));
  if (mutable_line.empty()) {
    return {};
  }
  std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
  GasMipsClInsn insn{};
  mips_ip(mutable_line.data(), &insn);
  if (insn_error != nullptr || insn.insn_mo == nullptr) {
    return {};
  }
  // Route 16-bit `li` / `dli` forms through `M_LI` / `M_DLI` (`load_register`)
  // so `RunGasMacro` executes `tc-mips.c:macro` for all `li` / `dli` inputs.
  if (insn.insn_mo->pinfo != 0xffffffffUL &&
      (std::strcmp(insn.insn_mo->name, "li") == 0 || std::strcmp(insn.insn_mo->name, "dli") == 0)) {
    while (insn.insn_mo[1].name != nullptr &&
           std::strcmp(insn.insn_mo->name, insn.insn_mo[1].name) == 0 &&
           insn.insn_mo->pinfo != 0xffffffffUL) {
      ++insn.insn_mo;
    }
  }
  if (insn.insn_mo->pinfo == 0xffffffffUL) {
    macro(&insn);
  }

  std::vector<uint32_t> words;
  for (GasFrag* f = first_frag; f != nullptr; f = f->fr_next) {
    const long nbytes = (f == frag_now) ? frag_now_fix() : f->fr_fix;
    for (long off = 0; off + 4 <= nbytes; off += 4) {
      words.push_back(ReadBigEndian32(f->fr_literal + off));
    }
    if (f == frag_now) {
      break;
    }
  }
  if (insn.insn_mo->pinfo == 0xffffffffUL && insn.insn_mo->args != nullptr &&
      std::strcmp(insn.insn_mo->args, "a") == 0 && offset_expr.X_op == 3 && words.size() == 1) {
    words[0] |= (static_cast<uint32_t>(offset_expr.X_add_number) >> 2) & 0x03ffffffu;
  }
  return words;
}

uint32_t FunctionHooks::RunGasAppendInsn(std::string_view insn_asm_line) {
  constexpr int kGasOpAbsent = 1;
  frag_wane(frag_now);
  frag_new(0);
  mips_noreorder = 1;
  prev_insn_valid = 0;
  imm_expr.X_op = kGasOpAbsent;
  imm_expr.X_add_number = 0;
  offset_expr.X_op = kGasOpAbsent;
  offset_expr.X_add_number = 0;
  insn_error = nullptr;

  std::string mutable_line(absl::StripAsciiWhitespace(insn_asm_line));
  if (mutable_line.empty()) {
    return 0;
  }
  std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
  GasMipsClInsn insn{};
  mips_ip(mutable_line.data(), &insn);
  if (insn_error != nullptr || insn.insn_mo == nullptr) {
    return 0;
  }
  if (offset_expr.X_op == 3 && insn.insn_mo->args != nullptr &&
      std::strchr(insn.insn_mo->args, 'p') != nullptr) {
    insn.insn_opcode |= static_cast<uint32_t>(offset_expr.X_add_number) & 0xffffu;
  }
  if (imm_expr.X_op != kGasOpAbsent) {
    append_insn(nullptr, &insn, &imm_expr, imm_reloc);
  } else if (offset_expr.X_op != kGasOpAbsent) {
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
  constexpr int kGasOpAbsent = 1;
  struct PendingBranch {
    size_t word_index = 0;
    std::string target_label;
    bool is_jump_26 = false;
  };

  absl::flat_hash_map<std::string, size_t> label_word_index;
  std::vector<PendingBranch> pending_branches;
  std::vector<uint32_t> words;

  frag_wane(frag_now);
  frag_new(0);
  frag_grow(8192);
  GasFrag* const first_frag = frag_now;
  auto count_emitted_words = [first_frag]() -> size_t {
    size_t total = 0;
    for (GasFrag* f = first_frag; f != nullptr; f = f->fr_next) {
      const long nbytes = (f == frag_now) ? frag_now_fix() : f->fr_fix;
      if (nbytes >= 4) {
        total += static_cast<size_t>(nbytes / 4);
      }
      if (f == frag_now) {
        break;
      }
    }
    return total;
  };

  mips_optimize = 2;
  mips_noreorder = 0;
  mips_no_prev_insn();
  uint64_t dummy_sym[8] = {};

  for (const std::string& raw_line : insn_asm_lines) {
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
      mips_emit_delays();
      label_word_index[std::string(before)] = count_emitted_words();
      mips_define_label(dummy_sym);
      line = absl::StripAsciiWhitespace(line.substr(colon_pos + 1));
    }
    if (line.empty()) {
      continue;
    }

    // Handle `.set noreorder` / `.set reorder` directives.
    if (line.front() == '.') {
      std::string_view rest = absl::StripAsciiWhitespace(line.substr(1));
      if (absl::StartsWith(rest, "set") && (rest.size() == 3 || absl::ascii_isspace(rest[3]))) {
        std::string_view opt = absl::StripAsciiWhitespace(rest.substr(3));
        if (opt == "noreorder") {
          mips_emit_delays();
          mips_noreorder = 1;
        } else if (opt == "reorder") {
          if (mips_noreorder != 0) {
            mips_no_prev_insn();
          }
          mips_noreorder = 0;
        }
      }
      continue;
    }

    size_t sp = 0;
    while (sp < line.size() && !absl::ascii_isspace(line[sp])) {
      ++sp;
    }
    std::string_view mnemonic = line.substr(0, sp);
    std::string target_lbl;
    const bool is_branch_or_jump =
        IsBranchMnemonic(mnemonic) || mnemonic == "j" || mnemonic == "jal";
    const bool is_jump_26 = (mnemonic == "j" || mnemonic == "jal");
    if (is_branch_or_jump) {
      size_t last_comma = line.rfind(',');
      std::string_view last_arg = absl::StripAsciiWhitespace(
          last_comma != std::string_view::npos ? line.substr(last_comma + 1) : line.substr(sp));
      if (!last_arg.empty() &&
          !(last_arg.front() == '$' && last_arg.size() > 1 && last_arg[1] != 'L') &&
          !absl::ascii_isdigit(last_arg.front()) && last_arg.front() != '-') {
        target_lbl = std::string(last_arg);
      }
    }

    imm_expr.X_op = kGasOpAbsent;
    imm_expr.X_add_number = 0;
    offset_expr.X_op = kGasOpAbsent;
    offset_expr.X_add_number = 0;
    insn_error = nullptr;

    std::string mutable_line(line);
    std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
    GasMipsClInsn insn{};
    mips_ip(mutable_line.data(), &insn);
    if (insn.insn_mo != nullptr && insn.insn_mo->pinfo == 0xffffffffUL &&
        insn.insn_mo->args != nullptr && std::strcmp(insn.insn_mo->args, "a") == 0) {
      insn.insn_mo = &insn.insn_mo[1];
      insn.insn_opcode = insn.insn_mo->match;
    }
    if (target_lbl.empty() && is_branch_or_jump && offset_expr.X_op != kGasOpAbsent) {
      if (is_jump_26) {
        insn.insn_opcode |= (static_cast<uint32_t>(offset_expr.X_add_number) >> 2) & 0x03ffffffu;
      } else {
        insn.insn_opcode |= static_cast<uint32_t>(offset_expr.X_add_number) & 0xffffu;
      }
    }

    const size_t pre_words = count_emitted_words();
    if (imm_expr.X_op != kGasOpAbsent) {
      append_insn(nullptr, &insn, &imm_expr, imm_reloc);
    } else if (offset_expr.X_op != kGasOpAbsent) {
      append_insn(nullptr, &insn, &offset_expr, offset_reloc);
    } else {
      append_insn(nullptr, &insn, nullptr, kBfdRelocUnused);
    }
    const size_t post_words = count_emitted_words();

    if (!target_lbl.empty() && post_words > pre_words) {
      size_t br_word = pre_words;
      if (mips_noreorder == 0 && post_words >= 2) {
        br_word = post_words - 2;
      } else if (mips_noreorder != 0 && post_words > pre_words) {
        br_word = post_words - 1;
      }
      pending_branches.push_back(PendingBranch{
          .word_index = br_word,
          .target_label = target_lbl,
          .is_jump_26 = is_jump_26,
      });
    }
  }
  mips_emit_delays();

  for (GasFrag* f = first_frag; f != nullptr; f = f->fr_next) {
    const long nbytes = (f == frag_now) ? frag_now_fix() : f->fr_fix;
    for (long off = 0; off + 4 <= nbytes; off += 4) {
      words.push_back(ReadBigEndian32(f->fr_literal + off));
    }
    if (f == frag_now) {
      break;
    }
  }

  for (const auto& br : pending_branches) {
    auto it = label_word_index.find(br.target_label);
    if (it != label_word_index.end() && br.word_index < words.size()) {
      if (br.is_jump_26) {
        words[br.word_index] = (words[br.word_index] & 0xfc000000u) |
                               (static_cast<uint32_t>(it->second) & 0x03ffffffu);
      } else {
        const int32_t offset_words =
            static_cast<int32_t>(it->second) - static_cast<int32_t>(br.word_index + 1);
        words[br.word_index] =
            (words[br.word_index] & 0xffff0000u) | (static_cast<uint32_t>(offset_words) & 0xffffu);
      }
    }
  }

  return words;
}

// -----------------------------------------------------------------------------
// Stage 1: Reorg (`reorg.c`)
// -----------------------------------------------------------------------------

namespace {

struct GccReorgResources {
  char memory;
  char unch_memory;
  char volatil;
  char cc;
  uint32_t regs[3];
};
static_assert(sizeof(GccReorgResources) == 16);

void SetReorgHardRegRange(GccReorgResources* res, int regno, int mode) {
  if (res == nullptr || regno < 0 || regno >= kFirstPseudoRegister) {
    return;
  }
  const int msize =
      (mode >= 0 && mode < kNumMachineModes && mode_size[mode] > 0) ? mode_size[mode] : 4;
  const bool is_fp_reg = (regno >= 32 && regno < 64);
  const int unit_bytes = is_fp_reg ? ((target_flags & 0x00000200) != 0 ? 8 : 4)
                                   : ((target_flags & 0x00002000) != 0 ? 8 : 4);
  const int nregs = std::max(1, (msize + unit_bytes - 1) / unit_bytes);
  for (int r = regno; r < regno + nregs && r < kFirstPseudoRegister; ++r) {
    res->regs[r / 32] |= (uint32_t{1} << (r % 32));
  }
}

void MarkReferencedHardRegsInEpilogueDelay(Rtx x, GccReorgResources* res) {
  if (x == nullptr) {
    return;
  }
  const int code = x->code;
  if (code == kReg) {
    SetReorgHardRegRange(res, x->fld[0].rtint, x->mode);
    return;
  }
  if (code == kSubreg && x->fld[0].rtx != nullptr && x->fld[0].rtx->code == kReg) {
    SetReorgHardRegRange(res, x->fld[0].rtx->fld[0].rtint, x->mode);
    return;
  }
  if (code == kSet) {
    MarkReferencedHardRegsInEpilogueDelay(x->fld[1].rtx, res);
    if (x->fld[0].rtx != nullptr && x->fld[0].rtx->code != kReg &&
        !(x->fld[0].rtx->code == kSubreg && x->fld[0].rtx->fld[0].rtx != nullptr &&
          x->fld[0].rtx->fld[0].rtx->code == kReg)) {
      MarkReferencedHardRegsInEpilogueDelay(x->fld[0].rtx, res);
    }
    return;
  }
  if (code == kClobber) {
    return;
  }
  if (code < 0 || code >= kNumRtxCode || rtx_format[code] == nullptr) {
    return;
  }
  const char* fmt = rtx_format[code];
  const int len = rtx_length[code];
  for (int i = 0; i < len; ++i) {
    if (fmt[i] == 'e') {
      MarkReferencedHardRegsInEpilogueDelay(x->fld[i].rtx, res);
    } else if (fmt[i] == 'E' && x->fld[i].rtvec != nullptr) {
      Rtvec vec = x->fld[i].rtvec;
      for (int j = 0; j < vec->num_elem; ++j) {
        MarkReferencedHardRegsInEpilogueDelay(vec->elem[j].rtx, res);
      }
    }
  }
}

void InitReorgEndOfFunctionNeeds() {
  // Match `reorg.c:4255-4304` (`end_of_function_needs` and
  // `start_of_epilogue_needs`) plus `reorg.c:3211` when an epilogue delay insn
  // is already present in `current_function_epilogue_delay_list`.
  GccReorgResources eof{};
  eof.memory = 1;
  SetReorgHardRegRange(&eof, 29, kSiMode);
  if (frame_pointer_needed != 0) {
    SetReorgHardRegRange(&eof, 30, kSiMode);
  }
  if (current_function_return_rtx != nullptr && current_function_return_rtx->code == kReg) {
    SetReorgHardRegRange(&eof, current_function_return_rtx->fld[0].rtint,
                         current_function_return_rtx->mode);
  }
  for (int i = 0; i < kFirstPseudoRegister; ++i) {
    if (global_regs[i] != 0) {
      eof.regs[i / 32] |= (uint32_t{1} << (i % 32));
    }
  }
  std::memcpy(start_of_epilogue_needs, &eof, sizeof(GccReorgResources));
  if (current_function_epilogue_delay_list != nullptr &&
      current_function_epilogue_delay_list->fld[0].rtx != nullptr) {
    Rtx ep_insn = current_function_epilogue_delay_list->fld[0].rtx;
    Rtx ep_pat =
        (ep_insn->code == kInsn || ep_insn->code == kJumpInsn || ep_insn->code == kCallInsn)
            ? ep_insn->fld[3].rtx
            : ep_insn;
    MarkReferencedHardRegsInEpilogueDelay(ep_pat, &eof);
  }
  std::memcpy(end_of_function_needs, &eof, sizeof(GccReorgResources));
}

struct SavedBlockLiveState {
  bool had_non_empty = false;
  std::vector<std::vector<uint32_t>> by_rpo;
  absl::flat_hash_map<int, std::vector<uint32_t>> by_head_uid;
};

SavedBlockLiveState SaveActiveBasicBlockLiveAtStart() {
  SavedBlockLiveState saved;
  if (basic_block_live_at_start == nullptr || n_basic_blocks <= 0 || regset_size <= 0) {
    return saved;
  }
  saved.by_rpo.resize(n_basic_blocks);
  for (int b = 0; b < n_basic_blocks; ++b) {
    const auto* src = reinterpret_cast<const uint32_t*>(basic_block_live_at_start[b]);
    if (src == nullptr) {
      continue;
    }
    saved.by_rpo[b].assign(src, src + regset_size);
    for (int w = 0; w < regset_size; ++w) {
      if (src[w] != 0u) {
        saved.had_non_empty = true;
      }
    }
    if (basic_block_head != nullptr && basic_block_head[b] != nullptr &&
        basic_block_head[b]->fld[0].rtint >= 0) {
      saved.by_head_uid[basic_block_head[b]->fld[0].rtint] = saved.by_rpo[b];
    }
  }
  return saved;
}

void RestoreBasicBlockLiveAtStartAfterFindBasicBlocks(const SavedBlockLiveState& saved,
                                                      const GccContext& ctx) {
  if (!saved.had_non_empty || basic_block_live_at_start == nullptr || basic_block_head == nullptr ||
      n_basic_blocks <= 0 || regset_size <= 0) {
    return;
  }
  for (int b = 0; b < n_basic_blocks; ++b) {
    auto* dst = reinterpret_cast<uint32_t*>(basic_block_live_at_start[b]);
    if (dst == nullptr || basic_block_head[b] == nullptr) {
      continue;
    }
    const std::vector<uint32_t>* src_vec = nullptr;
    const int head_uid = basic_block_head[b]->fld[0].rtint;
    if (auto it = saved.by_head_uid.find(head_uid); it != saved.by_head_uid.end()) {
      src_vec = &it->second;
    } else {
      int rpo_b = ctx.LookupMaterializedRpoBlock(head_uid);
      if (rpo_b < 0 && b == 0) {
        rpo_b = 0;
      }
      if (rpo_b >= 0 && rpo_b < static_cast<int>(saved.by_rpo.size())) {
        src_vec = &saved.by_rpo[rpo_b];
      }
    }
    if (src_vec != nullptr) {
      const int copy_words = std::min<int>(regset_size, static_cast<int>(src_vec->size()));
      for (int w = 0; w < copy_words; ++w) {
        dst[w] = (*src_vec)[w];
      }
    }
  }
}

bool IsUpdateBlockUseMarkerInsn(Rtx insn) {
  return insn != nullptr && insn->code == kInsn && insn->fld[3].rtx != nullptr &&
         insn->fld[3].rtx->code == kUse && insn->fld[3].rtx->fld[0].rtx != nullptr &&
         rtx_class[insn->fld[3].rtx->fld[0].rtx->code] == 'i';
}

bool IsLoopDematerializedNote(Rtx insn) {
  if (insn == nullptr || insn->code != kNote) {
    return false;
  }
  const int nk = insn->fld[4].rtint;
  return nk == kNoteInsnLoopBeg || nk == kNoteInsnLoopCont || nk == kNoteInsnLoopEnd ||
         nk == kNoteInsnLoopVtop;
}

Rtx EnsureReorgLeadingSentinelNote(Rtx first) {
  Rtx head = get_insns();
  if (head == nullptr) {
    return first;
  }
  if (head->code != kNote) {
    Rtx lead = emit_note_before(kNoteInsnDeleted, head);
    if ((head->code == kInsn || head->code == kJumpInsn || head->code == kCallInsn) &&
        head->fld[3].rtx != nullptr && head->fld[3].rtx->code == kSequence &&
        head->fld[3].rtx->fld[0].rtvec != nullptr && head->fld[3].rtx->fld[0].rtvec->num_elem > 0 &&
        head->fld[3].rtx->fld[0].rtvec->elem[0].rtx != nullptr) {
      head->fld[3].rtx->fld[0].rtvec->elem[0].rtx->fld[1].rtx = lead;
    }
    if (first == head) {
      first = lead;
    }
  }
  return first;
}

}  // namespace

void FunctionHooks::PrepareReorgState(Rtx first) {
  first = EnsureReorgLeadingSentinelNote(first);
  reload_completed = 1;
  max_regno = std::max(max_reg_num(), kFirstPseudoRegister);
  if (reg_renumber == nullptr) {
    const int alloc_regs = max_regno + 16;
    reg_renumber = AllocZeroed<short>(alloc_regs);
    for (int i = 0; i < alloc_regs; ++i) {
      reg_renumber[i] = -1;
    }
  }
  init_insn_lengths();
  init_recog();
  RefreshJumpLabelsAndNuses(first);

  // Save pre-existing `basic_block_live_at_start` populated by
  // `MaterializeFromCfg` (or an immediately preceding `reorg.c` subpass) before
  // `RunFindBasicBlocks` reallocates the block tables.
  const SavedBlockLiveState saved_live = SaveActiveBasicBlockLiveAtStart();

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
  RestoreBasicBlockLiveAtStartAfterFindBasicBlocks(saved_live, *ctx_);

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
        insn->fld[3].rtx->code != kUse && insn->fld[3].rtx->code != kClobber &&
        insn->fld[3].rtx->code != kAddrVec && insn->fld[3].rtx->code != kAddrDiffVec &&
        num_delay_slots(insn) > 0) {
      PushInsnToObstack(&unfilled_slots_obstack, insn);
    }
  }
  InitReorgEndOfFunctionNeeds();
  end_of_function_label = nullptr;
  target_hash_table = AllocZeroed<void*>(257);
  bb_ticks = AllocZeroed<int>(std::max(1, n_basic_blocks) + 4);
  reorg_pass_number = 0;
}

absl::flat_hash_map<int, absl::flat_hash_set<int>> FunctionHooks::CapturePostReorgLiveAtStart(
    Rtx first) {
  absl::flat_hash_map<int, absl::flat_hash_set<int>> post_live;
  if (first == nullptr || basic_block_live_at_start == nullptr || n_basic_blocks <= 0) {
    return post_live;
  }

  struct PostCfgItem {
    RtlInsnKind kind = RtlInsnKind::kOther;
    Rtx rtx = nullptr;
  };
  std::vector<PostCfgItem> items;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (IsUpdateBlockUseMarkerInsn(insn)) {
      continue;
    }
    const int code = insn->code;
    if (code == kNote) {
      if (IsLoopDematerializedNote(insn)) {
        items.push_back(PostCfgItem{.kind = RtlInsnKind::kNote, .rtx = insn});
      }
      continue;
    }
    if (code == kCodeLabel && insn->fld[2].rtx != nullptr && insn->fld[2].rtx->code == kJumpInsn) {
      Rtx next_pat = insn->fld[2].rtx->fld[3].rtx;
      if (next_pat != nullptr && (next_pat->code == kAddrVec || next_pat->code == kAddrDiffVec)) {
        insn = insn->fld[2].rtx;
        while (insn->fld[2].rtx != nullptr && insn->fld[2].rtx->code == kBarrier) {
          insn = insn->fld[2].rtx;
        }
        continue;
      }
    }
    if (code == kCodeLabel) {
      items.push_back(PostCfgItem{.kind = RtlInsnKind::kCodeLabel, .rtx = insn});
      continue;
    }
    if (code == kBarrier) {
      items.push_back(PostCfgItem{.kind = RtlInsnKind::kBarrier, .rtx = insn});
      continue;
    }
    if (code != kInsn && code != kJumpInsn && code != kCallInsn) {
      continue;
    }
    Rtx pat = insn->fld[3].rtx;
    if (code == kJumpInsn && pat != nullptr &&
        (pat->code == kAddrVec || pat->code == kAddrDiffVec)) {
      continue;
    }
    RtlInsnKind eff_kind =
        (code == kJumpInsn) ? RtlInsnKind::kJumpInsn
                            : ((code == kCallInsn) ? RtlInsnKind::kCallInsn : RtlInsnKind::kInsn);
    if (pat != nullptr && pat->code == kSequence && pat->fld[0].rtvec != nullptr) {
      Rtvec vec = pat->fld[0].rtvec;
      for (int j = 0; j < vec->num_elem; ++j) {
        if (vec->elem[j].rtx != nullptr && vec->elem[j].rtx->code == kJumpInsn) {
          eff_kind = RtlInsnKind::kJumpInsn;
        }
      }
    }
    items.push_back(PostCfgItem{.kind = eff_kind, .rtx = insn});
  }

  auto first_active_insn_from = [](Rtx start) -> Rtx {
    for (Rtx scan = start; scan != nullptr; scan = scan->fld[2].rtx) {
      if (IsUpdateBlockUseMarkerInsn(scan)) {
        continue;
      }
      if (scan->code == kInsn || scan->code == kJumpInsn || scan->code == kCallInsn) {
        Rtx pat = scan->fld[3].rtx;
        if (pat != nullptr && pat->code != kAddrVec && pat->code != kAddrDiffVec &&
            pat->code != kUse && pat->code != kClobber) {
          return scan;
        }
      }
    }
    return nullptr;
  };

  int block_idx = 0;
  size_t i = 0;
  std::vector<std::pair<Rtx, std::array<uint32_t, 3>>> live_blocks;
  while (i < items.size()) {
    if (items[i].kind == RtlInsnKind::kBarrier) {
      ++i;
      continue;
    }
    Rtx target_rtx = nullptr;
    bool has_label = false;
    bool has_insn = false;
    while (i < items.size() && items[i].kind == RtlInsnKind::kCodeLabel) {
      if (target_rtx == nullptr) {
        target_rtx = items[i].rtx;
      }
      has_label = true;
      ++i;
    }
    while (i < items.size() && items[i].kind != RtlInsnKind::kCodeLabel &&
           items[i].kind != RtlInsnKind::kBarrier) {
      if (target_rtx == nullptr) {
        target_rtx = items[i].rtx;
      }
      has_insn = true;
      if (items[i].kind == RtlInsnKind::kJumpInsn) {
        ++i;
        break;
      }
      ++i;
    }
    while (i < items.size() && items[i].kind == RtlInsnKind::kBarrier) {
      ++i;
    }
    if (!has_insn && !has_label) {
      continue;
    }
    Rtx query_target = target_rtx != nullptr ? first_active_insn_from(target_rtx) : nullptr;
    GccReorgResources res{};
    mark_target_live_regs(query_target, &res);
    auto& dst_set = post_live[block_idx];
    for (int r = 1; r < kFirstPseudoRegister; ++r) {
      if ((res.regs[r / 32] & (uint32_t{1} << (r % 32))) != 0u) {
        dst_set.insert(r);
      }
    }
    res.regs[0] &= ~uint32_t{1};
    Rtx head_rtx = (target_rtx != nullptr && target_rtx->code == kNote && query_target != nullptr)
                       ? query_target
                       : target_rtx;
    live_blocks.emplace_back(head_rtx,
                             std::array<uint32_t, 3>{res.regs[0], res.regs[1], res.regs[2]});
    ++block_idx;
  }

  if (!live_blocks.empty() && regset_size > 0) {
    const int num_live = static_cast<int>(live_blocks.size());
    const int alloc_blocks = num_live + 4;
    n_basic_blocks = num_live;
    basic_block_head = AllocZeroed<Rtx>(alloc_blocks);
    basic_block_live_at_start = AllocZeroed<Rtx>(alloc_blocks);
    for (int b = 0; b < alloc_blocks; ++b) {
      auto* words = AllocZeroed<uint32_t>(regset_size);
      basic_block_live_at_start[b] = reinterpret_cast<Rtx>(words);
      if (b < num_live) {
        basic_block_head[b] = live_blocks[b].first;
        const int copy_words = std::min<int>(regset_size, 3);
        for (int w = 0; w < copy_words; ++w) {
          words[w] = live_blocks[b].second[w];
        }
      }
    }
  }
  return post_live;
}

void CleanupReorgBlockMarkers(Rtx first) {
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (IsUpdateBlockUseMarkerInsn(insn)) {
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
  head = EnsureReorgLeadingSentinelNote(head);
  reload_completed = 1;
  max_regno = std::max(max_reg_num(), kFirstPseudoRegister);
  if (reg_renumber == nullptr) {
    const int alloc_regs = max_regno + 16;
    reg_renumber = AllocZeroed<short>(alloc_regs);
    for (int i = 0; i < alloc_regs; ++i) {
      reg_renumber[i] = -1;
    }
  }
  const SavedBlockLiveState saved_live = SaveActiveBasicBlockLiveAtStart();
  restore_reg_data(head);
  reg_scan(head, max_reg_num(), 0);
  RunFindBasicBlocks(head, nullptr);
  RefreshJumpLabelsAndNuses(head);
  RestoreBasicBlockLiveAtStartAfterFindBasicBlocks(saved_live, *ctx_);
  dbr_schedule(head, nullptr);
  CleanupReorgBlockMarkers(ctx_->GetInsns());
}

void FunctionHooks::RunFillSimpleDelaySlots(int non_jumps_p, Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  fill_simple_delay_slots(ctx_->GetInsns(), non_jumps_p);
  last_reorg_post_live_ = CapturePostReorgLiveAtStart(ctx_->GetInsns());
  CleanupReorgBlockMarkers(ctx_->GetInsns());
}

void FunctionHooks::RunFillEagerDelaySlots(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  fill_eager_delay_slots(ctx_->GetInsns());
  last_reorg_post_live_ = CapturePostReorgLiveAtStart(ctx_->GetInsns());
  CleanupReorgBlockMarkers(ctx_->GetInsns());
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
  if (ctx_->GetInsns() != nullptr) {
    CleanupReorgBlockMarkers(ctx_->GetInsns());
  }
  return res;
}

void FunctionHooks::RunRelaxDelaySlots(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  relax_delay_slots(ctx_->GetInsns());
  last_reorg_post_live_ = CapturePostReorgLiveAtStart(ctx_->GetInsns());
  CleanupReorgBlockMarkers(ctx_->GetInsns());
}

void FunctionHooks::RunMakeReturnInsns(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  PrepareReorgState(head);
  head = ctx_->GetInsns();
  // Match `reorg.c:4349-4355`: recover `end_of_function_label` if an earlier
  // `reorg.c` subpass created one at the tail of the insn chain, delete it if
  // unused (`LABEL_NUSES == 0` without `find_end_label()`'s temporary +1 pin),
  // and run `make_return_insns` when `HAVE_return` (`simple_epilogue_p() != 0`)
  // and `end_of_function_label != nullptr`.
  Rtx tail = get_last_insn();
  while (tail != nullptr &&
         (tail->code == kNote || tail->volatil != 0 ||
          (tail->code == kInsn && tail->fld[3].rtx != nullptr &&
           (tail->fld[3].rtx->code == kUse || tail->fld[3].rtx->code == kClobber)))) {
    tail = tail->fld[1].rtx;
  }
  if (tail != nullptr && tail->code == kCodeLabel) {
    end_of_function_label = tail;
  } else if (simple_epilogue_p() != 0 && tail != nullptr && tail->code == kBarrier) {
    Rtx prev = tail->fld[1].rtx;
    while (prev != nullptr && (prev->code == kNote || prev->volatil != 0)) {
      prev = prev->fld[1].rtx;
    }
    bool is_ret_jump = false;
    if (prev != nullptr && prev->code == kJumpInsn && prev->fld[3].rtx != nullptr &&
        prev->fld[3].rtx->code == kParallel) {
      is_ret_jump = true;
    } else if (prev != nullptr && prev->code == kInsn && prev->fld[3].rtx != nullptr &&
               prev->fld[3].rtx->code == kSequence && prev->fld[3].rtx->fld[0].rtvec != nullptr &&
               prev->fld[3].rtx->fld[0].rtvec->num_elem >= 1) {
      Rtx slot0 = prev->fld[3].rtx->fld[0].rtvec->elem[0].rtx;
      if (slot0 != nullptr && slot0->code == kJumpInsn && slot0->fld[3].rtx != nullptr &&
          slot0->fld[3].rtx->code == kParallel) {
        is_ret_jump = true;
      }
    }
    if (is_ret_jump) {
      Rtx lbl = prev->fld[1].rtx;
      while (lbl != nullptr && (lbl->code == kNote || lbl->volatil != 0)) {
        lbl = lbl->fld[1].rtx;
      }
      if (lbl != nullptr && lbl->code == kCodeLabel && lbl->fld[5].rtint == 0) {
        end_of_function_label = lbl;
      }
    }
  }
  if (end_of_function_label != nullptr && end_of_function_label->fld[5].rtint == 0) {
    delete_insn(end_of_function_label);
  }
  if (simple_epilogue_p() != 0 && end_of_function_label != nullptr) {
    make_return_insns(ctx_->GetInsns());
  }
  last_reorg_post_live_ = CapturePostReorgLiveAtStart(ctx_->GetInsns());
  CleanupReorgBlockMarkers(ctx_->GetInsns());
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
  const bool temp_deaths = (reg_n_deaths == nullptr);
  if (temp_deaths) {
    reg_n_deaths = AllocZeroed<short>(nregs + 16);
  }
  jump_optimize(head, cross_jump, noop_moves, after_regscan);
  // `jump.c:jump_optimize` returns early when `!optimize` without clearing the
  // stack-allocated `jump_chain` (`alloca`), which would leave a dangling pointer
  // for subsequent `delete_insn` calls.
  jump_chain = nullptr;
  max_jump_chain = 0;
  if (temp_deaths) {
    reg_n_deaths = nullptr;
  }
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
  PopulateInsnCodesWithVolatileOk(first);
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
  bb_dead_regs = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_size + 4));
  bb_live_regs = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_size + 4));
}

namespace {

struct SavedFlowState {
  bool valid = false;
  std::vector<std::pair<Rtx, Rtx>> insn_reg_notes;
  std::vector<int> saved_n_refs;
  std::vector<short> saved_n_sets;
  std::vector<short> saved_n_deaths;
  std::vector<int> saved_live_length;
  std::vector<int> saved_n_calls_crossed;
};

SavedFlowState CaptureExistingFlowState(Rtx first) {
  SavedFlowState state;
  if (first == nullptr || reg_live_length == nullptr || reg_n_deaths == nullptr ||
      basic_block_live_at_start == nullptr) {
    return state;
  }
  const int nregs = std::max(max_regno, max_reg_num());
  bool has_reg_dead = false;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) {
      for (Rtx link = insn->fld[6].rtx; link != nullptr; link = link->fld[1].rtx) {
        if (static_cast<int>(link->mode) == kRegDead ||
            static_cast<int>(link->mode) == kRegUnused) {
          has_reg_dead = true;
        }
      }
    }
  }
  bool has_live_lengths = false;
  for (int r = kFirstPseudoRegister; r < nregs; ++r) {
    if (reg_live_length[r] != 0) {
      has_live_lengths = true;
      break;
    }
  }
  if (!has_reg_dead && !has_live_lengths) {
    return SavedFlowState{};
  }
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) {
      state.insn_reg_notes.emplace_back(insn, insn->fld[6].rtx);
      insn->fld[6].rtx = nullptr;
    }
  }
  if (nregs > 0) {
    if (reg_n_refs != nullptr) {
      state.saved_n_refs.assign(reg_n_refs, reg_n_refs + nregs);
    }
    if (reg_n_sets != nullptr) {
      state.saved_n_sets.assign(reg_n_sets, reg_n_sets + nregs);
    }
    state.saved_n_deaths.assign(reg_n_deaths, reg_n_deaths + nregs);
    state.saved_live_length.assign(reg_live_length, reg_live_length + nregs);
    if (reg_n_calls_crossed != nullptr) {
      state.saved_n_calls_crossed.assign(reg_n_calls_crossed, reg_n_calls_crossed + nregs);
    }
  }
  state.valid = true;
  return state;
}

void RestoreExistingFlowState(const SavedFlowState& state) {
  if (!state.valid) {
    return;
  }
  for (const auto& [insn, notes] : state.insn_reg_notes) {
    insn->fld[6].rtx = notes;
  }
  const int nregs = std::min<int>(state.saved_live_length.size(), max_regno);
  for (int r = 0; r < nregs; ++r) {
    if (reg_live_length != nullptr && reg_live_length[r] == 0 && state.saved_live_length[r] != 0 &&
        r < static_cast<int>(state.saved_n_refs.size()) && state.saved_n_refs[r] > 0) {
      if (reg_n_refs != nullptr) {
        reg_n_refs[r] = state.saved_n_refs[r];
      }
      reg_live_length[r] = state.saved_live_length[r];
      continue;
    }
    if (reg_live_length != nullptr && reg_live_length[r] <= 0) {
      continue;
    }
    if (r < static_cast<int>(state.saved_n_refs.size()) && reg_n_refs != nullptr &&
        reg_n_refs[r] > 0 && state.saved_n_refs[r] > 0) {
      reg_n_refs[r] = state.saved_n_refs[r];
    }
    if (r < static_cast<int>(state.saved_n_sets.size()) && reg_n_sets != nullptr &&
        state.saved_n_sets[r] > 0) {
      reg_n_sets[r] = state.saved_n_sets[r];
    }
    if (r < static_cast<int>(state.saved_n_deaths.size()) && reg_n_deaths != nullptr &&
        state.saved_n_deaths[r] > 0) {
      reg_n_deaths[r] = state.saved_n_deaths[r];
    }
    if (reg_live_length != nullptr && state.saved_live_length[r] > 0) {
      reg_live_length[r] = state.saved_live_length[r];
    }
    if (r < static_cast<int>(state.saved_n_calls_crossed.size()) &&
        reg_n_calls_crossed != nullptr) {
      reg_n_calls_crossed[r] = state.saved_n_calls_crossed[r];
    }
  }
}

}  // namespace

void FunctionHooks::RunScheduleInsns(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  const int saved_reload_completed = reload_completed;
  if (saved_reload_completed == 0 || basic_block_live_at_start == nullptr) {
    const int saved_nregs = std::max(max_regno, max_reg_num());
    std::vector<int> saved_n_refs;
    std::vector<int> saved_live_length;
    if (saved_reload_completed == 0 && saved_nregs > 0) {
      if (reg_n_refs != nullptr) {
        saved_n_refs.assign(reg_n_refs, reg_n_refs + saved_nregs);
      }
      if (reg_live_length != nullptr) {
        saved_live_length.assign(reg_live_length, reg_live_length + saved_nregs);
      }
    }
    StripFlowGeneratedNotes(head);
    restore_reg_data(head);
    max_regno = max_reg_num();
    reg_scan(head, max_regno, 0);
    flow_analysis(head, max_regno, nullptr);
    if (!saved_n_refs.empty() && reg_n_refs != nullptr) {
      const int limit = std::min<int>(saved_n_refs.size(), max_regno);
      for (int r = 0; r < limit; ++r) {
        if (saved_n_refs[r] > 0) {
          if (reg_n_refs[r] > 0) {
            reg_n_refs[r] = saved_n_refs[r];
          } else if (r < static_cast<int>(saved_live_length.size()) && saved_live_length[r] != 0 &&
                     reg_live_length != nullptr && reg_live_length[r] == 0) {
            reg_n_refs[r] = saved_n_refs[r];
            reg_live_length[r] = saved_live_length[r];
          }
        }
      }
    }
  } else {
    restore_reg_data(head);
    max_regno = max_reg_num();
    reg_scan(head, max_regno, 0);
    if (n_basic_blocks == 0 || basic_block_head == nullptr || basic_block_end == nullptr) {
      const SavedBlockLiveState saved_live = SaveActiveBasicBlockLiveAtStart();
      RunFindBasicBlocks(head, nullptr);
      RefreshJumpLabelsAndNuses(head);
      RestoreBasicBlockLiveAtStartAfterFindBasicBlocks(saved_live, *ctx_);
    }
  }
  init_alias_analysis();
  PopulateInsnCodesWithVolatileOk(head);
  reload_completed = saved_reload_completed;
  if (n_basic_blocks > 0 && basic_block_end[n_basic_blocks - 1] != nullptr &&
      basic_block_end[n_basic_blocks - 1]->fld[2].rtx == nullptr) {
    emit_note(nullptr, kNoteInsnDeleted);
  }
  schedule_insns(nullptr);
}

void FunctionHooks::RunScheduleBlock(int bb_index, Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  if (!in_sched_pass_) {
    PrepareSchedState(head);
  }
  if (bb_index >= 0 && bb_index < n_basic_blocks) {
    schedule_block(bb_index, nullptr);
  }
}

void FunctionHooks::ExecuteSchedPassGranular(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  const int saved_reload_completed = reload_completed;
  if (saved_reload_completed == 0 || basic_block_live_at_start == nullptr) {
    const int saved_nregs = std::max(max_regno, max_reg_num());
    std::vector<int> saved_n_refs;
    std::vector<int> saved_live_length;
    if (saved_reload_completed == 0 && saved_nregs > 0) {
      if (reg_n_refs != nullptr) {
        saved_n_refs.assign(reg_n_refs, reg_n_refs + saved_nregs);
      }
      if (reg_live_length != nullptr) {
        saved_live_length.assign(reg_live_length, reg_live_length + saved_nregs);
      }
    }
    StripFlowGeneratedNotes(head);
    restore_reg_data(head);
    max_regno = max_reg_num();
    reg_scan(head, max_regno, 0);
    flow_analysis(head, max_regno, nullptr);
    if (!saved_n_refs.empty() && reg_n_refs != nullptr) {
      const int limit = std::min<int>(saved_n_refs.size(), max_regno);
      for (int r = 0; r < limit; ++r) {
        if (saved_n_refs[r] > 0) {
          if (reg_n_refs[r] > 0) {
            reg_n_refs[r] = saved_n_refs[r];
          } else if (r < static_cast<int>(saved_live_length.size()) && saved_live_length[r] != 0 &&
                     reg_live_length != nullptr && reg_live_length[r] == 0) {
            reg_n_refs[r] = saved_n_refs[r];
            reg_live_length[r] = saved_live_length[r];
          }
        }
      }
    }
  } else {
    const SavedBlockLiveState saved_live = SaveActiveBasicBlockLiveAtStart();
    restore_reg_data(head);
    max_regno = max_reg_num();
    reg_scan(head, max_regno, 0);
    RunFindBasicBlocks(head, nullptr);
    RefreshJumpLabelsAndNuses(head);
    RestoreBasicBlockLiveAtStartAfterFindBasicBlocks(saved_live, *ctx_);
  }
  init_alias_analysis();
  PopulateInsnCodesWithVolatileOk(head);
  reload_completed = saved_reload_completed;
  if (n_basic_blocks <= 0) {
    return;
  }

  const int max_uid = (get_max_uid() + 1) * 4;
  sched_before_next_call = make_insn_raw(nullptr);
  if (reload_completed == 0 || flag_schedule_insns == 0) {
    unused_insn_list = nullptr;
    unused_expr_list = nullptr;
  }
  insn_luid = AllocZeroed<int>(max_uid + 1);
  insn_priority = AllocZeroed<int>(max_uid + 1);
  insn_tick = AllocZeroed<int>(max_uid + 1);
  insn_costs = AllocZeroed<short>(max_uid + 1);
  insn_units = AllocZeroed<short>(max_uid + 1);
  insn_blockage = AllocZeroed<unsigned int>(max_uid + 1);
  insn_ref_count = AllocZeroed<int>(max_uid + 1);

  if (reload_completed != 0) {
    sched_reg_n_calls_crossed = nullptr;
    sched_reg_live_length = nullptr;
    bb_dead_regs = nullptr;
    bb_live_regs = nullptr;
  } else {
    sched_reg_n_calls_crossed = AllocZeroed<int>(max_regno + 16);
    sched_reg_live_length = AllocZeroed<int>(max_regno + 16);
    bb_dead_regs = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_size + 4));
    bb_live_regs = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_size + 4));
  }

  if (basic_block_end[n_basic_blocks - 1] != nullptr &&
      basic_block_end[n_basic_blocks - 1]->fld[2].rtx == nullptr) {
    emit_note(nullptr, kNoteInsnDeleted);
  }

  in_sched_pass_ = true;
  for (int b = 0; b < n_basic_blocks; ++b) {
    note_list = nullptr;
    for (Rtx insn = basic_block_head[b]; insn != nullptr;) {
      Rtx prev = insn->fld[1].rtx;
      Rtx next = insn->fld[2].rtx;
      if (insn->code == kInsn) {
        Rtx set = single_set(insn);
        if (set != nullptr && rtx_equal_p(set->fld[1].rtx, set->fld[0].rtx) != 0) {
          if (insn == basic_block_end[b]) {
            break;
          }
          if (reload_completed != 0) {
            insn->code = kNote;
            insn->fld[4].rtint = kNoteInsnDeleted;
            insn->fld[3].rtx = nullptr;
          }
          insn = next;
          continue;
        }
        if (reload_completed == 0) {
          Rtx notes = insn->fld[6].rtx;
          Rtx last = try_split(insn->fld[3].rtx, insn, 1);
          if (last != insn) {
            Rtx first_new = prev->fld[2].rtx;
            update_flow_info(notes, first_new, last, insn);
            insn->code = kNote;
            insn->fld[3].rtx = nullptr;
            insn->fld[4].rtint = kNoteInsnDeleted;
            if (basic_block_head[b] == insn) {
              basic_block_head[b] = first_new;
            }
            if (basic_block_end[b] == insn) {
              basic_block_end[b] = last;
              break;
            }
            insn = last->fld[2].rtx;
            continue;
          }
        }
      }
      if (insn == basic_block_end[b]) {
        break;
      }
      insn = next;
    }
    RunScheduleBlock(b, head);
  }
  in_sched_pass_ = false;

  if (reload_completed != 0) {
    reposition_prologue_and_epilogue_notes(get_insns());
  } else {
    for (int i = 0; i < max_regno; ++i) {
      if (sched_reg_live_length[i] != 0) {
        if (reg_live_length[i] >= 0) {
          reg_live_length[i] = sched_reg_live_length[i];
        }
        if (sched_reg_n_calls_crossed[i] != 0) {
          reg_n_calls_crossed[i] = sched_reg_n_calls_crossed[i];
        } else if (reg_n_calls_crossed[i] != 0 && reg_basic_block[i] != -2) {
          reg_n_calls_crossed[i] = 0;
        }
      }
    }
  }
}

void FunctionHooks::PrepareLocalAllocState(Rtx first) {
  const SavedFlowState saved_flow = CaptureExistingFlowState(first);
  StripFlowGeneratedNotes(first);
  restore_reg_data(first);
  max_regno = max_reg_num();
  reg_scan(first, max_regno, 1);
  flow_analysis(first, max_regno, nullptr);
  RestoreExistingFlowState(saved_flow);
  regclass(first, max_regno);

  const int max_qty_val = std::max(32, (max_regno - kFirstPseudoRegister) * 2 + max_scratch + 16);
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
        reg_basic_block[i] >= 0 && reg_n_deaths[i] == 1 &&
        (reg_alternate_class(i) == 0 || reg_class_size[reg_preferred_class(i)] != 1)) {
      lra_reg_qty[i] = -2;
    } else {
      lra_reg_qty[i] = -1;
    }
    reg_renumber[i] = -1;
  }
  lra_next_qty = max_qty_val;
}

void FunctionHooks::RunLocalAlloc(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  const SavedFlowState saved_flow = CaptureExistingFlowState(head);
  StripFlowGeneratedNotes(head);
  restore_reg_data(head);
  max_regno = max_reg_num();
  reg_scan(head, max_regno, 1);
  flow_analysis(head, max_regno, nullptr);
  RestoreExistingFlowState(saved_flow);
  regclass(head, max_regno);
  local_alloc();
}

void FunctionHooks::RunBlockAlloc(int bb_index, Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  if (!in_local_alloc_pass_) {
    PrepareLocalAllocState(head);
  }
  if (bb_index >= 0 && bb_index < n_basic_blocks) {
    for (int i = 0; i < lra_next_qty; ++i) {
      lra_qty_first_reg[i] = -1;
      qty_scratch_rtx[i] = nullptr;
      qty_size[i] = 0;
      static_cast<int*>(lra_qty_mode)[i] = 0;
      static_cast<int*>(qty_min_class)[i] = 0;
      static_cast<int*>(qty_alternate_class)[i] = 0;
      qty_birth[i] = -1;
      qty_death[i] = -1;
      qty_n_calls_crossed[i] = 0;
      qty_n_refs[i] = 0;
      qty_changes_size[i] = 0;
      qty_phys_reg[i] = -1;
      qty_phys_num_sugg[i] = 0;
      qty_phys_num_copy_sugg[i] = 0;
    }
    if (lra_next_qty > 0) {
      std::memset(qty_phys_sugg, 0, lra_next_qty * 2 * sizeof(uint64_t));
      std::memset(qty_phys_copy_sugg, 0, lra_next_qty * 2 * sizeof(uint64_t));
    }
    lra_next_qty = 0;
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
  DeduplicateRegEquivNotes(head);
}

namespace {

int MipsHardRegnoNregs(int regno, int mode) {
  const int msize =
      (mode >= 0 && mode < kNumMachineModes && mode_size[mode] > 0) ? mode_size[mode] : 4;
  const bool is_fp_reg = (regno >= 32 && regno < 64);
  const int unit_bytes = is_fp_reg ? ((target_flags & 0x00000200) != 0 ? 8 : 4)
                                   : ((target_flags & 0x00002000) != 0 ? 8 : 4);
  return std::max(1, (msize + unit_bytes - 1) / unit_bytes);
}

int MipsPseudoRegnoSize(int regno) {
  const int mode = (regno_reg_rtx != nullptr && regno >= 0 && regno < max_regno &&
                    regno_reg_rtx[regno] != nullptr)
                       ? static_cast<int>(regno_reg_rtx[regno]->mode)
                       : static_cast<int>(kSiMode);
  const int msize =
      (mode >= 0 && mode < kNumMachineModes && mode_size[mode] > 0) ? mode_size[mode] : 4;
  const int word_bytes = (target_flags & 0x00002000) != 0 ? 8 : 4;
  return std::max(1, (msize + word_bytes - 1) / word_bytes);
}

void SetHardRegBit(GccHardRegSet set, int regno) {
  if (regno >= 0 && regno < kFirstPseudoRegister) {
    set[regno / 32] |= (uint32_t{1} << (regno % 32));
  }
}

// Initializes all `global.c` static tables read by `reload(..., /*global=*/1, ...)`
// (`retry_global_alloc`, `find_reg`, `mark_elimination`) using heap-allocated
// buffers so `RunReload(first, 1)` never dereferences dangling `alloca` frames
// or uninitialized pointers.
void PrepareGlobalReloadState() {
  const int nregs = std::max({max_regno, max_reg_num(), kFirstPseudoRegister}) + 16;
  if (reg_changes_size == nullptr) {
    reg_changes_size = AllocZeroed<char>(nregs);
  }
  if (regno_reg_rtx != nullptr && reg_renumber != nullptr) {
    for (int i = kFirstPseudoRegister; i < max_regno; ++i) {
      if (reg_renumber[i] >= 0 && regno_reg_rtx[i] == nullptr) {
        regno_reg_rtx[i] = gen_rtx(kReg, kSiMode, i);
      }
    }
  }
  max_allocno = 0;
  std::memset(no_global_alloc_regs, 0, sizeof(GccHardRegSet));
  std::memset(eliminable_regset, 0, sizeof(GccHardRegSet));
  // `mips.h:1296-1305, 1734-1749`: `ELIMINABLE_REGS` pairs `ARG_POINTER_REGNUM`
  // (`0`) and `FRAME_POINTER_REGNUM` (`30`) with `STACK_POINTER_REGNUM` (`29`),
  // so `global.c:308-315` sets both `0` and `30` in `eliminable_regset` (and in
  // `no_global_alloc_regs` whenever `need_fp` is true).
  constexpr int kMipsArgPointerRegnum = 0;
  constexpr int kMipsFramePointerRegnum = 30;
  SetHardRegBit(eliminable_regset, kMipsArgPointerRegnum);
  SetHardRegBit(eliminable_regset, kMipsFramePointerRegnum);
  const bool need_fp = (flag_omit_frame_pointer == 0 || current_function_calls_alloca != 0 ||
                        frame_pointer_needed != 0);
  if (need_fp) {
    SetHardRegBit(no_global_alloc_regs, kMipsArgPointerRegnum);
    SetHardRegBit(no_global_alloc_regs, kMipsFramePointerRegnum);
  }

  std::memset(regs_used_so_far, 0, sizeof(GccHardRegSet));
  for (int i = 0; i < kFirstPseudoRegister; ++i) {
    if (regs_ever_live[i] != 0 || call_used_regs[i] != 0) {
      SetHardRegBit(regs_used_so_far, i);
    }
  }
  for (int i = kFirstPseudoRegister; i < max_regno; ++i) {
    if (reg_renumber != nullptr && reg_renumber[i] >= 0) {
      const int mode = (regno_reg_rtx != nullptr && regno_reg_rtx[i] != nullptr)
                           ? static_cast<int>(regno_reg_rtx[i]->mode)
                           : static_cast<int>(kSiMode);
      const int lim = reg_renumber[i] + MipsHardRegnoNregs(reg_renumber[i], mode);
      for (int r = reg_renumber[i]; r < lim; ++r) {
        SetHardRegBit(regs_used_so_far, r);
      }
    }
  }

  reg_allocno = AllocZeroed<int>(nregs);
  for (int i = 0; i < nregs; ++i) {
    reg_allocno[i] = -1;
  }
  reg_may_share = AllocZeroed<int>(nregs);
  for (Rtx x = regs_may_share; x != nullptr && x->fld[0].rtx != nullptr && x->fld[1].rtx != nullptr;
       x = x->fld[1].rtx->fld[1].rtx) {
    const int r1 = x->fld[0].rtx->fld[0].rtint;
    const int r2 =
        x->fld[1].rtx->fld[0].rtx != nullptr ? x->fld[1].rtx->fld[0].rtx->fld[0].rtint : -1;
    if (r1 >= 0 && r2 >= 0 && r1 < nregs && r2 < nregs) {
      if (r1 > r2) {
        reg_may_share[r1] = r2;
      } else {
        reg_may_share[r2] = r1;
      }
    }
  }

  for (int i = kFirstPseudoRegister; i < max_regno; ++i) {
    const bool is_global_candidate =
        regno_reg_rtx != nullptr && regno_reg_rtx[i] != nullptr && regno_reg_rtx[i]->code == kReg &&
        reg_n_refs != nullptr && reg_live_length != nullptr && reg_n_calls_crossed != nullptr &&
        reg_n_refs[i] != 0 && reg_live_length[i] != -1 && reg_live_length[i] != 0 &&
        (current_function_has_nonlocal_label == 0 || reg_n_calls_crossed[i] == 0) &&
        (reg_renumber == nullptr || reg_renumber[i] < 0 || reg_basic_block == nullptr ||
         reg_basic_block[i] < 0 || (lra_reg_qty != nullptr && lra_reg_qty[i] == -1) ||
         reg_renumber[i] == kMipsArgPointerRegnum || reg_renumber[i] == kMipsFramePointerRegnum ||
         reg_renumber[i] >= 64);
    if (is_global_candidate) {
      if (reg_may_share[i] != 0 && reg_allocno[reg_may_share[i]] >= 0) {
        reg_allocno[i] = reg_allocno[reg_may_share[i]];
      } else {
        reg_allocno[i] = max_allocno++;
      }
    }
  }

  const int alloc_cap = std::max(1, max_allocno);
  allocno_reg = AllocZeroed<int>(alloc_cap);
  allocno_size = AllocZeroed<int>(alloc_cap);
  allocno_calls_crossed = AllocZeroed<int>(alloc_cap);
  allocno_n_refs = AllocZeroed<int>(alloc_cap);
  allocno_live_length = AllocZeroed<int>(alloc_cap);
  allocno_order = AllocZeroed<int>(alloc_cap);

  for (int i = kFirstPseudoRegister; i < max_regno; ++i) {
    if (reg_allocno[i] >= 0) {
      const int a = reg_allocno[i];
      allocno_reg[a] = i;
      allocno_size[a] = MipsPseudoRegnoSize(i);
      allocno_calls_crossed[a] += reg_n_calls_crossed[i];
      allocno_n_refs[a] += reg_n_refs[i];
      if (allocno_live_length[a] < reg_live_length[i]) {
        allocno_live_length[a] = reg_live_length[i];
      }
    }
  }

  std::memset(local_reg_live_length, 0, sizeof(local_reg_live_length));
  std::memset(local_reg_n_refs, 0, sizeof(local_reg_n_refs));
  for (int i = kFirstPseudoRegister; i < max_regno; ++i) {
    if (reg_allocno[i] < 0 && reg_renumber != nullptr && reg_renumber[i] >= 0) {
      const int regno = reg_renumber[i];
      const int mode = (regno_reg_rtx != nullptr && regno_reg_rtx[i] != nullptr)
                           ? static_cast<int>(regno_reg_rtx[i]->mode)
                           : static_cast<int>(kSiMode);
      const int endregno = regno + MipsHardRegnoNregs(regno, mode);
      for (int j = regno; j < endregno && j < kFirstPseudoRegister; ++j) {
        local_reg_n_refs[j] += reg_n_refs[i];
        local_reg_live_length[j] += reg_live_length[i];
      }
    }
  }
  for (int i = 0; i < kFirstPseudoRegister; ++i) {
    if (regs_ever_live[i] != 0) {
      local_reg_n_refs[i] = 0;
    }
  }
  for (int i = 0; i < scratch_list_length; ++i) {
    if (scratch_list != nullptr && scratch_list[i] != nullptr && scratch_list[i]->code == kReg) {
      const int regno = scratch_list[i]->fld[0].rtint;
      const int lim = regno + MipsHardRegnoNregs(regno, scratch_list[i]->mode);
      for (int j = regno; j < lim && j < kFirstPseudoRegister; ++j) {
        local_reg_n_refs[j] = 0;
      }
    }
  }

  hard_reg_conflicts = AllocZeroed<GccHardRegSet>(alloc_cap);
  hard_reg_preferences = AllocZeroed<GccHardRegSet>(alloc_cap);
  hard_reg_copy_preferences = AllocZeroed<GccHardRegSet>(alloc_cap);
  hard_reg_full_preferences = AllocZeroed<GccHardRegSet>(alloc_cap);
  regs_someone_prefers = AllocZeroed<GccHardRegSet>(alloc_cap);

  allocno_row_words = std::max(1, (max_allocno + 31) / 32);
  conflicts = AllocZeroed<int>(alloc_cap * allocno_row_words);
  allocnos_live = AllocZeroed<int>(allocno_row_words);
  if (max_parallel < 1) {
    max_parallel = 16;
  }

  if (max_allocno > 0) {
    // On MIPS (`PRESERVE_DEATH_INFO_REGNO_P` undefined), `reload` mutates
    // spilled pseudo `REG` nodes in-place into `(mem ...)`, which can leave
    // `(expr_list:REG_DEAD (mem ...) ...)` in `REG_NOTES` on partially-lifted
    // CFGs; `global_conflicts()` assumes every `REG_DEAD`/`REG_INC` datum is a
    // `REG` (`global.c:722, 734`). Temporarily filter out non-`REG` or
    // out-of-range death/inc notes while computing conflicts and preferences.
    std::vector<std::pair<Rtx, Rtx>> saved_insn_notes;
    for (Rtx insn = get_insns(); insn != nullptr; insn = insn->fld[2].rtx) {
      if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
          insn->fld[6].rtx != nullptr) {
        bool has_non_reg_note = false;
        for (Rtx link = insn->fld[6].rtx; link != nullptr; link = link->fld[1].rtx) {
          const int kind = static_cast<int>(link->mode);
          if (kind == kRegDead || kind == kRegInc) {
            Rtx datum = link->fld[0].rtx;
            if (datum == nullptr || datum->code != kReg || datum->fld[0].rtint < 0 ||
                datum->fld[0].rtint >= nregs) {
              has_non_reg_note = true;
              break;
            }
          }
        }
        if (has_non_reg_note) {
          saved_insn_notes.emplace_back(insn, insn->fld[6].rtx);
          Rtx filtered = nullptr;
          for (Rtx link = insn->fld[6].rtx; link != nullptr; link = link->fld[1].rtx) {
            const int kind = static_cast<int>(link->mode);
            Rtx datum = link->fld[0].rtx;
            if ((kind == kRegDead || kind == kRegInc) &&
                (datum == nullptr || datum->code != kReg || datum->fld[0].rtint < 0 ||
                 datum->fld[0].rtint >= nregs)) {
              continue;
            }
            filtered = gen_rtx(link->code, link->mode, datum, filtered);
          }
          insn->fld[6].rtx = filtered;
        }
      }
    }

    global_conflicts();
    for (int a = 0; a < max_allocno; ++a) {
      for (int w = 0; w < 3; ++w) {
        hard_reg_conflicts[a][w] &= ~eliminable_regset[w];
        hard_reg_copy_preferences[a][w] &= ~eliminable_regset[w];
        hard_reg_preferences[a][w] &= ~eliminable_regset[w];
      }
    }
    expand_preferences();

    for (const auto& [insn, orig_notes] : saved_insn_notes) {
      insn->fld[6].rtx = orig_notes;
    }
    for (int a = 0; a < max_allocno; ++a) {
      allocno_order[a] = a;
      if (allocno_size[a] == 0) {
        allocno_size[a] = 1;
      }
      if (allocno_live_length[a] == 0) {
        allocno_live_length[a] = -1;
      }
    }
    std::qsort(allocno_order, max_allocno, sizeof(int), allocno_compare);
    prune_preferences();

    // Propagate already-allocated global hard registers into conflicting
    // allocnos' `hard_reg_conflicts` sets (`global.c:find_reg` lines 1175-1191).
    for (int a = 0; a < max_allocno; ++a) {
      const int p_reg = allocno_reg[a];
      if (reg_renumber != nullptr && p_reg >= kFirstPseudoRegister && p_reg < max_regno &&
          reg_renumber[p_reg] >= 0) {
        const int best_reg = reg_renumber[p_reg];
        const int mode = (regno_reg_rtx != nullptr && regno_reg_rtx[p_reg] != nullptr)
                             ? static_cast<int>(regno_reg_rtx[p_reg]->mode)
                             : static_cast<int>(kSiMode);
        const int lim = best_reg + MipsHardRegnoNregs(best_reg, mode);
        GccHardRegSet this_reg = {0, 0, 0};
        for (int r = best_reg; r < lim && r < kFirstPseudoRegister; ++r) {
          SetHardRegBit(this_reg, r);
          local_reg_n_refs[r] = 0;
        }
        for (int j = 0; j < max_allocno; ++j) {
          const bool conflict_aj =
              (conflicts[a * allocno_row_words + (j / 32)] & (1 << (j % 32))) != 0;
          const bool conflict_ja =
              (conflicts[j * allocno_row_words + (a / 32)] & (1 << (a % 32))) != 0;
          if (conflict_aj || conflict_ja) {
            for (int w = 0; w < 3; ++w) {
              hard_reg_conflicts[j][w] |= this_reg[w];
            }
          }
        }
      }
    }
  }
}

}  // namespace

int FunctionHooks::RunGlobalAlloc(Rtx first) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return 0;
  }
  const int saved_max_regno = std::max(max_regno, max_reg_num());
  std::vector<short> saved_renumber;
  if (reg_renumber != nullptr && saved_max_regno > 0) {
    saved_renumber.assign(reg_renumber, reg_renumber + saved_max_regno);
  }
  PrepareLocalAllocState(head);
  if (!saved_renumber.empty() && reg_renumber != nullptr) {
    const int limit = std::min<int>(saved_renumber.size(), max_regno + 16);
    for (int i = 0; i < limit; ++i) {
      reg_renumber[i] = saved_renumber[i];
    }
  }
  const int res = global_alloc(nullptr);
  // Clear `global_alloc`'s stack-allocated `alloca` pointers immediately on
  // return so they never dangle across calls.
  max_allocno = 0;
  reg_allocno = nullptr;
  return res;
}

int FunctionHooks::RunReload(Rtx first, int global) {
  Rtx head = first != nullptr ? first : ctx_->GetInsns();
  if (head == nullptr) {
    return 0;
  }
  const int global_flag = (global != 0) ? 1 : 0;
  const int saved_max_regno = std::max(max_regno, max_reg_num());
  std::vector<short> saved_renumber;
  if (reg_renumber != nullptr && saved_max_regno > 0) {
    saved_renumber.assign(reg_renumber, reg_renumber + saved_max_regno);
  }
  const int saved_frame_offset = frame_offset;
  char saved_regs_ever_live[kFirstPseudoRegister];
  std::memcpy(saved_regs_ever_live, regs_ever_live, sizeof(saved_regs_ever_live));

  reload_completed = 0;
  if ((obey_regdecls != 0 || optimize == 0) && global_flag == 0) {
    StripFlowGeneratedNotes(head);
    restore_reg_data(head);
    max_regno = max_reg_num();
    reg_scan(head, max_regno, 0);
    regclass_init();
    regclass(head, max_regno);
    stupid_life_analysis(head, max_regno, nullptr);
    scratch_list_length = 0;
    scratch_list = nullptr;
  } else {
    PrepareLocalAllocState(head);
  }

  if (!saved_renumber.empty() && reg_renumber != nullptr) {
    const int limit = std::min<int>(saved_renumber.size(), max_regno + 16);
    for (int i = 0; i < limit; ++i) {
      reg_renumber[i] = saved_renumber[i];
    }
  }
  frame_offset = saved_frame_offset;
  for (int i = 0; i < kFirstPseudoRegister; ++i) {
    if (saved_regs_ever_live[i] != 0) {
      regs_ever_live[i] = 1;
    }
  }

  if (global_flag != 0) {
    PrepareGlobalReloadState();
  }

  const int res = reload(head, global_flag, nullptr);
  max_allocno = 0;
  reg_allocno = nullptr;
  reload_completed = 1;
  return res;
}

void FunctionHooks::RunAlterReg(int regno, int from_reg) {
  Rtx head = ctx_->GetInsns();
  if (reg_renumber == nullptr && head != nullptr) {
    PrepareLocalAllocState(head);
  }
  if (reg_renumber != nullptr && regno >= kFirstPseudoRegister && reg_renumber[regno] < 0) {
    if (regno_reg_rtx != nullptr && regno_reg_rtx[regno] != nullptr &&
        regno_reg_rtx[regno]->code == kReg && (reg_n_refs == nullptr || reg_n_refs[regno] > 0)) {
      Rtx reg = regno_reg_rtx[regno];
      const int mode = reg->mode;
      const int inherent_size =
          (mode >= 0 && mode < kNumMachineModes && mode_size[mode] > 0) ? mode_size[mode] : 4;
      virtuals_instantiated = 1;
      Rtx slot = assign_stack_local(mode, inherent_size, -1);
      if (slot != nullptr && slot->code == kMem) {
        reg->code = kMem;
        reg->fld[0].rtx = slot->fld[0].rtx;
      }
    }
    return;
  }
  alter_reg(regno, from_reg);
}

// -----------------------------------------------------------------------------
// Stage 7: Combine (`combine.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::PrepareCombineState(Rtx first) {
  const int prev_max_reg = max_reg_num();
  restore_reg_data(first);
  const int nregs = std::max({max_reg_num(), prev_max_reg, 256}) + 16;
  max_regno = std::max(max_reg_num(), prev_max_reg);
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
    StripFlowGeneratedNotes(first);
    flow_analysis(first, max_regno, nullptr);
  } else {
    RunFindBasicBlocks(first, nullptr);
    reg_n_refs = AllocZeroed<int>(nregs);
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
  PopulateInsnCodesWithVolatileOk(first);
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
  nonzero_sign_valid = 0;
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
    StripFlowGeneratedNotes(head);
    flow_analysis(head, num_regs, nullptr);
  } else {
    PrepareCombineState(head);
  }
  combine_instructions(head, num_regs);
}

Rtx FunctionHooks::RunTryCombine(Rtx i3, Rtx i2, Rtx i1) {
  Rtx head = ctx_->GetInsns();
  if (!in_combine_pass_ && head != nullptr) {
    PrepareCombineState(head);
  }
  return try_combine(i3, i2, i1);
}

void FunctionHooks::ExecuteCombinePassGranular(Rtx first, int nregs) {
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
    StripFlowGeneratedNotes(head);
    flow_analysis(head, num_regs, nullptr);
  }

  const int alloc_regs = std::max(num_regs, max_reg_num()) + 16;
  combine_max_regno = num_regs;
  undobuf.num_undo = 0;
  undobuf.other_insn = nullptr;
  previous_num_undos = 0;

  reg_nonzero_bits = AllocZeroed<uint64_t>(alloc_regs);
  reg_sign_bit_copies = AllocZeroed<char>(alloc_regs);
  reg_last_death = AllocZeroed<Rtx>(alloc_regs);
  reg_last_set = AllocZeroed<Rtx>(alloc_regs);
  reg_last_set_value = AllocZeroed<Rtx>(alloc_regs);
  reg_last_set_table_tick = AllocZeroed<int>(alloc_regs);
  reg_last_set_label = AllocZeroed<int>(alloc_regs);
  reg_last_set_invalid = AllocZeroed<char>(alloc_regs);
  reg_last_set_mode = AllocZeroed<int>(alloc_regs);
  reg_last_set_nonzero_bits = AllocZeroed<uint64_t>(alloc_regs);
  reg_last_set_sign_bit_copies = AllocZeroed<char>(alloc_regs);
  init_reg_last_arrays();
  PopulateInsnCodesWithVolatileOk(head);
  init_recog_no_volatile();

  int max_uid = 0;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->fld[0].rtint > max_uid) {
      max_uid = insn->fld[0].rtint;
    }
  }
  max_uid += 64;
  max_uid_cuid = max_uid;
  combine_uid_cuid = AllocZeroed<int>(max_uid + 1);
  int cuid = 0;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    combine_uid_cuid[insn->fld[0].rtint] = ++cuid;
  }

  nonzero_bits_mode = kSiMode;
  label_tick = 1;
  last_call_cuid = 0;
  mem_last_set = 0;
  subst_prev_insn = nullptr;

  in_combine_pass_ = true;

  // Pre-pass scan to populate `reg_nonzero_bits` and `reg_sign_bit_copies`
  // (`combine.c:552-590`).
  nonzero_sign_valid = 0;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kInsn || insn->code == kCallInsn || insn->code == kJumpInsn) {
      subst_low_cuid = combine_uid_cuid[insn->fld[0].rtint];
      subst_insn = insn;
      record_dead_and_set_regs(insn);
      if (insn->code == kCallInsn) {
        last_call_cuid = combine_uid_cuid[insn->fld[0].rtint];
        mem_last_set = combine_uid_cuid[insn->fld[0].rtint];
      }
    } else if (insn->code == kCodeLabel) {
      ++label_tick;
    }
  }

  nonzero_sign_valid = 1;
  label_tick = 1;
  last_call_cuid = 0;
  mem_last_set = 0;
  init_reg_last_arrays();

  for (Rtx insn = head, next = nullptr; insn != nullptr; insn = next ? next : insn->fld[2].rtx) {
    next = nullptr;
    if (insn->code == kCodeLabel) {
      ++label_tick;
    } else if (insn->code == kInsn || insn->code == kCallInsn || insn->code == kJumpInsn) {
      for (Rtx links = insn->fld[5].rtx; links != nullptr; links = links->fld[1].rtx) {
        next = RunTryCombine(insn, links->fld[0].rtx, nullptr);
        if (next != nullptr) {
          goto retry;
        }
        for (Rtx nextlinks = links->fld[1].rtx; nextlinks != nullptr;
             nextlinks = nextlinks->fld[1].rtx) {
          next = RunTryCombine(insn, links->fld[0].rtx, nextlinks->fld[0].rtx);
          if (next != nullptr) {
            goto retry;
          }
        }
      }

      for (Rtx links = insn->fld[5].rtx; links != nullptr; links = links->fld[1].rtx) {
        Rtx link_insn = links->fld[0].rtx;
        if (link_insn != nullptr && link_insn->code != kNote) {
          for (Rtx nextlinks = link_insn->fld[5].rtx; nextlinks != nullptr;
               nextlinks = nextlinks->fld[1].rtx) {
            next = RunTryCombine(insn, link_insn, nextlinks->fld[0].rtx);
            if (next != nullptr) {
              goto retry;
            }
          }
        }
      }

      record_dead_and_set_regs(insn);
    retry:;
    }
  }

  in_combine_pass_ = false;
}

Rtx FunctionHooks::RunSubst(Rtx x, Rtx from, Rtx to, int in_dest, int unique_copy) {
  if (x == nullptr || from == nullptr || to == nullptr) {
    return nullptr;
  }
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    PrepareCombineState(head);
  } else {
    const int nregs = std::max(max_reg_num(), 256) + 16;
    max_regno = max_reg_num();
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
    nonzero_bits_mode = kSiMode;
    nonzero_sign_valid = 0;
    label_tick = 1;
    subst_prev_insn = nullptr;
    subst_low_cuid = 1;
  }
  return subst(x, from, to, in_dest, unique_copy);
}

Rtx FunctionHooks::RunSimplifyRtx(Rtx x, int op0_mode, int last, int in_dest) {
  if (x == nullptr) {
    return nullptr;
  }
  Rtx head = ctx_->GetInsns();
  if (head != nullptr) {
    PrepareCombineState(head);
  } else {
    const int nregs = std::max(max_reg_num(), 256) + 16;
    max_regno = max_reg_num();
    combine_max_regno = nregs;
    undobuf.num_undo = 0;
    undobuf.other_insn = nullptr;
    previous_num_undos = 0;
    reg_n_sets = AllocZeroed<short>(nregs);
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
    nonzero_bits_mode = kSiMode;
    nonzero_sign_valid = 0;
    label_tick = 1;
    subst_prev_insn = nullptr;
    subst_low_cuid = 1;
  }
  if (op0_mode == static_cast<int>(x->mode) && rtx_format[x->code][0] == 'e' &&
      x->fld[0].rtx != nullptr && x->fld[0].rtx->mode != kVoidMode) {
    op0_mode = static_cast<int>(x->fld[0].rtx->mode);
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
  // `flow_analysis` initializes `elim_reg_set` from `eliminables` (`$fp` = 30
  // and `$arg` = 66 on MIPS) so `mark_used_regs` does not prematurely mark `$fp`
  // in `regs_ever_live` before reload decides whether a frame pointer is needed.
  elim_reg_set[0] = (1ULL << 30);
  elim_reg_set[1] = (1ULL << (66 - 64));
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
  const int nregs = std::max(max_reg_num(), kFirstPseudoRegister) + 16;
  const int regset_words = (nregs / 32) + 2;
  regset_size = regset_words;
  regset_bytes = regset_words * static_cast<int>(sizeof(int));
  basic_block_live_at_start = AllocZeroed<Rtx>(alloc_blocks);
  for (int b = 0; b < alloc_blocks; ++b) {
    basic_block_live_at_start[b] = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_words));
  }
  find_basic_blocks(head, nonlocal_label_list);
  if (n_basic_blocks == 0) {
    basic_block_head[0] = head;
    basic_block_end[0] = head;
  }
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

namespace {

void ExecuteLifeAnalysisGranular(FunctionHooks* hooks, Rtx f, int nregs) {
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  const int alloc_regs = std::max(nregs, kFirstPseudoRegister) + 16;
  max_regno = nregs;
  reg_n_refs = AllocZeroed<int>(alloc_regs);
  reg_n_sets = AllocZeroed<short>(alloc_regs);
  reg_n_deaths = AllocZeroed<short>(alloc_regs);
  reg_n_calls_crossed = AllocZeroed<int>(alloc_regs);
  reg_live_length = AllocZeroed<int>(alloc_regs);
  reg_changes_size = AllocZeroed<char>(alloc_regs);
  reg_basic_block = AllocZeroed<int>(alloc_regs);
  reg_next_use = AllocZeroed<Rtx>(alloc_regs);
  for (int i = 0; i < alloc_regs; ++i) {
    reg_basic_block[i] = -1;
  }

  regset_size = (nregs + 31) / 32;
  if (regset_size < 1) {
    regset_size = 1;
  }
  regset_bytes = regset_size * static_cast<int>(sizeof(int));

  const int n_blocks = n_basic_blocks;
  const int alloc_blocks = std::max(1, n_blocks) + 4;
  basic_block_live_at_start = AllocZeroed<Rtx>(alloc_blocks);
  std::vector<int*> bb_new_live(alloc_blocks, nullptr);
  std::vector<int*> bb_significant(alloc_blocks, nullptr);
  for (int b = 0; b < alloc_blocks; ++b) {
    basic_block_live_at_start[b] = reinterpret_cast<Rtx>(AllocZeroed<int>(regset_size + 2));
    bb_new_live[b] = AllocZeroed<int>(regset_size + 2);
    bb_significant[b] = AllocZeroed<int>(regset_size + 2);
  }
  regs_live_at_setjmp = AllocZeroed<uint32_t>(regset_size + 2);

  // Mark stack pointer live at end of function (`flow.c:1039-1052`).
  if (n_blocks > 0) {
    reinterpret_cast<int*>(basic_block_live_at_start[n_blocks - 1])[29 / 32] |= (1u << (29 % 32));
    bb_new_live[n_blocks - 1][29 / 32] |= (1u << (29 % 32));
  }

  // Initial backward scan (`flow.c:1058-1071`).
  for (int i = n_blocks - 1; i >= 0; --i) {
    std::memcpy(bb_new_live[i], basic_block_live_at_start[i], regset_bytes);
    hooks->RunPropagateBlock(reinterpret_cast<Rtx>(bb_new_live[i]), basic_block_head[i],
                             basic_block_end[i], /*final_pass=*/0,
                             reinterpret_cast<Rtx>(bb_significant[i]), i);
    std::memcpy(basic_block_live_at_start[i], bb_new_live[i], regset_bytes);
  }

  // Iterative dataflow relaxation across control-flow edges (`flow.c:1076-1208`).
  std::vector<char> must_rescan(alloc_blocks, 1);
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = n_blocks - 1; i >= 0; --i) {
      if (!must_rescan[i]) {
        continue;
      }
      must_rescan[i] = 0;
      std::memset(bb_new_live[i], 0, regset_bytes);
      if (i == n_blocks - 1) {
        bb_new_live[i][29 / 32] |= (1u << (29 % 32));
      }
      if (i + 1 < n_blocks && basic_block_drops_in[i + 1] != 0) {
        int* src = reinterpret_cast<int*>(basic_block_live_at_start[i + 1]);
        for (int j = 0; j < regset_size; ++j) {
          bb_new_live[i][j] |= src[j];
        }
      }
      Rtx jump = basic_block_end[i];
      if (jump != nullptr && jump->code == kJumpInsn) {
        auto add_addr_vec_targets = [&](Rtx body) {
          const int elt_idx = (body->code == kAddrDiffVec) ? 1 : 0;
          const int len = body->fld[elt_idx].rtvec->num_elem;
          for (int k = 0; k < len; ++k) {
            Rtx targ = body->fld[elt_idx].rtvec->elem[k].rtx->fld[0].rtx;
            if (targ != nullptr && targ->fld[0].rtint <= max_uid_for_flow) {
              const int b = uid_block_number[targ->fld[0].rtint];
              if (b >= 0 && b < n_blocks) {
                int* src = reinterpret_cast<int*>(basic_block_live_at_start[b]);
                for (int j = 0; j < regset_size; ++j) {
                  bb_new_live[i][j] |= src[j];
                }
              }
            }
          }
        };
        if (jump->fld[3].rtx != nullptr &&
            (jump->fld[3].rtx->code == kAddrVec || jump->fld[3].rtx->code == kAddrDiffVec)) {
          add_addr_vec_targets(jump->fld[3].rtx);
        }
        for (Rtx insn = jump->fld[2].rtx;
             insn != nullptr &&
             (insn->code == kBarrier || insn->code == kNote || insn->code == kCodeLabel);
             insn = insn->fld[2].rtx) {
          if ((insn->code == kCodeLabel || (insn->code == kNote && insn->fld[4].rtint > 0)) &&
              insn->fld[2].rtx != nullptr && insn->fld[2].rtx->code == kJumpInsn &&
              (insn->fld[2].rtx->fld[3].rtx->code == kAddrVec ||
               insn->fld[2].rtx->fld[3].rtx->code == kAddrDiffVec)) {
            add_addr_vec_targets(insn->fld[2].rtx->fld[3].rtx);
          }
        }
        Rtx label = jump->fld[7].rtx;
        if (label != nullptr && label->fld[0].rtint <= max_uid_for_flow) {
          const int b = uid_block_number[label->fld[0].rtint];
          if (b >= 0 && b < n_blocks) {
            int* src = reinterpret_cast<int*>(basic_block_live_at_start[b]);
            for (int j = 0; j < regset_size; ++j) {
              bb_new_live[i][j] |= src[j];
            }
          }
        }
      }

      bool consider = false;
      bool first_pass_check = false;
      int* cur_live = reinterpret_cast<int*>(basic_block_live_at_start[i]);
      for (int j = 0; j < regset_size; ++j) {
        if (bb_new_live[i][j] & ~cur_live[j]) {
          first_pass_check = true;
          if ((bb_new_live[i][j] & ~cur_live[j]) & bb_significant[i][j]) {
            consider = true;
            break;
          }
        }
      }
      if (!first_pass_check) {
        continue;
      }
      if (!consider) {
        for (int j = 0; j < regset_size; ++j) {
          cur_live[j] |= bb_new_live[i][j];
        }
        if (basic_block_drops_in[i] != 0 && i > 0) {
          must_rescan[i - 1] = 1;
          changed = true;
        }
        if (basic_block_head[i]->code == kCodeLabel) {
          for (Rtx ref = basic_block_head[i]->fld[5].rtx;
               ref != nullptr && ref != basic_block_head[i]; ref = ref->fld[1].rtx) {
            Rtx containing = ref->fld[2].rtx;
            if (containing != nullptr && containing->fld[0].rtint <= max_uid_for_flow) {
              const int b = uid_block_number[containing->fld[0].rtint];
              if (b >= 0 && b < n_blocks) {
                must_rescan[b] = 1;
                changed = true;
              }
            }
          }
        }
        continue;
      }

      std::vector<int> old_live(cur_live, cur_live + regset_size);
      hooks->RunPropagateBlock(reinterpret_cast<Rtx>(bb_new_live[i]), basic_block_head[i],
                               basic_block_end[i], /*final_pass=*/0, nullptr, i);
      bool prop_changed = false;
      for (int j = 0; j < regset_size; ++j) {
        cur_live[j] |= bb_new_live[i][j];
        if (cur_live[j] != old_live[j]) {
          prop_changed = true;
        }
      }
      if (prop_changed) {
        if (basic_block_drops_in[i] != 0 && i > 0) {
          must_rescan[i - 1] = 1;
          changed = true;
        }
        if (basic_block_head[i]->code == kCodeLabel) {
          for (Rtx ref = basic_block_head[i]->fld[5].rtx;
               ref != nullptr && ref != basic_block_head[i]; ref = ref->fld[1].rtx) {
            Rtx containing = ref->fld[2].rtx;
            if (containing != nullptr && containing->fld[0].rtint <= max_uid_for_flow) {
              const int b = uid_block_number[containing->fld[0].rtint];
              if (b >= 0 && b < n_blocks) {
                must_rescan[b] = 1;
                changed = true;
              }
            }
          }
        }
      }
    }
  }

  // Final pass (`final_pass = 1`) to attach `REG_DEAD` / `REG_UNUSED` notes,
  // build `LOG_LINKS`, and delete dead instructions (`flow.c:1224-1262`).
  for (int i = 0; i < n_blocks; ++i) {
    std::memset(bb_new_live[i], 0, regset_bytes);
    if (i == n_blocks - 1) {
      bb_new_live[i][29 / 32] |= (1u << (29 % 32));
    }
    if (i + 1 < n_blocks && basic_block_drops_in[i + 1] != 0) {
      int* src = reinterpret_cast<int*>(basic_block_live_at_start[i + 1]);
      for (int j = 0; j < regset_size; ++j) {
        bb_new_live[i][j] |= src[j];
      }
    }
    Rtx jump = basic_block_end[i];
    if (jump != nullptr && jump->code == kJumpInsn) {
      auto add_addr_vec_targets = [&](Rtx body) {
        const int elt_idx = (body->code == kAddrDiffVec) ? 1 : 0;
        const int len = body->fld[elt_idx].rtvec->num_elem;
        for (int k = 0; k < len; ++k) {
          Rtx targ = body->fld[elt_idx].rtvec->elem[k].rtx->fld[0].rtx;
          if (targ != nullptr && targ->fld[0].rtint <= max_uid_for_flow) {
            const int b = uid_block_number[targ->fld[0].rtint];
            if (b >= 0 && b < n_blocks) {
              int* src = reinterpret_cast<int*>(basic_block_live_at_start[b]);
              for (int j = 0; j < regset_size; ++j) {
                bb_new_live[i][j] |= src[j];
              }
            }
          }
        }
      };
      if (jump->fld[3].rtx != nullptr &&
          (jump->fld[3].rtx->code == kAddrVec || jump->fld[3].rtx->code == kAddrDiffVec)) {
        add_addr_vec_targets(jump->fld[3].rtx);
      }
      for (Rtx insn = jump->fld[2].rtx;
           insn != nullptr &&
           (insn->code == kBarrier || insn->code == kNote || insn->code == kCodeLabel);
           insn = insn->fld[2].rtx) {
        if ((insn->code == kCodeLabel || (insn->code == kNote && insn->fld[4].rtint > 0)) &&
            insn->fld[2].rtx != nullptr && insn->fld[2].rtx->code == kJumpInsn &&
            (insn->fld[2].rtx->fld[3].rtx->code == kAddrVec ||
             insn->fld[2].rtx->fld[3].rtx->code == kAddrDiffVec)) {
          add_addr_vec_targets(insn->fld[2].rtx->fld[3].rtx);
        }
      }
      Rtx label = jump->fld[7].rtx;
      if (label != nullptr && label->fld[0].rtint <= max_uid_for_flow) {
        const int b = uid_block_number[label->fld[0].rtint];
        if (b >= 0 && b < n_blocks) {
          int* src = reinterpret_cast<int*>(basic_block_live_at_start[b]);
          for (int j = 0; j < regset_size; ++j) {
            bb_new_live[i][j] |= src[j];
          }
        }
      }
    }

    hooks->RunPropagateBlock(reinterpret_cast<Rtx>(bb_new_live[i]), basic_block_head[i],
                             basic_block_end[i], /*final_pass=*/1, nullptr, i);
    std::memset(reg_next_use, 0, nregs * sizeof(Rtx));
  }

  for (Rtx insn = f; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) {
      Rtx pat = insn->fld[3].rtx;
      if (pat != nullptr && pat->code == kUse && pat->fld[0].rtx != nullptr &&
          pat->fld[0].rtx->code == kReg) {
        const int regno = pat->fld[0].rtx->fld[0].rtint;
        if (regno >= 0 && regno < nregs && reg_n_deaths[regno] == 0 && reg_n_refs[regno] > 0) {
          reg_live_length[regno] = -1;
        }
      }
    }
  }
}

}  // namespace

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
  if (next_branch == nullptr && head != nullptr) {
    restore_reg_data(head);
    max_reg = std::max(256, max_reg_num() + 16);
    reg_scan(head, max_reg, 1);
    init_recog();
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

namespace {

int ExecuteCsePassGranular(FunctionHooks* hooks, GccContext* ctx, Rtx f, int nregs,
                           int after_loop) {
  CseBasicBlockData val{};
  Rtx insn = f;
  int i = 0;

  cse_jumps_altered = 0;
  recorded_label_ref = 0;
  constant_pool_entries_cost = 0;
  val.path_size = 0;
  init_recog();

  max_reg = nregs;
  all_minus_one = AllocZeroed<int>(nregs);
  consec_ints = AllocZeroed<int>(nregs);
  for (i = 0; i < nregs; ++i) {
    all_minus_one[i] = -1;
    consec_ints[i] = i;
  }

  reg_qty = AllocZeroed<int>(nregs);
  reg_in_table = AllocZeroed<int>(nregs);
  reg_tick = AllocZeroed<int>(nregs);
  reg_next_eqv = AllocZeroed<int>(nregs);
  reg_prev_eqv = AllocZeroed<int>(nregs);

  memory_extend_rtx = ctx->ParseRtx("(zero_extend (nil))");
  std::memset(cse_table, 0, sizeof(cse_table));
  free_element_chain = nullptr;
  n_elements_made = 0;

  cse_max_uid = get_max_uid();
  cse_uid_cuid = AllocZeroed<int>(cse_max_uid + 64);

  i = 0;
  for (Rtx p = f; p != nullptr; p = p->fld[2].rtx) {
    if (p->code != kNote || p->fld[4].rtint < 0) {
      ++i;
    }
    cse_uid_cuid[p->fld[0].rtint] = i;
  }

  regs_invalidated_by_call[0] = 0;
  regs_invalidated_by_call[1] = 0;
  regs_invalidated_by_call[2] = 0;
  for (int r = 0; r < 68; ++r) {
    bool inv = false;
    if (call_used_regs[r] != 0) {
      if (r != 29 && r != 30) {
        if (r == 0) {
          inv = (fixed_regs[0] == 0);
        } else if (r == 28) {
          inv = (flag_pic == 0);
        } else {
          inv = true;
        }
      }
    }
    if (!inv && global_regs[r] != 0) {
      inv = true;
    }
    if (inv) {
      regs_invalidated_by_call[r >> 5] |= (1u << (r & 31));
    }
  }

  // Loop over basic blocks using `cse_end_of_basic_block` and `RunCseBasicBlock`
  // (`cse_main` `e20b..e372`).
  while (insn != nullptr) {
    cse_end_of_basic_block(insn, &val, flag_cse_follow_jumps, after_loop, flag_cse_skip_blocks);

    if (val.nsets == 0 || insn->mode == kQiMode) {
      insn->mode = kVoidMode;
      if (val.last == nullptr) {
        break;
      }
      insn = val.last->fld[2].rtx;
      val.path_size = 0;
      continue;
    }

    cse_basic_block_start = val.low_cuid;
    cse_basic_block_end = val.high_cuid;
    max_qty = std::max(500, val.nsets * 2) + max_reg;

    if (val.path_size > 0) {
      hooks->RunCseBasicBlock(insn, val.last, val.path, 0);
      continue;
    }

    const int old_cse_jumps_altered = cse_jumps_altered;
    cse_jumps_altered = 0;
    Rtx temp = hooks->RunCseBasicBlock(insn, val.last, val.path, !after_loop);
    if (cse_jumps_altered != 0 && (flag_cse_follow_jumps != 0 || flag_cse_skip_blocks != 0)) {
      temp = insn;
    }
    cse_jumps_altered |= old_cse_jumps_altered;
    insn = temp;
  }

  qty_const = nullptr;
  return cse_jumps_altered != 0 || recorded_label_ref != 0;
}

void UndoCombineChanges() {
  while (undobuf.num_undo > 0) {
    --undobuf.num_undo;
    if (undobuf.undo[undobuf.num_undo].is_int != 0) {
      *undobuf.undo[undobuf.num_undo].where.i = undobuf.undo[undobuf.num_undo].old_contents.i;
    } else {
      *undobuf.undo[undobuf.num_undo].where.r = undobuf.undo[undobuf.num_undo].old_contents.r;
    }
  }
  undobuf.other_insn = nullptr;
  previous_num_undos = 0;
}

void PrepareCseFoldState(GccContext* ctx, Rtx head) {
  if (head != nullptr) {
    restore_reg_data(head);
  }
  max_reg = std::max(256, (head != nullptr ? max_reg_num() : 128) + 16);
  if (head != nullptr) {
    reg_scan(head, max_reg, 1);
  }
  init_recog();
  init_alias_analysis();

  max_qty = max_reg + 500;
  next_qty = max_reg;
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
    reg_next_eqv[i] = -1;
    reg_prev_eqv[i] = -1;
  }
  for (int i = max_reg; i < max_qty; ++i) {
    consec_ints[i] = i;
  }
  qty_first_reg = AllocZeroed<int>(max_qty);
  qty_last_reg = AllocZeroed<int>(max_qty);
  qty_mode = AllocZeroed<int>(max_qty);
  qty_const = AllocZeroed<Rtx>(max_qty);
  qty_const_insn = AllocZeroed<Rtx>(max_qty);
  qty_comparison_code = AllocZeroed<int>(max_qty);
  qty_comparison_qty = AllocZeroed<int>(max_qty);
  qty_comparison_const = AllocZeroed<Rtx>(max_qty);
  std::memset(cse_table, 0, sizeof(cse_table));
  free_element_chain = nullptr;
  n_elements_made = 0;
  if (ctx != nullptr) {
    memory_extend_rtx = ctx->ParseRtx("(zero_extend (nil))");
  }
}

}  // namespace

Rtx FunctionHooks::RunFoldRtx(Rtx x, Rtx insn) {
  PrepareCseFoldState(ctx_, ctx_->GetInsns());
  return fold_rtx(x, insn);
}

// -----------------------------------------------------------------------------
// Stage 10: Loop (`loop.c`)
// -----------------------------------------------------------------------------

void FunctionHooks::PrepareLoopState(Rtx first) {
  RefreshJumpLabelsAndNuses(first);
  restore_reg_data(first);
  PopulateInsnCodesWithVolatileOk(first);
  init_recog_no_volatile();
  init_alias_analysis();
  loop_dump_stream = nullptr;

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
  for (int i = 0; i <= max_uid_for_loop; ++i) {
    uid_loop_num[i] = -1;
  }
  loop_number_loop_starts = AllocZeroed<Rtx>(loop_cap);
  loop_number_loop_ends = AllocZeroed<Rtx>(loop_cap);
  loop_outer_loop = AllocZeroed<int>(loop_cap);
  for (int i = 0; i < loop_cap; ++i) {
    loop_outer_loop[i] = -1;
  }
  loop_invalid = AllocZeroed<char>(loop_cap);
  loop_has_call = 0;
  loop_has_volatile = 0;
  loop_continue = nullptr;
  loops_enclosed = 1;
  loop_number_exit_labels = AllocZeroed<Rtx>(loop_cap);
  loop_number_exit_count = AllocZeroed<int>(loop_cap);
  if (max_loop_num == 0) {
    return;
  }

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
  RefreshJumpLabelsAndNuses(head);
  restore_reg_data(head);
  reg_scan(head, max_reg_num(), 1);
  loop_optimize(head, nullptr, unroll_p);
}

void FunctionHooks::RunScanLoop(Rtx loop_start, Rtx end, int nregs) {
  Rtx head = ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  if (!in_loop_pass_) {
    PrepareLoopState(head);
  }
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
  const int saved_strength_reduce = flag_strength_reduce;
  flag_strength_reduce = 0;
  scan_loop(loop_start, end, nregs > 0 ? nregs : max_reg_num());
  flag_strength_reduce = saved_strength_reduce;
}

void FunctionHooks::RunStrengthReduce(Rtx scan_start, Rtx end, Rtx loop_top, int insn_count,
                                      Rtx loop_start, Rtx loop_end) {
  Rtx head = ctx_->GetInsns();
  if (head == nullptr) {
    return;
  }
  if (!in_loop_pass_) {
    PrepareLoopState(head);
  }
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
    Rtx p = loop_start->fld[2].rtx;
    while (p != end && p != nullptr && p->code != kCodeLabel && rtx_class[p->code] != 'i' &&
           (p->code != kNote ||
            (p->fld[4].rtint != kNoteInsnLoopBeg && p->fld[4].rtint != kNoteInsnLoopEnd))) {
      p = p->fld[2].rtx;
    }
    scan_start = p;
    prescan_loop(loop_start, end);
    if (p != nullptr && p->code == kJumpInsn) {
      if (simplejump_p(p) != 0 && p->fld[7].rtx != nullptr &&
          p->fld[7].rtx->fld[0].rtint < max_uid_for_loop &&
          uid_luid[p->fld[7].rtx->fld[0].rtint] >= uid_luid[loop_start->fld[0].rtint] &&
          uid_luid[p->fld[7].rtx->fld[0].rtint] < uid_luid[end->fld[0].rtint]) {
        loop_top = next_label(scan_start);
        scan_start = p->fld[7].rtx;
      }
    }
    if (scan_start == nullptr || scan_start == end ||
        scan_start->fld[0].rtint >= max_uid_for_loop || scan_start->code != kCodeLabel) {
      return;
    }
  } else {
    if (scan_start == end) {
      return;
    }
    prescan_loop(loop_start, end);
  }
  const int nregs = max_reg_num() + 16;
  n_times_set = AllocZeroed<short>(nregs);
  n_times_used = AllocZeroed<short>(nregs);
  may_not_optimize = AllocZeroed<char>(nregs);
  Rtx* single_usage = nullptr;
  if (loop_has_call != 0) {
    single_usage = AllocZeroed<Rtx>(nregs);
  }
  int computed_count = 0;
  count_loop_regs_set(loop_top != nullptr ? loop_top : loop_start, end, may_not_optimize,
                      single_usage, &computed_count, nregs);
  for (int i = 0; i < kFirstPseudoRegister && i < nregs; ++i) {
    may_not_optimize[i] = 1;
    n_times_set[i] = 1;
  }
  std::memcpy(n_times_used, n_times_set, nregs * sizeof(short));
  move_movables(nullptr, 0, 0, loop_start, end, nregs);
  strength_reduce(scan_start, end, loop_top, insn_count > 0 ? insn_count : computed_count,
                  loop_start, loop_end);
}

// -----------------------------------------------------------------------------
// Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`)
// -----------------------------------------------------------------------------

namespace {

void* GetTreeType(void* t) {
  return t != nullptr ? static_cast<void**>(t)[1] : nullptr;
}

int GetTreeTypeMode(void* type_node) {
  return type_node != nullptr ? static_cast<const unsigned char*>(type_node)[53] : kSiMode;
}

}  // namespace

RtlControlFlowGraph FunctionHooks::RunInstantiateVirtualRegs(GccContext& ctx,
                                                             const RtlControlFlowGraph& pre_cfg) {
  RtlControlFlowGraph rtl_cfg = pre_cfg;
  rtl_cfg.pass = RtlPassKind::kRtl;
  Rtx first = ctx.MaterializeFromCfg(rtl_cfg, /*reset_function=*/true);
  if (first != nullptr) {
    unshare_all_rtl(first);
    virtuals_instantiated = 0;
    instantiate_virtual_regs(current_function_decl, first);
  }
  return ctx.DematerializeToCfg(first, pre_cfg.pass);
}

RtlControlFlowGraph FunctionHooks::RunInstantiateVirtualRegs(const RtlControlFlowGraph& pre_cfg) {
  return RunInstantiateVirtualRegs(*ctx_, pre_cfg);
}

RtlControlFlowGraph FunctionHooks::RunExpandFunctionEnd(GccContext& ctx,
                                                        const RtlControlFlowGraph& body_cfg,
                                                        const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  RtlControlFlowGraph rtl_body = body_cfg;
  rtl_body.pass = RtlPassKind::kRtl;
  ctx.MaterializeFromCfg(rtl_body, /*reset_function=*/false);
  ctx.SyncReturnLabelFromMaterializedCfg();
  expand_function_end(const_cast<char*>("harness.c"), 1, 0);
  return ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
}

RtlControlFlowGraph FunctionHooks::RunExpandFunctionEnd(const RtlControlFlowGraph& body_cfg,
                                                        const FunctionDeclaration& func_decl) {
  return RunExpandFunctionEnd(*ctx_, body_cfg, func_decl);
}

RtlControlFlowGraph FunctionHooks::RunExpandFunctionEnd(GccContext& ctx,
                                                        const RtlControlFlowGraph& body_cfg) {
  FunctionDeclaration default_decl(CType::S32(), "harness_func", {},
                                   std::make_unique<CompoundStatement>());
  return RunExpandFunctionEnd(ctx, body_cfg, default_decl);
}

RtlControlFlowGraph FunctionHooks::RunExpandFunctionEnd(const RtlControlFlowGraph& body_cfg) {
  return RunExpandFunctionEnd(*ctx_, body_cfg);
}

RtlControlFlowGraph FunctionHooks::RunExpandReturn(GccContext& ctx, const ReturnStatement& ret_stmt,
                                                   const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  ctx.StartIsolatedExpandSequence();
  ctx.ExpandCStatement(ret_stmt);
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandReturn(const ReturnStatement& ret_stmt,
                                                   const FunctionDeclaration& func_decl) {
  return RunExpandReturn(*ctx_, ret_stmt, func_decl);
}

RtlControlFlowGraph FunctionHooks::RunExpandExpr(GccContext& ctx, const CExpression& expr,
                                                 const FunctionDeclaration& func_decl,
                                                 int target_pseudo_regno) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  void* expr_tree = ctx.BuildCExprTree(expr);
  ctx.StartIsolatedExpandSequence();
  void* expr_type = GetTreeType(expr_tree);
  int tmode = GetTreeTypeMode(expr_type);
  if (tmode == kVoidMode) {
    tmode = kSiMode;
  }
  Rtx target = nullptr;
  if (target_pseudo_regno >= kFirstPseudoRegister) {
    ctx.EnsurePseudoRegsUpTo(target_pseudo_regno);
    target = gen_rtx(kReg, tmode, target_pseudo_regno);
  }
  Rtx result = expand_expr(expr_tree, target, tmode, 0);
  if (target != nullptr && result != nullptr && result != target) {
    emit_move_insn(target, result);
  } else if (target == nullptr && result != nullptr && get_insns()->fld[2].rtx == nullptr) {
    Rtx dst = gen_reg_rtx(result->mode != kVoidMode ? result->mode : tmode);
    emit_move_insn(dst, result);
  }
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandExpr(const CExpression& expr,
                                                 const FunctionDeclaration& func_decl,
                                                 int target_pseudo_regno) {
  return RunExpandExpr(*ctx_, expr, func_decl, target_pseudo_regno);
}

RtlControlFlowGraph FunctionHooks::RunExpandAssignment(GccContext& ctx, const CExpression& lhs,
                                                       const CExpression& rhs,
                                                       const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  void* to_tree = ctx.BuildCExprTree(lhs);
  void* from_tree = ctx.BuildCExprTree(rhs);
  if (to_tree != nullptr && from_tree != nullptr && GetTreeType(to_tree) != nullptr) {
    from_tree = convert(GetTreeType(to_tree), from_tree);
  }
  ctx.StartIsolatedExpandSequence();
  expand_assignment(to_tree, from_tree, /*want_value=*/0, /*suggest_reg=*/0);
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandAssignment(const CExpression& lhs,
                                                       const CExpression& rhs,
                                                       const FunctionDeclaration& func_decl) {
  return RunExpandAssignment(*ctx_, lhs, rhs, func_decl);
}

RtlControlFlowGraph FunctionHooks::RunDoStoreFlag(GccContext& ctx,
                                                  const CExpression& comparison_expr,
                                                  const FunctionDeclaration& func_decl,
                                                  int target_pseudo_regno, int only_cheap) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  void* cmp_tree = ctx.BuildCExprTree(comparison_expr);
  ctx.StartIsolatedExpandSequence();
  Rtx target = nullptr;
  if (target_pseudo_regno >= kFirstPseudoRegister) {
    ctx.EnsurePseudoRegsUpTo(target_pseudo_regno);
    target = gen_rtx(kReg, kSiMode, target_pseudo_regno);
  } else {
    target = gen_reg_rtx(kSiMode);
  }
  Rtx res = do_store_flag(cmp_tree, target, kSiMode, only_cheap);
  if (res != nullptr && res != target) {
    emit_move_insn(target, res);
  }
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunDoStoreFlag(const CExpression& comparison_expr,
                                                  const FunctionDeclaration& func_decl,
                                                  int target_pseudo_regno, int only_cheap) {
  return RunDoStoreFlag(*ctx_, comparison_expr, func_decl, target_pseudo_regno, only_cheap);
}

RtlControlFlowGraph FunctionHooks::RunDoJump(GccContext& ctx, const CExpression& cond_expr,
                                             const FunctionDeclaration& func_decl,
                                             bool jump_if_true, bool jump_if_false) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  void* cond_tree = ctx.BuildCExprTree(cond_expr);
  ctx.StartIsolatedExpandSequence();
  Rtx false_lbl = jump_if_false ? gen_label_rtx() : nullptr;
  Rtx true_lbl = jump_if_true ? gen_label_rtx() : nullptr;
  do_jump(cond_tree, false_lbl, true_lbl);
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunDoJump(const CExpression& cond_expr,
                                             const FunctionDeclaration& func_decl,
                                             bool jump_if_true, bool jump_if_false) {
  return RunDoJump(*ctx_, cond_expr, func_decl, jump_if_true, jump_if_false);
}

RtlControlFlowGraph FunctionHooks::RunExpandCall(GccContext& ctx, const CallExpression& call_expr,
                                                 const FunctionDeclaration& caller_decl,
                                                 const CType& return_type, int target_pseudo_regno,
                                                 bool ignore_result) {
  ctx.BeginFunctionFromAst(caller_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  if (call_expr.Callee().Kind() == CExpressionKind::kIdentifier) {
    const auto& callee_id = static_cast<const IdentifierExpression&>(call_expr.Callee());
    ctx.DeclareExternalFunction(callee_id.Name(), return_type);
  }
  void* call_tree = ctx.BuildCExprTree(call_expr);
  ctx.StartIsolatedExpandSequence();
  Rtx target = nullptr;
  const bool is_void_ret = (return_type.name == "void" && return_type.pointer_depth == 0);
  if (!ignore_result && !is_void_ret && target_pseudo_regno >= kFirstPseudoRegister) {
    ctx.EnsurePseudoRegsUpTo(target_pseudo_regno);
    int rmode = GetTreeTypeMode(ctx.BuildCTypeTree(return_type));
    target = gen_rtx(kReg, rmode != kVoidMode ? rmode : kSiMode, target_pseudo_regno);
  }
  Rtx res = expand_call(call_tree, target, ignore_result ? 1 : 0);
  if (target != nullptr && res != nullptr && res != target) {
    emit_move_insn(target, res);
  }
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandCall(const CallExpression& call_expr,
                                                 const FunctionDeclaration& caller_decl,
                                                 const CType& return_type, int target_pseudo_regno,
                                                 bool ignore_result) {
  return RunExpandCall(*ctx_, call_expr, caller_decl, return_type, target_pseudo_regno,
                       ignore_result);
}

RtlControlFlowGraph FunctionHooks::RunExpandCond(GccContext& ctx, const IfStatement& if_stmt,
                                                 const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  ctx.StartIsolatedExpandSequence();
  ctx.ExpandCStatement(if_stmt);
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandCond(const IfStatement& if_stmt,
                                                 const FunctionDeclaration& func_decl) {
  return RunExpandCond(*ctx_, if_stmt, func_decl);
}

RtlControlFlowGraph FunctionHooks::RunExpandLoop(GccContext& ctx, const CStatement& loop_stmt,
                                                 const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  ctx.StartIsolatedExpandSequence();
  ctx.ExpandCStatement(loop_stmt);
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandLoop(const CStatement& loop_stmt,
                                                 const FunctionDeclaration& func_decl) {
  return RunExpandLoop(*ctx_, loop_stmt, func_decl);
}

RtlControlFlowGraph FunctionHooks::RunExpandCase(GccContext& ctx,
                                                 const SwitchStatement& switch_stmt,
                                                 const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  ctx.StartIsolatedExpandSequence();
  ctx.ExpandCStatement(switch_stmt);
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandCase(const SwitchStatement& switch_stmt,
                                                 const FunctionDeclaration& func_decl) {
  return RunExpandCase(*ctx_, switch_stmt, func_decl);
}

RtlControlFlowGraph FunctionHooks::RunExpandDecl(GccContext& ctx,
                                                 const VariableDeclarationStatement& var_decl,
                                                 const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/true);
  ctx.StartIsolatedExpandSequence();
  ctx.ExpandCStatement(var_decl);
  RtlControlFlowGraph out = ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
  ctx.EndIsolatedExpandSequence();
  return out;
}

RtlControlFlowGraph FunctionHooks::RunExpandDecl(const VariableDeclarationStatement& var_decl,
                                                 const FunctionDeclaration& func_decl) {
  return RunExpandDecl(*ctx_, var_decl, func_decl);
}

RtlControlFlowGraph FunctionHooks::RunExpandFunctionStart(GccContext& ctx,
                                                          const FunctionDeclaration& func_decl) {
  ctx.BeginFunctionFromAst(func_decl, /*run_expand_function_start=*/true,
                           /*declare_body_locals=*/false);
  return ctx.DematerializeToCfg(ctx.GetInsns(), RtlPassKind::kRtl);
}

RtlControlFlowGraph FunctionHooks::RunExpandFunctionStart(const FunctionDeclaration& func_decl) {
  return RunExpandFunctionStart(*ctx_, func_decl);
}

// -----------------------------------------------------------------------------
// In-Process Forward Compilation Tracer (`TraceRtlPipeline` &
// `TraceForwardCompilation`)
// -----------------------------------------------------------------------------

namespace {

constexpr SubfunctionId kAllSubfunctionIds[] = {
    SubfunctionId::kInstantiateVirtualRegs,
    SubfunctionId::kJumpRedirect,
    SubfunctionId::kJump1Optimize,
    SubfunctionId::kFoldRtx,
    SubfunctionId::kCseBasicBlockCse1,
    SubfunctionId::kScanLoop,
    SubfunctionId::kStrengthReduce,
    SubfunctionId::kCseBasicBlockCse2,
    SubfunctionId::kFindBasicBlocks,
    SubfunctionId::kPropagateBlock,
    SubfunctionId::kSimplifyRtx,
    SubfunctionId::kSubst,
    SubfunctionId::kTryCombine,
    SubfunctionId::kScheduleBlockSched1,
    SubfunctionId::kUpdateEquivRegs,
    SubfunctionId::kBlockAlloc,
    SubfunctionId::kGlobalAlloc,
    SubfunctionId::kReload,
    SubfunctionId::kAlterReg,
    SubfunctionId::kScheduleBlockSched2,
    SubfunctionId::kDoCrossJump,
    SubfunctionId::kFillSimpleDelaySlots,
    SubfunctionId::kFillEagerDelaySlots,
    SubfunctionId::kRelaxDelaySlots,
    SubfunctionId::kMakeReturnInsns,
    SubfunctionId::kFunctionPrologueEpilogue,
    SubfunctionId::kFinalScanInsn,
    SubfunctionId::kGasMipsIp,
    SubfunctionId::kGasMacro,
    SubfunctionId::kGasAppendInsn,
};

void SetGccFOption(std::string_view opt_name, int enabled) {
  for (int i = 0; i < 52; ++i) {
    if (f_options[i].string != nullptr && opt_name == f_options[i].string &&
        f_options[i].variable != nullptr) {
      if (enabled) {
        *f_options[i].variable = f_options[i].on_value;
      } else {
        *f_options[i].variable = !f_options[i].on_value;
      }
      return;
    }
  }
}

void ApplyOptimizeLevelFlags(int optimize_level) {
  mips_debug = 0;
  mips_optimize = (optimize_level <= 0) ? 1 : 2;
  if (optimize_level <= 0) {
    optimize = 0;
    obey_regdecls = 1;
    flag_defer_pop = 0;
    flag_thread_jumps = 0;
    flag_omit_frame_pointer = 0;
    flag_cse_follow_jumps = 0;
    flag_cse_skip_blocks = 0;
    flag_force_mem = 0;
    flag_caller_saves = 0;
    flag_expensive_optimizations = 0;
    flag_strength_reduce = 0;
    flag_schedule_insns = 0;
    flag_schedule_insns_after_reload = 0;
    flag_delayed_branch = 0;
    flag_inline_functions = 0;
    SetGccFOption("rerun-cse-after-loop", 0);
  } else if (optimize_level == 1) {
    optimize = 1;
    obey_regdecls = 0;
    flag_defer_pop = 1;
    flag_thread_jumps = 1;
    flag_omit_frame_pointer = 1;
    flag_cse_follow_jumps = 0;
    flag_cse_skip_blocks = 0;
    flag_force_mem = 0;
    flag_caller_saves = 0;
    flag_expensive_optimizations = 0;
    flag_strength_reduce = 0;
    flag_schedule_insns = 0;
    flag_schedule_insns_after_reload = 0;
    flag_delayed_branch = 1;
    flag_inline_functions = 0;
    SetGccFOption("rerun-cse-after-loop", 0);
  } else {
    optimize = optimize_level;
    obey_regdecls = 0;
    flag_defer_pop = 1;
    flag_thread_jumps = 1;
    flag_omit_frame_pointer = 1;
    flag_cse_follow_jumps = 1;
    flag_cse_skip_blocks = 1;
    flag_force_mem = 1;
    flag_caller_saves = 1;
    flag_expensive_optimizations = 1;
    flag_strength_reduce = 1;
    flag_schedule_insns = 1;
    flag_schedule_insns_after_reload = 1;
    flag_delayed_branch = 1;
    flag_inline_functions = (optimize_level >= 3) ? 1 : 0;
    SetGccFOption("rerun-cse-after-loop", 1);
  }
  for (GccOptab* tab :
       {sdiv_optab, sdivmod_optab, udiv_optab, udivmod_optab, smod_optab, umod_optab}) {
    if (tab != nullptr) {
      const int code_for_nothing = tab->handlers[kVoidMode].insn_code;
      tab->handlers[kSiMode].insn_code = code_for_nothing;
      tab->handlers[kDiMode].insn_code = code_for_nothing;
    }
  }
  init_all_optabs();
}

void ApplyCompilerFlags(absl::Span<const std::string> c_flags, int* out_optimize_level) {
  flag_signed_char = 1;
  ApplyOptimizeLevelFlags(out_optimize_level != nullptr ? *out_optimize_level : 2);

  target_flags = 0;
  mips_cpu_string = nullptr;
  mips_isa_string = nullptr;
  set_target_switch(const_cast<char*>(""));
  g_switch_set = 1;
  g_switch_value = 0;

  for (size_t i = 0; i < c_flags.size(); ++i) {
    std::string_view flag = c_flags[i];
    if (flag == "-O0") {
      ApplyOptimizeLevelFlags(0);
    } else if (flag == "-O" || flag == "-O1") {
      ApplyOptimizeLevelFlags(1);
    } else if (flag == "-O2") {
      ApplyOptimizeLevelFlags(2);
    } else if (flag == "-O3") {
      ApplyOptimizeLevelFlags(3);
    } else if (flag == "-G" && i + 1 < c_flags.size()) {
      int gval = 0;
      if (absl::SimpleAtoi(c_flags[i + 1], &gval) && gval >= 0) {
        g_switch_set = 1;
        g_switch_value = gval;
      }
      ++i;
    } else if (absl::StartsWith(flag, "-G") && flag.size() > 2) {
      int gval = 0;
      if (absl::SimpleAtoi(flag.substr(2), &gval) && gval >= 0) {
        g_switch_set = 1;
        g_switch_value = gval;
      }
    } else if (flag == "-funsigned-char") {
      flag_signed_char = 0;
    } else if (flag == "-fsigned-char") {
      flag_signed_char = 1;
    } else if (absl::StartsWith(flag, "-fno-")) {
      SetGccFOption(flag.substr(5), 0);
    } else if (absl::StartsWith(flag, "-f")) {
      SetGccFOption(flag.substr(2), 1);
    } else if (absl::StartsWith(flag, "-mcpu=")) {
      static std::string cpu_storage;
      cpu_storage = std::string(flag.substr(6));
      mips_cpu_string = cpu_storage.c_str();
    } else if (absl::StartsWith(flag, "-mips")) {
      static std::string isa_storage;
      isa_storage = std::string(flag.substr(5));
      mips_isa_string = isa_storage.c_str();
    } else if (absl::StartsWith(flag, "-m")) {
      std::string sw(flag.substr(2));
      set_target_switch(sw.data());
    }
  }
  override_options();

  // Configure in-process Binutils 2.6 GAS globals (`tc-mips.c:md_parse_option`)
  // to match `Toolchain::BuildKmcAsArgs`:
  // - Default `mips_optimize` is 1 at `-O0` and 2 at `-O1`/`-O2`/`-O3`.
  // - `-Wa,-g` / `-Wa,-g2` sets `mips_debug = 2` and `mips_optimize = 0`.
  // - `-Wa,-O0` sets `mips_optimize = 1`; `-Wa,-O` / `-Wa,-O1` / `-Wa,-O2` /
  //   `-Wa,-O3` sets `mips_optimize = 2`.
  mips_debug = 0;
  mips_optimize = (optimize == 0) ? 1 : 2;
  for (std::string_view flag : c_flags) {
    if (absl::StartsWith(flag, "-Wa,")) {
      for (std::string_view wa_opt : absl::StrSplit(flag.substr(4), ',')) {
        if (wa_opt == "-g" || wa_opt == "-g2") {
          mips_debug = 2;
          mips_optimize = 0;
        } else if (wa_opt == "-O0") {
          mips_optimize = 1;
        } else if (absl::StartsWith(wa_opt, "-O")) {
          mips_optimize = 2;
        }
      }
    }
  }

  if (out_optimize_level != nullptr) {
    *out_optimize_level = optimize;
  }
}

void InferStackFrameSidebandFromRtlCfg(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr) {
    return;
  }
  bool has_call = false;
  int max_neg_stack_var_offset = 0;
  for (const auto& blk : cfg->rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      if (IsCallPatternText(pat) || absl::StrContains(pat, "(call ")) {
        has_call = true;
      }
      size_t pos = 0;
      constexpr std::string_view kStackVarsMarker = "(reg:SI 69 virtual-stack-vars)";
      while ((pos = pat.find(kStackVarsMarker, pos)) != std::string::npos) {
        pos += kStackVarsMarker.size();
        size_t const_pos = pat.find("(const_int ", pos);
        if (const_pos != std::string::npos && const_pos - pos < 16) {
          size_t num_start = const_pos + std::string_view("(const_int ").size();
          size_t num_end = pat.find(')', num_start);
          int offset = 0;
          std::string_view pat_view = pat;
          if (num_end != std::string::npos &&
              absl::SimpleAtoi(pat_view.substr(num_start, num_end - num_start), &offset)) {
            if (offset < 0) {
              max_neg_stack_var_offset = std::max(max_neg_stack_var_offset, -offset);
            } else {
              max_neg_stack_var_offset = std::max(max_neg_stack_var_offset, offset + 8);
            }
          }
        } else {
          max_neg_stack_var_offset = std::max(max_neg_stack_var_offset, 8);
        }
      }
    }
  }
  if (cfg->sideband.frame_size_bytes == 0 && max_neg_stack_var_offset > 0) {
    cfg->sideband.frame_size_bytes = (max_neg_stack_var_offset + 7) & ~7;
  }
  if (cfg->sideband.outgoing_args_size == 0 && has_call) {
    cfg->sideband.outgoing_args_size = 16;
  }
}

std::vector<std::string> ExtractAsmLinesWithLabels(std::string_view asm_text) {
  std::vector<std::string> raw_out;
  absl::flat_hash_map<std::string, std::string> label_remap;
  int next_label_id = 1;

  auto is_local_label_prefix = [](std::string_view s, size_t idx) {
    return idx + 2 < s.size() && (s[idx] == '.' || s[idx] == '$') && s[idx + 1] == 'L' &&
           absl::ascii_isdigit(static_cast<unsigned char>(s[idx + 2]));
  };

  auto record_labels = [&](std::string_view line) {
    size_t i = 0;
    while (i + 2 < line.size()) {
      if (is_local_label_prefix(line, i)) {
        size_t j = i + 2;
        while (j < line.size() && absl::ascii_isdigit(static_cast<unsigned char>(line[j]))) {
          ++j;
        }
        std::string tok(line.substr(i, j - i));
        if (!label_remap.contains(tok)) {
          label_remap.emplace(tok, "$L" + std::to_string(next_label_id++));
        }
        i = j;
      } else {
        ++i;
      }
    }
  };

  if (absl::StrContains(asm_text, ".ent")) {
    raw_out.push_back(".set reorder");
  }

  for (std::string_view raw_line : absl::StrSplit(asm_text, '\n')) {
    size_t comment_pos = raw_line.find('#');
    if (comment_pos != std::string_view::npos) {
      raw_line = raw_line.substr(0, comment_pos);
    }
    std::string_view line = absl::StripAsciiWhitespace(raw_line);
    if (line.empty()) {
      continue;
    }
    if (line.front() == '.' && !absl::EndsWith(line, ":")) {
      std::string_view rest = absl::StripAsciiWhitespace(line.substr(1));
      if (absl::StartsWith(rest, "set") && (rest.size() == 3 || absl::ascii_isspace(rest[3]))) {
        std::string_view opt = absl::StripAsciiWhitespace(rest.substr(3));
        if (opt == "noreorder" || opt == "reorder") {
          raw_out.push_back(absl::StrCat(".set ", opt));
        }
      }
      continue;
    }
    std::string normalized(line);
    std::replace(normalized.begin(), normalized.end(), '\t', ' ');
    // Strip the function entry label `<fn_name>:` while keeping local labels
    // `$L<n>:` / `.L<n>:`.
    if (absl::EndsWith(normalized, ":") && !is_local_label_prefix(normalized, 0)) {
      continue;
    }
    record_labels(normalized);
    raw_out.push_back(std::move(normalized));
  }

  std::vector<std::string> out;
  out.reserve(raw_out.size());
  for (const std::string& line : raw_out) {
    std::string rewritten;
    size_t i = 0;
    while (i < line.size()) {
      if (is_local_label_prefix(line, i)) {
        size_t j = i + 2;
        while (j < line.size() && absl::ascii_isdigit(static_cast<unsigned char>(line[j]))) {
          ++j;
        }
        std::string tok = line.substr(i, j - i);
        auto it = label_remap.find(tok);
        rewritten.append(it != label_remap.end() ? it->second : tok);
        i = j;
      } else {
        rewritten.push_back(line[i]);
        ++i;
      }
    }
    out.push_back(std::move(rewritten));
  }
  return out;
}

std::vector<std::string> ExtractInstructionOnlyAsmLines(std::string_view asm_text) {
  std::vector<std::string> out;
  for (std::string_view raw_line : absl::StrSplit(asm_text, '\n')) {
    size_t comment_pos = raw_line.find('#');
    if (comment_pos != std::string_view::npos) {
      raw_line = raw_line.substr(0, comment_pos);
    }
    std::string_view line = absl::StripAsciiWhitespace(raw_line);
    while (true) {
      size_t colon_pos = line.find(':');
      if (colon_pos == std::string_view::npos) {
        break;
      }
      std::string_view before = line.substr(0, colon_pos);
      if (before.find_first_of(" \t,()") != std::string_view::npos) {
        break;
      }
      line = absl::StripAsciiWhitespace(line.substr(colon_pos + 1));
    }
    if (line.empty() || line.front() == '.') {
      continue;
    }
    std::string normalized(line);
    std::replace(normalized.begin(), normalized.end(), '\t', ' ');
    out.push_back(std::move(normalized));
  }
  return out;
}

bool IsLabelOrDirectiveAsmLine(std::string_view line) {
  std::string_view trimmed = absl::StripAsciiWhitespace(line);
  if (trimmed.empty() || trimmed.front() == '.') {
    return true;
  }
  return trimmed.back() == ':' && trimmed.find_first_of(" \t,()") == std::string_view::npos;
}

bool IsGasMacroOnlyAsmLine(std::string_view insn_asm_line) {
  constexpr int kGasOpAbsent = 1;
  imm_expr.X_op = kGasOpAbsent;
  imm_expr.X_add_number = 0;
  offset_expr.X_op = kGasOpAbsent;
  offset_expr.X_add_number = 0;
  insn_error = nullptr;
  std::string mutable_line(absl::StripAsciiWhitespace(insn_asm_line));
  if (mutable_line.empty()) {
    return false;
  }
  std::replace(mutable_line.begin(), mutable_line.end(), '\t', ' ');
  GasMipsClInsn insn{};
  mips_ip(mutable_line.data(), &insn);
  if (insn_error != nullptr || insn.insn_mo == nullptr || insn.insn_mo->pinfo != 0xffffffffUL) {
    return false;
  }
  return !(insn.insn_mo->args != nullptr && std::strcmp(insn.insn_mo->args, "a") == 0);
}

struct InProcessCc1CompilationResult {
  absl::flat_hash_map<RtlPassKind, RtlControlFlowGraph> pass_cfgs;
  std::string fn_asm_text;
  int frame_size_bytes = 0;
  int outgoing_args_size = 0;
  std::vector<int> saved_gpr_regnos;
  std::vector<int> saved_fpr_regnos;
  std::optional<std::string> epilogue_delay_pattern;
  std::vector<Stage0ConstantPoolEntry> constant_pool;
};

std::vector<Stage0ConstantPoolEntry> FilterReferencedConstantPool(
    const RtlControlFlowGraph& cfg, const std::vector<Stage0ConstantPoolEntry>& pool) {
  std::vector<Stage0ConstantPoolEntry> out;
  for (const auto& cp : pool) {
    const std::string quoted = absl::StrCat("\"", cp.label_name, "\"");
    bool found = false;
    for (const auto& blk : cfg.rpo_blocks) {
      for (const std::string& pat : blk.instructions) {
        if (absl::StrContains(pat, quoted)) {
          found = true;
          break;
        }
      }
      if (found) {
        break;
      }
    }
    if (found) {
      out.push_back(cp);
    }
  }
  return out;
}

std::string ExtractFunctionAssemblyBlock(std::string_view full_asm, std::string_view fn_name) {
  if (fn_name.empty()) {
    return std::string(full_asm);
  }
  std::vector<std::string_view> lines = absl::StrSplit(full_asm, '\n');
  size_t ent_idx = lines.size();
  size_t end_idx = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string_view trimmed = absl::StripAsciiWhitespace(lines[i]);
    if (absl::StartsWith(trimmed, ".ent") &&
        (trimmed.size() > 4 && absl::ascii_isspace(trimmed[4]))) {
      std::string_view sym = absl::StripAsciiWhitespace(trimmed.substr(4));
      if (sym == fn_name || absl::StartsWith(sym, absl::StrCat(fn_name, ","))) {
        ent_idx = i;
      }
    } else if (ent_idx < lines.size() && absl::StartsWith(trimmed, ".end") &&
               (trimmed.size() > 4 && absl::ascii_isspace(trimmed[4]))) {
      std::string_view sym = absl::StripAsciiWhitespace(trimmed.substr(4));
      if (sym == fn_name) {
        end_idx = i;
        break;
      }
    }
  }
  if (ent_idx < lines.size() && end_idx < lines.size() && end_idx >= ent_idx) {
    std::string out;
    for (size_t i = ent_idx; i <= end_idx; ++i) {
      absl::StrAppend(&out, lines[i], "\n");
    }
    return out;
  }
  return std::string(full_asm);
}

InProcessCc1CompilationResult RunInProcessCc1Compilation(std::string_view c_source,
                                                         std::string_view function_name,
                                                         bool stop_after_initial_rtl) {
  InProcessCc1CompilationResult res;

  std::string source_buf(c_source);
  source_buf.push_back('\n');

  clear_const_double_mem();
  const_labelno = 0;
  permanent_allocation(1);
  current_function_decl = nullptr;
  reload_completed = 0;
  void* saved_decls = getdecls();
  void* saved_tags = gettags();

  struct PassDumpStream {
    RtlPassKind kind;
    int* dump_flag;
    FILE** dump_file_ptr;
    char* buf = nullptr;
    size_t len = 0;
    FILE* fp = nullptr;
  };

  PassDumpStream streams[] = {
      {RtlPassKind::kRtl, &rtl_dump, &rtl_dump_file},
      {RtlPassKind::kJump, &jump_opt_dump, &jump_opt_dump_file},
      {RtlPassKind::kCse, &cse_dump, &cse_dump_file},
      {RtlPassKind::kLoop, &loop_dump, &loop_dump_file},
      {RtlPassKind::kCse2, &cse2_dump, &cse2_dump_file},
      {RtlPassKind::kFlow, &flow_dump, &flow_dump_file},
      {RtlPassKind::kCombine, &combine_dump, &combine_dump_file},
      {RtlPassKind::kSched, &sched_dump, &sched_dump_file},
      {RtlPassKind::kLreg, &local_reg_dump, &local_reg_dump_file},
      {RtlPassKind::kGreg, &global_reg_dump, &global_reg_dump_file},
      {RtlPassKind::kSched2, &sched2_dump, &sched2_dump_file},
      {RtlPassKind::kJump2, &jump2_opt_dump, &jump2_opt_dump_file},
      {RtlPassKind::kDbr, &dbr_sched_dump, &dbr_sched_dump_file},
  };

  for (auto& s : streams) {
    s.fp = open_memstream(&s.buf, &s.len);
    *s.dump_flag = 1;
    *s.dump_file_ptr = s.fp;
  }

  char* asm_buf = nullptr;
  size_t asm_len = 0;
  FILE* asm_fp = open_memstream(&asm_buf, &asm_len);
  FILE* saved_asm = asm_out_file;
  asm_out_file = asm_fp;

  FILE* in_fp = fmemopen(source_buf.data(), source_buf.size(), "r");
  FILE* saved_finput = finput;
  finput = in_fp;
  lineno = 1;
  errorcount = 0;
  sorrycount = 0;
  quiet_flag = 1;
  rtl_dump_and_exit = stop_after_initial_rtl ? 1 : 0;

  GccContext::SetCc1ConstantPoolCaptureTarget(&res.constant_pool);
  yyparse();
  GccContext::SetCc1ConstantPoolCaptureTarget(nullptr);
  jump_chain = nullptr;
  max_jump_chain = 0;

  res.frame_size_bytes = frame_offset;
  res.outgoing_args_size = current_function_outgoing_args_size;
  for (int r = 0; r < 32; ++r) {
    if (regs_ever_live[r] != 0) {
      res.saved_gpr_regnos.push_back(r);
    }
  }
  for (int r = 32; r < 64; ++r) {
    if (regs_ever_live[r] != 0) {
      res.saved_fpr_regnos.push_back(r);
    }
  }
  if (current_function_epilogue_delay_list != nullptr &&
      current_function_epilogue_delay_list->fld[0].rtx != nullptr) {
    Rtx delay_insn = current_function_epilogue_delay_list->fld[0].rtx;
    Rtx delay_pat = (delay_insn->code == kInsn || delay_insn->code == kJumpInsn ||
                     delay_insn->code == kCallInsn)
                        ? delay_insn->fld[3].rtx
                        : delay_insn;
    if (delay_pat != nullptr) {
      char* delay_buf = nullptr;
      size_t delay_len = 0;
      FILE* delay_fp = open_memstream(&delay_buf, &delay_len);
      print_rtl(delay_fp, delay_pat);
      std::fclose(delay_fp);
      std::string raw_delay(delay_buf != nullptr ? delay_buf : "", delay_len);
      std::free(delay_buf);
      absl::StrReplaceAll({{"\"*$LC", "\"$LC"}, {"\"*.LC", "\".LC"}}, &raw_delay);
      res.epilogue_delay_pattern = CanonicalizeRtlPattern(raw_delay);
    }
  }

  rtl_dump_and_exit = 0;
  finput = saved_finput;
  asm_out_file = saved_asm;
  std::fclose(in_fp);
  std::fclose(asm_fp);

  for (auto& s : streams) {
    *s.dump_flag = 0;
    *s.dump_file_ptr = nullptr;
    std::fclose(s.fp);
  }

  // Clean up any top-level declarations and struct/union/enum tags added during
  // `yyparse()` so subsequent calls in the same process start with a clean C
  // global scope.
  for (void* d = getdecls(); d != nullptr && d != saved_decls; d = *reinterpret_cast<void**>(d)) {
    void* id = *reinterpret_cast<void**>(static_cast<char*>(d) + 0x38);
    if (id != nullptr) {
      *reinterpret_cast<void**>(static_cast<char*>(id) + 0x28) = nullptr;
      *reinterpret_cast<void**>(static_cast<char*>(id) + 0x30) = nullptr;
      *reinterpret_cast<void**>(static_cast<char*>(id) + 0x38) = nullptr;
      *reinterpret_cast<void**>(static_cast<char*>(id) + 0x40) = nullptr;
    }
  }
  for (void* t = gettags(); t != nullptr && t != saved_tags; t = *reinterpret_cast<void**>(t)) {
    *reinterpret_cast<void**>(static_cast<char*>(t) + 0x18) = nullptr;
  }

  std::string raw_asm(asm_buf != nullptr ? asm_buf : "", asm_len);
  std::free(asm_buf);
  res.fn_asm_text = ExtractFunctionAssemblyBlock(raw_asm, function_name);

  for (auto& s : streams) {
    std::string raw_dump(s.buf != nullptr ? s.buf : "", s.len);
    std::free(s.buf);
    if (stop_after_initial_rtl && s.kind != RtlPassKind::kRtl) {
      continue;
    }
    absl::StrReplaceAll({{"\"*$LC", "\"$LC"}, {"\"*.LC", "\".LC"}}, &raw_dump);
    RtlPassDump pass_dump = ParseRtlPassDump(s.kind, raw_dump);
    const RtlFunctionDump* fn_dump = nullptr;
    if (!function_name.empty()) {
      fn_dump = pass_dump.FindFunction(function_name);
    }
    if (fn_dump == nullptr && !pass_dump.Functions().empty()) {
      fn_dump = &pass_dump.Functions().back();
    }
    RtlControlFlowGraph cfg;
    cfg.pass = s.kind;
    if (fn_dump != nullptr) {
      cfg = BuildCanonicalCfgFromRtlDump(*fn_dump);
    }
    res.pass_cfgs[s.kind] = std::move(cfg);
  }

  return res;
}

}  // namespace

std::string_view SubfunctionIdName(SubfunctionId id) {
  switch (id) {
    case SubfunctionId::kInstantiateVirtualRegs:
      return "instantiate_virtual_regs";
    case SubfunctionId::kJumpRedirect:
      return "redirect_jump";
    case SubfunctionId::kJump1Optimize:
      return "jump_optimize (jump1)";
    case SubfunctionId::kFoldRtx:
      return "fold_rtx";
    case SubfunctionId::kCseBasicBlockCse1:
      return "cse_basic_block (cse1)";
    case SubfunctionId::kScanLoop:
      return "scan_loop";
    case SubfunctionId::kStrengthReduce:
      return "strength_reduce";
    case SubfunctionId::kCseBasicBlockCse2:
      return "cse_basic_block (cse2)";
    case SubfunctionId::kFindBasicBlocks:
      return "find_basic_blocks";
    case SubfunctionId::kPropagateBlock:
      return "propagate_block";
    case SubfunctionId::kSimplifyRtx:
      return "simplify_rtx";
    case SubfunctionId::kSubst:
      return "subst";
    case SubfunctionId::kTryCombine:
      return "try_combine";
    case SubfunctionId::kScheduleBlockSched1:
      return "schedule_block (sched1)";
    case SubfunctionId::kUpdateEquivRegs:
      return "update_equiv_regs";
    case SubfunctionId::kBlockAlloc:
      return "block_alloc";
    case SubfunctionId::kGlobalAlloc:
      return "global_alloc";
    case SubfunctionId::kReload:
      return "reload";
    case SubfunctionId::kAlterReg:
      return "alter_reg";
    case SubfunctionId::kScheduleBlockSched2:
      return "schedule_block (sched2)";
    case SubfunctionId::kDoCrossJump:
      return "do_cross_jump (jump2)";
    case SubfunctionId::kFillSimpleDelaySlots:
      return "fill_simple_delay_slots";
    case SubfunctionId::kFillEagerDelaySlots:
      return "fill_eager_delay_slots";
    case SubfunctionId::kRelaxDelaySlots:
      return "relax_delay_slots";
    case SubfunctionId::kMakeReturnInsns:
      return "make_return_insns";
    case SubfunctionId::kFunctionPrologueEpilogue:
      return "function_prologue_epilogue";
    case SubfunctionId::kFinalScanInsn:
      return "final_scan_insn";
    case SubfunctionId::kGasMipsIp:
      return "gas_mips_ip";
    case SubfunctionId::kGasMacro:
      return "gas_macro";
    case SubfunctionId::kGasAppendInsn:
      return "gas_append_insn";
  }
  return "unknown";
}

absl::Span<const SubfunctionId> AllSubfunctionIds() {
  return absl::MakeConstSpan(kAllSubfunctionIds);
}

void FunctionHooks::ApplyCompilationProfile(const CompilationProfile& profile) {
  int opt = profile.optimize_level;
  ApplyCompilerFlags(profile.c_flags, &opt);
}

RtlControlFlowGraph FunctionHooks::CompileCSourceToInitialRtlCfg(
    std::string_view c_source, std::string_view function_name,
    absl::Span<const std::string> c_flags) {
  int opt_level = 2;
  ApplyCompilerFlags(c_flags, &opt_level);

  InProcessCc1CompilationResult cc1_res =
      RunInProcessCc1Compilation(c_source, function_name, /*stop_after_initial_rtl=*/true);
  RtlControlFlowGraph cfg = cc1_res.pass_cfgs[RtlPassKind::kRtl];
  cfg.pass = RtlPassKind::kRtl;
  cfg.sideband.frame_size_bytes = cc1_res.frame_size_bytes;
  cfg.sideband.outgoing_args_size = cc1_res.outgoing_args_size;
  cfg.sideband.constant_pool = FilterReferencedConstantPool(cfg, cc1_res.constant_pool);
  InferStackFrameSidebandFromRtlCfg(&cfg);

  // Materialize into `ctx_`, run `unshare_all_rtl` and `instantiate_virtual_regs`
  // (just as `rest_of_compilation` does right after dumping `.00.rtl`), and
  // dematerialize into the canonical `.00.rtl` CFG.
  if (!function_name.empty()) {
    ctx_->BeginFunction(function_name);
  }
  Rtx head = ctx_->MaterializeFromCfg(cfg);
  if (head != nullptr) {
    unshare_all_rtl(head);
    instantiate_virtual_regs(current_function_decl, head);
  }
  return ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kRtl);
}

SubfunctionBoundarySnapshot FunctionHooks::ExecuteSubfunctionBoundary(
    SubfunctionId subfunction_id, const RtlControlFlowGraph& pre_state,
    absl::Span<const std::string> pre_asm_lines, int optimize_level,
    absl::Span<const std::string> c_flags) {
  SubfunctionBoundarySnapshot snap;
  snap.subfunction_id = subfunction_id;
  snap.optimize_level = optimize_level;
  snap.c_flags.assign(c_flags.begin(), c_flags.end());
  snap.pre_state = pre_state;
  snap.pre_asm_lines.assign(pre_asm_lines.begin(), pre_asm_lines.end());

  ApplyCompilerFlags(c_flags, &optimize_level);
  snap.optimize_level = optimize_level;

  switch (subfunction_id) {
    case SubfunctionId::kInstantiateVirtualRegs: {
      snap.post_state = RunInstantiateVirtualRegs(*ctx_, pre_state);
      snap.post_state.pass = RtlPassKind::kRtl;
      break;
    }

    case SubfunctionId::kJumpRedirect: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr) {
        unshare_all_rtl(head);
        instantiate_virtual_regs(current_function_decl, head);
        RefreshJumpLabelsAndNuses(head);
        for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
          if (insn->code == kJumpInsn && insn->fld[7].rtx != nullptr) {
            Rtx target_lbl = insn->fld[7].rtx;
            Rtx next = target_lbl->fld[2].rtx;
            while (next != nullptr && next->code == kNote) {
              next = next->fld[2].rtx;
            }
            if (next != nullptr && next->code == kJumpInsn && next->fld[3].rtx != nullptr &&
                next->fld[3].rtx->code == kSet && next->fld[3].rtx->fld[0].rtx == pc_rtx &&
                next->fld[3].rtx->fld[1].rtx != nullptr &&
                next->fld[3].rtx->fld[1].rtx->code == kLabelRef && next->fld[7].rtx != nullptr &&
                next->fld[7].rtx != target_lbl) {
              redirect_jump(insn, next->fld[7].rtx);
            }
          }
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kJump);
      break;
    }

    case SubfunctionId::kJump1Optimize: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr) {
        unshare_all_rtl(head);
        instantiate_virtual_regs(current_function_decl, head);
        RunJumpOptimize(/*cross_jump=*/0, /*noop_moves=*/0, /*after_regscan=*/1, head);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kJump);
      break;
    }

    case SubfunctionId::kFoldRtx: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        PrepareCseFoldState(ctx_, head);
        for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
          if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
              insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code != kSequence &&
              insn->fld[3].rtx->code != kAddrVec && insn->fld[3].rtx->code != kAddrDiffVec) {
            int clobbers = 0;
            if (recog(insn->fld[3].rtx, insn, &clobbers) < 0) {
              insn->fld[3].rtx = fold_rtx(insn->fld[3].rtx, insn);
            }
          }
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kCse);
      break;
    }

    case SubfunctionId::kCseBasicBlockCse1: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        cse_not_expected = 0;
        RefreshJumpLabelsAndNuses(head);
        restore_reg_data(head);
        reg_scan(head, max_reg_num(), 1);
        if (flag_thread_jumps != 0) {
          thread_jumps(head, max_reg_num(), 1);
          head = ctx_->GetInsns();
          RefreshJumpLabelsAndNuses(head);
          restore_reg_data(head);
          reg_scan(head, max_reg_num(), 1);
        }
        const int tem = ExecuteCsePassGranular(this, ctx_, head, max_reg_num(), /*after_loop=*/0);
        head = ctx_->GetInsns();
        delete_dead_from_cse(head, max_reg_num());
        if (tem != 0 || optimize_level > 1) {
          head = ctx_->GetInsns();
          RefreshJumpLabelsAndNuses(head);
          jump_optimize(head, /*cross_jump=*/0, /*noop_moves=*/0, /*after_regscan=*/0);
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kCse);
      break;
    }

    case SubfunctionId::kScanLoop: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        PrepareLoopState(head);
        in_loop_pass_ = true;
        for (int i = max_loop_num - 1; i >= 0; --i) {
          if (loop_invalid[i] == 0 && loop_number_loop_ends[i] != nullptr) {
            RunScanLoop(loop_number_loop_starts[i], loop_number_loop_ends[i], max_reg_num());
          }
        }
        in_loop_pass_ = false;
        restore_reg_data(ctx_->GetInsns());
        reg_scan(ctx_->GetInsns(), max_reg_num(), 1);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kLoop);
      break;
    }

    case SubfunctionId::kStrengthReduce: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0 && flag_strength_reduce != 0) {
        PrepareLoopState(head);
        in_loop_pass_ = true;
        for (int i = max_loop_num - 1; i >= 0; --i) {
          if (loop_invalid[i] == 0 && loop_number_loop_ends[i] != nullptr) {
            RunStrengthReduce(nullptr, loop_number_loop_ends[i], nullptr, 0,
                              loop_number_loop_starts[i], loop_number_loop_ends[i]);
          }
        }
        in_loop_pass_ = false;
        restore_reg_data(ctx_->GetInsns());
        reg_scan(ctx_->GetInsns(), max_reg_num(), 1);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kLoop);
      break;
    }

    case SubfunctionId::kCseBasicBlockCse2: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        cse_not_expected = 0;
        int rerun_cse = flag_expensive_optimizations;
        for (const auto& opt : f_options) {
          if (opt.string != nullptr && std::strcmp(opt.string, "rerun-cse-after-loop") == 0 &&
              opt.variable != nullptr) {
            rerun_cse = *opt.variable;
            break;
          }
        }
        if (rerun_cse != 0) {
          RefreshJumpLabelsAndNuses(head);
          restore_reg_data(head);
          reg_scan(head, max_reg_num(), 0);
          jump_optimize(head, /*cross_jump=*/0, /*noop_moves=*/0, /*after_regscan=*/1);
          head = ctx_->GetInsns();
          RefreshJumpLabelsAndNuses(head);
          restore_reg_data(head);
          reg_scan(head, max_reg_num(), 0);
          const int tem = ExecuteCsePassGranular(this, ctx_, head, max_reg_num(), /*after_loop=*/1);
          if (tem != 0) {
            head = ctx_->GetInsns();
            RefreshJumpLabelsAndNuses(head);
            jump_optimize(head, /*cross_jump=*/0, /*noop_moves=*/0, /*after_regscan=*/0);
          }
        }
        if (flag_thread_jumps != 0) {
          head = ctx_->GetInsns();
          RefreshJumpLabelsAndNuses(head);
          restore_reg_data(head);
          reg_scan(head, max_reg_num(), 0);
          thread_jumps(head, max_reg_num(), 0);
        }
        cse_not_expected = 1;
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kCse2);
      break;
    }

    case SubfunctionId::kFindBasicBlocks: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        RunFindBasicBlocks(head, nullptr);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kFlow);
      break;
    }

    case SubfunctionId::kPropagateBlock: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr) {
        if (optimize_level > 0) {
          StripFlowGeneratedNotes(head);
          restore_reg_data(head);
          const int num_regs = max_reg_num();
          max_regno = num_regs;
          reg_scan(head, num_regs, 0);
          RunFindBasicBlocks(head, nullptr);
          ExecuteLifeAnalysisGranular(this, head, num_regs);
        } else {
          StripFlowGeneratedNotes(head);
          restore_reg_data(head);
          max_regno = max_reg_num();
          reg_scan(head, max_regno, 0);
          regclass_init();
          regclass(head, max_regno);
          stupid_life_analysis(head, max_regno, nullptr);
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kFlow);
      break;
    }

    case SubfunctionId::kSimplifyRtx: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        PrepareCombineState(head);
        for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
          if (insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) {
            continue;
          }
          Rtx pat = insn->fld[3].rtx;
          if (pat == nullptr || pat->code != kSet) {
            continue;
          }
          Rtx src = pat->fld[1].rtx;
          if (src == nullptr || src->code == kConstInt || src->code == kReg ||
              src->code == kClobber || src->code == kMem || src->code == kCall) {
            continue;
          }
          int old_clobbers = 0;
          const int old_recog = recog(pat, insn, &old_clobbers);
          undobuf.num_undo = 0;
          undobuf.other_insn = nullptr;
          previous_num_undos = 0;
          subst_prev_insn = nullptr;
          const int op0_mode = (rtx_format[src->code][0] == 'e' && src->fld[0].rtx != nullptr &&
                                src->fld[0].rtx->mode != kVoidMode)
                                   ? static_cast<int>(src->fld[0].rtx->mode)
                                   : static_cast<int>(src->mode);
          Rtx simplified = simplify_rtx(src, op0_mode, /*last=*/1, /*in_dest=*/0);
          if (simplified != nullptr && simplified->code != kClobber) {
            pat->fld[1].rtx = simplified;
            int new_clobbers = 0;
            const int new_recog = recog(pat, insn, &new_clobbers);
            if (old_recog < 0 && undobuf.other_insn == nullptr) {
              if (new_recog >= 0) {
                insn->fld[4].rtint = new_recog;
              }
            } else {
              pat->fld[1].rtx = src;
              UndoCombineChanges();
            }
          } else {
            UndoCombineChanges();
          }
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kCombine);
      break;
    }

    case SubfunctionId::kSubst: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        PrepareCombineState(head);
        for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
          if (insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) {
            continue;
          }
          Rtx pat = insn->fld[3].rtx;
          if (pat == nullptr || pat->code == kSequence || pat->code == kUse ||
              pat->code == kClobber || pat->code == kAddrVec || pat->code == kAddrDiffVec) {
            continue;
          }
          int old_clobbers = 0;
          const int old_recog = recog(pat, insn, &old_clobbers);
          undobuf.num_undo = 0;
          undobuf.other_insn = nullptr;
          previous_num_undos = 0;
          subst_prev_insn = nullptr;
          Rtx new_pat = subst(pat, pc_rtx, pc_rtx, /*in_dest=*/0, /*unique_copy=*/0);
          if (new_pat != nullptr && new_pat->code != kClobber) {
            insn->fld[3].rtx = new_pat;
            int new_clobbers = 0;
            const int new_recog = recog(new_pat, insn, &new_clobbers);
            if (old_recog < 0 && undobuf.other_insn == nullptr) {
              if (new_recog >= 0) {
                insn->fld[4].rtint = new_recog;
              }
            } else {
              insn->fld[3].rtx = pat;
              UndoCombineChanges();
            }
          } else {
            UndoCombineChanges();
          }
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kCombine);
      break;
    }

    case SubfunctionId::kTryCombine: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        ExecuteCombinePassGranular(head, 0);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kCombine);
      break;
    }

    case SubfunctionId::kScheduleBlockSched1: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0 && flag_schedule_insns != 0) {
        reload_completed = 0;
        ExecuteSchedPassGranular(head);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kSched);
      break;
    }

    case SubfunctionId::kUpdateEquivRegs: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr && optimize_level > 0) {
        RunUpdateEquivRegs(head);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kLreg);
      break;
    }

    case SubfunctionId::kBlockAlloc: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr) {
        if (optimize_level > 0) {
          PrepareLocalAllocState(head);
          in_local_alloc_pass_ = true;
          for (int b = 0; b < n_basic_blocks; ++b) {
            RunBlockAlloc(b, head);
          }
          in_local_alloc_pass_ = false;
        } else {
          StripFlowGeneratedNotes(head);
          restore_reg_data(head);
          max_regno = max_reg_num();
          reg_scan(head, max_regno, 0);
          regclass_init();
          regclass(head, max_regno);
          stupid_life_analysis(head, max_regno, nullptr);
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kLreg);
      break;
    }

    case SubfunctionId::kGlobalAlloc: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      snap.post_state = pre_state;
      snap.post_state.pass = RtlPassKind::kGreg;
      if (head != nullptr) {
        if (optimize_level > 0) {
          RunGlobalAlloc(head);
        } else {
          StripFlowGeneratedNotes(head);
          restore_reg_data(head);
          max_regno = max_reg_num();
          reg_scan(head, max_regno, 0);
          regclass_init();
          regclass(head, max_regno);
          stupid_life_analysis(head, max_regno, nullptr);
        }
        const int nregs = std::max(max_reg_num(), max_regno);
        snap.post_state.sideband.max_regno = std::max(snap.post_state.sideband.max_regno, nregs);
        if (reg_renumber != nullptr) {
          snap.post_state.sideband.reg_renumber.clear();
          for (int r = kFirstPseudoRegister; r < nregs; ++r) {
            if (reg_renumber[r] >= 0) {
              snap.post_state.sideband.reg_renumber[r] = reg_renumber[r];
            }
          }
        }
      }
      break;
    }

    case SubfunctionId::kReload: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr) {
        RunReload(head, /*global=*/optimize_level > 0 ? 1 : 0);
        for (const auto& [regno, hard_reg] : pre_state.sideband.reg_renumber) {
          if (regno >= kFirstPseudoRegister && hard_reg >= 0 && regno < max_regno &&
              reg_renumber != nullptr && reg_renumber[regno] >= 0 && regno_reg_rtx != nullptr &&
              regno_reg_rtx[regno] != nullptr && regno_reg_rtx[regno]->code == kReg) {
            regno_reg_rtx[regno]->fld[0].rtint = regno;
          }
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kGreg);
      break;
    }

    case SubfunctionId::kAlterReg: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr) {
        std::vector<int> regnos;
        regnos.reserve(pre_state.sideband.reg_renumber.size());
        for (const auto& [regno, _] : pre_state.sideband.reg_renumber) {
          if (regno >= kFirstPseudoRegister) {
            regnos.push_back(regno);
          }
        }
        std::sort(regnos.begin(), regnos.end());
        for (int regno : regnos) {
          RunAlterReg(regno, -1);
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kGreg);
      break;
    }

    case SubfunctionId::kScheduleBlockSched2: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      if (head != nullptr) {
        reload_completed = 1;
        bool has_prologue_sp_adjust = false;
        if (!pre_state.rpo_blocks.empty()) {
          for (const std::string& pat : pre_state.rpo_blocks[0].instructions) {
            if (absl::StartsWith(pat, "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp)")) {
              has_prologue_sp_adjust = true;
              break;
            }
          }
        }
        if (!has_prologue_sp_adjust) {
          frame_pointer_needed =
              (pre_state.sideband.frame_pointer_needed || flag_omit_frame_pointer == 0) ? 1 : 0;
          const SavedBlockLiveState saved_live = SaveActiveBasicBlockLiveAtStart();
          RunFindBasicBlocks(head, nullptr);
          RefreshJumpLabelsAndNuses(head);
          RestoreBasicBlockLiveAtStartAfterFindBasicBlocks(saved_live, *ctx_);
          thread_prologue_and_epilogue_insns(head);
          head = ctx_->GetInsns();
        }
        if (optimize_level > 0 && flag_schedule_insns_after_reload != 0) {
          ExecuteSchedPassGranular(head);
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kSched2);
      break;
    }

    case SubfunctionId::kDoCrossJump: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      const bool had_live = !pre_state.sideband.basic_block_live_at_start.empty();
      if (head != nullptr && optimize_level > 0) {
        if (had_live) {
          PrepareReorgState(head);
          head = ctx_->GetInsns();
        }
        RunJumpOptimize(/*cross_jump=*/1, /*noop_moves=*/1, /*after_regscan=*/0, head);
        if (had_live) {
          last_reorg_post_live_ = CapturePostReorgLiveAtStart(ctx_->GetInsns());
        }
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kJump2);
      if (head != nullptr && optimize_level > 0 && had_live) {
        snap.post_state.sideband.basic_block_live_at_start = last_reorg_post_live_;
      }
      break;
    }

    case SubfunctionId::kFillSimpleDelaySlots: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      const bool ran = (head != nullptr && optimize_level > 0 && flag_delayed_branch != 0);
      if (ran) {
        RunFillSimpleDelaySlots(/*non_jumps_p=*/0, head);
        RunFillSimpleDelaySlots(/*non_jumps_p=*/1, ctx_->GetInsns());
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kDbr);
      if (ran && !pre_state.sideband.basic_block_live_at_start.empty()) {
        snap.post_state.sideband.basic_block_live_at_start = last_reorg_post_live_;
      }
      break;
    }

    case SubfunctionId::kFillEagerDelaySlots: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      const bool ran = (head != nullptr && optimize_level > 0 && flag_delayed_branch != 0);
      if (ran) {
        RunFillEagerDelaySlots(head);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kDbr);
      if (ran && !pre_state.sideband.basic_block_live_at_start.empty()) {
        snap.post_state.sideband.basic_block_live_at_start = last_reorg_post_live_;
      }
      break;
    }

    case SubfunctionId::kRelaxDelaySlots: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      const bool ran = (head != nullptr && optimize_level > 0 && flag_delayed_branch != 0);
      if (ran) {
        RunRelaxDelaySlots(head);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kDbr);
      if (ran && !pre_state.sideband.basic_block_live_at_start.empty()) {
        snap.post_state.sideband.basic_block_live_at_start = last_reorg_post_live_;
      }
      break;
    }

    case SubfunctionId::kMakeReturnInsns: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      const bool ran = (head != nullptr && optimize_level > 0 && flag_delayed_branch != 0);
      if (ran) {
        RunMakeReturnInsns(head);
      }
      snap.post_state = ctx_->DematerializeToCfg(ctx_->GetInsns(), RtlPassKind::kDbr);
      if (ran && !pre_state.sideband.basic_block_live_at_start.empty()) {
        snap.post_state.sideband.basic_block_live_at_start = last_reorg_post_live_;
      }
      break;
    }

    case SubfunctionId::kFunctionPrologueEpilogue: {
      ctx_->MaterializeFromCfg(pre_state);
      uint32_t saved_mask = 0;
      for (int r : pre_state.sideband.saved_gpr_regnos) {
        if (r >= 0 && r < 32) {
          saved_mask |= (1u << r);
        }
      }
      uint32_t saved_fpr_mask = 0;
      for (int r : pre_state.sideband.saved_fpr_regnos) {
        if (r >= 32 && r < 64) {
          saved_fpr_mask |= (1u << (r - 32));
        }
      }
      const std::string asm_text = RunFunctionPrologueEpilogue(pre_state.sideband.frame_size_bytes,
                                                               saved_mask, saved_fpr_mask);
      snap.post_state = pre_state;
      for (std::string_view raw_line : absl::StrSplit(asm_text, '\n')) {
        std::string_view line = absl::StripAsciiWhitespace(raw_line);
        if (!line.empty() && !absl::StartsWith(line, ".file")) {
          snap.post_asm_lines.emplace_back(line);
        }
      }
      break;
    }

    case SubfunctionId::kFinalScanInsn: {
      Rtx head = ctx_->MaterializeFromCfg(pre_state);
      snap.post_state = pre_state;
      const std::string asm_text = RunFinal(head, /*include_prologue_epilogue=*/false);
      snap.post_asm_lines = ExtractAsmLinesWithLabels(asm_text);
      break;
    }

    case SubfunctionId::kGasMipsIp: {
      snap.post_state = pre_state;
      snap.post_asm_lines = snap.pre_asm_lines;
      for (const std::string& line : snap.pre_asm_lines) {
        if (IsLabelOrDirectiveAsmLine(line) || IsGasMacroOnlyAsmLine(line)) {
          continue;
        }
        snap.post_machine_words.push_back(RunGasMipsIp(line));
      }
      break;
    }

    case SubfunctionId::kGasMacro: {
      snap.post_state = pre_state;
      snap.post_asm_lines = snap.pre_asm_lines;
      for (const std::string& line : snap.pre_asm_lines) {
        if (IsLabelOrDirectiveAsmLine(line)) {
          continue;
        }
        std::vector<uint32_t> macro_words = RunGasMacro(line);
        if (!macro_words.empty()) {
          snap.post_machine_words.insert(snap.post_machine_words.end(), macro_words.begin(),
                                         macro_words.end());
        } else {
          snap.post_machine_words.push_back(RunGasAppendInsn(line));
        }
      }
      break;
    }

    case SubfunctionId::kGasAppendInsn: {
      snap.post_state = pre_state;
      snap.post_asm_lines = snap.pre_asm_lines;
      const std::string joined_asm = absl::StrJoin(snap.pre_asm_lines, "\n");
      snap.post_machine_words = RunGasMdAssemble(joined_asm);
      while (snap.post_machine_words.size() % 4 != 0) {
        snap.post_machine_words.push_back(0x00000000u);
      }
      break;
    }
  }

  return snap;
}

SubfunctionBoundarySnapshot FunctionHooks::ExecuteSubfunctionBoundary(
    const SubfunctionBoundarySnapshot& snap_template,
    const RtlControlFlowGraph& candidate_pre_state,
    absl::Span<const std::string> candidate_pre_asm_lines) {
  const absl::Span<const std::string> asm_lines =
      !candidate_pre_asm_lines.empty() ? candidate_pre_asm_lines
                                       : absl::MakeConstSpan(snap_template.pre_asm_lines);
  return ExecuteSubfunctionBoundary(snap_template.subfunction_id, candidate_pre_state, asm_lines,
                                    snap_template.optimize_level, snap_template.c_flags);
}

FunctionCompilationTrace FunctionHooks::TraceRtlPipeline(const RtlControlFlowGraph& rtl_00_cfg,
                                                         int optimize_level,
                                                         std::string_view function_name,
                                                         std::string_view test_name,
                                                         absl::Span<const std::string> c_flags) {
  FunctionCompilationTrace trace;
  trace.test_name = std::string(test_name);
  trace.function_name = std::string(function_name);
  trace.optimize_level = optimize_level;
  trace.c_flags.assign(c_flags.begin(), c_flags.end());

  ApplyCompilerFlags(c_flags, &optimize_level);
  trace.optimize_level = optimize_level;

  if (!function_name.empty()) {
    ctx_->BeginFunction(function_name);
  }

  RtlControlFlowGraph initial_cfg = rtl_00_cfg;
  initial_cfg.pass = RtlPassKind::kRtl;
  InferStackFrameSidebandFromRtlCfg(&initial_cfg);
  trace.pass_snapshots[RtlPassKind::kRtl] = initial_cfg;

  RtlControlFlowGraph current_cfg = initial_cfg;
  std::vector<std::string> current_asm_lines;

  for (SubfunctionId sub_id : AllSubfunctionIds()) {
    SubfunctionBoundarySnapshot snap =
        ExecuteSubfunctionBoundary(sub_id, current_cfg, current_asm_lines, optimize_level, c_flags);
    current_cfg = snap.post_state;
    if (sub_id != SubfunctionId::kFunctionPrologueEpilogue && !snap.post_asm_lines.empty()) {
      current_asm_lines = snap.post_asm_lines;
    }

    switch (sub_id) {
      case SubfunctionId::kJump1Optimize:
        trace.pass_snapshots[RtlPassKind::kJump] = current_cfg;
        break;
      case SubfunctionId::kCseBasicBlockCse1:
        trace.pass_snapshots[RtlPassKind::kCse] = current_cfg;
        break;
      case SubfunctionId::kStrengthReduce:
        trace.pass_snapshots[RtlPassKind::kLoop] = current_cfg;
        break;
      case SubfunctionId::kCseBasicBlockCse2:
        trace.pass_snapshots[RtlPassKind::kCse2] = current_cfg;
        break;
      case SubfunctionId::kPropagateBlock:
        trace.pass_snapshots[RtlPassKind::kFlow] = current_cfg;
        break;
      case SubfunctionId::kTryCombine:
        trace.pass_snapshots[RtlPassKind::kCombine] = current_cfg;
        break;
      case SubfunctionId::kScheduleBlockSched1:
        trace.pass_snapshots[RtlPassKind::kSched] = current_cfg;
        break;
      case SubfunctionId::kBlockAlloc:
        trace.pass_snapshots[RtlPassKind::kLreg] = current_cfg;
        break;
      case SubfunctionId::kAlterReg:
        trace.pass_snapshots[RtlPassKind::kGreg] = current_cfg;
        break;
      case SubfunctionId::kScheduleBlockSched2:
        trace.pass_snapshots[RtlPassKind::kSched2] = current_cfg;
        break;
      case SubfunctionId::kDoCrossJump:
        trace.pass_snapshots[RtlPassKind::kJump2] = current_cfg;
        break;
      case SubfunctionId::kMakeReturnInsns:
        trace.pass_snapshots[RtlPassKind::kDbr] = current_cfg;
        break;
      case SubfunctionId::kFinalScanInsn: {
        Rtx final_head = ctx_->MaterializeFromCfg(current_cfg);
        trace.final_asm_text = RunFinal(final_head);
        trace.final_insn_asm_lines = ExtractInstructionOnlyAsmLines(trace.final_asm_text);
        current_asm_lines = ExtractAsmLinesWithLabels(trace.final_asm_text);
        break;
      }
      case SubfunctionId::kGasAppendInsn: {
        trace.final_machine_words = snap.post_machine_words;
        trace.stage0_input = RunGasMdAssembleWithMetadata(trace.final_asm_text);
        trace.stage0_input.profile.optimize_level = trace.optimize_level;
        trace.stage0_input.profile.c_flags = trace.c_flags;
        break;
      }
      default:
        break;
    }

    trace.subfunction_snapshots.push_back(std::move(snap));
  }

  clear_const_double_mem();
  return trace;
}

FunctionCompilationTrace FunctionHooks::TraceForwardCompilation(
    std::string_view c_source, std::string_view function_name,
    absl::Span<const std::string> c_flags, std::string_view test_name) {
  int optimize_level = 2;
  ApplyCompilerFlags(c_flags, &optimize_level);
  if (!function_name.empty()) {
    ctx_->BeginFunction(function_name);
  }

  InProcessCc1CompilationResult cc1_res =
      RunInProcessCc1Compilation(c_source, function_name, /*stop_after_initial_rtl=*/true);

  RtlControlFlowGraph rtl_00_cfg = cc1_res.pass_cfgs[RtlPassKind::kRtl];
  rtl_00_cfg.pass = RtlPassKind::kRtl;
  rtl_00_cfg.sideband.frame_size_bytes = cc1_res.frame_size_bytes;
  rtl_00_cfg.sideband.outgoing_args_size = cc1_res.outgoing_args_size;
  rtl_00_cfg.sideband.constant_pool =
      FilterReferencedConstantPool(rtl_00_cfg, cc1_res.constant_pool);
  InferStackFrameSidebandFromRtlCfg(&rtl_00_cfg);
  return TraceRtlPipeline(rtl_00_cfg, optimize_level, function_name, test_name, c_flags);
}

FunctionCompilationTrace FunctionHooks::TraceForwardCompilation(
    const RoundTripTestCase& test_case) {
  static constexpr std::string_view kBaseTypesPreamble = R"(
typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
typedef struct { u8 b[4]; } UnalignedWord;
typedef struct { u8 b[6]; } UnalignedBlock6;
typedef struct { u8 b[8]; } UnalignedBlock8;
typedef struct { u8 b[12]; } UnalignedBlock12;
typedef struct { u8 b[16]; } UnalignedBlock16;
)";
  std::string full_source = absl::StrCat(kBaseTypesPreamble, "\n");
  if (!test_case.options.extra_types_header.empty()) {
    absl::StrAppend(&full_source, test_case.options.extra_types_header, "\n");
  }
  absl::StrAppend(&full_source, test_case.original_c);
  return TraceForwardCompilation(full_source, test_case.function_name, test_case.options.c_flags,
                                 test_case.test_name);
}

}  // namespace rom_nom_nom::gcc_harness
