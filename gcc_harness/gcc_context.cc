#include "gcc_harness/gcc_context.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "core/rtl_expr.h"
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
void clear_const_double_mem();
void temporary_allocation();
void init_function_start(void* subr, char* filename, int line);
void expand_function_start(void* subr, int parms_have_cleanups);
void expand_function_end(char* filename, int line, int end_bindings);
void instantiate_virtual_regs(void* fndecl, rtx_def* insns);
void restore_reg_data(rtx_def* first);
void reg_scan(rtx_def* f, int nregs, int repeat);
void regclass_init();
int compute_frame_size(int size);
void set_last_insn(rtx_def* insn);
void start_sequence();
void end_sequence();
void push_temp_slots();
void pop_temp_slots();
void free_temp_slots();
void emit_queue();
void emit_nop();
rtx_def* emit_insn_before(rtx_def* pattern, rtx_def* before);
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
void init_insn_lengths();

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
extern int frame_pointer_needed;
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
constexpr int kMaxMachineMode = 32;
extern rtx_def* const_tiny_rtx[3][kMaxMachineMode];
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
extern int current_function_calls_alloca;
extern int virtuals_instantiated;
extern int immediate_size_expand;
extern rtx_def* return_label;
extern rtx_def* stack_pointer_rtx;
extern rtx_def* frame_pointer_rtx;
extern rtx_def* arg_pointer_rtx;
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
extern int mips_optimize;

// GCC 2.7.2 `varasm.c` constant-pool & `real.c` floating-point conversion symbols.
extern int const_labelno;
int get_pool_size();
rtx_def* get_pool_constant(rtx_def* addr);
int get_pool_mode(rtx_def* addr);
rtx_def* immed_double_const(int i0, int i1, int mode);
rtx_def* immed_real_const_1(double d, int mode);
rtx_def* force_const_mem(int mode, rtx_def* x);
void output_constant_pool(char* fnname, void* fndecl);
void text_section();
void function_section(void* decl);
void save_varasm_status(void* p);
void restore_varasm_status(void* p);
double ereal_from_float(int f);
double ereal_from_double(int* d);
long etarsingle(double r);
void etardouble(double r, long* out);

rom_nom_nom::gcc_harness::Rtx __real_immed_double_const(int i0, int i1, int mode);
rom_nom_nom::gcc_harness::Rtx __real_immed_real_const(void* exp);
rom_nom_nom::gcc_harness::Rtx __real_immed_real_const_1(double d, int mode);
rom_nom_nom::gcc_harness::Rtx __real_force_const_mem(int mode, rom_nom_nom::gcc_harness::Rtx x);
int __real_reload(rom_nom_nom::gcc_harness::Rtx first, int global, void* dumpfile);

}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace rom_nom_nom::gcc_harness {
namespace {

std::vector<Stage0ConstantPoolEntry>* g_cc1_pool_capture_target = nullptr;
bool g_in_pool_snapshot = false;
bool g_skip_reload_in_global_alloc = false;

}  // namespace
}  // namespace rom_nom_nom::gcc_harness

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {

// On LP64, `immed_double_const` allocates `(const_double:M 0 i0 i1)` via
// `gen_rtx`, populating `fld[2].rtint = i0` and `fld[3].rtint = i1` while
// leaving the upper 32 bits of 8-byte `fld[2]` (`(&fld[2].rtint)[1]`)
// uninitialized. Meanwhile `decode_rtx_const` and `REAL_VALUE_FROM_CONST_DOUBLE`
// read 8 contiguous bytes starting at `&fld[2].rtint`. Mirror `fld[3].rtint`
// into `(&fld[2].rtint)[1]` across all `varasm.o` entrypoints so floating-point
// constant hashing and extraction are deterministic across functions.
rom_nom_nom::gcc_harness::Rtx __wrap_immed_double_const(int i0, int i1, int mode) {
  rom_nom_nom::gcc_harness::Rtx r = __real_immed_double_const(i0, i1, mode);
  if (r != nullptr && r->code == kConstDouble) {
    (&r->fld[2].rtint)[1] = r->fld[3].rtint;
  }
  return r;
}

rom_nom_nom::gcc_harness::Rtx __wrap_immed_real_const(void* exp) {
  rom_nom_nom::gcc_harness::Rtx r = __real_immed_real_const(exp);
  if (r != nullptr && r->code == kConstDouble) {
    (&r->fld[2].rtint)[1] = r->fld[3].rtint;
  }
  return r;
}

rom_nom_nom::gcc_harness::Rtx __wrap_immed_real_const_1(double d, int mode) {
  rom_nom_nom::gcc_harness::Rtx r = __real_immed_real_const_1(d, mode);
  if (r != nullptr && r->code == kConstDouble) {
    (&r->fld[2].rtint)[1] = r->fld[3].rtint;
  }
  return r;
}

rom_nom_nom::gcc_harness::Rtx __wrap_force_const_mem(int mode, rom_nom_nom::gcc_harness::Rtx x) {
  if (x != nullptr && x->code == kConstDouble) {
    (&x->fld[2].rtint)[1] = x->fld[3].rtint;
  }
  rom_nom_nom::gcc_harness::Rtx def = __real_force_const_mem(mode, x);
  if (rom_nom_nom::gcc_harness::g_cc1_pool_capture_target != nullptr &&
      !rom_nom_nom::gcc_harness::g_in_pool_snapshot && def != nullptr) {
    std::vector<rom_nom_nom::Stage0ConstantPoolEntry> active =
        rom_nom_nom::gcc_harness::GccContext::SnapshotActiveConstantPool();
    for (const auto& entry : active) {
      bool updated = false;
      for (auto& existing : *rom_nom_nom::gcc_harness::g_cc1_pool_capture_target) {
        if (existing.label_name == entry.label_name) {
          existing = entry;
          updated = true;
          break;
        }
      }
      if (!updated) {
        rom_nom_nom::gcc_harness::g_cc1_pool_capture_target->push_back(entry);
      }
    }
  }
  return def;
}

int __wrap_reload(rom_nom_nom::gcc_harness::Rtx first, int global, void* dumpfile) {
  if (rom_nom_nom::gcc_harness::g_skip_reload_in_global_alloc) {
    return 0;
  }
  return __real_reload(first, global, dumpfile);
}

}  // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace rom_nom_nom::gcc_harness {
namespace {

constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
constexpr int kArgPointerPrintSentinel = 1073741823;

// Temporarily replaces `arg_pointer_rtx->fld[0].rtint` (`ARG_POINTER_REGNUM == 0`
// on MIPS) with `kArgPointerPrintSentinel` during `print_rtl` so `arg_pointer_rtx`
// is printed distinctly from hardware `$zero` (`(reg:SI 0 $0)`).
class ScopedArgPointerSentinel {
 public:
  ScopedArgPointerSentinel() {
    if (arg_pointer_rtx != nullptr && arg_pointer_rtx->code == kReg) {
      saved_regno_ = arg_pointer_rtx->fld[0].rtint;
      arg_pointer_rtx->fld[0].rtint = kArgPointerPrintSentinel;
      active_ = true;
    }
  }
  ~ScopedArgPointerSentinel() {
    if (active_ && arg_pointer_rtx != nullptr) {
      arg_pointer_rtx->fld[0].rtint = saved_regno_;
    }
  }

 private:
  int saved_regno_ = 0;
  bool active_ = false;
};

void NormalizePrintedRtl(std::string* s) {
  if (s == nullptr) {
    return;
  }
  absl::StrReplaceAll({{" 1073741823)", " 0 argp)"}, {"\"*$LC", "\"$LC"}, {"\"*.LC", "\".LC"}}, s);
}

bool ParsePoolIntLiteral(std::string_view tok, uint64_t* out_val) {
  std::string s(absl::StripAsciiWhitespace(tok));
  if (s.empty()) {
    return false;
  }
  char* endptr = nullptr;
  errno = 0;
  if (s.front() == '-') {
    const long long v = std::strtoll(s.c_str(), &endptr, 0);
    if (errno != 0 || endptr == s.c_str() || *endptr != '\0') {
      return false;
    }
    *out_val = static_cast<uint64_t>(static_cast<int64_t>(v));
    return true;
  }
  const unsigned long long v = std::strtoull(s.c_str(), &endptr, 0);
  if (errno != 0 || endptr == s.c_str() || *endptr != '\0') {
    return false;
  }
  *out_val = static_cast<uint64_t>(v);
  return true;
}

std::vector<Stage0ConstantPoolEntry> ParseConstantPoolAsm(std::string_view asm_text) {
  std::vector<Stage0ConstantPoolEntry> entries;
  std::string current_section = ".rdata";
  size_t active_idx = static_cast<size_t>(-1);

  for (std::string_view raw_line : absl::StrSplit(asm_text, '\n')) {
    const size_t comment_pos = raw_line.find('#');
    if (comment_pos != std::string_view::npos) {
      raw_line = raw_line.substr(0, comment_pos);
    }
    std::string_view line = absl::StripAsciiWhitespace(raw_line);
    if (line.empty()) {
      continue;
    }
    while (true) {
      const size_t colon_pos = line.find(':');
      if (colon_pos == std::string_view::npos) {
        break;
      }
      std::string_view before = line.substr(0, colon_pos);
      if (before.find_first_of(" \t,()") != std::string_view::npos) {
        break;
      }
      if (!before.empty() && before.front() == '*') {
        before.remove_prefix(1);
      }
      entries.push_back(Stage0ConstantPoolEntry{
          .label_name = std::string(before),
          .section_name = current_section,
      });
      active_idx = entries.size() - 1;
      line = absl::StripAsciiWhitespace(line.substr(colon_pos + 1));
    }
    if (line.empty() || line.front() != '.') {
      continue;
    }
    std::string_view rest = absl::StripAsciiWhitespace(line.substr(1));
    auto match_dir = [&](std::string_view name) {
      return absl::StartsWith(rest, name) &&
             (rest.size() == name.size() ||
              absl::ascii_isspace(static_cast<unsigned char>(rest[name.size()])) ||
              rest[name.size()] == ',');
    };
    if (match_dir("rdata")) {
      current_section = ".rdata";
      active_idx = static_cast<size_t>(-1);
    } else if (match_dir("rodata")) {
      current_section = ".rodata";
      active_idx = static_cast<size_t>(-1);
    } else if (match_dir("sdata")) {
      current_section = ".sdata";
      active_idx = static_cast<size_t>(-1);
    } else if (match_dir("text")) {
      current_section = ".text";
      active_idx = static_cast<size_t>(-1);
    } else if (active_idx < entries.size()) {
      if (match_dir("word")) {
        std::string_view args = absl::StripAsciiWhitespace(rest.substr(4));
        for (std::string_view item : absl::StrSplit(args, ',')) {
          uint64_t val = 0;
          if (ParsePoolIntLiteral(item, &val)) {
            entries[active_idx].words.push_back(static_cast<uint32_t>(val));
          }
        }
      } else if (match_dir("dword")) {
        std::string_view args = absl::StripAsciiWhitespace(rest.substr(5));
        for (std::string_view item : absl::StrSplit(args, ',')) {
          uint64_t val = 0;
          if (ParsePoolIntLiteral(item, &val)) {
            entries[active_idx].words.push_back(static_cast<uint32_t>(val >> 32));
            entries[active_idx].words.push_back(static_cast<uint32_t>(val & 0xffffffffu));
          }
        }
      } else if (match_dir("float") || match_dir("single")) {
        const size_t kw_len = match_dir("float") ? 5 : 6;
        std::string_view args = absl::StripAsciiWhitespace(rest.substr(kw_len));
        for (std::string_view item : absl::StrSplit(args, ',')) {
          std::string s(absl::StripAsciiWhitespace(item));
          char* endptr = nullptr;
          const float fv = std::strtof(s.c_str(), &endptr);
          if (!s.empty() && endptr != s.c_str() && *endptr == '\0') {
            entries[active_idx].words.push_back(absl::bit_cast<uint32_t>(fv));
          }
        }
      } else if (match_dir("double")) {
        std::string_view args = absl::StripAsciiWhitespace(rest.substr(6));
        for (std::string_view item : absl::StrSplit(args, ',')) {
          std::string s(absl::StripAsciiWhitespace(item));
          char* endptr = nullptr;
          const double dv = std::strtod(s.c_str(), &endptr);
          if (!s.empty() && endptr != s.c_str() && *endptr == '\0') {
            const uint64_t bits = absl::bit_cast<uint64_t>(dv);
            entries[active_idx].words.push_back(static_cast<uint32_t>(bits >> 32));
            entries[active_idx].words.push_back(static_cast<uint32_t>(bits & 0xffffffffu));
          }
        }
      }
    }
  }

  std::vector<Stage0ConstantPoolEntry> non_empty;
  non_empty.reserve(entries.size());
  for (auto& cp : entries) {
    if (!cp.words.empty()) {
      cp.byte_size = cp.words.size() * 4u;
      non_empty.push_back(std::move(cp));
    }
  }
  return non_empty;
}

bool ParseLcLabelNumber(std::string_view label, int* out_num) {
  if (absl::StartsWith(label, "*")) {
    label.remove_prefix(1);
  }
  if (!absl::StartsWith(label, "$LC") && !absl::StartsWith(label, ".LC")) {
    return false;
  }
  label.remove_prefix(3);
  return !label.empty() && absl::SimpleAtoi(label, out_num) && *out_num >= 0;
}

absl::flat_hash_map<int, int> InferPreferredPoolModesFromCfg(const RtlControlFlowGraph& cfg) {
  absl::flat_hash_map<int, int> preferred;
  auto mode_to_gcc = [](RtlMachineMode mode) -> int {
    switch (mode) {
      case RtlMachineMode::kSf:
        return kSfMode;
      case RtlMachineMode::kDf:
        return kDfMode;
      case RtlMachineMode::kDi:
        return kDiMode;
      case RtlMachineMode::kSi:
        return kSiMode;
      default:
        return kVoidMode;
    }
  };
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        continue;
      }
      parsed->pattern.Visit([&](const RtlExpr& node) {
        if (node.op != RtlOp::kMem) {
          return;
        }
        const int mem_mode = mode_to_gcc(node.mode);
        if (mem_mode == kVoidMode) {
          return;
        }
        node.Visit([&](const RtlExpr& sub) {
          if (sub.op != RtlOp::kSymbolRef) {
            return;
          }
          int lc_num = -1;
          if (ParseLcLabelNumber(sub.SymbolName(), &lc_num)) {
            // Prefer 8-byte/float modes over `SImode` if a split load accesses the same pool entry.
            auto it = preferred.find(lc_num);
            if (it == preferred.end() || (it->second == kSiMode && mem_mode != kSiMode)) {
              preferred[lc_num] = mem_mode;
            }
          }
        });
      });
    }
  }
  return preferred;
}

