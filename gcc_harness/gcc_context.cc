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

// NOLINTBEGIN(readability-identifier-naming)
extern "C" {

// GCC 2.7.2 initialization and pass setup routines.
void init_reg_sets();
void init_reg_sets_1();
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
void restore_reg_data(rtx_def* first);
void reg_scan(rtx_def* f, int nregs, int repeat);
int compute_frame_size(int size);
void* make_node(int code);
void* get_identifier(const char* text);
void* _obstack_newchunk(GccObstack* ob, int length);

extern int flag_caller_saves;
extern int flag_expensive_optimizations;
extern int flag_strength_reduce;
extern int flag_schedule_insns;
extern int flag_schedule_insns_after_reload;
extern int flag_delayed_branch;
extern int rtx_equal_function_value_matters;
extern char regs_ever_live[];
extern rtx_def* current_function_epilogue_delay_list;
extern rtx_def* pc_rtx;
extern rtx_def* cc0_rtx;
extern char* (*decl_printable_name)(void*, char**);

// GAS 2.6 initialization symbols.
void hex_init();
void bfd_init();
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
constexpr int kFunctionDeclCode = 29;
constexpr int kResultDeclCode = 35;
constexpr int kBlockCode = 5;
constexpr int kVoidTypeCode = 6;

void SkipSpaces(std::string_view text, size_t* pos) {
  while (*pos < text.size() && absl::ascii_isspace(text[*pos])) {
    ++(*pos);
  }
}

std::string_view ReadToken(std::string_view text, size_t* pos) {
  SkipSpaces(text, pos);
  const size_t start = *pos;
  while (*pos < text.size()) {
    char c = text[*pos];
    if (absl::ascii_isspace(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == ':' ||
        c == '/' || c == '"') {
      break;
    }
    ++(*pos);
  }
  return text.substr(start, *pos - start);
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

bool IsJumpPatternText(std::string_view pat) {
  std::string_view s = absl::StripAsciiWhitespace(pat);
  return absl::StartsWith(s, "(return)") || absl::StartsWith(s, "(set (pc)") ||
         absl::StartsWith(s, "(parallel [(set (pc)") ||
         absl::StartsWith(s, "(parallel[ (set (pc)") ||
         absl::StartsWith(s, "(parallel [(return)") || absl::StartsWith(s, "(parallel[ (return)");
}

bool IsUnconditionalJumpOrReturnPatternText(std::string_view pat) {
  std::string_view s = absl::StripAsciiWhitespace(pat);
  if (absl::StartsWith(s, "(return)") || absl::StartsWith(s, "(parallel [(return)") ||
      absl::StartsWith(s, "(parallel[ (return)")) {
    return true;
  }
  if (absl::StartsWith(s, "(set (pc) (label_ref") ||
      absl::StartsWith(s, "(parallel [(set (pc) (reg:") ||
      absl::StartsWith(s, "(parallel[ (set (pc) (reg:")) {
    return true;
  }
  return false;
}

bool IsCallPatternText(std::string_view pat) {
  std::string_view s = absl::StripAsciiWhitespace(pat);
  return absl::StartsWith(s, "(call ") ||
         (absl::StartsWith(s, "(set ") && absl::StrContains(s, " (call ")) ||
         absl::StartsWith(s, "(parallel [(call ") || absl::StartsWith(s, "(parallel[ (call ") ||
         (absl::StartsWith(s, "(parallel") && absl::StrContains(s, "(call (mem"));
}

// Splits `(<main_pattern>) (<reg_notes>)` if a trailing `(expr_list:...` or
// `(insn_list:...` S-expression is appended to `pattern_rtl`.
std::pair<std::string_view, std::string_view> SplitPatternAndTrailingRegNotes(
    std::string_view text) {
  std::string_view s = absl::StripAsciiWhitespace(text);
  if (s.empty() || s.front() != '(') {
    return {s, {}};
  }
  int depth = 0;
  bool in_quotes = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (in_quotes) {
      if (c == '\\' && i + 1 < s.size()) {
        ++i;
      } else if (c == '"') {
        in_quotes = false;
      }
      continue;
    }
    if (c == '"') {
      in_quotes = true;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
      if (depth == 0) {
        std::string_view first = s.substr(0, i + 1);
        std::string_view rest = absl::StripAsciiWhitespace(s.substr(i + 1));
        if (absl::StartsWith(rest, "(expr_list") || absl::StartsWith(rest, "(insn_list")) {
          return {first, rest};
        }
        return {s, {}};
      }
    }
  }
  return {s, {}};
}

std::vector<std::string> ExtractLabelRefTokens(std::string_view text) {
  std::vector<std::string> tokens;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t idx = text.find("(label_ref", pos);
    if (idx == std::string_view::npos) {
      break;
    }
    size_t p = idx + std::string_view("(label_ref").size();
    while (p < text.size() && !absl::ascii_isspace(text[p]) && text[p] != ')') {
      ++p;
    }
    while (p < text.size() && absl::ascii_isspace(text[p])) {
      ++p;
    }
    size_t tok_start = p;
    while (p < text.size() && !absl::ascii_isspace(text[p]) && text[p] != ')' && text[p] != '(') {
      ++p;
    }
    if (p > tok_start) {
      tokens.emplace_back(text.substr(tok_start, p - tok_start));
    }
    pos = p;
  }
  return tokens;
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
    target_flags = 16;  // MASK_GAS (KMC default)
    flag_caller_saves = 1;
    flag_expensive_optimizations = 1;
    flag_strength_reduce = 1;
    flag_schedule_insns = 1;
    flag_schedule_insns_after_reload = 1;
    flag_delayed_branch = 1;
    rtx_equal_function_value_matters = 1;
    decl_printable_name =
        +[](void* /*decl*/, char** /*kind*/) -> char* { return const_cast<char*>("harness_func"); };

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

    // Initialize Binutils 2.6 GAS for in-process MIPS assembly.
    target_big_endian = 1;
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

  void* fn_decl = make_node(kFunctionDeclCode);
  void* id_node = get_identifier(current_func_name_.c_str());
  void* res_decl = make_node(kResultDeclCode);
  void* blk_node = make_node(kBlockCode);
  void* void_type = make_node(kVoidTypeCode);

  *reinterpret_cast<void**>(static_cast<char*>(fn_decl) + 8) = void_type;
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
  reload_completed = 0;
  std::memset(regs_ever_live, 0, kFirstPseudoRegister);
  current_function_epilogue_delay_list = nullptr;
  init_function_start(fn_decl, const_cast<char*>("harness.c"), 1);
  rtx_equal_function_value_matters = 1;
  emit_note(nullptr, kNoteInsnFunctionBeg);
  compute_frame_size(0);
}

void GccContext::EnsurePseudoRegsUpTo(int min_max_regno, int default_mode) {
  while (max_reg_num() <= min_max_regno) {
    gen_reg_rtx(default_mode);
  }
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
  SkipSpaces(text, pos);
  if (*pos >= text.size() || text[*pos] != '(') {
    return nullptr;
  }
  ++(*pos);  // Consume '('.

  std::string_view code_name = ReadToken(text, pos);
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
    std::string_view mode_tok = ReadToken(text, pos);
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
    SkipSpaces(text, pos);
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
        std::string_view tok = ReadToken(text, pos);
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
          SkipSpaces(text, pos);
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
        SkipSpaces(text, pos);
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
        std::string_view tok = ReadToken(text, pos);
        rt->fld[i].rtstr = CopyStringToRtlObstack(tok);
      }
      if (saw_paren) {
        SkipSpaces(text, pos);
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
        SkipSpaces(text, pos);
      }
      std::string_view tok = ReadToken(text, pos);
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
        SkipSpaces(text, pos);
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
    std::string_view tok = ReadToken(s, &pos);
    Rtx lbl = GetOrCreateLabel(tok);
    return emit_to_chain ? emit_label(lbl) : lbl;
  }

  // Case 4: `(sequence [ ... ])`
  if (absl::StartsWith(s, "(sequence")) {
    size_t bracket = s.find('[');
    if (bracket == std::string_view::npos) {
      return nullptr;
    }
    size_t pos = bracket + 1;
    std::vector<Rtx> sub_insns;
    while (pos < s.size()) {
      SkipSpaces(s, &pos);
      if (pos >= s.size() || s[pos] == ']') {
        break;
      }
      if (s[pos] != '(') {
        ++pos;
        continue;
      }
      size_t start = pos;
      int depth = 0;
      bool in_quotes = false;
      for (; pos < s.size(); ++pos) {
        char c = s[pos];
        if (in_quotes) {
          if (c == '\\' && pos + 1 < s.size()) {
            ++pos;
          } else if (c == '"') {
            in_quotes = false;
          }
          continue;
        }
        if (c == '"') {
          in_quotes = true;
        } else if (c == '(') {
          ++depth;
        } else if (c == ')') {
          --depth;
          if (depth == 0) {
            ++pos;
            break;
          }
        }
      }
      Rtx sub = BuildInsnFromContainerOrPattern(s.substr(start, pos - start),
                                                /*emit_to_chain=*/false);
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
    std::string_view tag = ReadToken(s, &pos);
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
        ReadToken(s, &pos);
      }
      // Skip optional numeric UID tokens (`uid prev next`) if present.
      for (int k = 0; k < 3; ++k) {
        SkipSpaces(s, &pos);
        if (pos < s.size() && (absl::ascii_isdigit(s[pos]) || s[pos] == '-')) {
          ReadToken(s, &pos);
        } else {
          break;
        }
      }
      SkipSpaces(s, &pos);
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

Rtx GccContext::MaterializeFromCfg(const RtlControlFlowGraph& cfg) {
  BeginFunction(current_func_name_);

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
    for (const auto& pat : cfg.rpo_blocks[b].instructions) {
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
  if (!has_sequence) {
    restore_reg_data(first);
    reg_scan(first, max_reg_num(), 0);
  }
  return first;
}

RtlControlFlowGraph GccContext::DematerializeToCfg(Rtx first_insn, RtlPassKind pass) const {
  RtlControlFlowGraph result;
  result.pass = pass;

  Rtx head = first_insn != nullptr ? first_insn : get_insns();
  if (head == nullptr) {
    result.rpo_blocks.push_back(RtlBasicBlock{});
    return result;
  }

  // First pass: assign sequential canonical label names (`L0`, `L1`, ...) to
  // all `CODE_LABEL` nodes in linear chain order.
  absl::flat_hash_map<int, std::string> label_map;
  size_t next_lbl_idx = 0;
  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    if (insn->code == kCodeLabel && insn->fld[0].rtint >= 0) {
      if (!label_map.contains(insn->fld[0].rtint)) {
        label_map.emplace(insn->fld[0].rtint, absl::StrCat("L", next_lbl_idx++));
      }
    }
  }

  auto canonicalize_label_refs = [&](std::string_view pat) -> std::string {
    std::string out;
    out.reserve(pat.size());
    size_t i = 0;
    while (i < pat.size()) {
      if (pat[i] == '(' && absl::StartsWith(pat.substr(i + 1), "label_ref")) {
        out.push_back('(');
        ++i;
        while (i < pat.size() && !absl::ascii_isspace(pat[i]) && pat[i] != ')') {
          out.push_back(pat[i++]);
        }
        while (i < pat.size() && absl::ascii_isspace(pat[i])) {
          ++i;
        }
        size_t num_start = i;
        while (i < pat.size() && absl::ascii_isdigit(pat[i])) {
          ++i;
        }
        int label_uid = -1;
        if (i > num_start && absl::SimpleAtoi(pat.substr(num_start, i - num_start), &label_uid)) {
          auto it = label_map.find(label_uid);
          if (it != label_map.end()) {
            absl::StrAppend(&out, " ", it->second);
          } else {
            absl::StrAppend(&out, " ", label_uid);
          }
        }
        continue;
      }
      out.push_back(pat[i++]);
    }
    return out;
  };

  auto extract_label_refs = [](std::string_view pat) -> std::vector<int> {
    std::vector<int> refs;
    size_t pos = 0;
    while (pos < pat.size()) {
      size_t idx = pat.find("(label_ref", pos);
      if (idx == std::string_view::npos) {
        break;
      }
      size_t p = idx + std::string_view("(label_ref").size();
      while (p < pat.size() && !absl::ascii_isspace(pat[p]) && pat[p] != ')') {
        ++p;
      }
      while (p < pat.size() && absl::ascii_isspace(pat[p])) {
        ++p;
      }
      size_t num_start = p;
      while (p < pat.size() && absl::ascii_isdigit(pat[p])) {
        ++p;
      }
      int label_uid = -1;
      if (p > num_start && absl::SimpleAtoi(pat.substr(num_start, p - num_start), &label_uid)) {
        refs.push_back(label_uid);
      }
      pos = p;
    }
    return refs;
  };

  auto format_insn_tag = [](Rtx sub) -> std::string {
    std::string tag =
        (sub->code == kJumpInsn) ? "jump_insn" : ((sub->code == kCallInsn) ? "call_insn" : "insn");
    if (sub->in_struct) absl::StrAppend(&tag, "/s");
    if (sub->volatil) absl::StrAppend(&tag, "/v");
    if (sub->unchanging) absl::StrAppend(&tag, "/u");
    if (sub->integrated) absl::StrAppend(&tag, "/i");
    return tag;
  };

  struct FlatItem {
    int code = 0;
    int uid = -1;
    std::string raw_pattern_for_targets;
    std::string pattern;
  };
  std::vector<FlatItem> items;

  for (Rtx insn = head; insn != nullptr; insn = insn->fld[2].rtx) {
    const int code = insn->code;
    if (code == kNote) {
      const int note_kind = insn->fld[4].rtint;
      if (note_kind == kNoteInsnLoopBeg) {
        items.push_back(FlatItem{
            .code = kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_BEG)",
        });
      } else if (note_kind == kNoteInsnLoopCont) {
        items.push_back(FlatItem{
            .code = kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_CONT)",
        });
      } else if (note_kind == kNoteInsnLoopEnd) {
        items.push_back(FlatItem{
            .code = kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_END)",
        });
      } else if (note_kind == kNoteInsnLoopVtop) {
        items.push_back(FlatItem{
            .code = kNote,
            .uid = insn->fld[0].rtint,
            .pattern = "(note NOTE_INSN_LOOP_VTOP)",
        });
      }
      continue;
    }
    if (code == kCodeLabel || code == kBarrier) {
      items.push_back(FlatItem{
          .code = code,
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
      int effective_code = code;
      std::string jump_raw_pat;
      for (int j = 0; j < vec->num_elem; ++j) {
        Rtx sub = vec->elem[j].rtx;
        if (sub == nullptr) {
          continue;
        }
        const std::string tag = format_insn_tag(sub);
        std::string sub_pat = canonicalize_label_refs(FormatRtx(sub->fld[3].rtx));
        if (sub->fld[6].rtx != nullptr) {
          absl::StrAppend(&sub_pat, " ", canonicalize_label_refs(FormatRtx(sub->fld[6].rtx)));
        }
        formatted_inner.push_back(
            absl::StrCat("(", tag, " ", CanonicalizeRtlPattern(sub_pat), ")"));
        if (sub->code == kJumpInsn) {
          effective_code = kJumpInsn;
          jump_raw_pat = FormatRtx(sub->fld[3].rtx);
        }
      }
      items.push_back(FlatItem{
          .code = effective_code,
          .uid = insn->fld[0].rtint,
          .raw_pattern_for_targets = jump_raw_pat,
          .pattern = absl::StrCat("(sequence [", absl::StrJoin(formatted_inner, " "), "])"),
      });
      continue;
    }

    std::string raw_pat = FormatRtx(pat);
    std::string canon_pat = canonicalize_label_refs(raw_pat);
    if (insn->fld[6].rtx != nullptr) {
      absl::StrAppend(&canon_pat, " ", canonicalize_label_refs(FormatRtx(insn->fld[6].rtx)));
    }
    items.push_back(FlatItem{
        .code = code,
        .uid = insn->fld[0].rtint,
        .raw_pattern_for_targets = raw_pat,
        .pattern = CanonicalizeRtlPattern(canon_pat),
    });
  }

  if (items.empty()) {
    result.rpo_blocks.push_back(RtlBasicBlock{});
    return result;
  }

  struct RawBlock {
    std::vector<int> label_uids;
    std::vector<std::string> instructions;
    std::vector<int> jump_target_labels;
    bool ends_with_jump = false;
    bool ends_with_uncond_jump = false;
    bool ends_with_return = false;
    bool followed_by_barrier = false;
  };

  std::vector<RawBlock> raw_blocks;
  absl::flat_hash_map<int, size_t> label_to_block;

  size_t i = 0;
  while (i < items.size()) {
    while (i < items.size() && items[i].code == kBarrier) {
      if (!raw_blocks.empty()) {
        raw_blocks.back().followed_by_barrier = true;
      }
      ++i;
    }
    if (i >= items.size()) {
      break;
    }

    RawBlock blk;
    while (i < items.size() && items[i].code == kCodeLabel) {
      blk.label_uids.push_back(items[i].uid);
      ++i;
    }
    while (i < items.size() && items[i].code != kCodeLabel && items[i].code != kBarrier) {
      const auto& it = items[i];
      if (it.code == kInsn || it.code == kCallInsn || it.code == kNote) {
        blk.instructions.push_back(CanonicalizeRtlPattern(it.pattern));
        ++i;
      } else if (it.code == kJumpInsn) {
        blk.ends_with_jump = true;
        blk.instructions.push_back(CanonicalizeRtlPattern(it.pattern));
        const std::string raw_jump_pat = CanonicalizeRtlPattern(it.raw_pattern_for_targets);
        if (absl::StrContains(raw_jump_pat, "(return)")) {
          blk.ends_with_return = true;
        } else if (absl::StartsWith(raw_jump_pat, "(set (pc) (label_ref")) {
          blk.ends_with_uncond_jump = true;
          blk.jump_target_labels = extract_label_refs(it.raw_pattern_for_targets);
        } else {
          blk.jump_target_labels = extract_label_refs(it.raw_pattern_for_targets);
        }
        ++i;
        break;
      } else {
        ++i;
      }
    }
    while (i < items.size() && items[i].code == kBarrier) {
      blk.followed_by_barrier = true;
      ++i;
    }
    if (blk.instructions.empty() && blk.label_uids.empty() && !blk.ends_with_return) {
      continue;
    }
    raw_blocks.push_back(std::move(blk));
  }

  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    for (int l_uid : raw_blocks[b].label_uids) {
      label_to_block[l_uid] = b;
    }
  }

  std::vector<std::vector<size_t>> raw_succs(raw_blocks.size());
  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    const auto& blk = raw_blocks[b];
    if (blk.ends_with_return) {
      continue;
    }
    for (int target_lbl : blk.jump_target_labels) {
      auto it = label_to_block.find(target_lbl);
      if (it != label_to_block.end()) {
        raw_succs[b].push_back(it->second);
      }
    }
    const bool has_fallthrough =
        !blk.followed_by_barrier && !blk.ends_with_uncond_jump && !blk.ends_with_return;
    if (has_fallthrough && b + 1 < raw_blocks.size()) {
      raw_succs[b].push_back(b + 1);
    }
  }

  result.rpo_blocks.reserve(raw_blocks.size());
  for (size_t b = 0; b < raw_blocks.size(); ++b) {
    RtlBasicBlock out_blk;
    out_blk.id = static_cast<uint32_t>(b);
    for (const auto& pat : raw_blocks[b].instructions) {
      out_blk.AddPatternOnly(pat);
    }
    out_blk.successors = std::move(raw_succs[b]);
    result.rpo_blocks.push_back(std::move(out_blk));
  }
  for (size_t b = 0; b < result.rpo_blocks.size(); ++b) {
    for (size_t s : result.rpo_blocks[b].successors) {
      if (s < result.rpo_blocks.size()) {
        result.rpo_blocks[s].predecessors.push_back(b);
      }
    }
  }
  return result;
}

Rtx GccContext::GetInsns() const {
  return get_insns();
}

}  // namespace rom_nom_nom::gcc_harness
