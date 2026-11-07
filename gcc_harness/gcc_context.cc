#include "gcc_harness/gcc_context.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "core/rtl_sexpr_utils.h"

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {

// GCC 2.7.2 initialization and pass setup routines.
void init_reg_sets();
void init_reg_sets_1();
void set_target_switch(char* name);
void override_options();
void init_obstacks();
void init_tree_codes();
void init_lex();
void init_rtl();
void init_emit_once(int line_numbers);
void init_regs();
void init_decl_processing();
void init_optabs();
void init_stmt();
void init_expmed();
void init_expr_once();
void init_loop();
void init_reload();
void init_caller_save();
void init_recog();
void init_alias_analysis();
void init_final(char* filename);
void init_function_start(void* subr, char* filename, int line);
void expand_function_start(void* subr, int parms_have_cleanups);
void expand_function_end(char* filename, int line, int end_bindings);
void instantiate_virtual_regs(void* fndecl, rtx_def* insns);
void restore_reg_data(rtx_def* first);
void reg_scan(rtx_def* f, int nregs, int repeat);
int compute_frame_size(int size);
void set_last_insn(rtx_def* insn);
void start_sequence();
void end_sequence();
void push_temp_slots();
void pop_temp_slots();
void free_temp_slots();
void emit_queue();
void emit_nop();
rtx_def* emit_move_insn(rtx_def* x, rtx_def* y);
void* make_node(int code);
void* get_identifier(const char* text);
void* build_decl(int code, void* name, void* type);
void layout_decl(void* decl, unsigned int known_align);
void layout_type(void* type);
void* build_pointer_type(void* to_type);
void* c_build_type_variant(void* type, int constp, int volatilep);
void* build_array_type(void* elt_type, void* index_type);
void* build_index_type(void* maxval);
void* build_function_type(void* value_type, void* arg_types);
void* tree_cons(void* purpose, void* value, void* chain);
void* nreverse(void* t);
void* chainon(void* op1, void* op2);
void* build_int_2_wide(int low, int hi);
void* build_real(void* type, double d);
void* build_string(int len, const char* str);
void* build(int code, void* tt, ...);
void* build1(int code, void* type, void* node);
void* fold(void* expr);
void* convert(void* type, void* expr);
void* convert_and_check(void* type, void* expr);
void* default_conversion(void* exp);
void* truthvalue_conversion(void* expr);
void* check_case_value(void* value);
void* build_binary_op(int code, void* orig_op0, void* orig_op1, int convert_p);
void* build_unary_op(int code, void* xarg, int noconvert);
void* build_modify_expr(void* lhs, int modifycode, void* rhs);
void* build_indirect_ref(void* ptr, const char* errorstring);
void* build_array_ref(void* array, void* index);
void* build_c_cast(void* type, void* expr);
void* build_conditional_expr(void* ifexp, void* op1, void* op2);
void* build_compound_expr(void* list);
void* build_function_call(void* function, void* params);
void make_decl_rtl(void* decl, char* asmspec, int top_level);
void expand_decl(void* decl);
void expand_decl_init(void* decl);
void c_expand_return(void* retval);
void expand_return(void* retval);
void expand_null_return();
void c_expand_expr_stmt(void* expr);
rtx_def* expand_expr(void* exp, rtx_def* target, int tmode, int modifier);
rtx_def* expand_assignment(void* to, void* from, int want_value, int suggest_reg);
rtx_def* store_expr(void* exp, rtx_def* target, int want_value);
rtx_def* do_store_flag(void* exp, rtx_def* target, int mode, int only_cheap);
void do_jump(void* exp, rtx_def* if_false_label, rtx_def* if_true_label);
rtx_def* expand_call(void* exp, rtx_def* target, int ignore);
void expand_start_cond(void* cond, int exitflag);
void expand_start_else();
void expand_end_cond();
void* expand_start_loop(int exit_flag);
void* expand_start_loop_continue_elsewhere(int exit_flag);
void expand_loop_continue_here();
int expand_exit_loop_if_false(void* whichloop, void* cond);
int expand_continue_loop(void* whichloop);
int expand_exit_loop(void* whichloop);
int expand_exit_something();
void expand_end_loop();
void expand_start_bindings(int exit_flag);
void c_expand_start_case(void* exp);
int pushcase(void* value, void* (*converter)(void*, void*), void* label, void** duplicate);
void expand_end_case(void* orig_index);
void expand_label(void* body);
void expand_goto(void* body);
void* _obstack_newchunk(GccObstack* ob, int length);

struct GccLangIndepOption {
  const char* string;
  int* variable;
  int on_value;
};
extern GccLangIndepOption f_options[39];
extern int flag_signed_char;
extern int obey_regdecls;
extern int flag_defer_pop;
extern int flag_thread_jumps;
extern int flag_omit_frame_pointer;
extern int flag_cse_follow_jumps;
extern int flag_cse_skip_blocks;
extern int flag_force_mem;
extern int flag_caller_saves;
extern int flag_expensive_optimizations;
extern int flag_strength_reduce;
extern int flag_schedule_insns;
extern int flag_schedule_insns_after_reload;
extern int flag_delayed_branch;
extern int g_switch_set;
extern int g_switch_value;
extern int rtx_equal_function_value_matters;
extern char regs_ever_live[];
extern rtx_def* current_function_epilogue_delay_list;
extern rtx_def* pc_rtx;
extern rtx_def* cc0_rtx;
extern char* (*decl_printable_name)(void*, char**);
extern int* reg_n_refs;
extern short* reg_n_sets;
extern short* reg_n_deaths;
extern int* reg_live_length;
extern int* reg_n_calls_crossed;
extern rtx_def** basic_block_live_at_start;
extern int regset_size;
extern int regset_bytes;
extern int frame_offset;
extern int current_function_outgoing_args_size;
extern int virtuals_instantiated;
extern int immediate_size_expand;
extern rtx_def* return_label;
extern rtx_def* virtual_incoming_args_rtx;
extern rtx_def* virtual_stack_vars_rtx;
extern rtx_def* virtual_stack_dynamic_rtx;
extern rtx_def* virtual_outgoing_args_rtx;
extern void* void_type_node;
extern void* integer_type_node;
extern void* unsigned_type_node;
extern void* short_integer_type_node;
extern void* short_unsigned_type_node;
extern void* signed_char_type_node;
extern void* unsigned_char_type_node;
extern void* char_type_node;
extern void* long_integer_type_node;
extern void* long_unsigned_type_node;
extern void* long_long_integer_type_node;
extern void* long_long_unsigned_type_node;
extern void* float_type_node;
extern void* double_type_node;
extern void* ptr_type_node;
extern void* integer_zero_node;
extern void* integer_one_node;
extern void* error_mark_node;

// GAS 2.6 initialization symbols.
void hex_init();
void bfd_init();
int md_parse_option(int c, char* arg);
void mips_init_after_args();
void output_file_create(char* name);
void subsegs_begin();
void frag_init();
void read_begin();
void expr_begin();
void md_begin();
void symbol_begin();
void* subseg_new(const char* name, int subseg);
void subseg_set(void* seg, int subseg);
extern void* gas_text_section;
extern void* gas_data_section;
extern void* bss_section;
extern int target_big_endian;

}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace rom_nom_nom::gcc_harness {
namespace {

constexpr int kFirstPseudoRegister = 67;
constexpr int kBlockCode = 5;
constexpr int kVoidTypeCode = 6;
constexpr int kIntegerTypeCode = 7;
constexpr int kEnumeralTypeCode = 10;
constexpr int kFunctionDeclCode = 29;
constexpr int kLabelDeclCode = 30;
constexpr int kVarDeclCode = 33;
constexpr int kParmDeclCode = 34;
constexpr int kResultDeclCode = 35;
constexpr int kModifyExprCode = 45;
constexpr int kPlusExprCode = 56;
constexpr int kMinusExprCode = 57;
constexpr int kMultExprCode = 58;
constexpr int kTruncDivExprCode = 59;
constexpr int kTruncModExprCode = 63;
constexpr int kNegateExprCode = 75;
constexpr int kLshiftExprCode = 80;
constexpr int kRshiftExprCode = 81;
constexpr int kBitIorExprCode = 84;
constexpr int kBitXorExprCode = 85;
constexpr int kBitAndExprCode = 86;
constexpr int kBitNotExprCode = 88;
constexpr int kTruthAndifExprCode = 89;
constexpr int kTruthOrifExprCode = 90;
constexpr int kTruthNotExprCode = 94;
constexpr int kLtExprCode = 95;
constexpr int kLeExprCode = 96;
constexpr int kGtExprCode = 97;
constexpr int kGeExprCode = 98;
constexpr int kEqExprCode = 99;
constexpr int kNeExprCode = 100;
constexpr int kNopExprCode = 106;
constexpr int kAddrExprCode = 110;
constexpr int kPredecrementExprCode = 117;
constexpr int kPreincrementExprCode = 118;
constexpr int kPostdecrementExprCode = 119;
constexpr int kPostincrementExprCode = 120;

int TreeCodeOf(const void* node) {
  if (node == nullptr) {
    return 0;
  }
  return static_cast<int>(*reinterpret_cast<const uint8_t*>(static_cast<const char*>(node) + 16));
}

void* TreeTypeOf(const void* node) {
  if (node == nullptr) {
    return nullptr;
  }
  return *reinterpret_cast<void* const*>(static_cast<const char*>(node) + 8);
}

void SetTreeType(void* node, void* type) {
  *reinterpret_cast<void**>(static_cast<char*>(node) + 8) = type;
}

void SetTreeChain(void* node, void* chain) {
  *reinterpret_cast<void**>(static_cast<char*>(node) + 0) = chain;
}

void SetTreeAddressable(void* node, bool val) {
  auto* flags = reinterpret_cast<uint32_t*>(static_cast<char*>(node) + 16);
  if (val) {
    *flags |= (1u << 11);
  } else {
    *flags &= ~(1u << 11);
  }
}

void SetTreeSideEffects(void* node, bool val) {
  auto* flags = reinterpret_cast<uint32_t*>(static_cast<char*>(node) + 16);
  if (val) {
    *flags |= (1u << 8);
  } else {
    *flags &= ~(1u << 8);
  }
}

void SetTreePublic(void* node, bool val) {
  auto* flags = reinterpret_cast<uint32_t*>(static_cast<char*>(node) + 16);
  if (val) {
    *flags |= (1u << 19);
  } else {
    *flags &= ~(1u << 19);
  }
}

void SetDeclExternal(void* decl, bool val) {
  auto* flags = reinterpret_cast<uint32_t*>(static_cast<char*>(decl) + 52);
  if (val) {
    *flags |= (1u << 8);
  } else {
    *flags &= ~(1u << 8);
  }
}

void SetDeclRegister(void* decl, bool val) {
  auto* flags = reinterpret_cast<uint32_t*>(static_cast<char*>(decl) + 52);
  if (val) {
    *flags |= (1u << 10);
  } else {
    *flags &= ~(1u << 10);
  }
}

int TypePrecisionOf(const void* type_node) {
  if (type_node == nullptr) {
    return 0;
  }
  return static_cast<int>(
      *reinterpret_cast<const uint8_t*>(static_cast<const char*>(type_node) + 52));
}

void ResetVirtualRegisterFlags() {
  for (Rtx vreg : {virtual_incoming_args_rtx, virtual_stack_vars_rtx, virtual_stack_dynamic_rtx,
                   virtual_outgoing_args_rtx}) {
    if (vreg != nullptr) {
      vreg->mode = kSiMode;
      vreg->volatil = 0;
      vreg->integrated = 0;
      vreg->unchanging = 0;
      vreg->in_struct = 0;
    }
  }
}

template <typename T>
T* AllocSidebandArray(size_t count) {
  static std::vector<void*> sideband_arena;
  const size_t bytes = std::max<size_t>(1, count) * sizeof(T);
  void* ptr = std::calloc(1, bytes);
  sideband_arena.push_back(ptr);
  return static_cast<T*>(ptr);
}

char* CopyStringToRtlObstack(std::string_view str) {
  GccObstack* ob = rtl_obstack;
  const int len = static_cast<int>(str.size()) + 1;
  if (ob->chunk_limit - ob->next_free < len) {
    _obstack_newchunk(ob, len);
  }
  std::memcpy(ob->next_free, str.data(), str.size());
  ob->next_free[str.size()] = '\0';
  char* result = ob->object_base;
  ob->next_free += len;
  ob->object_base = ob->next_free;
  return result;
}

Rtx FindCodeLabelInRtx(Rtx x) {
  if (x == nullptr) {
    return nullptr;
  }
  const int code = x->code;
  if (code == kLabelRef) {
    return x->fld[0].rtx;
  }
  const char* fmt = rtx_format[code];
  const int len = rtx_length[code];
  for (int i = 0; i < len; ++i) {
    if (fmt[i] == 'e') {
      if (Rtx found = FindCodeLabelInRtx(x->fld[i].rtx); found != nullptr) {
        return found;
      }
    } else if (fmt[i] == 'E' && x->fld[i].rtvec != nullptr) {
      for (int j = 0; j < x->fld[i].rtvec->num_elem; ++j) {
        if (Rtx found = FindCodeLabelInRtx(x->fld[i].rtvec->elem[j].rtx); found != nullptr) {
          return found;
        }
      }
    }
  }
  return nullptr;
}

}  // namespace

void GccContext::EnsureGlobalInit() {
  static std::once_flag init_once;
  std::call_once(init_once, []() {
    optimize = 2;
    obey_regdecls = 0;
    flag_signed_char = 1;
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
    for (const auto& opt : f_options) {
      if (opt.string != nullptr && std::strcmp(opt.string, "rerun-cse-after-loop") == 0 &&
          opt.variable != nullptr) {
        *opt.variable = 1;
      }
    }
    target_flags = 0;
    set_target_switch(const_cast<char*>(""));
    g_switch_set = 1;
    g_switch_value = 0;
    rtx_equal_function_value_matters = 1;
    decl_printable_name = +[](void* decl, char** /*kind*/) -> char* {
      if (decl != nullptr) {
        void* id = *reinterpret_cast<void**>(static_cast<char*>(decl) + 56);
        if (id != nullptr) {
          char* name = *reinterpret_cast<char**>(static_cast<char*>(id) + 32);
          if (name != nullptr) {
            return name;
          }
        }
      }
      return const_cast<char*>("harness_func");
    };

    asm_out_file = std::fopen("/dev/null", "w");

    init_reg_sets();
    override_options();
    init_obstacks();
    init_tree_codes();
    init_lex();
    init_rtl();
    init_emit_once(0);
    init_regs();
    init_decl_processing();
    init_optabs();
    init_stmt();
    init_expmed();
    init_expr_once();
    init_loop();
    init_reload();
    init_caller_save();
    init_recog();
    init_final(const_cast<char*>("harness.c"));

    // Initialize Binutils 2.6 GAS for in-process MIPS assembly (`-mips3 -mcpu=vr4300 -EB -G 0`).
    target_big_endian = 1;
    md_parse_option(173, nullptr);
    md_parse_option(175, const_cast<char*>("vr4300"));
    md_parse_option(181, nullptr);
    md_parse_option('G', const_cast<char*>("0"));
    mips_init_after_args();
    hex_init();
    bfd_init();
    symbol_begin();
    frag_init();
    subsegs_begin();
    read_begin();
    expr_begin();
    output_file_create(const_cast<char*>("/dev/null"));
    gas_text_section = subseg_new(".text", 0);
    gas_data_section = subseg_new(".data", 0);
    bss_section = subseg_new(".bss", 0);
    subseg_set(gas_text_section, 0);
    md_begin();
  });
}

GccContext::GccContext() {
  EnsureGlobalInit();
  BeginFunction("harness_func");
}

GccContext::~GccContext() = default;

void GccContext::BeginFunction(std::string_view func_name) {
  current_func_name_ = std::string(func_name);
  label_map_.clear();
  decl_map_.clear();
  c_label_decl_map_.clear();
  ResetVirtualRegisterFlags();

  void* fn_decl = make_node(kFunctionDeclCode);
  void* id_node = get_identifier(current_func_name_.c_str());
  void* res_decl = make_node(kResultDeclCode);
  void* blk_node = make_node(kBlockCode);
  void* void_type = void_type_node != nullptr ? void_type_node : make_node(kVoidTypeCode);
  void* fn_type = build_function_type(void_type, nullptr);

  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 8) = fn_type;
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 56) = id_node;
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 80) = res_decl;
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 88) = blk_node;
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 104) = id_node;
  *reinterpret_cast<void**>(static_cast<char*>(res_decl) + 8) = void_type;

  Rtx fn_mem = gen_rtx(kMem, kSiMode,
                       gen_rtx(kSymbolRef, kSiMode, CopyStringToRtlObstack(current_func_name_)));
  *reinterpret_cast<Rtx*>(static_cast<char*>(fn_decl) + 128) = fn_mem;

  current_function_decl = fn_decl;
  reg_renumber = nullptr;
  reg_n_refs = nullptr;
  reg_n_deaths = nullptr;
  reg_live_length = nullptr;
  reg_n_calls_crossed = nullptr;
  basic_block_live_at_start = nullptr;
  n_basic_blocks = 0;
  regset_size = 0;
  regset_bytes = 0;
  reload_completed = 0;
  immediate_size_expand = 0;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  current_function_epilogue_delay_list = nullptr;
  init_function_start(fn_decl, const_cast<char*>("harness.c"), 1);
  reg_scan(nullptr, max_reg_num(), 0);
  rtx_equal_function_value_matters = 1;
  emit_note(nullptr, kNoteInsnFunctionBeg);
  compute_frame_size(0);
}