constexpr int kBlockCode = 5;
constexpr int kVoidTypeCode = 6;
constexpr int kIntegerTypeCode = 7;
constexpr int kEnumeralTypeCode = 10;
constexpr int kRecordTypeCode = 20;
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
  struct SavedSpecialReg {
    Rtx* ptr;
    int initial_regno;
  };
  static SavedSpecialReg special_regs[] = {
      {&stack_pointer_rtx, -1},         {&frame_pointer_rtx, -1},
      {&arg_pointer_rtx, -1},           {&virtual_incoming_args_rtx, -1},
      {&virtual_stack_vars_rtx, -1},    {&virtual_stack_dynamic_rtx, -1},
      {&virtual_outgoing_args_rtx, -1},
  };
  for (auto& sr : special_regs) {
    Rtx reg = *sr.ptr;
    if (reg != nullptr) {
      if (sr.initial_regno < 0) {
        sr.initial_regno = reg->fld[0].rtint;
      }
      reg->code = kReg;
      reg->fld[0].rtint = sr.initial_regno;
      reg->mode = kSiMode;
      reg->volatil = 0;
      reg->integrated = 0;
      reg->unchanging = 0;
      reg->in_struct = 0;
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
  static auto* interned_strings = new absl::flat_hash_map<std::string, std::unique_ptr<char[]>>();
  if (auto it = interned_strings->find(str); it != interned_strings->end()) {
    return it->second.get();
  }
  auto buf = std::make_unique<char[]>(str.size() + 1);
  if (!str.empty()) {
    std::memcpy(buf.get(), str.data(), str.size());
  }
  buf[str.size()] = '\0';
  char* result = buf.get();
  interned_strings->emplace(std::string(str), std::move(buf));
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
    asm_out_data_file = asm_out_file;
    asm_out_text_file = asm_out_file;

    init_reg_sets();
    override_options();
    init_obstacks();
    init_tree_codes();
    init_lex();
    init_rtl();
    init_emit_once(0);
    // Re-run `override_options()` once `real.c` / `emit-rtl.c` mode tables are
    // initialized so `dfhigh`/`dflow`/`sfhigh`/`sflow` match `ApplyCompilerFlags`.
    override_options();
    // On LP64, `init_emit_once` populates `const_tiny_rtx` via
    // `bcopy(&u, &CONST_DOUBLE_LOW(tem), 8)` which writes `u.i[0]` and `u.i[1]`
    // into the two 32-bit halves of 8-byte `fld[2]`, leaving `fld[3].rtint`
    // (`CONST_DOUBLE_HIGH`) uninitialized for `print_rtl`. Copy the high word
    // into `fld[3].rtint` so `print_rtl` is deterministic.
    for (int i = 0; i < 3; ++i) {
      for (int m = 0; m < kMaxMachineMode; ++m) {
        Rtx tiny = const_tiny_rtx[i][m];
        if (tiny != nullptr && tiny->code == kConstDouble) {
          tiny->fld[3].rtint = (&tiny->fld[2].rtint)[1];
        }
      }
    }
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
    mips_optimize = 2;
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
  active_is_variadic_ = false;
  active_returns_aggregate_in_memory_ = false;
  active_prologue_args_.clear();
  label_map_.clear();
  decl_map_.clear();
  c_label_decl_map_.clear();
  pool_symbol_map_.clear();
  insn_uid_to_rpo_block_.clear();
  const_labelno = 0;
  ResetVirtualRegisterFlags();
  init_insn_lengths();
  clear_const_double_mem();
  temporary_allocation();

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
  // Reset `regclass.c`'s static `prefclass` pointer (`regclass.c:620-623`)
  // after `temporary_allocation()` frees `rtl_obstack`, mirroring
  // `toplev.c:2957`. Without this, `prefclass` dangles across functions and
  // `record_reg_classes` (`regclass.c:1299`) reads stale/freed obstack memory
  // on `pass == 0` under `-O1` (`flag_expensive_optimizations == 0`).
  regclass_init();
  reg_renumber = nullptr;
  reg_n_refs = nullptr;
  reg_n_sets = nullptr;
  reg_n_deaths = nullptr;
  reg_live_length = nullptr;
  reg_n_calls_crossed = nullptr;
  basic_block_live_at_start = nullptr;
  basic_block_head = nullptr;
  basic_block_end = nullptr;
  n_basic_blocks = 0;
  regset_size = 0;
  regset_bytes = 0;
  reload_completed = 0;
  frame_pointer_needed = 0;
  immediate_size_expand = 0;
  active_pass_ = RtlPassKind::kRtl;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  current_function_epilogue_delay_list = nullptr;
  current_function_return_rtx = nullptr;
  clear_const_double_mem();
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
  } else if (base == "char") {
    t = char_type_node;
  } else if (base == "s8" || base == "signed char" || base == "int8_t") {
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
  pool_symbol_map_.clear();
  insn_uid_to_rpo_block_.clear();
  const_labelno = 0;
  ResetVirtualRegisterFlags();
  init_insn_lengths();
  clear_const_double_mem();
  temporary_allocation();

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
  // Reset `regclass.c`'s static `prefclass` pointer (`regclass.c:620-623`)
  // after `temporary_allocation()` frees `rtl_obstack`, mirroring
  // `toplev.c:2957`. Without this, `prefclass` dangles across functions and
  // `record_reg_classes` (`regclass.c:1299`) reads stale/freed obstack memory
  // on `pass == 0` under `-O1` (`flag_expensive_optimizations == 0`).
  regclass_init();
  reg_renumber = nullptr;
  reg_n_refs = nullptr;
  reg_n_sets = nullptr;
  reg_n_deaths = nullptr;
  reg_live_length = nullptr;
  reg_n_calls_crossed = nullptr;
  basic_block_live_at_start = nullptr;
  basic_block_head = nullptr;
  basic_block_end = nullptr;
  n_basic_blocks = 0;
  regset_size = 0;
  regset_bytes = 0;
  reload_completed = 0;
  frame_pointer_needed = 0;
  immediate_size_expand = 0;
  active_pass_ = RtlPassKind::kRtl;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  current_function_epilogue_delay_list = nullptr;
  current_function_return_rtx = nullptr;

  clear_const_double_mem();
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
  if (absl::StartsWith(label_token, "L") && label_token.size() > 1) {
    int explicit_num = 0;
    if (absl::SimpleAtoi(label_token.substr(1), &explicit_num) && explicit_num > 0) {
      lbl->fld[3].rtint = explicit_num;
    }
  }
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

  // Parse optional `/v`, `/i`, `/u`, `/s`, `/f` flags immediately following code name.
  bool flag_v = false;
  bool flag_i = false;
  bool flag_u = false;
  bool flag_s = false;
  bool flag_f = false;
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
    if (f == 'f') flag_f = true;
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

  if (rtx_code == kCodeLabel) {
    std::string_view label_tok = ReadRtlToken(text, pos);
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
    return GetOrCreateLabel(label_tok.empty() ? "__anon_lbl" : label_tok);
  }

  Rtx rt = rtx_alloc(rtx_code);
  rt->mode = mode;
  rt->volatil = flag_v ? 1 : 0;
  rt->integrated = flag_i ? 1 : 0;
  // On `SYMBOL_REF`, `rt->unchanging` is `CONSTANT_POOL_ADDRESS_P`, which causes
  // `varasm.c:find_pool_constant` to abort unless the `char*` pointer was
  // allocated by `force_const_mem()` in the active `const_rtx_sym_hash_table`.
  rt->unchanging = (flag_u && rtx_code != kSymbolRef) ? 1 : 0;
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
      if (rtx_code == kInsnList && i == 0) {
        if (text[*pos] != '(') {
          ReadRtlToken(text, pos);
        }
        Rtx dummy_note = rtx_alloc(kNote);
        for (int k = 0; k < rtx_length[kNote]; ++k) {
          dummy_note->fld[k].rtx = nullptr;
        }
        dummy_note->fld[4].rtint = kNoteInsnDeleted;
        rt->fld[i].rtx = dummy_note;
      } else if (text[*pos] == '(') {
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

  // Capture any trailing register name token (`$0`, `argp`, `$arg`, `v0`, ...)
  // on `(reg:M regno [name])` before skipping to the closing ')'.
  std::string reg_name_tok;
  if (rtx_code == kReg) {
    SkipRtlWhitespace(text, pos);
    if (*pos < text.size() && text[*pos] != ')' && text[*pos] != '(' && text[*pos] != '[') {
      reg_name_tok = std::string(ReadRtlToken(text, pos));
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

  if (rtx_code == kLabelRef) {
    if (rt->fld[0].rtx == nullptr) {
      rt->fld[0].rtx = GetOrCreateLabel("__anon_lbl");
    }
    rt->fld[0].rtx->fld[5].rtint += 1;
  }

  // Canonicalize `(reg:M regno)` while preserving `/v`, `/i`, `/u`, `/s`, `/f`
  // flags and disambiguating `arg_pointer_rtx` (`(reg:SI 0 argp)`) from hardware
  // `$zero` (`(reg:SI 0 $0)`).
  if (rtx_code == kReg) {
    const int regno = rt->fld[0].rtint;
    const bool is_argp_tok = (reg_name_tok == "argp" || reg_name_tok == "$arg");
    if (regno >= kFirstPseudoRegister) {
      if (regno_reg_rtx != nullptr) {
        EnsurePseudoRegsUpTo(regno, mode);
        Rtx canon = regno_reg_rtx[regno];
        if (canon != nullptr) {
          canon->mode = mode;
          if (flag_v) canon->volatil = 1;
          if (flag_i) canon->integrated = 1;
          if (flag_u) canon->unchanging = 1;
          if (flag_s) canon->in_struct = 1;
          if (flag_f && regno_pointer_flag != nullptr && regno >= 0 &&
              regno < regno_pointer_flag_length) {
            mark_reg_pointer(canon);
          }
          return canon;
        }
      }
      return rt;
    } else if (regno == 0) {
      if (is_argp_tok && !flag_v && !flag_i && !flag_u && !flag_s && arg_pointer_rtx != nullptr &&
          mode == kSiMode) {
        return arg_pointer_rtx;
      }
      // Hardware `$zero` (`$0`): return freshly allocated `rt` (`rt != arg_pointer_rtx`).
      // Never call `gen_rtx(kReg, mode, 0)` here, because `emit-rtl.c:gen_rtx`
      // returns `arg_pointer_rtx` when `regno == ARG_POINTER_REGNUM (0)` and
      // `mode == Pmode && !reload_in_progress`.
      return rt;
    } else {
      if (flag_f && regno >= 0 && regno_pointer_flag != nullptr) {
        if (regno >= regno_pointer_flag_length) {
          EnsurePseudoRegsUpTo(regno, mode);
        }
        if (regno < regno_pointer_flag_length) {
          mark_reg_pointer(rt);
        }
      }
      if (!flag_v && !flag_i && !flag_u && !flag_s) {
        return gen_rtx(kReg, mode, regno);
      }
    }
  } else if (rtx_code == kSymbolRef) {
    const std::string sym_str = rt->fld[0].rtstr != nullptr ? rt->fld[0].rtstr : "";
    if (flag_u) {
      auto it = pool_symbol_map_.find(sym_str);
      if (it != pool_symbol_map_.end() && it->second != nullptr) {
        return it->second;
      }
      int label_num = 0;
      if (ParseLcLabelNumber(sym_str, &label_num)) {
        Stage0ConstantPoolEntry synth_cp{
            .label_name = absl::StrCat("$LC", label_num),
            .section_name = ".rdata",
            .byte_size = 4,
            .words = {0},
        };
        Rtx registered = RegisterConstantPoolEntry(label_num, synth_cp, kSfMode);
        if (registered != nullptr) {
          return registered;
        }
      }
    }
  } else if (rtx_code == kConstInt) {
    return gen_rtx(kConstInt, kVoidMode, rt->fld[0].rtint);
  } else if (rtx_code == kConstDouble) {
    if (rt->fld[0].rtx == nullptr && cc0_rtx != nullptr) {
      rt->fld[0].rtx = cc0_rtx;
    }
    if (!flag_v && !flag_i && !flag_u && !flag_s && rt->fld[0].rtx == cc0_rtx &&
        rt->fld[2].rtint == 0 && rt->fld[3].rtint == 0 && (mode == kSfMode || mode == kDfMode)) {
      return immed_real_const_1(0.0, mode);
    }
    (&rt->fld[2].rtint)[1] = rt->fld[3].rtint;
    if (rt->fld[0].rtx == cc0_rtx && mode >= 0 && mode < kMaxMachineMode) {
      for (int tiny_idx = 0; tiny_idx < 3; ++tiny_idx) {
        Rtx tiny = const_tiny_rtx[tiny_idx][mode];
        if (tiny != nullptr && tiny->code == kConstDouble &&
            tiny->fld[2].rtint == rt->fld[2].rtint && tiny->fld[3].rtint == rt->fld[3].rtint) {
          return tiny;
        }
      }
    }
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
  {
    ScopedArgPointerSentinel argp_guard;
    print_rtl(mem_fp, rtx);
  }
  std::fclose(mem_fp);
  std::string raw(buf != nullptr ? buf : "", len);
  std::free(buf);
  NormalizePrintedRtl(&raw);
  return CanonicalizeRtlPattern(raw);
}

std::string GccContext::DumpInsnChain(Rtx first_insn) const {
  Rtx head = first_insn != nullptr ? first_insn : get_insns();
  if (head == nullptr) {
    return "";
  }
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  {
    ScopedArgPointerSentinel argp_guard;
    print_rtl(mem_fp, head);
  }
  std::fclose(mem_fp);
  std::string result(buf != nullptr ? buf : "", len);
  std::free(buf);
  NormalizePrintedRtl(&result);
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
  int container_mode = kVoidMode;
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
        std::string_view mode_tok = ReadRtlToken(s, &pos);
        for (int m = 0; m < kNumMachineModes; ++m) {
          if (mode_name[m] != nullptr && mode_tok == mode_name[m]) {
            container_mode = m;
            break;
          }
        }
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

  std::string_view pat_str = body_and_notes;
  std::string_view notes_str;
  std::string_view call_fusage_str;
  if (!body_and_notes.empty() && body_and_notes.front() == '(') {
    const size_t pat_close = FindMatchingCloseParen(body_and_notes, 0);
    if (pat_close != std::string_view::npos) {
      pat_str = body_and_notes.substr(0, pat_close + 1);
      size_t rpos = pat_close + 1;
      while (rpos < body_and_notes.size()) {
        SkipRtlWhitespace(body_and_notes, &rpos);
        if (rpos >= body_and_notes.size()) {
          break;
        }
        if (body_and_notes[rpos] == '{') {
          while (rpos < body_and_notes.size() && body_and_notes[rpos] != '}') {
            ++rpos;
          }
          if (rpos < body_and_notes.size()) {
            ++rpos;
          }
          continue;
        }
        if (body_and_notes[rpos] != '(') {
          ReadRtlToken(body_and_notes, &rpos);
          continue;
        }
        const size_t sub_close = FindMatchingCloseParen(body_and_notes, rpos);
        if (sub_close == std::string_view::npos) {
          break;
        }
        std::string_view sub_sexpr = body_and_notes.substr(rpos, sub_close - rpos + 1);
        rpos = sub_close + 1;
        if (absl::StartsWith(sub_sexpr, "(expr_list:") ||
            absl::StartsWith(sub_sexpr, "(insn_list:")) {
          notes_str = sub_sexpr;
        } else if (absl::StartsWith(sub_sexpr, "(expr_list") ||
                   absl::StartsWith(sub_sexpr, "(insn_list")) {
          if (container_code == kCallInsn || (!is_container && IsCallPatternText(pat_str))) {
            call_fusage_str = sub_sexpr;
          } else if (notes_str.empty()) {
            notes_str = sub_sexpr;
          }
        }
      }
    }
  }
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

  if (container_mode != kVoidMode) insn->mode = container_mode;
  if (flag_v) insn->volatil = 1;
  if (flag_i) insn->integrated = 1;
  if (flag_u) insn->unchanging = 1;
  if (flag_s) insn->in_struct = 1;

  if (!notes_str.empty()) {
    insn->fld[6].rtx = ParseRtx(notes_str);
    if (pat != nullptr && insn->fld[6].rtx != nullptr) {
      std::vector<Rtx> clobber_scratches;
      std::vector<Rtx> set_dest_regs;
      if (pat->code == kClobber && pat->fld[0].rtx != nullptr &&
          pat->fld[0].rtx->code == kScratch) {
        clobber_scratches.push_back(pat->fld[0].rtx);
      } else if (pat->code == kSet && pat->fld[0].rtx != nullptr && pat->fld[0].rtx->code == kReg) {
        set_dest_regs.push_back(pat->fld[0].rtx);
      } else if (pat->code == kParallel && pat->fld[0].rtvec != nullptr) {
        for (int i = 0; i < pat->fld[0].rtvec->num_elem; ++i) {
          Rtx elem = pat->fld[0].rtvec->elem[i].rtx;
          if (elem != nullptr && elem->code == kClobber && elem->fld[0].rtx != nullptr &&
              elem->fld[0].rtx->code == kScratch) {
            clobber_scratches.push_back(elem->fld[0].rtx);
          } else if (elem != nullptr && elem->code == kSet && elem->fld[0].rtx != nullptr &&
                     elem->fld[0].rtx->code == kReg) {
            set_dest_regs.push_back(elem->fld[0].rtx);
          }
        }
      }
      if (!clobber_scratches.empty() || !set_dest_regs.empty()) {
        bool reverse_scratch_order = false;
        if (clobber_scratches.size() >= 2 &&
            (active_pass_ == RtlPassKind::kCombine || active_pass_ == RtlPassKind::kSched ||
             active_pass_ == RtlPassKind::kLreg)) {
          for (Rtx dst_reg : set_dest_regs) {
            if (dst_reg != nullptr && dst_reg->fld[0].rtint >= 0 && dst_reg->fld[0].rtint < 64) {
              reverse_scratch_order = true;
              break;
            }
          }
          if (!reverse_scratch_order && absl::StrContains(pat_str, "(truncate:")) {
            reverse_scratch_order = true;
          }
        }
        std::vector<bool> used(clobber_scratches.size(), false);
        for (Rtx note = insn->fld[6].rtx; note != nullptr; note = note->fld[1].rtx) {
          if (note->code == kExprList && note->mode == kRegUnused && note->fld[0].rtx != nullptr) {
            if (note->fld[0].rtx->code == kScratch) {
              for (size_t step = 0; step < clobber_scratches.size(); ++step) {
                const size_t i =
                    reverse_scratch_order ? (clobber_scratches.size() - 1 - step) : step;
                if (!used[i] && clobber_scratches[i]->mode == note->fld[0].rtx->mode) {
                  note->fld[0].rtx = clobber_scratches[i];
                  used[i] = true;
                  break;
                }
              }
            } else if (note->fld[0].rtx->code == kReg) {
              for (Rtx dst_reg : set_dest_regs) {
                if (dst_reg->fld[0].rtint == note->fld[0].rtx->fld[0].rtint &&
                    dst_reg->mode == note->fld[0].rtx->mode) {
                  note->fld[0].rtx = dst_reg;
                  break;
                }
              }
            }
          }
        }
      }
    }
  }

  if (container_code == kCallInsn && !call_fusage_str.empty()) {
    insn->fld[7].rtx = ParseRtx(call_fusage_str);
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

namespace {

constexpr std::array<int, 8> kMipsArgRegnos = {4, 5, 6, 7, 44, 45, 46, 47};

int ArgRegIndex(int regno) {
  if (regno >= 4 && regno <= 7) {
    return regno - 4;
  }
  if (regno >= 44 && regno <= 47) {
    return 4 + (regno - 44);
  }
  return -1;
}

void PopulateMissingPostFlowCallRegDeadNotes(const RtlControlFlowGraph& cfg, Rtx c_insn) {
  if (c_insn == nullptr || c_insn->code != kCallInsn || c_insn->fld[6].rtx != nullptr ||
      c_insn->fld[7].rtx == nullptr) {
    return;
  }
  const bool is_post_flow_pass = (cfg.pass != RtlPassKind::kRtl && cfg.pass != RtlPassKind::kJump &&
                                  cfg.pass != RtlPassKind::kCse && cfg.pass != RtlPassKind::kLoop &&
                                  cfg.pass != RtlPassKind::kCse2);
  if (!is_post_flow_pass && cfg.sideband.basic_block_live_at_start.empty()) {
    return;
  }
  absl::flat_hash_map<int, Rtx> used_arg_regs;
  for (Rtx node = c_insn->fld[7].rtx; node != nullptr; node = node->fld[1].rtx) {
    if (node->code == kExprList && node->fld[0].rtx != nullptr && node->fld[0].rtx->code == kUse &&
        node->fld[0].rtx->fld[0].rtx != nullptr && node->fld[0].rtx->fld[0].rtx->code == kReg) {
      Rtx reg = node->fld[0].rtx->fld[0].rtx;
      const int regno = reg->fld[0].rtint;
      if (ArgRegIndex(regno) >= 0 && !used_arg_regs.contains(regno)) {
        used_arg_regs.emplace(regno, gen_rtx(kReg, reg->mode, regno));
      }
    }
  }
  if (used_arg_regs.empty()) {
    return;
  }
  std::vector<int> sorted_regnos;
  sorted_regnos.reserve(used_arg_regs.size());
  for (const auto& [regno, _] : used_arg_regs) {
    sorted_regnos.push_back(regno);
  }
  std::sort(sorted_regnos.begin(), sorted_regnos.end());
  Rtx notes = nullptr;
  for (int regno : sorted_regnos) {
    notes = gen_rtx(kExprList, kRegDead, used_arg_regs.at(regno), notes);
  }
  c_insn->fld[6].rtx = notes;
}

void ReconstructCallInsnFunctionUsage(const RtlControlFlowGraph& cfg, Rtx first,
                                      const absl::flat_hash_map<int, int>& insn_uid_to_rpo_block,
                                      const absl::flat_hash_set<Rtx>& explicit_fusage_calls) {
  using ArgState = std::array<int, 8>;
  constexpr ArgState kEmptyState = {-1, -1, -1, -1, -1, -1, -1, -1};

  const size_t num_blocks = cfg.rpo_blocks.size();
  if (num_blocks == 0) {
    return;
  }

  std::vector<std::vector<Rtx>> block_insns_ordered(num_blocks);
  bool has_unpopulated_call = false;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kCallInsn) {
      if (explicit_fusage_calls.contains(insn) || insn->fld[7].rtx != nullptr) {
        PopulateMissingPostFlowCallRegDeadNotes(cfg, insn);
      } else {
        has_unpopulated_call = true;
      }
    } else if (insn->code == kInsn && insn->fld[3].rtx != nullptr &&
               insn->fld[3].rtx->code == kSequence && insn->fld[3].rtx->fld[0].rtvec != nullptr) {
      Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
      for (int j = 0; j < vec->num_elem; ++j) {
        Rtx sub = vec->elem[j].rtx;
        if (sub != nullptr && sub->code == kCallInsn && !explicit_fusage_calls.contains(sub) &&
            sub->fld[7].rtx == nullptr) {
          has_unpopulated_call = true;
        }
      }
    }
    if (insn->fld[0].rtint >= 0) {
      auto blk_it = insn_uid_to_rpo_block.find(insn->fld[0].rtint);
      if (blk_it != insn_uid_to_rpo_block.end() && blk_it->second >= 0 &&
          static_cast<size_t>(blk_it->second) < num_blocks) {
        block_insns_ordered[blk_it->second].push_back(insn);
      }
    }
  }
  if (!has_unpopulated_call) {
    return;
  }

  auto kill_arg_reg = [](ArgState* st, int regno, int mode) {
    const int idx = ArgRegIndex(regno);
    if (idx >= 0) {
      (*st)[idx] = -1;
      if ((mode == kDiMode || mode == kDfMode) && (regno % 2 == 0) && idx + 1 < 8) {
        (*st)[idx + 1] = -1;
      }
    }
    if ((regno % 2 == 1) && idx > 0 && ((*st)[idx - 1] == kDiMode || (*st)[idx - 1] == kDfMode)) {
      (*st)[idx - 1] = (regno >= 44) ? kSfMode : kSiMode;
    }
  };

  auto def_arg_reg = [&](ArgState* st, int regno, int mode) {
    const int idx = ArgRegIndex(regno);
    if (idx < 0) {
      return;
    }
    if ((regno % 2 == 1) && idx > 0 && ((*st)[idx - 1] == kDiMode || (*st)[idx - 1] == kDfMode)) {
      (*st)[idx - 1] = (regno >= 44) ? kSfMode : kSiMode;
    }
    (*st)[idx] = mode;
    if ((mode == kDiMode || mode == kDfMode) && (regno % 2 == 0) && idx + 1 < 8) {
      (*st)[idx + 1] = -1;
    }
  };

  auto step_non_call_insn = [&](Rtx r, ArgState* st) {
    if (r == nullptr || (r->code != kInsn && r->code != kJumpInsn) || r->fld[3].rtx == nullptr) {
      return;
    }
    // 1. Input `REG_DEAD` notes kill reaching argument values consumed by `r`.
    for (Rtx note = r->fld[6].rtx; note != nullptr; note = note->fld[1].rtx) {
      if (note->mode == kRegDead && note->fld[0].rtx != nullptr && note->fld[0].rtx->code == kReg) {
        kill_arg_reg(st, note->fld[0].rtx->fld[0].rtint, note->fld[0].rtx->mode);
      }
    }
    // 2. Pattern `CLOBBER`s and `SET`s update `st`.
    if (r->code == kInsn) {
      auto apply_elem = [&](Rtx x) {
        if (x == nullptr) {
          return;
        }
        if (x->code == kClobber && x->fld[0].rtx != nullptr) {
          Rtx dst = x->fld[0].rtx;
          if (dst->code == kSubreg && dst->fld[0].rtx != nullptr) {
            dst = dst->fld[0].rtx;
          }
          if (dst->code == kReg) {
            kill_arg_reg(st, dst->fld[0].rtint, dst->mode);
          }
        } else if (x->code == kSet && x->fld[0].rtx != nullptr) {
          Rtx dst = x->fld[0].rtx;
          if (dst->code == kReg) {
            def_arg_reg(st, dst->fld[0].rtint, dst->mode);
          } else if (dst->code == kSubreg && dst->fld[0].rtx != nullptr &&
                     dst->fld[0].rtx->code == kReg) {
            const int base_regno = dst->fld[0].rtx->fld[0].rtint + dst->fld[1].rtint;
            def_arg_reg(st, base_regno, dst->mode);
          }
        }
      };
      Rtx pat = r->fld[3].rtx;
      if (pat->code == kParallel && pat->fld[0].rtvec != nullptr) {
        Rtvec pvec = pat->fld[0].rtvec;
        for (int k = 0; k < pvec->num_elem; ++k) {
          apply_elem(pvec->elem[k].rtx);
        }
      } else {
        apply_elem(pat);
      }
    }
    // 3. Output `REG_UNUSED` notes kill dead outputs immediately.
    for (Rtx note = r->fld[6].rtx; note != nullptr; note = note->fld[1].rtx) {
      if (note->mode == kRegUnused && note->fld[0].rtx != nullptr &&
          note->fld[0].rtx->code == kReg) {
        kill_arg_reg(st, note->fld[0].rtx->fld[0].rtint, note->fld[0].rtx->mode);
      }
    }
  };

  auto step_top_insn = [&](Rtx insn, ArgState* st, const auto& on_call_cb) {
    if (insn == nullptr) {
      return;
    }
    if (insn->code == kCallInsn) {
      on_call_cb(insn, *st);
      *st = kEmptyState;
      return;
    }
    if ((insn->code == kInsn || insn->code == kJumpInsn) && insn->fld[3].rtx != nullptr &&
        insn->fld[3].rtx->code == kSequence && insn->fld[3].rtx->fld[0].rtvec != nullptr) {
      Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
      if (vec->num_elem > 0 && vec->elem[0].rtx != nullptr && vec->elem[0].rtx->code == kCallInsn) {
        for (int j = 1; j < vec->num_elem; ++j) {
          step_non_call_insn(vec->elem[j].rtx, st);
        }
        on_call_cb(vec->elem[0].rtx, *st);
        *st = kEmptyState;
      } else {
        for (int j = 0; j < vec->num_elem; ++j) {
          step_non_call_insn(vec->elem[j].rtx, st);
        }
      }
      return;
    }
    step_non_call_insn(insn, st);
  };

  // Seed `basic_block_live_at_start` entry states where explicitly provided.
  const bool post_alter_reg_pass =
      (cfg.pass == RtlPassKind::kSched2 || cfg.pass == RtlPassKind::kJump2 ||
       cfg.pass == RtlPassKind::kDbr);
  std::vector<ArgState> seed_in(num_blocks, kEmptyState);
  std::vector<bool> has_explicit_seed(num_blocks, false);
  for (size_t b = 0; b < num_blocks; ++b) {
    auto live_it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
    if (live_it == cfg.sideband.basic_block_live_at_start.end()) {
      continue;
    }
    has_explicit_seed[b] = true;
    absl::flat_hash_set<int> eff_live_hard_regs;
    for (int raw_r : live_it->second) {
      if (raw_r >= kFirstPseudoRegister) {
        if (!post_alter_reg_pass) {
          continue;
        }
        auto ren_it = cfg.sideband.reg_renumber.find(raw_r);
        if (ren_it == cfg.sideband.reg_renumber.end() || ren_it->second < 0) {
          continue;
        }
        eff_live_hard_regs.insert(ren_it->second);
      } else {
        eff_live_hard_regs.insert(raw_r);
      }
    }
    for (int r : eff_live_hard_regs) {
      const int idx = ArgRegIndex(r);
      if (idx < 0) {
        continue;
      }
      if (r >= 44) {
        if ((r % 2 == 1) && eff_live_hard_regs.contains(r - 1)) {
          continue;
        }
        seed_in[b][idx] = eff_live_hard_regs.contains(r + 1) && (r % 2 == 0) ? kDfMode : kSfMode;
      } else {
        seed_in[b][idx] = kSiMode;
      }
    }
  }

  // Forward fixed-point reaching-definitions analysis across `cfg.rpo_blocks`.
  std::vector<ArgState> in_state = seed_in;
  std::vector<ArgState> out_state(num_blocks, kEmptyState);
  auto join_states = [](const ArgState& a, const ArgState& b_st) -> ArgState {
    ArgState joined = a;
    for (size_t i = 0; i < 8; ++i) {
      if (b_st[i] < 0) {
        continue;
      }
      if (joined[i] < 0 || b_st[i] == kDiMode || b_st[i] == kDfMode) {
        joined[i] = b_st[i];
        if ((b_st[i] == kDiMode || b_st[i] == kDfMode) && (i % 2 == 0) && i + 1 < 8) {
          joined[i + 1] = -1;
        }
      }
    }
    for (size_t i = 0; i < 8; i += 2) {
      if (joined[i] == kDiMode || joined[i] == kDfMode) {
        joined[i + 1] = -1;
      }
    }
    return joined;
  };

  // Derive predecessor lists directly from `successors` so hand-built CFGs
  // that omit `.predecessors` still propagate reaching definitions.
  std::vector<std::vector<size_t>> preds(num_blocks);
  for (size_t p = 0; p < num_blocks; ++p) {
    for (size_t succ : cfg.rpo_blocks[p].successors) {
      if (succ < num_blocks) {
        preds[succ].push_back(p);
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < num_blocks; ++b) {
      ArgState cur_in = seed_in[b];
      if (!has_explicit_seed[b]) {
        for (size_t pred_b : preds[b]) {
          cur_in = join_states(cur_in, out_state[pred_b]);
        }
      }
      if (cur_in != in_state[b]) {
        in_state[b] = cur_in;
        changed = true;
      }
      ArgState cur = cur_in;
      for (Rtx insn : block_insns_ordered[b]) {
        step_top_insn(insn, &cur, [](Rtx, const ArgState&) {});
      }
      if (cur != out_state[b]) {
        out_state[b] = cur;
        changed = true;
      }
    }
  }

  // Final pass: materialize `CALL_INSN_FUNCTION_USAGE` (`fld[7]`) on synthetic calls.
  for (size_t b = 0; b < num_blocks; ++b) {
    ArgState cur = in_state[b];
    for (Rtx insn : block_insns_ordered[b]) {
      const bool is_top_call = (insn->code == kCallInsn);
      step_top_insn(insn, &cur, [&](Rtx c_insn, const ArgState& live_args) {
        if (explicit_fusage_calls.contains(c_insn) || c_insn->fld[7].rtx != nullptr) {
          return;
        }
        absl::flat_hash_map<int, Rtx> arg_regs;
        for (size_t i = 0; i < 8; ++i) {
          if (live_args[i] >= 0) {
            const int regno = kMipsArgRegnos[i];
            arg_regs.emplace(regno, gen_rtx(kReg, live_args[i], regno));
          }
        }
        for (Rtx note = c_insn->fld[6].rtx; note != nullptr; note = note->fld[1].rtx) {
          if (note->mode == kRegDead && note->fld[0].rtx != nullptr &&
              note->fld[0].rtx->code == kReg) {
            Rtx reg = note->fld[0].rtx;
            if (ArgRegIndex(reg->fld[0].rtint) >= 0 && !arg_regs.contains(reg->fld[0].rtint)) {
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
        Rtx fusage = nullptr;
        for (int regno : sorted_arg_regnos) {
          Rtx use_rtx = gen_rtx(kUse, kVoidMode, arg_regs.at(regno));
          fusage = gen_rtx(kExprList, kVoidMode, use_rtx, fusage);
        }
        c_insn->fld[7].rtx = fusage;
        if (is_top_call) {
          PopulateMissingPostFlowCallRegDeadNotes(cfg, c_insn);
        }
      });
    }
  }
}

void PopulateMaterializedSidebandState(const RtlControlFlowGraph& cfg, Rtx first, size_t num_blocks,
                                       int alloc_regs) {
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
  } else {
    reg_renumber = nullptr;
  }

  // Always allocate a fresh `reg_n_sets` buffer sized for `alloc_regs` rather
  // than reusing a pointer that may belong to a smaller function or a reset
  // `rtl_obstack` (`flow.c:life_analysis` allocates `reg_n_sets` via `oballoc`).
  reg_n_sets = AllocSidebandArray<short>(alloc_regs);
  if (!cfg.sideband.reg_n_sets.empty() && reg_n_sets != nullptr) {
    for (const auto& [regno, val] : cfg.sideband.reg_n_sets) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_n_sets[regno] = static_cast<short>(val);
      }
    }
  }

  const bool has_any_flow_sideband =
      !cfg.sideband.reg_n_refs.empty() || !cfg.sideband.reg_n_deaths.empty() ||
      !cfg.sideband.reg_live_length.empty() || !cfg.sideband.reg_n_calls_crossed.empty() ||
      !cfg.sideband.basic_block_live_at_start.empty();

  if (has_any_flow_sideband) {
    reg_n_refs = AllocSidebandArray<int>(alloc_regs);
    for (const auto& [regno, val] : cfg.sideband.reg_n_refs) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_n_refs[regno] = val;
      }
    }
    reg_n_deaths = AllocSidebandArray<short>(alloc_regs);
    for (const auto& [regno, val] : cfg.sideband.reg_n_deaths) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_n_deaths[regno] = static_cast<short>(val);
      }
    }
    reg_live_length = AllocSidebandArray<int>(alloc_regs);
    for (const auto& [regno, val] : cfg.sideband.reg_live_length) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_live_length[regno] = val;
      }
    }
    reg_n_calls_crossed = AllocSidebandArray<int>(alloc_regs);
    for (const auto& [regno, val] : cfg.sideband.reg_n_calls_crossed) {
      if (regno >= 0 && regno < alloc_regs) {
        reg_n_calls_crossed[regno] = val;
      }
    }
  } else {
    reg_n_refs = nullptr;
    reg_n_deaths = nullptr;
    reg_live_length = nullptr;
    reg_n_calls_crossed = nullptr;
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
}

}  // namespace

Rtx GccContext::MaterializeFromCfg(const RtlControlFlowGraph& cfg, bool reset_function) {
  if (reset_function) {
    BeginFunction(current_func_name_);
    active_pass_ = cfg.pass;
    active_is_variadic_ = cfg.sideband.is_variadic;
    active_returns_aggregate_in_memory_ = cfg.sideband.returns_aggregate_in_memory;
    active_prologue_args_ = cfg.sideband.prologue_args;

    if ((active_is_variadic_ || active_returns_aggregate_in_memory_ ||
         !active_prologue_args_.empty()) &&
        current_function_decl != nullptr) {
      constexpr int kBlkMode = 26;
      auto make_struct_arg_type = [&](int byte_size, int align_bits, int gcc_mode) -> void* {
        void* rtype = make_node(kRecordTypeCode);
        void* sz_cst = build_int_2_wide(std::max(1, byte_size) * 8, 0);
        *reinterpret_cast<void**>(static_cast<char*>(rtype) + 32) = sz_cst;
        *reinterpret_cast<uint8_t*>(static_cast<char*>(rtype) + 53) =
            static_cast<uint8_t>(gcc_mode);
        *reinterpret_cast<uint32_t*>(static_cast<char*>(rtype) + 56) =
            static_cast<uint32_t>(std::max(8, align_bits));
        return rtype;
      };
      auto build_arg_tree_type = [&](const MipsPrologueArgSpec& spec) -> void* {
        switch (spec.kind) {
          case MipsPrologueArgKind::kGeneralWord:
            return integer_type_node;
          case MipsPrologueArgKind::kInt64:
            return long_long_integer_type_node;
          case MipsPrologueArgKind::kFloat32:
            return float_type_node;
          case MipsPrologueArgKind::kFloat64:
            return double_type_node;
          case MipsPrologueArgKind::kStructOrUnion:
            return make_struct_arg_type(spec.byte_size, spec.align_bits, spec.gcc_mode);
        }
        return integer_type_node;
      };

      void* ret_type = void_type_node;
      if (active_returns_aggregate_in_memory_) {
        ret_type = make_struct_arg_type(/*byte_size=*/16, /*align_bits=*/32, kBlkMode);
        void* res_decl = *reinterpret_cast<void**>(static_cast<char*>(current_function_decl) + 80);
        if (res_decl != nullptr) {
          *reinterpret_cast<void**>(static_cast<char*>(res_decl) + 8) = ret_type;
          layout_decl(res_decl, 0);
        }
      }

      void* arg_types = active_is_variadic_ ? nullptr : tree_cons(nullptr, void_type_node, nullptr);
      for (auto it = active_prologue_args_.rbegin(); it != active_prologue_args_.rend(); ++it) {
        arg_types = tree_cons(nullptr, build_arg_tree_type(*it), arg_types);
      }
      if (active_is_variadic_ && arg_types == nullptr) {
        arg_types = tree_cons(nullptr, integer_type_node, nullptr);
      }
      void* fn_type = build_function_type(ret_type, arg_types);
      *reinterpret_cast<void**>(static_cast<char*>(current_function_decl) + 8) = fn_type;

      void* parm_head = nullptr;
      void* parm_tail = nullptr;
      for (size_t i = 0; i < active_prologue_args_.size(); ++i) {
        void* p_type = build_arg_tree_type(active_prologue_args_[i]);
        std::string p_name = absl::StrCat("arg", i);
        void* p_id = get_identifier(p_name.c_str());
        void* parm = build_decl(kParmDeclCode, p_id, p_type);
        *reinterpret_cast<void**>(static_cast<char*>(parm) + 64) = current_function_decl;
        layout_decl(parm, 0);
        *reinterpret_cast<void**>(static_cast<char*>(parm) + 88) = p_type;
        if (parm_head == nullptr) {
          parm_head = parm;
        } else {
          SetTreeChain(parm_tail, parm);
        }
        parm_tail = parm;
      }
      *reinterpret_cast<void**>(static_cast<char*>(current_function_decl) + 72) = parm_head;
    }
  } else {
    init_insn_lengths();
    label_map_.clear();
    insn_uid_to_rpo_block_.clear();
    Rtx head_note = get_insns();
    if (head_note != nullptr) {
      head_note->fld[2].rtx = nullptr;
      set_last_insn(head_note);
    }
    emit_note(nullptr, kNoteInsnFunctionBeg);
  }
  ResetVirtualRegisterFlags();
  if (regno_pointer_flag != nullptr && regno_pointer_flag_length > 0) {
    std::memset(regno_pointer_flag, 0, static_cast<size_t>(regno_pointer_flag_length));
    for (Rtx sreg :
         {stack_pointer_rtx, frame_pointer_rtx, arg_pointer_rtx, virtual_incoming_args_rtx,
          virtual_stack_vars_rtx, virtual_stack_dynamic_rtx, virtual_outgoing_args_rtx}) {
      if (sreg != nullptr && sreg->fld[0].rtint >= 0 &&
          sreg->fld[0].rtint < regno_pointer_flag_length) {
        regno_pointer_flag[sreg->fld[0].rtint] = 1;
      }
    }
  }
  if (regno_reg_rtx != nullptr) {
    for (Rtx vreg : {virtual_incoming_args_rtx, virtual_stack_vars_rtx, virtual_stack_dynamic_rtx,
                     virtual_outgoing_args_rtx}) {
      if (vreg != nullptr && vreg->fld[0].rtint >= 0) {
        regno_reg_rtx[vreg->fld[0].rtint] = vreg;
      }
    }
  }

  // Register any per-function `varasm.c` constant-pool entries (`$LC<n>`) from
  // `cfg.sideband.constant_pool` via `force_const_mem` BEFORE parsing RTL
  // instructions so `(symbol_ref/u:SI ("$LC<n>"))` resolves directly to its
  // active `const_rtx_sym_hash_table` `SYMBOL_REF` (`CONSTANT_POOL_ADDRESS_P == 1`).
  if (!cfg.sideband.constant_pool.empty()) {
    const absl::flat_hash_map<int, int> preferred_modes = InferPreferredPoolModesFromCfg(cfg);
    int max_lc_num = -1;
    for (const Stage0ConstantPoolEntry& cp : cfg.sideband.constant_pool) {
      int lc_num = -1;
      if (!ParseLcLabelNumber(cp.label_name, &lc_num)) {
        continue;
      }
      int pref_mode = kVoidMode;
      if (auto it = preferred_modes.find(lc_num); it != preferred_modes.end()) {
        pref_mode = it->second;
      }
      RegisterConstantPoolEntry(lc_num, cp, pref_mode);
      max_lc_num = std::max(max_lc_num, lc_num);
    }
    if (max_lc_num >= 0) {
      const_labelno = std::max(const_labelno, max_lc_num + 1);
    }
  }

  const size_t num_blocks = cfg.rpo_blocks.size();
  std::vector<Rtx> block_labels(num_blocks, nullptr);
  std::vector<bool> block_needs_label(num_blocks, false);
  std::vector<std::string> block_tablejump_tokens(num_blocks);
  absl::flat_hash_set<std::string> tablejump_label_tokens;

  auto is_tablejump_pattern_text = [](std::string_view pat) {
    return absl::StrContains(pat, "(set (pc) (reg:") && absl::StrContains(pat, "(use (label_ref");
  };
  auto is_control_branch_or_jump_text = [](std::string_view pat) {
    const std::string_view s = absl::StripAsciiWhitespace(pat);
    return IsJumpPatternText(s) || absl::StartsWith(s, "(jump_insn") ||
           (absl::StartsWith(s, "(sequence") && absl::StrContains(s, "(jump_insn"));
  };
  auto extract_tablejump_label_token = [](std::string_view pat) -> std::string {
    const size_t use_pos = pat.find("(use (label_ref");
    if (use_pos == std::string_view::npos) {
      return "";
    }
    const std::vector<std::string> refs = ExtractLabelRefTokens(pat.substr(use_pos));
    return refs.empty() ? "" : refs[0];
  };

  // First pass: map `(label_ref <tok>)` tokens in jump instructions to their
  // target basic blocks and determine which blocks require a `CODE_LABEL`.
  absl::flat_hash_map<std::string, size_t> token_to_block;
  std::vector<std::string> ordered_tokens;

  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
    if (blk.has_label) {
      block_needs_label[b] = true;
    }
    bool ends_with_uncond = false;
    if (!blk.instructions.empty()) {
      std::string_view last_pat = blk.instructions.back();
      ends_with_uncond =
          IsUnconditionalJumpOrReturnPatternText(last_pat) || is_tablejump_pattern_text(last_pat);
      if (is_tablejump_pattern_text(last_pat)) {
        std::string tbl_tok = extract_tablejump_label_token(last_pat);
        if (!tbl_tok.empty()) {
          block_tablejump_tokens[b] = tbl_tok;
          tablejump_label_tokens.insert(tbl_tok);
        }
        for (size_t succ : blk.addr_vec_targets) {
          if (succ < num_blocks) {
            block_needs_label[succ] = true;
          }
        }
      } else if (is_control_branch_or_jump_text(last_pat)) {
        std::vector<std::string> refs = ExtractLabelRefTokens(last_pat);
        if ((!refs.empty() || absl::StrContains(last_pat, "(label_ref)")) &&
            !blk.successors.empty()) {
          size_t target_b = blk.successors[0];
          if (blk.successors.size() >= 2 && blk.successors[0] == b + 1) {
            target_b = blk.successors[1];
          }
          if (target_b < num_blocks) {
            block_needs_label[target_b] = true;
            if (!refs.empty() && !absl::StartsWith(refs[0], "__blk_") &&
                !token_to_block.contains(refs[0])) {
              token_to_block.emplace(refs[0], target_b);
              ordered_tokens.push_back(refs[0]);
            }
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
        if (tablejump_label_tokens.contains(tok)) {
          continue;
        }
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
    if (!block_tablejump_tokens[b].empty() && !label_map_.contains(block_tablejump_tokens[b])) {
      label_map_[block_tablejump_tokens[b]] = gen_label_rtx();
    }
  }
  for (const auto& [tok, target_b] : token_to_block) {
    if (target_b < num_blocks && block_labels[target_b] != nullptr) {
      label_map_[tok] = block_labels[target_b];
    }
  }

  // Second pass: emit basic blocks in linear order.
  absl::flat_hash_set<Rtx> explicit_fusage_calls;
  for (size_t b = 0; b < num_blocks; ++b) {
    Rtx prev_tail = get_last_insn();
    const auto& blk = cfg.rpo_blocks[b];
    if (block_labels[b] != nullptr) {
      emit_label(block_labels[b]);
    } else if (blk.has_unreachable_entry_label) {
      emit_label(gen_label_rtx());
    }
    bool emitted_uncond_jump_or_return = false;
    bool emitted_any_jump = false;
    absl::flat_hash_set<int> entry_word_copied_arg_regs;
    for (size_t insn_idx = 0; insn_idx < blk.instructions.size(); ++insn_idx) {
      const std::string& pat_str = blk.instructions[insn_idx];
      if (b == 0 && cfg.pass == RtlPassKind::kCombine) {
        const std::string_view core_pat = SplitPatternAndTrailingRegNotes(pat_str).first;
        if (absl::StartsWith(core_pat, "(set (reg") && (absl::StrContains(core_pat, " (reg:SI ") ||
                                                        absl::StrContains(core_pat, " (reg:DI "))) {
          for (int arg_r = 4; arg_r <= 7; ++arg_r) {
            if (absl::StrContains(core_pat, absl::StrFormat(" %d ", arg_r))) {
              entry_word_copied_arg_regs.insert(arg_r);
            }
          }
        } else if ((absl::StartsWith(core_pat, "(set (reg/v:QI ") ||
                    absl::StartsWith(core_pat, "(set (reg/v:HI ") ||
                    absl::StartsWith(core_pat, "(set (reg:QI ") ||
                    absl::StartsWith(core_pat, "(set (reg:HI ")) &&
                   (absl::StrContains(core_pat, " (reg:QI ") ||
                    absl::StrContains(core_pat, " (reg:HI "))) {
          for (int arg_r = 4; arg_r <= 7; ++arg_r) {
            if ((absl::StrContains(core_pat, absl::StrFormat(" (reg:QI %d ", arg_r)) ||
                 absl::StrContains(core_pat, absl::StrFormat(" (reg:HI %d ", arg_r))) &&
                !entry_word_copied_arg_regs.contains(arg_r)) {
              emit_note(nullptr, kNoteInsnDeleted);
              entry_word_copied_arg_regs.insert(arg_r);
              break;
            }
          }
        }
      }
      std::string resolved_pat = pat_str;
      if (insn_idx + 1 == blk.instructions.size() && is_control_branch_or_jump_text(resolved_pat) &&
          !is_tablejump_pattern_text(resolved_pat) &&
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
          const std::string_view resolved_view = resolved_pat;
          const std::vector<std::string> sub_refs =
              ExtractLabelRefTokens(resolved_view.substr(pos, close - pos + 1));
          if (!sub_refs.empty() && (tablejump_label_tokens.contains(sub_refs[0]) ||
                                    absl::StartsWith(sub_refs[0], "__blk_"))) {
            pos = close + 1;
            continue;
          }
          resolved_pat.replace(pos, close - pos + 1, named_ref);
          pos += named_ref.size();
        }
      }
      Rtx insn = EmitPattern(resolved_pat);
      if (insn == nullptr) {
        continue;
      }
      const RtlInsn* rinsn = (insn_idx < blk.rtl_insns.size()) ? &blk.rtl_insns[insn_idx] : nullptr;
      auto apply_explicit_call_fusage = [&](Rtx c_insn) {
        if (c_insn == nullptr || c_insn->code != kCallInsn) {
          return;
        }
        if (rinsn != nullptr && rinsn->has_explicit_call_fusage) {
          c_insn->fld[7].rtx =
              rinsn->call_fusage.empty()
                  ? nullptr
                  : ParseRtx(BuildCallFusageChainExpr(rinsn->call_fusage).ToGccPrintRtlString());
          explicit_fusage_calls.insert(c_insn);
        } else if (c_insn->fld[7].rtx != nullptr) {
          explicit_fusage_calls.insert(c_insn);
        }
      };
      if (insn->code == kCallInsn) {
        apply_explicit_call_fusage(insn);
      } else if (insn->code == kInsn && insn->fld[3].rtx != nullptr &&
                 insn->fld[3].rtx->code == kSequence && insn->fld[3].rtx->fld[0].rtvec != nullptr) {
        Rtvec cvec = insn->fld[3].rtx->fld[0].rtvec;
        for (int j = 0; j < cvec->num_elem; ++j) {
          apply_explicit_call_fusage(cvec->elem[j].rtx);
        }
      }
      if (insn->code == kJumpInsn) {
        emitted_any_jump = true;
        if (IsUnconditionalJumpOrReturnPatternText(pat_str) || is_tablejump_pattern_text(pat_str)) {
          emitted_uncond_jump_or_return = true;
        }
      } else if (insn->code == kInsn && insn->fld[3].rtx != nullptr &&
                 insn->fld[3].rtx->code == kSequence) {
        Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
        if (vec != nullptr && vec->num_elem > 0 && vec->elem[0].rtx != nullptr &&
            vec->elem[0].rtx->code == kJumpInsn) {
          emitted_any_jump = true;
          std::string first_fmt = FormatRtx(vec->elem[0].rtx->fld[3].rtx);
          if (IsUnconditionalJumpOrReturnPatternText(first_fmt) ||
              is_tablejump_pattern_text(first_fmt)) {
            emitted_uncond_jump_or_return = true;
          }
          if (cfg.pass == RtlPassKind::kDbr && !cfg.sideband.basic_block_live_at_start.empty()) {
            auto collect_slot_dst_regs = [](Rtx s_insn, std::vector<int>* out_regs) {
              if (s_insn == nullptr || s_insn->fld[3].rtx == nullptr) {
                return;
              }
              auto add_set_dst = [&](Rtx elem) {
                if (elem == nullptr || elem->code != kSet || elem->fld[0].rtx == nullptr) {
                  return;
                }
                Rtx dst = elem->fld[0].rtx;
                if (dst->code == kSubreg && dst->fld[0].rtx != nullptr) {
                  dst = dst->fld[0].rtx;
                }
                if (dst->code == kReg && dst->fld[0].rtint > 0 &&
                    dst->fld[0].rtint < kFirstPseudoRegister) {
                  out_regs->push_back(dst->fld[0].rtint);
                }
              };
              Rtx spat = s_insn->fld[3].rtx;
              if (spat->code == kParallel && spat->fld[0].rtvec != nullptr) {
                for (int m = 0; m < spat->fld[0].rtvec->num_elem; ++m) {
                  add_set_dst(spat->fld[0].rtvec->elem[m].rtx);
                }
              } else {
                add_set_dst(spat);
              }
            };
            auto all_regs_live_at_block = [&](size_t blk_i, const std::vector<int>& regs) {
              if (regs.empty()) {
                return false;
              }
              auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(blk_i));
              if (it == cfg.sideband.basic_block_live_at_start.end()) {
                return false;
              }
              for (int r : regs) {
                if (!it->second.contains(r)) {
                  return false;
                }
              }
              return true;
            };
            for (int k = 1; k < vec->num_elem; ++k) {
              Rtx slot_insn = vec->elem[k].rtx;
              if (slot_insn != nullptr && slot_insn->code == kInsn && slot_insn->in_struct == 0) {
                std::vector<int> slot_dst_regs;
                collect_slot_dst_regs(slot_insn, &slot_dst_regs);
                if (emitted_uncond_jump_or_return || all_regs_live_at_block(b, slot_dst_regs)) {
                  emit_insn_before(gen_rtx(kUse, kVoidMode, slot_insn), insn);
                } else if (b + 1 < num_blocks && all_regs_live_at_block(b + 1, slot_dst_regs)) {
                  emit_insn(gen_rtx(kUse, kVoidMode, slot_insn));
                }
              }
            }
          }
        }
      }
    }
    // Emit an explicit unconditional jump if block `b` does not end with a jump
    // or return and its sole successor is not `b + 1`.
    if (!emitted_uncond_jump_or_return && !emitted_any_jump && blk.successors.size() == 1 &&
        blk.successors[0] != b + 1 && blk.successors[0] < num_blocks &&
        block_labels[blk.successors[0]] != nullptr) {
      Rtx target_lbl = block_labels[blk.successors[0]];
      Rtx jmp_pat = gen_rtx(kSet, kVoidMode, pc_rtx, gen_rtx(kLabelRef, kVoidMode, target_lbl));
      Rtx jmp = emit_jump_insn(jmp_pat);
      jmp->fld[7].rtx = target_lbl;
      target_lbl->fld[5].rtint += 1;
      emitted_uncond_jump_or_return = true;
    }
    if (emitted_uncond_jump_or_return || blk.has_trailing_barrier) {
      emit_barrier();
    }
    for (Rtx scan = (prev_tail != nullptr ? prev_tail->fld[2].rtx : get_insns()); scan != nullptr;
         scan = scan->fld[2].rtx) {
      if (scan->fld[0].rtint >= 0) {
        insn_uid_to_rpo_block_[scan->fld[0].rtint] = static_cast<int>(b);
      }
      if ((scan->code == kInsn || scan->code == kJumpInsn || scan->code == kCallInsn) &&
          scan->fld[3].rtx != nullptr && scan->fld[3].rtx->code == kSequence &&
          scan->fld[3].rtx->fld[0].rtvec != nullptr) {
        Rtvec seq = scan->fld[3].rtx->fld[0].rtvec;
        for (int k = 0; k < seq->num_elem; ++k) {
          if (seq->elem[k].rtx != nullptr && seq->elem[k].rtx->fld[0].rtint >= 0) {
            insn_uid_to_rpo_block_[seq->elem[k].rtx->fld[0].rtint] = static_cast<int>(b);
          }
        }
      }
    }
    if (!block_tablejump_tokens[b].empty()) {
      Rtx tbl_lbl = GetOrCreateLabel(block_tablejump_tokens[b]);
      emit_label(tbl_lbl);
      const std::vector<size_t>& targets =
          !blk.addr_vec_targets.empty() ? blk.addr_vec_targets : blk.successors;
      Rtvec vec = rtvec_alloc(static_cast<int>(targets.size()));
      for (size_t j = 0; j < targets.size(); ++j) {
        Rtx case_lbl = (targets[j] < num_blocks && block_labels[targets[j]] != nullptr)
                           ? block_labels[targets[j]]
                           : tbl_lbl;
        vec->elem[j].rtx = gen_rtx(kLabelRef, kSiMode, case_lbl);
        case_lbl->fld[5].rtint += 1;
      }
      emit_jump_insn(gen_rtx(kAddrVec, kSiMode, vec));
      emit_barrier();
    }
  }

  Rtx first = get_insns();
  Rtx last_in_chain = nullptr;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    last_in_chain = insn;
  }
  if (last_in_chain != nullptr &&
      (last_in_chain->code == kInsn || last_in_chain->code == kJumpInsn ||
       last_in_chain->code == kCallInsn) &&
      last_in_chain->fld[3].rtx != nullptr && last_in_chain->fld[3].rtx->code == kSequence) {
    // Ensure a `SEQUENCE` wrapper at the end of the chain has a non-null `NEXT_INSN`
    // so `reorg.c:delete_from_delay_slot` (`PREV_INSN (NEXT_INSN (trial))`) never
    // dereferences `nullptr`.
    emit_note(nullptr, kNoteInsnDeleted);
  }
  bool has_sequence = false;
  for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
    if ((insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) &&
        insn->fld[3].rtx != nullptr && insn->fld[3].rtx->code == kSequence) {
      has_sequence = true;
      Rtvec vec = insn->fld[3].rtx->fld[0].rtvec;
      if (vec != nullptr && vec->num_elem > 0) {
        vec->elem[0].rtx->fld[1].rtx = insn->fld[1].rtx;
        vec->elem[vec->num_elem - 1].rtx->fld[2].rtx = insn->fld[2].rtx;
        if (insn->fld[1].rtx != nullptr &&
            (insn->fld[1].rtx->code == kInsn || insn->fld[1].rtx->code == kJumpInsn ||
             insn->fld[1].rtx->code == kCallInsn) &&
            insn->fld[1].rtx->fld[3].rtx != nullptr &&
            insn->fld[1].rtx->fld[3].rtx->code == kSequence) {
          Rtvec prev_vec = insn->fld[1].rtx->fld[3].rtx->fld[0].rtvec;
          if (prev_vec != nullptr && prev_vec->num_elem > 0) {
            prev_vec->elem[prev_vec->num_elem - 1].rtx->fld[2].rtx = insn;
          }
        }
        if (insn->fld[2].rtx != nullptr &&
            (insn->fld[2].rtx->code == kInsn || insn->fld[2].rtx->code == kJumpInsn ||
             insn->fld[2].rtx->code == kCallInsn) &&
            insn->fld[2].rtx->fld[3].rtx != nullptr &&
            insn->fld[2].rtx->fld[3].rtx->code == kSequence) {
          Rtvec next_vec = insn->fld[2].rtx->fld[3].rtx->fld[0].rtvec;
          if (next_vec != nullptr && next_vec->num_elem > 0) {
            next_vec->elem[0].rtx->fld[1].rtx = insn;
          }
        }
      }
    }
  }

  // Resolve `INSN_LIST` `REG_NOTES` (`REG_LIBCALL` <-> `REG_RETVAL` pairs and
  // `REG_WAS_0` predecessors) across the materialized instruction chain.
  {
    std::vector<std::pair<Rtx, Rtx>> open_libcalls;
    for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
      if (insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) {
        continue;
      }
      if (insn->code == kCallInsn && !open_libcalls.empty() && cfg.pass != RtlPassKind::kJump2 &&
          cfg.pass != RtlPassKind::kDbr) {
        insn->unchanging = 1;
      }
      for (Rtx note = insn->fld[6].rtx; note != nullptr; note = note->fld[1].rtx) {
        if (note->code != kInsnList) {
          continue;
        }
        if (note->mode == kRegLibcall) {
          open_libcalls.emplace_back(insn, note);
        } else if (note->mode == kRegRetval) {
          if (!open_libcalls.empty()) {
            auto [libcall_insn, libcall_note] = open_libcalls.back();
            open_libcalls.pop_back();
            libcall_note->fld[0].rtx = insn;
            note->fld[0].rtx = libcall_insn;
          } else {
            note->fld[0].rtx = insn;
          }
        } else if (note->mode == kRegWas0) {
          Rtx pat = insn->fld[3].rtx;
          Rtx dst = (pat != nullptr && pat->code == kSet) ? pat->fld[0].rtx : nullptr;
          if (dst != nullptr && dst->code == kReg) {
            for (Rtx prev = insn->fld[1].rtx; prev != nullptr; prev = prev->fld[1].rtx) {
              if (prev->code == kCodeLabel || prev->code == kBarrier) {
                break;
              }
              if (prev->code == kInsn && prev->fld[3].rtx != nullptr &&
                  prev->fld[3].rtx->code == kSet) {
                Rtx prev_dst = prev->fld[3].rtx->fld[0].rtx;
                Rtx prev_src = prev->fld[3].rtx->fld[1].rtx;
                if (prev_dst != nullptr && prev_dst->code == kReg &&
                    prev_dst->fld[0].rtint == dst->fld[0].rtint && prev_src != nullptr &&
                    prev_src->code == kConstInt && prev_src->fld[0].rtint == 0) {
                  note->fld[0].rtx = prev;
                  break;
                }
              }
            }
          }
        }
      }
    }
  }

  // Reconstruct `CALL_INSN_FUNCTION_USAGE` (`call_insn->fld[7].rtx`) for any
  // synthetic `CALL_INSN` lacking explicit `call_fusage`, using inter-block
  // reaching-definitions dataflow across `cfg.rpo_blocks`.
  ReconstructCallInsnFunctionUsage(cfg, first, insn_uid_to_rpo_block_, explicit_fusage_calls);
  // Synchronize `cfg.sideband` into GCC 2.7.2 global side-band arrays.
  int target_max_regno = std::max(max_reg_num(), cfg.sideband.max_regno);
  for (int regno : cfg.sideband.pointer_regnos) {
    target_max_regno = std::max(target_max_regno, regno + 1);
  }
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

  std::vector<int> explicit_ptr_regnos;
  if (regno_pointer_flag != nullptr) {
    for (int r = 0; r < regno_pointer_flag_length; ++r) {
      if (regno_pointer_flag[r] != 0) {
        explicit_ptr_regnos.push_back(r);
      }
    }
  }
  if (!has_sequence) {
    restore_reg_data(first);
    if (regno_reg_rtx != nullptr) {
      for (Rtx vreg : {virtual_incoming_args_rtx, virtual_stack_vars_rtx, virtual_stack_dynamic_rtx,
                       virtual_outgoing_args_rtx}) {
        if (vreg != nullptr && vreg->fld[0].rtint >= 0) {
          regno_reg_rtx[vreg->fld[0].rtint] = vreg;
        }
      }
    }
    reg_scan(first, max_reg_num(), 0);
  } else {
    reg_scan(nullptr, max_reg_num(), 0);
  }
  if (regno_pointer_flag != nullptr && regno_pointer_flag_length > 0) {
    std::memset(regno_pointer_flag, 0, static_cast<size_t>(regno_pointer_flag_length));
    for (int regno : explicit_ptr_regnos) {
      if (regno >= 0 && regno < regno_pointer_flag_length) {
        regno_pointer_flag[regno] = 1;
      }
    }
    for (int regno : cfg.sideband.pointer_regnos) {
      if (regno >= 0 && regno < regno_pointer_flag_length) {
        regno_pointer_flag[regno] = 1;
      }
    }
  }

  PopulateMaterializedSidebandState(cfg, first, num_blocks, alloc_regs);

  if (cfg.pass != RtlPassKind::kRtl && current_function_decl != nullptr) {
    instantiate_virtual_regs(current_function_decl, nullptr);
  }
  if (cfg.pass == RtlPassKind::kGreg || cfg.pass == RtlPassKind::kSched2 ||
      cfg.pass == RtlPassKind::kJump2 || cfg.pass == RtlPassKind::kDbr) {
    reload_completed = 1;
  }
  frame_pointer_needed = cfg.sideband.frame_pointer_needed ? 1 : 0;
  frame_offset = cfg.sideband.frame_size_bytes;
  current_function_outgoing_args_size = cfg.sideband.outgoing_args_size;
  {
    int calls_alloca = (cfg.sideband.frame_pointer_needed && flag_omit_frame_pointer != 0) ? 1 : 0;
    if (calls_alloca == 0) {
      int neg_const_sp_adjusts = 0;
      auto check_pat_for_alloca = [&](auto& self, Rtx pat) -> void {
        if (pat == nullptr || calls_alloca != 0) {
          return;
        }
        if (pat->code == kReg && pat->fld[0].rtint == kMipsFirstVirtualRegister + 2) {
          calls_alloca = 1;
          return;
        }
        if (pat->code == kSequence && pat->fld[0].rtvec != nullptr) {
          Rtvec vec = pat->fld[0].rtvec;
          for (int i = 0; i < vec->num_elem; ++i) {
            Rtx inner = vec->elem[i].rtx;
            if (inner != nullptr &&
                (inner->code == kInsn || inner->code == kJumpInsn || inner->code == kCallInsn)) {
              self(self, inner->fld[3].rtx);
            }
          }
          return;
        }
        if (pat->code == kParallel && pat->fld[0].rtvec != nullptr) {
          Rtvec vec = pat->fld[0].rtvec;
          for (int i = 0; i < vec->num_elem; ++i) {
            self(self, vec->elem[i].rtx);
          }
          return;
        }
        if (pat->code == kSet && pat->fld[0].rtx != nullptr && pat->fld[1].rtx != nullptr) {
          Rtx dst = pat->fld[0].rtx;
          Rtx src = pat->fld[1].rtx;
          const std::string_view src_name =
              (rtx_name[src->code] != nullptr) ? std::string_view(rtx_name[src->code]) : "";
          if (dst->code == kReg && dst->fld[0].rtint == 29 &&
              (src_name == "plus" || src_name == "minus") && src->fld[0].rtx != nullptr &&
              src->fld[0].rtx->code == kReg && src->fld[0].rtx->fld[0].rtint == 29 &&
              src->fld[1].rtx != nullptr) {
            Rtx op1 = src->fld[1].rtx;
            if (op1->code != kConstInt && !(op1->code == kReg && op1->fld[0].rtint == 12)) {
              calls_alloca = 1;
              return;
            }
            if (cfg.sideband.frame_pointer_needed && op1->code == kConstInt &&
                ((src_name == "plus" && op1->fld[0].rtint < 0) ||
                 (src_name == "minus" && op1->fld[0].rtint > 0))) {
              ++neg_const_sp_adjusts;
              if (neg_const_sp_adjusts > 1 || cfg.pass == RtlPassKind::kGreg) {
                calls_alloca = 1;
                return;
              }
            }
          }
        }
        const char* fmt = rtx_format[pat->code];
        const int len = rtx_length[pat->code];
        for (int i = 0; i < len && fmt[i] != '\0'; ++i) {
          if (fmt[i] == 'e') {
            self(self, pat->fld[i].rtx);
          }
        }
      };
      for (Rtx insn = first; insn != nullptr && calls_alloca == 0; insn = insn->fld[2].rtx) {
        if (insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) {
          check_pat_for_alloca(check_pat_for_alloca, insn->fld[3].rtx);
        }
      }
    }
    current_function_calls_alloca = calls_alloca;
  }
  compute_frame_size(cfg.sideband.frame_size_bytes);
  if (cfg.sideband.epilogue_delay_pattern.has_value() &&
      !cfg.sideband.epilogue_delay_pattern->empty()) {
    Rtx delay_insn = BuildInsnFromContainerOrPattern(*cfg.sideband.epilogue_delay_pattern,
                                                     /*emit_to_chain=*/false);
    if (delay_insn != nullptr) {
      current_function_epilogue_delay_list = gen_rtx(kInsnList, kVoidMode, delay_insn, nullptr);
    }
  }

  // Deserialize `REG_FUNCTION_VALUE_P` (`reg/i` on MIPS return registers
  // `$2`/`$3` or `$f0`/`$f1`) into `current_function_return_rtx` (`reorg.c:4274`).
  if (reset_function) {
    uint64_t ret_mask = 0;
    int ret_mode_2 = kSiMode;
    int ret_mode_32 = kSfMode;
    auto scan_rtx_for_ret_reg = [&](auto& self, Rtx x) -> void {
      if (x == nullptr) {
        return;
      }
      const int code = x->code;
      if (code == kReg) {
        const int r = x->fld[0].rtint;
        if (x->integrated != 0 && (r == 2 || r == 3 || r == 32 || r == 33)) {
          ret_mask |= (uint64_t{1} << r);
          if (r == 2) {
            ret_mode_2 = x->mode;
          } else if (r == 32) {
            ret_mode_32 = x->mode;
          }
        }
        return;
      }
      if (code < 0 || code >= kNumRtxCode || rtx_format[code] == nullptr) {
        return;
      }
      const char* fmt = rtx_format[code];
      const int len = rtx_length[code];
      for (int i = 0; i < len; ++i) {
        if (fmt[i] == 'e') {
          self(self, x->fld[i].rtx);
        } else if (fmt[i] == 'E' && x->fld[i].rtvec != nullptr) {
          Rtvec vec = x->fld[i].rtvec;
          for (int j = 0; j < vec->num_elem; ++j) {
            self(self, vec->elem[j].rtx);
          }
        }
      }
    };
    for (Rtx insn = first; insn != nullptr; insn = insn->fld[2].rtx) {
      if (insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn) {
        scan_rtx_for_ret_reg(scan_rtx_for_ret_reg, insn->fld[3].rtx);
      }
    }
    if (current_function_epilogue_delay_list != nullptr &&
        current_function_epilogue_delay_list->fld[0].rtx != nullptr) {
      Rtx ep_insn = current_function_epilogue_delay_list->fld[0].rtx;
      Rtx ep_pat =
          (ep_insn->code == kInsn || ep_insn->code == kJumpInsn || ep_insn->code == kCallInsn)
              ? ep_insn->fld[3].rtx
              : ep_insn;
      scan_rtx_for_ret_reg(scan_rtx_for_ret_reg, ep_pat);
    }
    if ((ret_mask & ((uint64_t{1} << 2) | (uint64_t{1} << 3))) != 0) {
      const int mode = ((ret_mask & (uint64_t{1} << 3)) != 0) ? kDiMode : ret_mode_2;
      current_function_return_rtx = gen_rtx(kReg, mode, 2);
      current_function_return_rtx->integrated = 1;
    } else if ((ret_mask & ((uint64_t{1} << 32) | (uint64_t{1} << 33))) != 0) {
      const int mode = ((ret_mask & (uint64_t{1} << 33)) != 0) ? kDfMode : ret_mode_32;
      current_function_return_rtx = gen_rtx(kReg, mode, 32);
      current_function_return_rtx->integrated = 1;
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
  auto is_deleted_chain_entry = [](Rtx r) -> bool {
    return r != nullptr &&
           (r->code == kInsn || r->code == kJumpInsn || r->code == kCallInsn ||
            r->code == kCodeLabel || r->code == kBarrier) &&
           r->volatil != 0;
  };
  auto next_non_deleted = [&](Rtx r) -> Rtx {
    Rtx cur = r != nullptr ? r->fld[2].rtx : nullptr;
    while (cur != nullptr && is_deleted_chain_entry(cur)) {
      cur = cur->fld[2].rtx;
    }
    return cur;
  };

  absl::flat_hash_map<int, std::string> label_map;
  size_t next_lbl_idx = 0;
  std::string current_group_label;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if (is_deleted_chain_entry(insn)) {
      // `rtl.h:277`: `#define INSN_DELETED_P(INSN) ((INSN)->volatil)`. When
      // `optimize == 0`, `delete_insn` marks unreachable labels/barriers/insns
      // deleted via `volatil = 1` without unlinking them from `NEXT_INSN`.
      continue;
    }
    if (insn->code == kCodeLabel && insn->fld[0].rtint >= 0) {
      Rtx next_insn = next_non_deleted(insn);
      const bool is_tablejump_label = (next_insn != nullptr && next_insn->code == kJumpInsn &&
                                       next_insn->fld[3].rtx != nullptr &&
                                       (next_insn->fld[3].rtx->code == kAddrVec ||
                                        next_insn->fld[3].rtx->code == kAddrDiffVec));
      if (is_tablejump_label || current_group_label.empty()) {
        current_group_label = absl::StrCat("L", next_lbl_idx++);
      }
      if (!label_map.contains(insn->fld[0].rtint)) {
        label_map.emplace(insn->fld[0].rtint, current_group_label);
      }
      if (is_tablejump_label) {
        current_group_label.clear();
      }
    } else if (insn->code == kInsn || insn->code == kJumpInsn || insn->code == kCallInsn ||
               insn->code == kBarrier ||
               (insn->code == kNote &&
                (insn->fld[4].rtint == kNoteInsnLoopBeg || insn->fld[4].rtint == kNoteInsnLoopEnd ||
                 insn->fld[4].rtint == kNoteInsnLoopCont ||
                 insn->fld[4].rtint == kNoteInsnLoopVtop))) {
      current_group_label.clear();
    }
  }
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if ((insn->code != kInsn && insn->code != kJumpInsn && insn->code != kCallInsn) ||
        insn->volatil != 0) {
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
    if (insn->fld[6].rtx != nullptr) {
      for (const std::string& tok : ExtractLabelRefTokens(FormatRtx(insn->fld[6].rtx))) {
        int uid_val = 0;
        if (absl::SimpleAtoi(tok, &uid_val) && uid_val >= 0 && !label_map.contains(uid_val)) {
          label_map.emplace(uid_val, absl::StrCat("L", next_lbl_idx++));
        }
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
  std::vector<RtlInsnEntry> raw_tablejump_entries;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    const int code = insn->code;
    if (is_deleted_chain_entry(insn)) {
      continue;
    }
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
    if (code == kCodeLabel) {
      Rtx next_insn = next_non_deleted(insn);
      if (next_insn != nullptr && next_insn->code == kJumpInsn) {
        Rtx next_pat = next_insn->fld[3].rtx;
        if (next_pat != nullptr && (next_pat->code == kAddrVec || next_pat->code == kAddrDiffVec)) {
          raw_tablejump_entries.push_back(RtlInsnEntry{
              .kind = RtlInsnKind::kCodeLabel,
              .uid = insn->fld[0].rtint,
          });
          raw_tablejump_entries.push_back(RtlInsnEntry{
              .kind = RtlInsnKind::kJumpInsn,
              .uid = next_insn->fld[0].rtint,
              .raw_sexpr = absl::StrCat("(jump_insn ", next_insn->fld[0].rtint, " 0 0 ",
                                        FormatRtx(next_pat), ")"),
          });
          insn = next_insn;
          while (true) {
            Rtx maybe_barrier = next_non_deleted(insn);
            if (maybe_barrier == nullptr || maybe_barrier->code != kBarrier) {
              break;
            }
            insn = maybe_barrier;
          }
          continue;
        }
      }
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
    if (code == kInsn && pat != nullptr && pat->code == kUse && pat->fld[0].rtx != nullptr &&
        (pat->fld[0].rtx->code == kInsn || pat->fld[0].rtx->code == kJumpInsn ||
         pat->fld[0].rtx->code == kCallInsn)) {
      continue;
    }
    if (code == kJumpInsn && pat != nullptr &&
        (pat->code == kAddrVec || pat->code == kAddrDiffVec)) {
      continue;
    }
    if (pat != nullptr && pat->code == kSequence && pat->fld[0].rtvec != nullptr) {
      Rtvec vec = pat->fld[0].rtvec;
      std::vector<std::string> formatted_inner;
      formatted_inner.reserve(vec->num_elem);
      RtlInsnKind effective_kind = code_to_kind(code);
      std::string jump_raw_pat;
      bool seq_has_explicit_call_fusage = false;
      std::vector<RtlExpr> seq_call_fusage;
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
        if (sub->code == kCallInsn) {
          seq_has_explicit_call_fusage = true;
          if (sub->fld[7].rtx != nullptr) {
            if (auto parsed_fu = ParseCallFusageChainSexpr(
                    CanonicalizeLabelRefsInPattern(FormatRtx(sub->fld[7].rtx), label_map));
                parsed_fu.ok()) {
              seq_call_fusage = std::move(*parsed_fu);
            }
          }
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
          .has_explicit_call_fusage = seq_has_explicit_call_fusage,
          .call_fusage = std::move(seq_call_fusage),
      });
      continue;
    }

    std::string raw_pat = FormatRtx(pat);
    std::string canon_pat = CanonicalizeLabelRefsInPattern(raw_pat, label_map);
    if (code != kCallInsn && insn->fld[6].rtx != nullptr) {
      absl::StrAppend(&canon_pat, " ",
                      CanonicalizeLabelRefsInPattern(FormatRtx(insn->fld[6].rtx), label_map));
    }
    bool item_has_explicit_call_fusage = false;
    std::vector<RtlExpr> item_call_fusage;
    if (code == kCallInsn) {
      item_has_explicit_call_fusage = true;
      if (insn->fld[7].rtx != nullptr) {
        if (auto parsed_fu = ParseCallFusageChainSexpr(
                CanonicalizeLabelRefsInPattern(FormatRtx(insn->fld[7].rtx), label_map));
            parsed_fu.ok()) {
          item_call_fusage = std::move(*parsed_fu);
        }
      }
    }
    items.push_back(FlatRtlItem{
        .kind = code_to_kind(code),
        .uid = insn->fld[0].rtint,
        .raw_pattern_for_targets = raw_pat,
        .pattern = CanonicalizeRtlPattern(canon_pat),
        .has_explicit_call_fusage = item_has_explicit_call_fusage,
        .call_fusage = std::move(item_call_fusage),
    });
  }

  RtlControlFlowGraph cfg = BuildCanonicalCfgFromFlatRtlItems(pass, items, raw_tablejump_entries);

  // Dematerialize GCC 2.7.2 global side-band arrays into `cfg.sideband`.
  const int nregs = std::max(max_reg_num(), max_regno);
  cfg.sideband.max_regno = nregs;
  if (regno_pointer_flag != nullptr) {
    const int ptr_limit = std::min(nregs, regno_pointer_flag_length);
    for (int r = kFirstPseudoRegister + 4; r < ptr_limit; ++r) {
      if (regno_pointer_flag[r] != 0) {
        cfg.sideband.pointer_regnos.push_back(r);
      }
    }
  }

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
      // Preserve negative sentinel `-1` (`global.c:388` skips
      // `reg_live_length[i] == -1`, e.g. phantom `alter_reg` slots with
      // `reg_n_refs > 0` and `reg_n_deaths == 0`).
      if (reg_live_length[r] != 0) {
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
    absl::flat_hash_map<int, int> uid_to_rpo;
    if (basic_block_head != nullptr) {
      size_t idx = 0;
      int cur_rpo = 0;
      while (idx < items.size()) {
        while (idx < items.size() && items[idx].kind == RtlInsnKind::kBarrier) {
          ++idx;
        }
        if (idx >= items.size()) {
          break;
        }
        bool has_content = false;
        std::vector<int> blk_uids;
        while (idx < items.size() && items[idx].kind == RtlInsnKind::kCodeLabel) {
          blk_uids.push_back(items[idx].uid);
          has_content = true;
          ++idx;
        }
        while (idx < items.size() && items[idx].kind != RtlInsnKind::kCodeLabel &&
               items[idx].kind != RtlInsnKind::kBarrier) {
          const auto& it = items[idx];
          blk_uids.push_back(it.uid);
          if (it.kind == RtlInsnKind::kInsn || it.kind == RtlInsnKind::kCallInsn ||
              it.kind == RtlInsnKind::kNote) {
            has_content = true;
            ++idx;
          } else if (it.kind == RtlInsnKind::kJumpInsn) {
            has_content = true;
            ++idx;
            break;
          } else {
            ++idx;
          }
        }
        while (idx < items.size() && items[idx].kind == RtlInsnKind::kBarrier) {
          ++idx;
        }
        if (!has_content) {
          continue;
        }
        for (int u : blk_uids) {
          if (u >= 0) {
            uid_to_rpo[u] = cur_rpo;
          }
        }
        ++cur_rpo;
      }
    }
    std::vector<bool> assigned_rpo(cfg.rpo_blocks.size(), false);
    for (int b = 0; b < n_basic_blocks; ++b) {
      if (basic_block_live_at_start[b] == nullptr) {
        continue;
      }
      int dst_b = b;
      if (!uid_to_rpo.empty() && basic_block_head != nullptr && basic_block_head[b] != nullptr) {
        if (auto it = uid_to_rpo.find(basic_block_head[b]->fld[0].rtint); it != uid_to_rpo.end()) {
          dst_b = it->second;
        } else if (b == 0) {
          dst_b = 0;
        } else {
          continue;
        }
      }
      if (dst_b >= 0 && static_cast<size_t>(dst_b) < assigned_rpo.size()) {
        assigned_rpo[dst_b] = true;
      }
      const auto* words = reinterpret_cast<const uint32_t*>(basic_block_live_at_start[b]);
      for (int w = 0; w < regset_size; ++w) {
        uint32_t mask = words[w];
        while (mask != 0) {
          const int bit = __builtin_ctz(mask);
          const int regno = w * 32 + bit;
          if (regno < nregs) {
            cfg.sideband.basic_block_live_at_start[dst_b].insert(regno);
          }
          mask &= (mask - 1);
        }
      }
    }
    if (!uid_to_rpo.empty()) {
      for (int r = static_cast<int>(cfg.rpo_blocks.size()) - 2; r >= 0; --r) {
        if (!assigned_rpo[r] && cfg.rpo_blocks[r].successors.size() == 1) {
          const int succ = static_cast<int>(cfg.rpo_blocks[r].successors[0]);
          if (auto it = cfg.sideband.basic_block_live_at_start.find(succ);
              it != cfg.sideband.basic_block_live_at_start.end()) {
            auto live_copy = it->second;
            cfg.sideband.basic_block_live_at_start[r] = std::move(live_copy);
          }
        }
      }
    }
  }
  cfg.sideband.frame_size_bytes = frame_offset;
  cfg.sideband.outgoing_args_size = current_function_outgoing_args_size;
  cfg.sideband.frame_pointer_needed = (frame_pointer_needed != 0);
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
  cfg.sideband.constant_pool = SnapshotActiveConstantPool();
  if (pass != RtlPassKind::kSched2 && pass != RtlPassKind::kJump2 && pass != RtlPassKind::kDbr) {
    cfg.sideband.is_variadic = active_is_variadic_;
    cfg.sideband.returns_aggregate_in_memory = active_returns_aggregate_in_memory_;
    cfg.sideband.prologue_args = active_prologue_args_;
  }

  return cfg;
}

int GccContext::LookupMaterializedRpoBlock(int uid) const {
  auto it = insn_uid_to_rpo_block_.find(uid);
  return it != insn_uid_to_rpo_block_.end() ? it->second : -1;
}

std::vector<Stage0ConstantPoolEntry> GccContext::SnapshotActiveConstantPool() {
  if (get_pool_size() <= 0) {
    return {};
  }
  g_in_pool_snapshot = true;
  alignas(16) uint8_t saved_fn_status[2048] = {};
  save_varasm_status(saved_fn_status);

  char* buf = nullptr;
  size_t len = 0;
  FILE* mem_fp = open_memstream(&buf, &len);
  FILE* saved_asm = asm_out_file;
  FILE* saved_data = asm_out_data_file;
  FILE* saved_text = asm_out_text_file;
  const int saved_optimize = optimize;
  asm_out_file = mem_fp;
  asm_out_data_file = mem_fp;
  asm_out_text_file = mem_fp;
  // Temporarily set `optimize = 0` so `output_constant_pool` snapshots all
  // entries in `first_pool` (including entries currently held only in
  // `CONST_DOUBLE_MEM` prior to `reload`) without mutating `pool->mark`; real
  // `mark_constant_pool` pruning runs at `FunctionHooks::RunFinal` time.
  optimize = 0;
  text_section();
  output_constant_pool(const_cast<char*>("harness_func"), current_function_decl);
  text_section();
  optimize = saved_optimize;
  asm_out_file = saved_asm;
  asm_out_data_file = saved_data;
  asm_out_text_file = saved_text;
  std::fclose(mem_fp);

  restore_varasm_status(saved_fn_status);
  g_in_pool_snapshot = false;

  std::string asm_text(buf != nullptr ? buf : "", len);
  std::free(buf);
  return ParseConstantPoolAsm(asm_text);
}

void GccContext::SetCc1ConstantPoolCaptureTarget(std::vector<Stage0ConstantPoolEntry>* target) {
  g_cc1_pool_capture_target = target;
}

GccContext::ScopedSkipReloadInGlobalAlloc::ScopedSkipReloadInGlobalAlloc()
    : prev_(g_skip_reload_in_global_alloc) {
  g_skip_reload_in_global_alloc = true;
}

GccContext::ScopedSkipReloadInGlobalAlloc::~ScopedSkipReloadInGlobalAlloc() {
  g_skip_reload_in_global_alloc = prev_;
}

Rtx GccContext::RegisterConstantPoolEntry(int label_num, const Stage0ConstantPoolEntry& cp,
                                          int preferred_mode) {
  const std::string canon_key = absl::StrCat("$LC", label_num);
  if (auto it = pool_symbol_map_.find(canon_key);
      it != pool_symbol_map_.end() && it->second != nullptr) {
    return it->second;
  }

  int pool_mode = kSfMode;
  Rtx val_rtx = nullptr;
  if (cp.words.size() >= 2) {
    const int hi_w = static_cast<int>(static_cast<int32_t>(cp.words[0]));
    const int lo_w = static_cast<int>(static_cast<int32_t>(cp.words[1]));
    if (preferred_mode == kDiMode) {
      pool_mode = kDiMode;
      val_rtx = immed_double_const(lo_w, hi_w, kVoidMode);
    } else {
      int d_in[2] = {hi_w, lo_w};
      const double rv = ereal_from_double(d_in);
      long d_out[2] = {0, 0};
      etardouble(rv, d_out);
      if (static_cast<uint32_t>(d_out[0]) == cp.words[0] &&
          static_cast<uint32_t>(d_out[1]) == cp.words[1]) {
        pool_mode = kDfMode;
        val_rtx = immed_real_const_1(rv, kDfMode);
      } else {
        pool_mode = kDiMode;
        val_rtx = immed_double_const(lo_w, hi_w, kVoidMode);
      }
    }
  } else {
    const uint32_t w0 = cp.words.empty() ? 0u : cp.words[0];
    const int raw_w = static_cast<int>(static_cast<int32_t>(w0));
    if (preferred_mode == kSiMode) {
      pool_mode = kSiMode;
      val_rtx = gen_rtx(kConstInt, kVoidMode, raw_w);
    } else {
      const double rv = ereal_from_float(raw_w);
      const uint32_t rt_w = static_cast<uint32_t>(etarsingle(rv));
      if (rt_w == w0) {
        pool_mode = kSfMode;
        val_rtx = immed_real_const_1(rv, kSfMode);
      } else {
        pool_mode = kSiMode;
        val_rtx = gen_rtx(kConstInt, kVoidMode, raw_w);
      }
    }
  }

  if (val_rtx == nullptr) {
    return nullptr;
  }
  const int saved_next_labelno = std::max(const_labelno, label_num + 1);
  const_labelno = label_num;
  Rtx mem_rtx = force_const_mem(pool_mode, val_rtx);
  const_labelno = std::max(const_labelno, saved_next_labelno);
  if (mem_rtx == nullptr || mem_rtx->code != kMem || mem_rtx->fld[0].rtx == nullptr ||
      mem_rtx->fld[0].rtx->code != kSymbolRef) {
    return nullptr;
  }

  Rtx sym_rtx = mem_rtx->fld[0].rtx;
  pool_symbol_map_[canon_key] = sym_rtx;
  pool_symbol_map_[absl::StrCat("*$LC", label_num)] = sym_rtx;
  if (sym_rtx->fld[0].rtstr != nullptr) {
    std::string actual_sym(sym_rtx->fld[0].rtstr);
    pool_symbol_map_[actual_sym] = sym_rtx;
    if (!actual_sym.empty() && actual_sym.front() == '*') {
      pool_symbol_map_[actual_sym.substr(1)] = sym_rtx;
    }
  }
  return sym_rtx;
}

Rtx GccContext::GetInsns() const {
  return get_insns();
}

}  // namespace rom_nom_nom::gcc_harness