void* GccContext::BuildCTypeTree(const CType& c_type) {
  std::string_view base = absl::StripAsciiWhitespace(c_type.name);
  void* t = integer_type_node;
  if (base == "void") {
    t = void_type_node;
  } else if (base == "s8" || base == "signed char" || base == "char" || base == "int8_t") {
    t = signed_char_type_node;
  } else if (base == "u8" || base == "unsigned char" || base == "uint8_t") {
    t = unsigned_char_type_node;
  } else if (base == "s16" || base == "short" || base == "short int" || base == "signed short" ||
             base == "int16_t") {
    t = short_integer_type_node;
  } else if (base == "u16" || base == "unsigned short" || base == "short unsigned int" ||
             base == "uint16_t") {
    t = short_unsigned_type_node;
  } else if (base == "s32" || base == "int" || base == "signed int" || base == "int32_t") {
    t = integer_type_node;
  } else if (base == "u32" || base == "unsigned int" || base == "unsigned" || base == "uint32_t") {
    t = unsigned_type_node;
  } else if (base == "long" || base == "long int" || base == "signed long") {
    t = long_integer_type_node;
  } else if (base == "unsigned long" || base == "long unsigned int") {
    t = long_unsigned_type_node;
  } else if (base == "s64" || base == "long long" || base == "long long int" || base == "int64_t") {
    t = long_long_integer_type_node;
  } else if (base == "u64" || base == "unsigned long long" || base == "long long unsigned int" ||
             base == "uint64_t") {
    t = long_long_unsigned_type_node;
  } else if (base == "f32" || base == "float") {
    t = float_type_node;
  } else if (base == "f64" || base == "double") {
    t = double_type_node;
  }
  if (c_type.is_const) {
    t = c_build_type_variant(t, 1, 0);
  }
  for (int i = 0; i < c_type.pointer_depth; ++i) {
    t = build_pointer_type(t);
  }
  return t;
}

void* GccContext::DeclareExternalFunction(std::string_view name, const CType& return_type) {
  std::string key(name);
  void* ret_type = BuildCTypeTree(return_type);
  void* fn_type = build_function_type(ret_type, nullptr);
  void* id_node = get_identifier(key.c_str());
  void* fn_decl = build_decl(kFunctionDeclCode, id_node, fn_type);
  SetDeclExternal(fn_decl, true);
  SetTreePublic(fn_decl, true);
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 104) = id_node;
  Rtx fn_mem = gen_rtx(kMem, kSiMode, gen_rtx(kSymbolRef, kSiMode, CopyStringToRtlObstack(key)));
  *reinterpret_cast<Rtx*>(static_cast<char*>(fn_decl) + 128) = fn_mem;
  decl_map_[key] = fn_decl;
  return fn_decl;
}

void* GccContext::BuildVarDeclTree(const VariableDeclarationStatement& var_decl) {
  void* var_type = BuildCTypeTree(var_decl.Type());
  if (var_decl.ArraySize().has_value() && *var_decl.ArraySize() > 0) {
    void* max_idx = build_int_2_wide(static_cast<int>(*var_decl.ArraySize() - 1), 0);
    void* idx_type = build_index_type(max_idx);
    var_type = build_array_type(var_type, idx_type);
    layout_type(var_type);
  }
  void* id_node = get_identifier(var_decl.Name().c_str());
  void* decl = build_decl(kVarDeclCode, id_node, var_type);
  *reinterpret_cast<void**>(static_cast<char*>(decl) + 64) = current_function_decl;
  if (var_decl.RegisterAnnotation().has_value()) {
    SetDeclRegister(decl, true);
  }
  layout_decl(decl, 0);
  if (var_decl.Initializer() != nullptr) {
    void* init_tree = BuildCExprTree(*var_decl.Initializer());
    if (init_tree != nullptr && TreeTypeOf(init_tree) != var_type) {
      init_tree = convert(var_type, init_tree);
    }
    *reinterpret_cast<void**>(static_cast<char*>(decl) + 88) = init_tree;
  }
  decl_map_[var_decl.Name()] = decl;
  return decl;
}

void* GccContext::BeginFunctionFromAst(const FunctionDeclaration& func_decl,
                                       bool run_expand_function_start, bool declare_body_locals) {
  current_func_name_ = func_decl.Name().empty() ? "harness_func" : func_decl.Name();
  label_map_.clear();
  decl_map_.clear();
  c_label_decl_map_.clear();
  ResetVirtualRegisterFlags();

  void* ret_type = BuildCTypeTree(func_decl.ReturnType());
  void* arg_types = tree_cons(nullptr, void_type_node, nullptr);
  for (auto it = func_decl.Parameters().rbegin(); it != func_decl.Parameters().rend(); ++it) {
    arg_types = tree_cons(nullptr, BuildCTypeTree(it->type), arg_types);
  }
  void* fn_type = build_function_type(ret_type, arg_types);
  void* id_node = get_identifier(current_func_name_.c_str());
  void* fn_decl = build_decl(kFunctionDeclCode, id_node, fn_type);
  SetTreePublic(fn_decl, !func_decl.IsStatic());

  void* res_decl = build_decl(kResultDeclCode, nullptr, ret_type);
  *reinterpret_cast<void**>(static_cast<char*>(res_decl) + 64) = fn_decl;
  layout_decl(res_decl, 0);

  void* blk_node = make_node(kBlockCode);
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 80) = res_decl;
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 88) = blk_node;
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 104) = id_node;

  Rtx fn_mem = gen_rtx(kMem, kSiMode,
                       gen_rtx(kSymbolRef, kSiMode, CopyStringToRtlObstack(current_func_name_)));
  *reinterpret_cast<Rtx*>(static_cast<char*>(fn_decl) + 128) = fn_mem;

  void* parm_head = nullptr;
  void* parm_tail = nullptr;
  for (size_t i = 0; i < func_decl.Parameters().size(); ++i) {
    const CParameter& p = func_decl.Parameters()[i];
    std::string p_name = p.name.empty() ? absl::StrCat("arg", i) : p.name;
    void* p_type = BuildCTypeTree(p.type);
    void* p_id = get_identifier(p_name.c_str());
    void* parm = build_decl(kParmDeclCode, p_id, p_type);
    *reinterpret_cast<void**>(static_cast<char*>(parm) + 64) = fn_decl;
    layout_decl(parm, 0);

    // Set `DECL_ARG_TYPE(parm)` (`decl.initial` at offset 88), promoting subword
    // integer types (`PROMOTE_PROTOTYPES`) to `integer_type_node` as in `c-decl.c`.
    void* arg_type = p_type;
    const int p_code = TreeCodeOf(p_type);
    if ((p_code == kIntegerTypeCode || p_code == kEnumeralTypeCode) &&
        TypePrecisionOf(p_type) < TypePrecisionOf(integer_type_node)) {
      arg_type = integer_type_node;
    }
    *reinterpret_cast<void**>(static_cast<char*>(parm) + 88) = arg_type;

    if (parm_head == nullptr) {
      parm_head = parm;
    } else {
      SetTreeChain(parm_tail, parm);
    }
    parm_tail = parm;
    decl_map_[p_name] = parm;
  }
  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 72) = parm_head;
  decl_map_[current_func_name_] = fn_decl;

  current_function_decl = fn_decl;
  reg_renumber = nullptr;
  reg_n_refs = nullptr;
  reg_n_deaths = nullptr;
  reg_live_length = nullptr;
  reg_n_calls_crossed = nullptr;
  basic_block_live_at_start = nullptr;
  n_basic_blocks = 0;
  regset_size = 0;
  regset_bytes = 0;
  reload_completed = 0;
  immediate_size_expand = 0;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  current_function_epilogue_delay_list = nullptr;

  init_function_start(fn_decl, const_cast<char*>("gcc_harness.c"), 1);
  if (run_expand_function_start) {
    expand_function_start(fn_decl, 0);
  } else {
    emit_note(nullptr, kNoteInsnFunctionBeg);
  }
  expand_start_bindings(0);

  if (declare_body_locals && &func_decl.Body() != nullptr) {
    for (const auto& stmt : func_decl.Body().Statements()) {
      if (stmt != nullptr && stmt->Kind() == CStatementKind::kVariableDeclarationStatement) {
        const auto& vdecl = static_cast<const VariableDeclarationStatement&>(*stmt);
        void* d = BuildVarDeclTree(vdecl);
        expand_decl(d);
      }
    }
  }
  return fn_decl;
}

void* GccContext::BuildCExprTree(const CExpression& expr) {
  switch (expr.Kind()) {
    case CExpressionKind::kIntegerLiteral: {
      const auto& lit = static_cast<const IntegerLiteralExpression&>(expr);
      const uint64_t uval = static_cast<uint64_t>(lit.Value());
      const int low = static_cast<int>(static_cast<uint32_t>(uval & 0xFFFFFFFFULL));
      const int high = static_cast<int>(static_cast<uint32_t>(uval >> 32));
      void* cst = build_int_2_wide(low, high);
      SetTreeType(cst, lit.IsUnsigned() ? unsigned_type_node : integer_type_node);
      return cst;
    }
    case CExpressionKind::kFloatLiteral: {
      const auto& flit = static_cast<const FloatLiteralExpression&>(expr);
      void* ftype = flit.IsFloat() ? float_type_node : double_type_node;
      return build_real(ftype, flit.Value());
    }
    case CExpressionKind::kStringLiteral: {
      const auto& slit = static_cast<const StringLiteralExpression&>(expr);
      void* s = build_string(static_cast<int>(slit.Value().size()) + 1, slit.Value().c_str());
      void* idx_t = build_index_type(build_int_2_wide(static_cast<int>(slit.Value().size()), 0));
      SetTreeType(s, build_array_type(char_type_node, idx_t));
      return default_conversion(s);
    }
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      auto it = decl_map_.find(id.Name());
      if (it != decl_map_.end()) {
        return it->second;
      }
      void* sym_type =
          id.SymbolType().has_value() ? BuildCTypeTree(*id.SymbolType()) : integer_type_node;
      if (id.IsArraySymbol()) {
        void* idx_t = build_index_type(build_int_2_wide(255, 0));
        sym_type = build_array_type(sym_type, idx_t);
        layout_type(sym_type);
      }
      void* id_node = get_identifier(id.Name().c_str());
      void* ext_decl = build_decl(kVarDeclCode, id_node, sym_type);
      SetDeclExternal(ext_decl, true);
      SetTreePublic(ext_decl, true);
      *reinterpret_cast<void**>(static_cast<char*>(ext_decl) + 104) = id_node;
      layout_decl(ext_decl, 0);
      make_decl_rtl(ext_decl, nullptr, 1);
      decl_map_[id.Name()] = ext_decl;
      return ext_decl;
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      void* op = BuildCExprTree(un.Operand());
      const std::string& op_str = un.Op();
      if (op_str == "-") {
        return fold(build_unary_op(kNegateExprCode, op, 0));
      }
      if (op_str == "~") {
        return fold(build_unary_op(kBitNotExprCode, op, 0));
      }
      if (op_str == "!") {
        return fold(build_unary_op(kTruthNotExprCode, op, 0));
      }
      if (op_str == "*") {
        return build_indirect_ref(op, "unary *");
      }
      if (op_str == "&") {
        SetTreeAddressable(op, true);
        return build1(kAddrExprCode, build_pointer_type(TreeTypeOf(op)), op);
      }
      if (op_str == "++") {
        return build_unary_op(un.IsPrefix() ? kPreincrementExprCode : kPostincrementExprCode, op,
                              0);
      }
      if (op_str == "--") {
        return build_unary_op(un.IsPrefix() ? kPredecrementExprCode : kPostdecrementExprCode, op,
                              0);
      }
      return op;
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      void* lhs = BuildCExprTree(bin.Lhs());
      void* rhs = BuildCExprTree(bin.Rhs());
      const std::string& op = bin.Op();
      int code = kPlusExprCode;
      if (op == "+") {
        code = kPlusExprCode;
      } else if (op == "-") {
        code = kMinusExprCode;
      } else if (op == "*") {
        code = kMultExprCode;
      } else if (op == "/") {
        code = kTruncDivExprCode;
      } else if (op == "%") {
        code = kTruncModExprCode;
      } else if (op == "<<") {
        code = kLshiftExprCode;
      } else if (op == ">>") {
        code = kRshiftExprCode;
      } else if (op == "&") {
        code = kBitAndExprCode;
      } else if (op == "|") {
        code = kBitIorExprCode;
      } else if (op == "^") {
        code = kBitXorExprCode;
      } else if (op == "&&") {
        code = kTruthAndifExprCode;
      } else if (op == "||") {
        code = kTruthOrifExprCode;
      } else if (op == "==") {
        code = kEqExprCode;
      } else if (op == "!=") {
        code = kNeExprCode;
      } else if (op == "<") {
        code = kLtExprCode;
      } else if (op == "<=") {
        code = kLeExprCode;
      } else if (op == ">") {
        code = kGtExprCode;
      } else if (op == ">=") {
        code = kGeExprCode;
      } else if (op == ",") {
        return build_compound_expr(tree_cons(nullptr, lhs, tree_cons(nullptr, rhs, nullptr)));
      }
      return fold(build_binary_op(code, lhs, rhs, 1));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& asgn = static_cast<const AssignmentExpression&>(expr);
      void* lhs = BuildCExprTree(asgn.Lhs());
      void* rhs = BuildCExprTree(asgn.Rhs());
      const std::string& op = asgn.Op();
      int modcode = kNopExprCode;
      if (op == "+=") {
        modcode = kPlusExprCode;
      } else if (op == "-=") {
        modcode = kMinusExprCode;
      } else if (op == "*=") {
        modcode = kMultExprCode;
      } else if (op == "/=") {
        modcode = kTruncDivExprCode;
      } else if (op == "%=") {
        modcode = kTruncModExprCode;
      } else if (op == "<<=") {
        modcode = kLshiftExprCode;
      } else if (op == ">>=") {
        modcode = kRshiftExprCode;
      } else if (op == "&=") {
        modcode = kBitAndExprCode;
      } else if (op == "|=") {
        modcode = kBitIorExprCode;
      } else if (op == "^=") {
        modcode = kBitXorExprCode;
      }
      void* res = build_modify_expr(lhs, modcode, rhs);
      SetTreeSideEffects(res, true);
      return res;
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      void* fn_tree = nullptr;
      if (call.Callee().Kind() == CExpressionKind::kIdentifier) {
        const auto& callee_id = static_cast<const IdentifierExpression&>(call.Callee());
        auto it = decl_map_.find(callee_id.Name());
        if (it != decl_map_.end() && TreeCodeOf(it->second) == kFunctionDeclCode) {
          fn_tree = it->second;
        } else {
          CType ret_t = callee_id.SymbolType().value_or(CType::S32());
          fn_tree = DeclareExternalFunction(callee_id.Name(), ret_t);
        }
      } else {
        fn_tree = BuildCExprTree(call.Callee());
      }
      void* arg_list = nullptr;
      for (const auto& arg : call.Arguments()) {
        void* a_tree = default_conversion(BuildCExprTree(*arg));
        arg_list = tree_cons(nullptr, a_tree, arg_list);
      }
      arg_list = nreverse(arg_list);
      return build_function_call(fn_tree, arg_list);
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expr);
      void* target_t = BuildCTypeTree(cast.TargetType());
      void* op = BuildCExprTree(cast.Operand());
      return build_c_cast(target_t, op);
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      void* base = BuildCExprTree(arr.Array());
      void* idx = BuildCExprTree(arr.Index());
      return build_array_ref(base, idx);
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      void* cond = BuildCExprTree(tern.Condition());
      void* t_val = BuildCExprTree(tern.TrueExpression());
      void* f_val = BuildCExprTree(tern.FalseExpression());
      return build_conditional_expr(cond, t_val, f_val);
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& mem = static_cast<const MemberAccessExpression&>(expr);
      return BuildCExprTree(mem.Object());
    }
  }
  return integer_zero_node;
}

void GccContext::ExpandCStatement(const CStatement& stmt) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      for (const auto& s : comp.Statements()) {
        if (s != nullptr) {
          ExpandCStatement(*s);
        }
      }
      break;
    }
    case CStatementKind::kExpressionStatement: {
      const auto& expr_stmt = static_cast<const ExpressionStatement&>(stmt);
      void* expr_tree = BuildCExprTree(expr_stmt.Expression());
      if (expr_tree != nullptr) {
        c_expand_expr_stmt(expr_tree);
      }
      break;
    }
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& var_decl = static_cast<const VariableDeclarationStatement&>(stmt);
      void* decl = nullptr;
      auto it = decl_map_.find(var_decl.Name());
      if (it != decl_map_.end() && TreeCodeOf(it->second) == kVarDeclCode) {
        decl = it->second;
      } else {
        decl = BuildVarDeclTree(var_decl);
        expand_decl(decl);
      }
      if (var_decl.Initializer() != nullptr) {
        void* init_tree = BuildCExprTree(*var_decl.Initializer());
        void* var_type = TreeTypeOf(decl);
        if (init_tree != nullptr && var_type != nullptr && TreeTypeOf(init_tree) != var_type) {
          init_tree = convert(var_type, init_tree);
        }
        *reinterpret_cast<void**>(static_cast<char*>(decl) + 88) = init_tree;
        expand_decl_init(decl);
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      void* cond = truthvalue_conversion(BuildCExprTree(if_stmt.Condition()));
      expand_start_cond(cond, 0);
      ExpandCStatement(if_stmt.ThenBranch());
      if (if_stmt.ElseBranch() != nullptr) {
        expand_start_else();
        ExpandCStatement(*if_stmt.ElseBranch());
      }
      expand_end_cond();
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_stmt = static_cast<const WhileStatement&>(stmt);
      emit_nop();
      expand_start_loop(1);
      void* cond = truthvalue_conversion(BuildCExprTree(while_stmt.Condition()));
      expand_exit_loop_if_false(nullptr, cond);
      ExpandCStatement(while_stmt.Body());
      expand_end_loop();
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& do_stmt = static_cast<const DoWhileStatement&>(stmt);
      emit_nop();
      expand_start_loop_continue_elsewhere(1);
      ExpandCStatement(do_stmt.Body());
      expand_loop_continue_here();
      void* cond = truthvalue_conversion(BuildCExprTree(do_stmt.Condition()));
      expand_exit_loop_if_false(nullptr, cond);
      expand_end_loop();
      break;
    }
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(stmt);
      emit_nop();
      if (for_stmt.Init() != nullptr) {
        ExpandCStatement(*for_stmt.Init());
      }
      expand_start_loop_continue_elsewhere(1);
      if (for_stmt.Condition() != nullptr) {
        void* cond = truthvalue_conversion(BuildCExprTree(*for_stmt.Condition()));
        expand_exit_loop_if_false(nullptr, cond);
      }
      ExpandCStatement(for_stmt.Body());
      expand_loop_continue_here();
      if (for_stmt.Step() != nullptr) {
        c_expand_expr_stmt(BuildCExprTree(*for_stmt.Step()));
      }
      expand_end_loop();
      break;
    }
    case CStatementKind::kReturnStatement: {
      const auto& ret_stmt = static_cast<const ReturnStatement&>(stmt);
      if (ret_stmt.ReturnValue() == nullptr) {
        c_expand_return(nullptr);
      } else {
        c_expand_return(BuildCExprTree(*ret_stmt.ReturnValue()));
      }
      break;
    }
    case CStatementKind::kBreakStatement: {
      expand_exit_something();
      break;
    }
    case CStatementKind::kContinueStatement: {
      expand_continue_loop(nullptr);
      break;
    }
    case CStatementKind::kGotoStatement: {
      const auto& goto_stmt = static_cast<const GotoStatement&>(stmt);
      void*& lbl_decl = c_label_decl_map_[goto_stmt.Label()];
      if (lbl_decl == nullptr) {
        lbl_decl =
            build_decl(kLabelDeclCode, get_identifier(goto_stmt.Label().c_str()), void_type_node);
      }
      expand_goto(lbl_decl);
      break;
    }
    case CStatementKind::kLabelStatement: {
      const auto& lbl_stmt = static_cast<const LabelStatement&>(stmt);
      void*& lbl_decl = c_label_decl_map_[lbl_stmt.Label()];
      if (lbl_decl == nullptr) {
        lbl_decl =
            build_decl(kLabelDeclCode, get_identifier(lbl_stmt.Label().c_str()), void_type_node);
      }
      expand_label(lbl_decl);
      break;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& sw_stmt = static_cast<const SwitchStatement&>(stmt);
      void* idx_expr = BuildCExprTree(sw_stmt.Condition());
      c_expand_start_case(idx_expr);
      for (const SwitchCase& sc : sw_stmt.Cases()) {
        for (int64_t val : sc.case_values) {
          void* raw_cst = build_int_2_wide(static_cast<int>(val), static_cast<int>(val >> 32));
          SetTreeType(raw_cst, integer_type_node);
          void* val_tree = check_case_value(raw_cst);
          void* label_decl = build_decl(kLabelDeclCode, nullptr, nullptr);
          void* duplicate = nullptr;
          pushcase(val_tree, convert_and_check, label_decl, &duplicate);
        }
        if (sc.is_default) {
          void* label_decl = build_decl(kLabelDeclCode, nullptr, nullptr);
          void* duplicate = nullptr;
          pushcase(nullptr, nullptr, label_decl, &duplicate);
        }
        if (sc.body != nullptr) {
          ExpandCStatement(*sc.body);
        }
      }
      expand_end_case(idx_expr);
      break;
    }
    case CStatementKind::kCaseStatement: {
      const auto& cs = static_cast<const CaseStatement&>(stmt);
      void* label_decl = build_decl(kLabelDeclCode, nullptr, nullptr);
      void* duplicate = nullptr;
      if (cs.IsDefault()) {
        pushcase(nullptr, nullptr, label_decl, &duplicate);
      } else {
        void* raw_cst =
            build_int_2_wide(static_cast<int>(cs.Value()), static_cast<int>(cs.Value() >> 32));
        SetTreeType(raw_cst, integer_type_node);
        void* val_tree = check_case_value(raw_cst);
        pushcase(val_tree, convert_and_check, label_decl, &duplicate);
      }
      break;
    }
    case CStatementKind::kCommentStatement:
      break;
  }
}

void GccContext::StartIsolatedExpandSequence() {
  start_sequence();
  emit_note(nullptr, kNoteInsnDeleted);
}

void GccContext::EndIsolatedExpandSequence() {
  end_sequence();
}

void GccContext::SyncReturnLabelFromMaterializedCfg() {
  for (const auto& [key, lbl] : label_map_) {
    if (!absl::StartsWith(key, "__blk_") && lbl != nullptr && lbl->fld[1].rtx == nullptr &&
        lbl->fld[2].rtx == nullptr) {
      return_label = lbl;
      return;
    }
  }
  if (return_label == nullptr) {
    return_label = gen_label_rtx();
  }
}

void GccContext::EnsurePseudoRegsUpTo(int min_max_regno, int default_mode) {
  while (max_reg_num() <= min_max_regno) {
    gen_reg_rtx(default_mode);
  }
}

int GccContext::MaxRegNum() const {
  return max_reg_num();
}

void GccContext::SetRegRenumber(int regno, int hard_reg) {
  if (regno < 0) {
    return;
  }
  if (regno >= kFirstPseudoRegister) {
    EnsurePseudoRegsUpTo(regno);
  }
  const int cap = std::max(max_reg_num(), regno + 1) + 16;
  if (reg_renumber == nullptr) {
    reg_renumber = AllocSidebandArray<short>(cap);
    for (int i = 0; i < cap; ++i) {
      reg_renumber[i] = -1;
    }
  }
  reg_renumber[regno] = static_cast<short>(hard_reg);
}

int GccContext::GetRegRenumber(int regno) const {
  if (reg_renumber == nullptr || regno < 0 || regno >= max_reg_num()) {
    return -1;
  }
  return reg_renumber[regno];
}

Rtx GccContext::GetOrCreateLabel(std::string_view label_token) {
  std::string key(label_token);
  auto it = label_map_.find(key);
  if (it != label_map_.end()) {
    return it->second;
  }
  Rtx lbl = gen_label_rtx();
  label_map_.emplace(std::move(key), lbl);
  return lbl;
}

Rtx GccContext::ParseRtx(std::string_view rtl_text) {
  size_t pos = 0;
  return ParseRtxImpl(rtl_text, &pos);
}

Rtx GccContext::ParseRtxImpl(std::string_view text, size_t* pos) {
  SkipRtlWhitespace(text, pos);
  if (*pos >= text.size() || text[*pos] != '(') {
    return nullptr;
  }
  ++(*pos);  // Consume '('.

  std::string_view code_name = ReadRtlToken(text, pos);
  if (code_name == "nil" || code_name == "NIL") {
    while (*pos < text.size() && text[*pos] != ')') {
      ++(*pos);
    }
    if (*pos < text.size() && text[*pos] == ')') {
      ++(*pos);
    }
    return nullptr;
  }

  int rtx_code = kUnknown;
  for (int i = 0; i < kNumRtxCode; ++i) {
    if (rtx_name[i] != nullptr && code_name == rtx_name[i]) {
      rtx_code = i;
      break;
    }
  }
  if (rtx_code == kUnknown) {
    return nullptr;
  }

  // Parse optional `/v`, `/i`, `/u`, `/s` flags immediately following code name.
  bool flag_v = false;
  bool flag_i = false;
  bool flag_u = false;
  bool flag_s = false;
  while (*pos < text.size() && text[*pos] == '/') {
    ++(*pos);
    if (*pos >= text.size()) {
      break;
    }
    char f = text[(*pos)++];
    if (f == 'v') flag_v = true;
    if (f == 'i') flag_i = true;
    if (f == 'u') flag_u = true;
    if (f == 's') flag_s = true;
  }

  // Parse optional `:mode` (either machine mode or `REG_NOTE` name).
  int mode = kVoidMode;
  if (*pos < text.size() && text[*pos] == ':') {
    ++(*pos);
    std::string_view mode_tok = ReadRtlToken(text, pos);
    bool found_mode = false;
    for (int m = 0; m < kNumMachineModes; ++m) {
      if (mode_name[m] != nullptr && mode_tok == mode_name[m]) {
        mode = m;
        found_mode = true;
        break;
      }
    }
    if (!found_mode) {
      for (int rn = 1; rn <= 15; ++rn) {
        if (reg_note_name[rn] != nullptr && mode_tok == reg_note_name[rn]) {
          mode = rn;
          break;
        }
      }
    }
  }

  Rtx rt = rtx_alloc(rtx_code);
  rt->mode = mode;
  rt->volatil = flag_v ? 1 : 0;
  rt->integrated = flag_i ? 1 : 0;
  rt->unchanging = flag_u ? 1 : 0;
  rt->in_struct = flag_s ? 1 : 0;

  const int len = rtx_length[rtx_code];
  const char* fmt = rtx_format[rtx_code];
  for (int i = 0; i < len; ++i) {
    rt->fld[i].rtx = nullptr;
  }
  if (rtx_code == kInsn || rtx_code == kJumpInsn || rtx_code == kCallInsn) {
    rt->fld[4].rtint = -1;
  }

  for (int i = 0; i < len; ++i) {
    SkipRtlWhitespace(text, pos);
    if (*pos >= text.size() || text[*pos] == ')') {
      break;
    }
    const char f = fmt[i];
    if (f == '0') {
      continue;
    }
    if (f == 'e') {
      rt->fld[i].rtx = ParseRtxImpl(text, pos);
    } else if (f == 'u') {
      if (text[*pos] == '(') {
        rt->fld[i].rtx = ParseRtxImpl(text, pos);
      } else {
        std::string_view tok = ReadRtlToken(text, pos);
        if (rtx_code == kLabelRef || (rtx_code == kReg && i == 0) ||
            (rtx_code == kExprList && mode == kRegLabel && i == 0)) {
          rt->fld[i].rtx = GetOrCreateLabel(tok);
        } else {
          rt->fld[i].rtx = nullptr;
        }
      }
    } else if (f == 'V' || f == 'E') {
      if (f == 'V' && text[*pos] != '[') {
        if (text[*pos] == '(' && absl::StartsWith(text.substr(*pos), "(nil)")) {
          ParseRtxImpl(text, pos);
        }
        rt->fld[i].rtvec = nullptr;
        continue;
      }
      if (text[*pos] == '[') {
        ++(*pos);
        std::vector<Rtx> elems;
        while (true) {
          SkipRtlWhitespace(text, pos);
          if (*pos >= text.size() || text[*pos] == ']') {
            if (*pos < text.size() && text[*pos] == ']') {
              ++(*pos);
            }
            break;
          }
          elems.push_back(ParseRtxImpl(text, pos));
        }
        if (!elems.empty()) {
          Rtvec vec = rtvec_alloc(static_cast<int>(elems.size()));
          for (size_t j = 0; j < elems.size(); ++j) {
            vec->elem[j].rtx = elems[j];
          }
          rt->fld[i].rtvec = vec;
        }
      }
    } else if (f == 'S' || f == 's') {
      if (f == 'S' && text[*pos] != '"' && text[*pos] != '(') {
        rt->fld[i].rtstr = nullptr;
        continue;
      }
      if (text[*pos] == '(' && absl::StartsWith(text.substr(*pos), "(nil)")) {
        ParseRtxImpl(text, pos);
        rt->fld[i].rtstr = nullptr;
        continue;
      }
      bool saw_paren = false;
      if (text[*pos] == '(') {
        saw_paren = true;
        ++(*pos);
        SkipRtlWhitespace(text, pos);
      }
      if (*pos < text.size() && text[*pos] == '"') {
        ++(*pos);
        std::string buf;
        while (*pos < text.size() && text[*pos] != '"') {
          if (text[*pos] == '\\' && *pos + 1 < text.size()) {
            ++(*pos);
          }
          buf.push_back(text[(*pos)++]);
        }
        if (*pos < text.size() && text[*pos] == '"') {
          ++(*pos);
        }
        rt->fld[i].rtstr = CopyStringToRtlObstack(buf);
      } else {
        std::string_view tok = ReadRtlToken(text, pos);
        rt->fld[i].rtstr = CopyStringToRtlObstack(tok);
      }
      if (saw_paren) {
        SkipRtlWhitespace(text, pos);
        if (*pos < text.size() && text[*pos] == ')') {
          ++(*pos);
        }
      }
    } else if (f == 'w' || f == 'i' || f == 'n') {
      if (text[*pos] == '{') {
        while (*pos < text.size() && text[*pos] != '}') {
          ++(*pos);
        }
        if (*pos < text.size() && text[*pos] == '}') {
          ++(*pos);
        }
        SkipRtlWhitespace(text, pos);
      }
      std::string_view tok = ReadRtlToken(text, pos);
      int val = 0;
      if (absl::StartsWith(tok, "NOTE_INSN_")) {
        for (int n = 1; n <= 13; ++n) {
          if (note_insn_name[n] != nullptr && tok == note_insn_name[n]) {
            val = -n;
            break;
          }
        }
      } else if (int64_t parsed64 = 0; absl::SimpleAtoi(tok, &parsed64)) {
        val = static_cast<int>(static_cast<int32_t>(parsed64));
      }
      rt->fld[i].rtint = val;
      if (f == 'n') {
        SkipRtlWhitespace(text, pos);
        if (*pos < text.size() && text[*pos] == '[') {
          while (*pos < text.size() && text[*pos] != ']') {
            ++(*pos);
          }
          if (*pos < text.size() && text[*pos] == ']') {
            ++(*pos);
          }
        }
      }
    }
  }

  // Skip any trailing tokens inside this S-expression (such as `{insn_name}`
  // or `[num_refs]` on `code_label`) up to the matching ')'.
  int extra_depth = 0;
  bool in_quotes = false;
  while (*pos < text.size()) {
    char c = text[*pos];
    if (in_quotes) {
      if (c == '\\' && *pos + 1 < text.size()) {
        ++(*pos);
      } else if (c == '"') {
        in_quotes = false;
      }
      ++(*pos);
      continue;
    }
    if (c == '"') {
      in_quotes = true;
      ++(*pos);
    } else if (c == '(') {
      ++extra_depth;
      ++(*pos);
    } else if (c == ')') {
      if (extra_depth == 0) {
        ++(*pos);
        break;
      }
      --extra_depth;
      ++(*pos);
    } else {
      ++(*pos);
    }
  }

  if (rtx_code == kLabelRef && rt->fld[0].rtx == nullptr) {
    rt->fld[0].rtx = GetOrCreateLabel("__anon_lbl");
  }

  // Canonicalize `(reg:M regno)` while preserving `/v`, `/i`, `/u`, `/s` flags.
  if (rtx_code == kReg) {
    const int regno = rt->fld[0].rtint;
    if (regno >= kFirstPseudoRegister) {
      EnsurePseudoRegsUpTo(regno, mode);
      Rtx canon = regno_reg_rtx[regno];
      if (canon != nullptr) {
        canon->mode = mode;
        if (flag_v) canon->volatil = 1;
        if (flag_i) canon->integrated = 1;
        if (flag_u) canon->unchanging = 1;
        if (flag_s) canon->in_struct = 1;
        return canon;
      }
    } else if (!flag_v && !flag_i && !flag_u && !flag_s) {
      return gen_rtx(kReg, mode, regno);
    }
  } else if (rtx_code == kConstInt) {
    return gen_rtx(kConstInt, kVoidMode, rt->fld[0].rtint);
  } else if (rtx_code == kPc && pc_rtx != nullptr) {
    return pc_rtx;
  } else if (rtx_code == kCc0 && cc0_rtx != nullptr) {
    return cc0_rtx;
  }

  return rt;
}

std::string GccContext::FormatRtx(Rtx rtx) const {
  if (rtx == nullptr) {
    return "(nil)";
  }
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  print_rtl(mem_fp, rtx);
  std::fclose(mem_fp);
  std::string result =
      CanonicalizeRtlPattern(buf != nullptr ? std::string_view(buf, len) : std::string_view());
  std::free(buf);
  return result;
}

std::string GccContext::DumpInsnChain(Rtx first_insn) const {
  Rtx head = first_insn != nullptr ? first_insn : get_insns();
  if (head == nullptr) {
    return "";
  }
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  print_rtl(mem_fp, head);
  std::fclose(mem_fp);
  std::string result(buf != nullptr ? buf : "", len);
  std::free(buf);
  return result;
}

Rtx GccContext::BuildInsnFromContainerOrPattern(std::string_view item_text, bool emit_to_chain) {
  std::string_view s = absl::StripAsciiWhitespace(item_text);
  if (s.empty()) {
    return nullptr;
  }

  // Case 1: `(note NOTE_INSN_...)`
  if (absl::StartsWith(s, "(note ") || absl::StartsWith(s, "(note\t")) {
    for (int n = 1; n <= 13; ++n) {
      if (note_insn_name[n] != nullptr && absl::StrContains(s, note_insn_name[n])) {
        if (emit_to_chain) {
          return emit_note(nullptr, -n);
        }
      }
    }
    return nullptr;
  }

  // Case 2: `(barrier ...)`
  if (absl::StartsWith(s, "(barrier")) {
    return emit_to_chain ? emit_barrier() : nullptr;
  }

  // Case 3: `(code_label ...)`
  if (absl::StartsWith(s, "(code_label")) {
    size_t pos = std::string_view("(code_label").size();
    std::string_view tok = ReadRtlToken(s, &pos);
    Rtx lbl = GetOrCreateLabel(tok);
    return emit_to_chain ? emit_label(lbl) : lbl;
  }

  // Case 4: `(sequence [ ... ])`
  if (absl::StartsWith(s, "(sequence")) {
    size_t bracket = s.find('[');
    if (bracket == std::string_view::npos) {
      return nullptr;
    }
    std::vector<Rtx> sub_insns;
    for (std::string_view elem : ExtractBracketedSexprElements(s, bracket)) {
      Rtx sub = BuildInsnFromContainerOrPattern(elem, /*emit_to_chain=*/false);
      if (sub != nullptr) {
        sub_insns.push_back(sub);
      }
    }
    Rtvec vec = rtvec_alloc(static_cast<int>(sub_insns.size()));
    for (size_t j = 0; j < sub_insns.size(); ++j) {
      sub_insns[j]->fld[1].rtx = (j > 0) ? sub_insns[j - 1] : nullptr;
      sub_insns[j]->fld[2].rtx = (j + 1 < sub_insns.size()) ? sub_insns[j + 1] : nullptr;
      vec->elem[j].rtx = sub_insns[j];
    }
    Rtx seq = gen_rtx(kSequence, kVoidMode, vec);
    Rtx wrapper = make_insn_raw(seq);
    if (emit_to_chain) {
      add_insn(wrapper);
    }
    return wrapper;
  }

  // Case 5: Container `(insn[/s][/u] ...)` / `(jump_insn[/u] ...)` / `(call_insn ...)`
  bool is_container = false;
  int container_code = kInsn;
  bool flag_v = false;
  bool flag_i = false;
  bool flag_u = false;
  bool flag_s = false;
  std::string_view body_and_notes = s;

  if (s.front() == '(') {
    size_t pos = 1;
    std::string_view tag = ReadRtlToken(s, &pos);
    if (tag == "insn" || tag == "jump_insn" || tag == "call_insn") {
      is_container = true;
      if (tag == "jump_insn") container_code = kJumpInsn;
      if (tag == "call_insn") container_code = kCallInsn;
      while (pos < s.size() && s[pos] == '/') {
        ++pos;
        if (pos < s.size()) {
          char f = s[pos++];
          if (f == 'v') flag_v = true;
          if (f == 'i') flag_i = true;
          if (f == 'u') flag_u = true;
          if (f == 's') flag_s = true;
        }
      }
      if (pos < s.size() && s[pos] == ':') {
        ++pos;
        ReadRtlToken(s, &pos);
      }
      // Skip optional numeric UID tokens (`uid prev next`) if present.
      for (int k = 0; k < 3; ++k) {
        SkipRtlWhitespace(s, &pos);
        if (pos < s.size() && (absl::ascii_isdigit(s[pos]) || s[pos] == '-')) {
          ReadRtlToken(s, &pos);
        } else {
          break;
        }
      }
      SkipRtlWhitespace(s, &pos);
      if (pos < s.size() && s.back() == ')') {
        body_and_notes = absl::StripAsciiWhitespace(s.substr(pos, s.size() - pos - 1));
      }
    }
  }

  auto [pat_str, notes_str] = SplitPatternAndTrailingRegNotes(body_and_notes);
  Rtx pat = ParseRtx(pat_str);
  if (pat == nullptr) {
    return nullptr;
  }
  // If `ParseRtx` was handed a full `(insn UID PREV NEXT ...)` where `pat` itself
  // is already an `INSN`/`JUMP_INSN`/`CALL_INSN`, never double-wrap it.
  if (pat->code == kInsn || pat->code == kJumpInsn || pat->code == kCallInsn) {
    if (emit_to_chain) {
      add_insn(pat);
    }
    return pat;
  }

  if (!is_container) {
    if (IsJumpPatternText(pat_str)) {
      container_code = kJumpInsn;
    } else if (IsCallPatternText(pat_str)) {
      container_code = kCallInsn;
    } else {
      container_code = kInsn;
    }
  }

  Rtx insn = nullptr;
  if (container_code == kJumpInsn) {
    if (emit_to_chain) {
      insn = emit_jump_insn(pat);
    } else {
      insn = make_insn_raw(pat);
      // Allocate a full 8-field `JUMP_INSN` (`rtx_length[kJumpInsn] == 8`) so
      // `JUMP_LABEL(insn)` (`fld[7]`) never overwrites the next obstack object.
      Rtx j_insn = rtx_alloc(kJumpInsn);
      std::memcpy(j_insn, insn, sizeof(rtx_def) + 6 * sizeof(rtunion_def));
      j_insn->code = kJumpInsn;
      j_insn->fld[7].rtx = nullptr;
      insn = j_insn;
    }
  } else if (container_code == kCallInsn) {
    if (emit_to_chain) {
      insn = emit_call_insn(pat);
    } else {
      insn = make_insn_raw(pat);
      Rtx c_insn = rtx_alloc(kCallInsn);
      std::memcpy(c_insn, insn, sizeof(rtx_def) + 6 * sizeof(rtunion_def));
      c_insn->code = kCallInsn;
      c_insn->fld[7].rtx = nullptr;
      insn = c_insn;
    }
  } else {
    insn = emit_to_chain ? emit_insn(pat) : make_insn_raw(pat);
  }

  if (flag_v) insn->volatil = 1;
  if (flag_i) insn->integrated = 1;
  if (flag_u) insn->unchanging = 1;
  if (flag_s) insn->in_struct = 1;

  if (!notes_str.empty()) {
    insn->fld[6].rtx = ParseRtx(notes_str);
  }

  if (container_code == kJumpInsn) {
    if (Rtx target_lbl = FindCodeLabelInRtx(pat); target_lbl != nullptr) {
      insn->fld[7].rtx = target_lbl;
      target_lbl->fld[5].rtint += 1;
    }
  }

  return insn;
}

Rtx GccContext::EmitPattern(std::string_view pattern_rtl) {
  return BuildInsnFromContainerOrPattern(pattern_rtl, /*emit_to_chain=*/true);
}

Rtx GccContext::MaterializeFromCfg(const RtlControlFlowGraph& cfg, bool reset_function) {
  if (reset_function) {
    BeginFunction(current_func_name_);
  } else {
    label_map_.clear();
    Rtx head_note = get_insns();
    if (head_note != nullptr) {
      head_note->fld[2].rtx = nullptr;
      set_last_insn(head_note);
    }
    emit_note(nullptr, kNoteInsnFunctionBeg);
  }
  if (regno_reg_rtx != nullptr) {
    regno_reg_rtx[67] = virtual_incoming_args_rtx;
    regno_reg_rtx[68] = virtual_stack_vars_rtx;
    regno_reg_rtx[69] = virtual_stack_dynamic_rtx;
    regno_reg_rtx[70] = virtual_outgoing_args_rtx;
  }

  const size_t num_blocks = cfg.rpo_blocks.size();
  std::vector<Rtx> block_labels(num_blocks, nullptr);
  std::vector<bool> block_needs_label(num_blocks, false);

  // First pass: map `(label_ref <tok>)` tokens in jump instructions to their
  // target basic blocks and determine which blocks require a `CODE_LABEL`.
  absl::flat_hash_map<std::string, size_t> token_to_block;
  std::vector<std::string> ordered_tokens;

  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
    bool ends_with_uncond = false;
    if (!blk.instructions.empty()) {
      std::string_view last_pat = blk.instructions.back();
      ends_with_uncond = IsUnconditionalJumpOrReturnPatternText(last_pat);
      std::vector<std::string> refs = ExtractLabelRefTokens(last_pat);
      if ((!refs.empty() || absl::StrContains(last_pat, "(label_ref)")) &&
          !blk.successors.empty()) {
        size_t target_b = blk.successors[0];
        if (blk.successors.size() >= 2 && blk.successors[0] == b + 1) {
          target_b = blk.successors[1];
        }
        if (target_b < num_blocks) {
          block_needs_label[target_b] = true;
          if (!refs.empty() && !token_to_block.contains(refs[0])) {
            token_to_block.emplace(refs[0], target_b);
            ordered_tokens.push_back(refs[0]);
          }
        }
      }
    }
    for (size_t succ : blk.successors) {
      if (succ < num_blocks && (succ != b + 1 || ends_with_uncond)) {
        block_needs_label[succ] = true;
      }
    }
  }

  // Also collect any `(label_ref <tok>)` tokens appearing in non-final instructions
  // or `REG_NOTES`.
  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
    for (size_t insn_idx = 0; insn_idx < blk.instructions.size(); ++insn_idx) {
      const auto& pat = blk.instructions[insn_idx];
      if (insn_idx + 1 == blk.instructions.size() && blk.successors.empty() &&
          IsJumpPatternText(pat)) {
        continue;
      }
      for (const std::string& tok : ExtractLabelRefTokens(pat)) {
        if (!token_to_block.contains(tok)) {
          size_t idx = 0;
          std::string_view num_part = tok;
          if (absl::StartsWith(num_part, "L")) {
            num_part.remove_prefix(1);
          }
          if (absl::SimpleAtoi(num_part, &idx) && idx < num_blocks) {
            size_t target_b = (idx + 1 < num_blocks && !block_needs_label[idx]) ? idx + 1 : idx;
            block_needs_label[target_b] = true;
            token_to_block.emplace(tok, target_b);
            ordered_tokens.push_back(tok);
          }
        }
      }
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    if (block_needs_label[b]) {
      block_labels[b] = gen_label_rtx();
      label_map_[absl::StrFormat("__blk_%zu", b)] = block_labels[b];
    }
  }
  for (const auto& [tok, target_b] : token_to_block) {
    if (target_b < num_blocks && block_labels[target_b] != nullptr) {
      label_map_[tok] = block_labels[target_b];
    }
  }

  // Second pass: emit basic blocks in linear order.
  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
    if (block_labels[b] != nullptr) {
      emit_label(block_labels[b]);
    }
    bool emitted_uncond_jump_or_return = false;
    for (size_t insn_idx = 0; insn_idx < blk.instructions.size(); ++insn_idx) {
      const std::string& pat_str = blk.instructions[insn_idx];
      std::string resolved_pat = pat_str;
      if (insn_idx + 1 == blk.instructions.size() &&
          absl::StrContains(resolved_pat, "(label_ref") && !blk.successors.empty()) {
        size_t target_b = blk.successors[0];
        if (blk.successors.size() >= 2 && blk.successors[0] == b + 1) {
          target_b = blk.successors[1];
        }
        const std::string named_ref = absl::StrFormat("(label_ref __blk_%zu)", target_b);
        size_t pos = 0;
        while ((pos = resolved_pat.find("(label_ref", pos)) != std::string::npos) {
          size_t close = resolved_pat.find(')', pos);
          if (close == std::string::npos) {
            break;
          }
          resolved_pat.replace(pos, close - pos + 1, named_ref);
          pos += named_ref.size();
        }
      }
      Rtx insn = EmitPattern(resolved_pat);
      if (insn == nullptr) {
        continue;
      }
      if (insn->code == kJumpInsn) {
        if (IsUnconditionalJumpOrReturnPatternText(pat_str)) {
          emitted_uncond_jump_or_return = true;
        }
      } else if (insn->code == kInsn && insn->fld[3].rtx != nullptr &&
                 insn->fld[3].rtx->code == kSequence) {
        Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
        if (vec != nullptr && vec->num_elem > 0 && vec->elem[0].rtx != nullptr &&
            vec->elem[0].rtx->code == kJumpInsn) {
          std::string first_fmt = FormatRtx(vec->elem[0].rtx->fld[3].rtx);
          if (IsUnconditionalJumpOrReturnPatternText(first_fmt)) {
            emitted_uncond_jump_or_return = true;
          }
        }
      }
    }
    // Emit an explicit unconditional jump if block `b` does not end with a jump
    // or return and its sole successor is not `b + 1`.
    if (!emitted_uncond_jump_or_return && blk.successors.size() == 1 &&
        blk.successors[0] != b + 1 && blk.successors[0] < num_blocks &&
        block_labels[blk.successors[0]] != nullptr) {
      Rtx target_lbl = block_labels[blk.successors[0]];
      Rtx jmp_pat = gen_rtx(kSet, kVoidMode, pc_rtx, gen_rtx(kLabelRef, kVoidMode, target_lbl));
      Rtx jmp = emit_jump_insn(jmp_pat);
      jmp->fld[7].rtx = target_lbl;
      target_lbl->fld[5].rtint += 1;
      emitted_uncond_jump_or_return = true;
    }
    if (emitted_uncond_jump_or_return) {
      emit_barrier();
    }
  }

  Rtx first = get_insns();
  bool has_sequence = false;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence) {
      has_sequence = true;
      break;
    }
  }

  // Reconstruct `CALL_INSN_FUNCTION_USAGE` (`call_insn->fld[7].rtx`) for every
  // `CALL_INSN` from argument-register setup instructions (`$4..$7`, `$f12..$f15`)
  // preceding the call in the same basic block and any existing `REG_DEAD` notes
  // on the call instruction. Note that `print_rtl` never prints `fld[7]` on a
  // `CALL_INSN`, so reconstructing it here ensures `flow_analysis` knows which
  // argument registers are consumed by the call.
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code != kCallInsn) {
      continue;
    }
    absl::flat_hash_map<int, Rtx> arg_regs;
    auto is_arg_regno = [](int regno) {
      return (regno >= 4 && regno <= 7) || (regno >= 44 && regno <= 47);
    };
    for (Rtx prev = insn->fld[1].rtx; prev != nullptr; prev = prev->fld[1].rtx) {
      if (prev->code == kCallInsn || prev->code == kJumpInsn || prev->code == kCodeLabel ||
          prev->code == kBarrier) {
        break;
      }
      if (prev->code == kInsn && prev->fld[3].rtx != nullptr && prev->fld[3].rtx->code == kSet) {
        Rtx dst = prev->fld[3].rtx->fld[0].rtx;
        if (dst != nullptr && dst->code == kReg && is_arg_regno(dst->fld[0].rtint) &&
            !arg_regs.contains(dst->fld[0].rtint)) {
          arg_regs.emplace(dst->fld[0].rtint, gen_rtx(kReg, dst->mode, dst->fld[0].rtint));
        }
      }
    }
    for (Rtx note = insn->fld[6].rtx; note != nullptr; note = note->fld[1].rtx) {
      if (note->mode == kRegDead && note->fld[0].rtx != nullptr && note->fld[0].rtx->code == kReg) {
        Rtx reg = note->fld[0].rtx;
        if (is_arg_regno(reg->fld[0].rtint) && !arg_regs.contains(reg->fld[0].rtint)) {
          arg_regs.emplace(reg->fld[0].rtint, gen_rtx(kReg, reg->mode, reg->fld[0].rtint));
        }
      }
    }
    std::vector<int> sorted_arg_regnos;
    sorted_arg_regnos.reserve(arg_regs.size());
    for (const auto& [regno, _] : arg_regs) {
      sorted_arg_regnos.push_back(regno);
    }
    std::sort(sorted_arg_regnos.begin(), sorted_arg_regnos.end());
    Rtx fusage = insn->fld[7].rtx;
    for (int regno : sorted_arg_regnos) {
      Rtx use_rtx = gen_rtx(kUse, kVoidMode, arg_regs.at(regno));
      fusage = gen_rtx(kExprList, kVoidMode, use_rtx, fusage);
    }
    const bool is_post_flow_pass =
        (cfg.pass != RtlPassKind::kRtl && cfg.pass != RtlPassKind::kJump &&
         cfg.pass != RtlPassKind::kCse && cfg.pass != RtlPassKind::kLoop &&
         cfg.pass != RtlPassKind::kCse2);
    if ((is_post_flow_pass || !cfg.sideband.basic_block_live_at_start.empty()) &&
        insn->fld[6].rtx == nullptr) {
      Rtx notes = nullptr;
      for (int regno : sorted_arg_regnos) {
        notes = gen_rtx(kExprList, kRegDead, arg_regs.at(regno), notes);
      }
      insn->fld[6].rtx = notes;
    }
    insn->fld[7].rtx = fusage;
  }
  // Synchronize `cfg.sideband` into GCC 2.7.2 global side-band arrays.
  int target_max_regno = std::max(max_reg_num(), cfg.sideband.max_regno);
  for (const auto& [regno, _] : cfg.sideband.reg_renumber) {
    target_max_regno = std::max(target_max_regno, regno + 1);
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_refs) {
    target_max_regno = std::max(target_max_regno, regno + 1);
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_sets) {
    target_max_regno = std::max(target_max_regno, regno + 1);
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_deaths) {
    target_max_regno = std::max(target_max_regno, regno + 1);
  }
  for (const auto& [regno, _] : cfg.sideband.reg_live_length) {
    target_max_regno = std::max(target_max_regno, regno + 1);
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_calls_crossed) {
    target_max_regno = std::max(target_max_regno, regno + 1);
  }
  for (const auto& [_, live_set] : cfg.sideband.basic_block_live_at_start) {
    for (int regno : live_set) {
      target_max_regno = std::max(target_max_regno, regno + 1);
    }
  }
  if (target_max_regno > kFirstPseudoRegister) {
    EnsurePseudoRegsUpTo(target_max_regno - 1);
  }
  max_regno = max_reg_num();
  const int alloc_regs = std::max(max_regno, target_max_regno) + 16;

  if (!has_sequence) {
    restore_reg_data(first);
    if (regno_reg_rtx != nullptr) {
      regno_reg_rtx[67] = virtual_incoming_args_rtx;
      regno_reg_rtx[68] = virtual_stack_vars_rtx;
      regno_reg_rtx[69] = virtual_stack_dynamic_rtx;
      regno_reg_rtx[70] = virtual_outgoing_args_rtx;
    }
    reg_scan(first, max_reg_num(), 0);
  } else {
    reg_scan(nullptr, max_reg_num(), 0);
  }

  if (!cfg.sideband.reg_renumber.empty()) {
    reg_renumber = AllocSidebandArray<short>(alloc_regs);
    for (int i = 0; i < alloc_regs; ++i) {
      reg_renumber[i] = -1;
    }
    for (const auto& [regno, hard_reg] : cfg.sideband.reg_renumber) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_renumber[regno] = static_cast<short>(hard_reg);
      }
    }
  }

  if (!cfg.sideband.reg_n_sets.empty() && reg_n_sets != nullptr) {
    for (const auto& [regno, val] : cfg.sideband.reg_n_sets) {
      if (regno >= 0 && regno < max_regno) {
        reg_n_sets[regno] = static_cast<short>(val);
      }
    }
  }

  if (!cfg.sideband.reg_n_refs.empty() || !cfg.sideband.reg_n_deaths.empty() ||
      !cfg.sideband.reg_live_length.empty() || !cfg.sideband.reg_n_calls_crossed.empty()) {
    reg_n_refs = AllocSidebandArray<int>(alloc_regs);
    reg_n_deaths = AllocSidebandArray<short>(alloc_regs);
    reg_live_length = AllocSidebandArray<int>(alloc_regs);
    reg_n_calls_crossed = AllocSidebandArray<int>(alloc_regs);
    for (const auto& [regno, val] : cfg.sideband.reg_n_refs) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_n_refs[regno] = val;
      }
    }
    for (const auto& [regno, val] : cfg.sideband.reg_n_deaths) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_n_deaths[regno] = static_cast<short>(val);
      }
    }
    for (const auto& [regno, val] : cfg.sideband.reg_live_length) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_live_length[regno] = val;
      }
    }
    for (const auto& [regno, val] : cfg.sideband.reg_n_calls_crossed) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_n_calls_crossed[regno] = val;
      }
    }
  }

  if (!cfg.sideband.basic_block_live_at_start.empty()) {
    int max_bb = static_cast<int>(num_blocks);
    for (const auto& [bb_idx, _] : cfg.sideband.basic_block_live_at_start) {
      max_bb = std::max(max_bb, bb_idx + 1);
    }
    n_basic_blocks = max_bb;
    regset_size = (alloc_regs / 32) + 2;
    regset_bytes = regset_size * static_cast<int>(sizeof(uint32_t));
    basic_block_live_at_start = AllocSidebandArray<Rtx>(max_bb + 4);
    for (int b = 0; b < max_bb + 4; ++b) {
      basic_block_live_at_start[b] =
          reinterpret_cast<Rtx>(AllocSidebandArray<uint32_t>(regset_size));
    }
    for (const auto& [bb_idx, live_set] : cfg.sideband.basic_block_live_at_start) {
      if (bb_idx < 0 || bb_idx >= max_bb) {
        continue;
      }
      auto* words = reinterpret_cast<uint32_t*>(basic_block_live_at_start[bb_idx]);
      for (int regno : live_set) {
        if (regno >= 0 && (regno / 32) < regset_size) {
          words[regno / 32] |= (1u << (regno % 32));
        }
      }
    }
  }

  for (int gpr : cfg.sideband.saved_gpr_regnos) {
    if (gpr >= 0 && gpr < kFirstPseudoRegister) {
      regs_ever_live[gpr] = 1;
    }
  }
  for (int fpr : cfg.sideband.saved_fpr_regnos) {
    if (fpr >= 0 && fpr < kFirstPseudoRegister) {
      regs_ever_live[fpr] = 1;
    }
  }
  for (const auto& [_, hard_reg] : cfg.sideband.reg_renumber) {
    if (hard_reg >= 0 && hard_reg < kFirstPseudoRegister) {
      regs_ever_live[hard_reg] = 1;
    }
  }
  auto mark_set_hard_regs = [](Rtx pat) {
    if (pat == nullptr) {
      return;
    }
    auto mark_one = [](Rtx x) {
      if (x == nullptr || (x->code != kSet && x->code != kClobber)) {
        return;
      }
      Rtx dst = x->fld[0].rtx;
      if (dst != nullptr && dst->code == kSubreg) {
        dst = dst->fld[0].rtx;
      }
      if (dst != nullptr && dst->code == kReg && dst->fld[0].rtint >= 0 &&
          dst->fld[0].rtint < kFirstPseudoRegister) {
        regs_ever_live[dst->fld[0].rtint] = 1;
      }
    };
    if (pat->code == kParallel && pat->fld[0].rtvec != nullptr) {
      Rtvec vec = pat->fld[0].rtvec;
      for (int i = 0; i < vec->num_elem; ++i) {
        mark_one(vec->elem[i].rtx);
      }
    } else {
      mark_one(pat);
    }
  };
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) {
      Rtx pat = insn->fld[3].rtx;
      if (pat != nullptr && pat->code == kSequence && pat->fld[0].rtvec != nullptr) {
        Rtvec seq = pat->fld[0].rtvec;
        for (int i = 0; i < seq->num_elem; ++i) {
          if (seq->elem[i].rtx != nullptr) {
            mark_set_hard_regs(seq->elem[i].rtx->fld[3].rtx);
          }
        }
      } else {
        mark_set_hard_regs(pat);
      }
    }
  }
  if (cfg.pass != RtlPassKind::kRtl && current_function_decl != nullptr) {
    instantiate_virtual_regs(current_function_decl, nullptr);
  }
  if (cfg.pass == RtlPassKind::kGreg || cfg.pass == RtlPassKind::kSched2 ||
      cfg.pass == RtlPassKind::kJump2 || cfg.pass == RtlPassKind::kDbr) {
    reload_completed = 1;
  }
  frame_offset = cfg.sideband.frame_size_bytes;
  current_function_outgoing_args_size = cfg.sideband.outgoing_args_size;
  compute_frame_size(cfg.sideband.frame_size_bytes);
  if (cfg.sideband.epilogue_delay_pattern.has_value() &&
      !cfg.sideband.epilogue_delay_pattern->empty()) {
    Rtx delay_insn = BuildInsnFromContainerOrPattern(*cfg.sideband.epilogue_delay_pattern,
                                                     /*emit_to_chain=*/false);
    if (delay_insn != nullptr) {
      current_function_epilogue_delay_list = gen_rtx(kInsnList, kVoidMode, delay_insn, nullptr);
    }
  }

  return first;
}

RtlControlFlowGraph GccContext::DematerializeToCfg(Rtx first_insn, RtlPassKind pass) const {
  Rtx head = first_insn != nullptr ? first_insn : get_insns();
  if (head == nullptr) {
    return BuildCanonicalCfgFromFlatRtlItems(pass, {});
  }

  // First pass: assign sequential canonical label names (`L0`, `L1`, ...) to
  // all `CODE_LABEL` nodes in linear chain order, followed by any un-emitted
  // label UIDs referenced by `(label_ref <uid>)` (e.g. `return_label` or
  // `if_false_label` during isolated Stage 13 sub-hook expansion).
  absl::flat_hash_map<int, std::string> label_map;
  size_t next_lbl_idx = 0;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kCodeLabel && insn->fld[0].rtint >= 0) {
      if (!label_map.contains(insn->fld[0].rtint)) {
        label_map.emplace(insn->fld[0].rtint, absl::StrCat("L", next_lbl_idx++));
      }
    }
  }
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) {
      continue;
    }
    Rtx pat = insn->fld[3].rtx;
    if (pat == nullptr) {
      continue;
    }
    for (const std::string& tok : ExtractLabelRefTokens(FormatRtx(pat))) {
      int uid_val = 0;
      if (absl::SimpleAtoi(tok, &uid_val) && uid_val >= 0 && !label_map.contains(uid_val)) {
        label_map.emplace(uid_val, absl::StrCat("L", next_lbl_idx++));
      }
    }
  }

  auto format_insn_tag = [](Rtx sub) -> std::string {
    std::string tag =
        (sub->code == kJumpInsn) ? "jump_insn" : ((sub->code == kCallInsn) ? "call_insn" : "insn");
    if (sub->in_struct) absl::StrAppend(&tag, "/s");
    if (sub->volatil) absl::StrAppend(&tag, "/v");
    if (sub->unchanging) absl::StrAppend(&tag, "/u");
    if (sub->integrated) absl::StrAppend(&tag, "/i");
    return tag;
  };

  auto code_to_kind = [](int code) -> RtlInsnKind {
    switch (code) {
      case kInsn:
        return RtlInsnKind::kInsn;
      case kJumpInsn:
        return RtlInsnKind::kJumpInsn;
      case kCallInsn:
        return RtlInsnKind::kCallInsn;
      case kCodeLabel:
        return RtlInsnKind::kCodeLabel;
      case kBarrier:
        return RtlInsnKind::kBarrier;
      case kNote:
        return RtlInsnKind::kNote;
      default:
        return RtlInsnKind::kOther;
    }
  };

  std::vector<FlatRtlItem> items;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    const int code = insn->code;
    if (code == kNote) {
      const int note_kind = insn->fld[4].rtint;
      if (note_kind == kNoteInsnLoopBeg) {
        items.push_back(FlatRtlItem{
            .kind = RtlInsnKind::kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_BEG)",
        });
      } else if (note_kind == kNoteInsnLoopCont) {
        items.push_back(FlatRtlItem{
            .kind = RtlInsnKind::kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_CONT)",
        });
      } else if (note_kind == kNoteInsnLoopEnd) {
        items.push_back(FlatRtlItem{
            .kind = RtlInsnKind::kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_END)",
        });
      } else if (note_kind == kNoteInsnLoopVtop) {
        items.push_back(FlatRtlItem{
            .kind = RtlInsnKind::kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_VTOP)",
        });
      }
      continue;
    }
    if (code == kCodeLabel || code == kBarrier) {
      items.push_back(FlatRtlItem{
          .kind = code_to_kind(code),
          .uid = insn->fld[0].rtint,
      });
      continue;
    }
    if (code != kInsn && code != kJumpInsn && code != kCallInsn) {
      continue;
    }

    Rtx pat = insn->fld[3].rtx;
    if (pat != nullptr && pat->code == kSequence && pat->fld[0].rtvec != nullptr) {
      Rtvec vec = pat->fld[0].rtvec;
      std::vector<std::string> formatted_inner;
      formatted_inner.reserve(vec->num_elem);
      RtlInsnKind effective_kind = code_to_kind(code);
      std::string jump_raw_pat;
      for (int j = 0; j < vec->num_elem; ++j) {
        Rtx sub = vec->elem[j].rtx;
        if (sub == nullptr) {
          continue;
        }
        const std::string tag = format_insn_tag(sub);
        std::string sub_pat = CanonicalizeLabelRefsInPattern(FormatRtx(sub->fld[3].rtx), label_map);
        if (sub->code != kCallInsn && sub->fld[6].rtx != nullptr) {
          absl::StrAppend(&sub_pat, " ",
                          CanonicalizeLabelRefsInPattern(FormatRtx(sub->fld[6].rtx), label_map));
        }
        formatted_inner.push_back(
            absl::StrCat("(", tag, " ", CanonicalizeRtlPattern(sub_pat), ")"));
        if (sub->code == kJumpInsn) {
          effective_kind = RtlInsnKind::kJumpInsn;
          jump_raw_pat = FormatRtx(sub->fld[3].rtx);
        }
      }
      items.push_back(FlatRtlItem{
          .kind = effective_kind,
          .uid = insn->fld[0].rtint,
          .raw_pattern_for_targets = jump_raw_pat,
          .pattern = absl::StrCat("(sequence [", absl::StrJoin(formatted_inner, " "), "])"),
      });
      continue;
    }

    std::string raw_pat = FormatRtx(pat);
    std::string canon_pat = CanonicalizeLabelRefsInPattern(raw_pat, label_map);
    if (code != kCallInsn && insn->fld[6].rtx != nullptr) {
      absl::StrAppend(&canon_pat, " ",
                      CanonicalizeLabelRefsInPattern(FormatRtx(insn->fld[6].rtx), label_map));
    }
    items.push_back(FlatRtlItem{
        .kind = code_to_kind(code),
        .uid = insn->fld[0].rtint,
        .raw_pattern_for_targets = raw_pat,
        .pattern = CanonicalizeRtlPattern(canon_pat),
    });
  }

  RtlControlFlowGraph cfg = BuildCanonicalCfgFromFlatRtlItems(pass, items);

  // Dematerialize GCC 2.7.2 global side-band arrays into `cfg.sideband`.
  const int nregs = std::max(max_reg_num(), max_regno);
  cfg.sideband.max_regno = nregs;

  if (reg_renumber != nullptr) {
    for (int r = kFirstPseudoRegister; r < nregs; ++r) {
      if (reg_renumber[r] >= 0) {
        cfg.sideband.reg_renumber[r] = reg_renumber[r];
      }
    }
  }
  if (reg_n_refs != nullptr) {
    for (int r = 0; r < nregs; ++r) {
      if (reg_n_refs[r] != 0) {
        cfg.sideband.reg_n_refs[r] = reg_n_refs[r];
      }
    }
  }
  if (reg_n_sets != nullptr) {
    for (int r = 0; r < nregs; ++r) {
      if (reg_n_sets[r] != 0) {
        cfg.sideband.reg_n_sets[r] = reg_n_sets[r];
      }
    }
  }
  if (reg_n_deaths != nullptr) {
    for (int r = 0; r < nregs; ++r) {
      if (reg_n_deaths[r] != 0) {
        cfg.sideband.reg_n_deaths[r] = reg_n_deaths[r];
      }
    }
  }
  if (reg_live_length != nullptr) {
    for (int r = 0; r < nregs; ++r) {
      if (reg_live_length[r] > 0) {
        cfg.sideband.reg_live_length[r] = reg_live_length[r];
      }
    }
  }
  if (reg_n_calls_crossed != nullptr) {
    for (int r = 0; r < nregs; ++r) {
      if (reg_n_calls_crossed[r] > 0) {
        cfg.sideband.reg_n_calls_crossed[r] = reg_n_calls_crossed[r];
      }
    }
  }
  if (basic_block_live_at_start != nullptr && n_basic_blocks > 0 && regset_size > 0) {
    for (int b = 0; b < n_basic_blocks; ++b) {
      if (basic_block_live_at_start[b] == nullptr) {
        continue;
      }
      const auto* words = reinterpret_cast<const uint32_t*>(basic_block_live_at_start[b]);
      for (int w = 0; w < regset_size; ++w) {
        uint32_t mask = words[w];
        while (mask != 0) {
          const int bit = __builtin_ctz(mask);
          const int regno = w * 32 + bit;
          if (regno < nregs) {
            cfg.sideband.basic_block_live_at_start[b].insert(regno);
          }
          mask &= (mask - 1);
        }
      }
    }
  }
  cfg.sideband.frame_size_bytes = frame_offset;
  cfg.sideband.outgoing_args_size = current_function_outgoing_args_size;
  for (int r = 0; r < 32; ++r) {
    if (regs_ever_live[r] != 0) {
      cfg.sideband.saved_gpr_regnos.push_back(r);
    }
  }
  for (int r = 32; r < 64; ++r) {
    if (regs_ever_live[r] != 0) {
      cfg.sideband.saved_fpr_regnos.push_back(r);
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
      cfg.sideband.epilogue_delay_pattern =
          CanonicalizeRtlPattern(CanonicalizeLabelRefsInPattern(FormatRtx(delay_pat), label_map));
    }
  }

  return cfg;
}

Rtx GccContext::GetInsns() const {
  return get_insns();
}

}  // namespace rom_nom_nom::gcc_harness
