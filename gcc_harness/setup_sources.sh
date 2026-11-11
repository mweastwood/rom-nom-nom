#!/usr/bin/env bash
# Clean-room source bootstrap and static library builder for gcc_harness.
#
# Never commits or distributes GPL source code in git. Checks if
# toolchain/gcc-2.7.2-src and toolchain/binutils-2.6-src exist locally; if
# missing, downloads the official archives with SHA-256 verification, applies
# the KMC MIPS target configuration, compiles GCC 2.7.2 and GAS 2.6 with
# inlining disabled so internal static pass helper functions remain distinct
# symbols, globalizes those symbols via objcopy, and produces:
#   - toolchain/libgcc272_harness.a
#   - toolchain/libgas26_harness.a

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN_DIR="${REPO_ROOT}/toolchain"
GCC_SRC="${TOOLCHAIN_DIR}/gcc-2.7.2-src"
BINUTILS_SRC="${TOOLCHAIN_DIR}/binutils-2.6-src"

GCC_URL="https://ftp.gnu.org/old-gnu/gcc/gcc-2.7.2.tar.gz"
GCC_SHA256="7cd8bce5c3aeec59a72ecc2d3d5123864a817b14cdbd0680b1a969c3bccc5da5"

BINUTILS_URL="https://ftp.gnu.org/old-gnu/binutils/binutils-2.6.tar.gz"
BINUTILS_SHA256="ee5f5ef22688c8fb45c2784b26c443a9a97c0e97f14b3cb1ca39d1325c46f097"
KMC_BINUTILS_URL="https://github.com/decompals/mips-binutils-2.6/archive/refs/tags/v0.3.tar.gz"
KMC_BINUTILS_SHA256="ffd30d2200a6a1f258c8144f89ba2ed6513512477f20eb1fc3e995d5cbdba34a"

HARNESS_CFLAGS="-O1 -fno-inline -fno-inline-functions -fno-inline-small-functions -fno-inline-functions-called-once -fgnu89-inline -fcommon -w"
CC_CMD="gcc -std=gnu89"

mkdir -p "${TOOLCHAIN_DIR}"

# -----------------------------------------------------------------------------
# 1. Bootstrap GCC 2.7.2 and Binutils 2.6 sources if not already present
# -----------------------------------------------------------------------------
if [[ ! -d "${GCC_SRC}" ]]; then
  echo "[gcc_harness] Downloading ${GCC_URL}..."
  TMP_GCC_TAR="$(mktemp /tmp/gcc-2.7.2.XXXXXX.tar.gz)"
  curl -fSL "${GCC_URL}" -o "${TMP_GCC_TAR}"
  echo "${GCC_SHA256}  ${TMP_GCC_TAR}" | sha256sum -c -
  mkdir -p "${GCC_SRC}"
  tar -xzf "${TMP_GCC_TAR}" --strip-components=1 -C "${GCC_SRC}"
  rm -f "${TMP_GCC_TAR}"
fi

if [[ ! -d "${BINUTILS_SRC}" ]]; then
  echo "[gcc_harness] Downloading Binutils 2.6 source archive..."
  TMP_BINUTILS_TAR="$(mktemp /tmp/binutils-2.6.XXXXXX.tar.gz)"
  if curl -fsSL "${KMC_BINUTILS_URL}" -o "${TMP_BINUTILS_TAR}"; then
    echo "${KMC_BINUTILS_SHA256}  ${TMP_BINUTILS_TAR}" | sha256sum -c -
  else
    curl -fSL "${BINUTILS_URL}" -o "${TMP_BINUTILS_TAR}"
    echo "${BINUTILS_SHA256}  ${TMP_BINUTILS_TAR}" | sha256sum -c -
  fi
  mkdir -p "${BINUTILS_SRC}"
  tar -xzf "${TMP_BINUTILS_TAR}" --strip-components=1 -C "${BINUTILS_SRC}"
  rm -f "${TMP_BINUTILS_TAR}"
fi

# Patch obstack.h lvalue casts for modern GCC in both source trees.
python3 - "${GCC_SRC}/obstack.h" "${BINUTILS_SRC}/include/obstack.h" << 'PYEOF'
import sys

for path in sys.argv[1:]:
  with open(path, "r", encoding="latin-1") as f:
    text = f.read()
  text = text.replace(
      "*((void **)__o->next_free)++ = ((void *)datum);",
      "*(void **)__o->next_free = (void *)(datum), __o->next_free += sizeof (void *);",
  )
  text = text.replace(
      "*((int *)__o->next_free)++ = ((int)datum);",
      "*(int *)__o->next_free = (int)(datum), __o->next_free += sizeof (int);",
  )
  text = text.replace(
      "#define obstack_ptr_grow_fast(h,aptr) (*((void **)(h)->next_free)++ = (void *)aptr)",
      "#define obstack_ptr_grow_fast(h,aptr) (*(void **)(h)->next_free = (void *)(aptr), (h)->next_free += sizeof (void *))",
  )
  text = text.replace(
      "#define obstack_int_grow_fast(h,aint) (*((int *)(h)->next_free)++ = (int)aint)",
      "#define obstack_int_grow_fast(h,aint) (*(int *)(h)->next_free = (int)(aint), (h)->next_free += sizeof (int))",
  )
  with open(path, "w", encoding="latin-1") as f:
    f.write(text)
PYEOF

# Ensure `mips.md` matches stock KMC GCC 2.7.2 (`define_expand "return"` and
# `define_insn "return_internal"` only, with no synthetic `return_internal_bare`
# pattern).
python3 - "${GCC_SRC}/config/mips/mips.md" << 'PYEOF'
import glob
import os
import re
import sys

path = sys.argv[1]
with open(path, "r", encoding="latin-1") as f:
  text = f.read()

orig_expand = """(define_expand "return"
  [(parallel [(return)
\t      (use (reg:SI 31))])]
  "simple_epilogue_p ()"
  "")"""

broken_expand = """(define_expand "return"
  [(return)]
  "simple_epilogue_p ()"
  "")"""

updated = text
if broken_expand in updated:
  updated = updated.replace(broken_expand, orig_expand, 1)

updated = re.sub(
    r'\n*\(define_insn "return_internal_bare"[\s\S]*?\[(?:\(set_attr[^\]]*\)\s*)+\]\)\n*',
    "\n\n",
    updated,
)

if updated != text:
  with open(path, "w", encoding="latin-1") as f:
    f.write(updated)
  gcc_dir = os.path.dirname(os.path.dirname(os.path.dirname(path)))
  for pat in ("*.o", "insn-*.c", "insn-*.h", "s-*"):
    for fn in glob.glob(os.path.join(gcc_dir, pat)):
      os.remove(fn)
PYEOF


# -----------------------------------------------------------------------------
# 2. Configure and build GCC 2.7.2 backend objects
# -----------------------------------------------------------------------------
echo "[gcc_harness] Configuring and compiling GCC 2.7.2 backend..."
if [[ ! -f "${GCC_SRC}/Makefile" ]]; then
  (cd "${GCC_SRC}" && ./configure --target=mips-gnu --host=i386-pc-linux --build=i386-pc-linux)
fi

cat << 'EOF' > "${GCC_SRC}/tm.h"
#ifndef TARGET_DEFAULT
#define TARGET_DEFAULT 16
#endif
#ifndef TARGET_CPU_DEFAULT
#define TARGET_CPU_DEFAULT 16
#endif
#include "mips/gnu.h"
#undef SMALL_DATA_SECTION
#define SMALL_DATA_SECTION const_section
EOF

GCC_OBJS=(
  c-parse.o c-lang.o c-lex.o c-pragma.o c-decl.o c-typeck.o c-convert.o
  c-aux-info.o c-common.o c-iterate.o toplev.o version.o tree.o print-tree.o
  stor-layout.o fold-const.o function.o stmt.o expr.o calls.o expmed.o
  explow.o optabs.o varasm.o rtl.o print-rtl.o rtlanal.o emit-rtl.o real.o
  dbxout.o sdbout.o dwarfout.o xcoffout.o integrate.o jump.o cse.o loop.o
  unroll.o flow.o stupid.o combine.o regclass.o local-alloc.o global.o
  reload.o reload1.o caller-save.o insn-peep.o reorg.o sched.o final.o
  recog.o reg-stack.o insn-opinit.o insn-recog.o insn-extract.o insn-output.o
  insn-emit.o insn-attrtab.o mips.o getpwd.o convert.o bc-emit.o bc-optab.o
  obstack.o
)

make -C "${GCC_SRC}" -j"$(nproc)" \
  CC="${CC_CMD}" HOST_CC="${CC_CMD}" \
  CFLAGS="${HARNESS_CFLAGS}" HOST_CFLAGS="${HARNESS_CFLAGS}" \
  insn-config.h insn-flags.h insn-codes.h insn-attr.h \
  insn-emit.c insn-recog.c insn-opinit.c insn-extract.c \
  insn-peep.c insn-attrtab.c insn-output.c \
  bc-arity.h bc-opcode.h bc-opname.h \
  "${GCC_OBJS[@]}"

GCC_STAGE_DIR="$(mktemp -d /tmp/gcc272_harness_stage.XXXXXX)"
trap 'rm -rf "${GCC_STAGE_DIR}" "${GAS_STAGE_DIR:-}"' EXIT

for obj in "${GCC_OBJS[@]}"; do
  cp "${GCC_SRC}/${obj}" "${GCC_STAGE_DIR}/${obj}"
done

# Disambiguate file-static variables that share names across different GCC passes
objcopy --redefine-sym main=gcc_toplev_main "${GCC_STAGE_DIR}/toplev.o"
objcopy --redefine-sym max_uid=reorg_max_uid "${GCC_STAGE_DIR}/reorg.o"
objcopy --redefine-sym uid_cuid=combine_uid_cuid "${GCC_STAGE_DIR}/combine.o"
objcopy \
  --redefine-sym reg_qty=lra_reg_qty \
  --redefine-sym next_qty=lra_next_qty \
  --redefine-sym qty_first_reg=lra_qty_first_reg \
  --redefine-sym qty_mode=lra_qty_mode \
  --redefine-sym this_insn=lra_this_insn \
  "${GCC_STAGE_DIR}/local-alloc.o"
objcopy \
  --redefine-sym max_uid=cse_max_uid \
  --redefine-sym uid_cuid=cse_uid_cuid \
  --redefine-sym table=cse_table \
  "${GCC_STAGE_DIR}/cse.o"

# Globalize internal static functions and pass-state variables
cat << 'EOF' > "${GCC_STAGE_DIR}/gcc_globalize.txt"
find_end_label
make_return_insns
fill_simple_delay_slots
fill_eager_delay_slots
fill_slots_from_thread
relax_delay_slots
steal_delay_list_from_target
steal_delay_list_from_fallthrough
own_thread_p
mark_target_live_regs
emit_delay_sequence
redundant_insn
reorg_max_uid
uid_to_ruid
unfilled_slots_obstack
unfilled_firstobj
end_of_function_needs
start_of_epilogue_needs
end_of_function_label
target_hash_table
bb_ticks
reorg_pass_number
find_cross_jump
do_cross_jump
delete_jump
redirect_exp
jump_back_p
tension_vector_labels
jump_chain
max_jump_chain
schedule_block
sched_analyze
sched_analyze_insn
rank_for_schedule
priority
sched_before_next_call
insn_luid
insn_priority
insn_tick
insn_costs
insn_units
insn_blockage
insn_ref_count
sched_reg_n_calls_crossed
sched_reg_live_length
bb_dead_regs
bb_live_regs
dead_notes
old_live_regs
line_note
line_note_head
note_list
reg_last_uses
reg_last_sets
reg_pending_sets
reg_pending_sets_all
block_alloc
update_equiv_regs
find_free_reg
alloc_qty
wipe_dead_reg
qty_phys_reg
qty_phys_copy_sugg
qty_phys_num_copy_sugg
qty_phys_sugg
qty_phys_num_sugg
qty_birth
qty_death
qty_scratch_rtx
lra_qty_first_reg
qty_size
lra_qty_mode
qty_n_calls_crossed
qty_min_class
qty_alternate_class
qty_n_refs
qty_changes_size
lra_reg_qty
reg_offset
reg_next_in_qty
lra_next_qty
scratch_index
find_reg
record_one_conflict
prune_preferences
global_conflicts
expand_preferences
allocno_compare
max_allocno
reg_allocno
allocno_reg
allocno_order
allocno_size
reg_may_share
conflicts
allocno_row_words
hard_reg_conflicts
hard_reg_preferences
hard_reg_copy_preferences
hard_reg_full_preferences
regs_someone_prefers
no_global_alloc_regs
regs_used_so_far
allocno_calls_crossed
allocno_n_refs
allocno_live_length
local_reg_n_refs
local_reg_live_length
allocnos_live
eliminable_regset
alter_reg
choose_reload_regs
emit_reload_insns
eliminate_regs_in_insn
insert_save_restore
try_combine
subst
simplify_rtx
simplify_logical
simplify_shift_const
make_compound_operation
expand_compound_operation
make_extraction
make_field_assignment
known_cond
recog_for_combine
set_nonzero_bits_and_sign_copies
init_reg_last_arrays
setup_incoming_promotions
combine_max_regno
undobuf
previous_num_undos
reg_nonzero_bits
reg_sign_bit_copies
reg_last_death
reg_last_set
reg_last_set_value
reg_last_set_table_tick
reg_last_set_label
reg_last_set_invalid
reg_last_set_mode
reg_last_set_nonzero_bits
reg_last_set_sign_bit_copies
combine_uid_cuid
max_uid_cuid
nonzero_bits_mode
label_tick
subst_prev_insn
subst_low_cuid
this_basic_block
last_call_cuid
mem_last_set
find_basic_blocks
life_analysis
propagate_block
mark_used_regs
mark_set_regs
insn_dead_p
try_pre_increment_1
uid_block_number
uid_volatile
max_uid_for_flow
basic_block_drops_in
basic_block_loop_depth
cse_basic_block
fold_rtx
equiv_constant
canon_reg
cse_insn
new_basic_block
cse_max_uid
cse_uid_cuid
max_reg
max_qty
next_qty
qty_first_reg
qty_last_reg
qty_mode
qty_const
qty_const_insn
qty_comparison_code
qty_comparison_const
qty_comparison_qty
reg_qty
reg_in_table
reg_tick
reg_next_eqv
reg_prev_eqv
cse_basic_block_start
cse_basic_block_end
cse_jumps_altered
consec_ints
all_minus_one
cse_table
free_element_chain
memory_extend_rtx
regs_invalidated_by_call
hard_regs_in_table
max_elements_made
n_elements_made
scan_loop
strength_reduce
count_loop_regs_set
find_and_verify_loops
record_biv
record_giv
check_dbra_loop
final_biv_value
final_giv_value
move_movables
prescan_loop
moved_once
max_loop_num
loop_number_loop_starts
loop_number_loop_ends
loop_invalid
loop_has_call
loop_has_volatile
loop_continue
may_not_optimize
n_times_set
n_times_used
loops_enclosed
temp_obstack
synth_mult
choose_multiplier
print_rtx
do_store_flag
virtuals_instantiated
cse_end_of_basic_block
recorded_label_ref
constant_pool_entries_cost
loop_dump_stream
reg_next_use
regs_live_at_setjmp
nonzero_sign_valid
subst_insn
record_dead_and_set_regs
unused_insn_list
unused_expr_list
update_flow_info
line_note_exists
last_ignored_compare
elim_reg_set
EOF

for obj in "${GCC_OBJS[@]}"; do
  objcopy --globalize-symbols="${GCC_STAGE_DIR}/gcc_globalize.txt" \
    "${GCC_STAGE_DIR}/${obj}"
done

rm -f "${TOOLCHAIN_DIR}/libgcc272_harness.a"
(cd "${GCC_STAGE_DIR}" && ar rcs "${TOOLCHAIN_DIR}/libgcc272_harness.a" "${GCC_OBJS[@]}")

# -----------------------------------------------------------------------------
# 3. Configure and build Binutils 2.6 GAS objects
# -----------------------------------------------------------------------------
echo "[gcc_harness] Configuring and compiling Binutils 2.6 GAS..."
if [[ ! -f "${BINUTILS_SRC}/gas/Makefile" ]]; then
  (cd "${BINUTILS_SRC}" && ./configure --target=mips-kmc-elf --host=i386-pc-linux --build=i386-pc-linux)
fi

for dir in libiberty bfd opcodes; do
  make -C "${BINUTILS_SRC}/${dir}" -j"$(nproc)" \
    CC="${CC_CMD}" CFLAGS="${HARNESS_CFLAGS}" all
done
make -C "${BINUTILS_SRC}/gas" -j"$(nproc)" \
  CC="${CC_CMD}" CFLAGS="${HARNESS_CFLAGS}" as.new

GAS_STAGE_DIR="$(mktemp -d /tmp/gas26_harness_stage.XXXXXX)"

(cd "${GAS_STAGE_DIR}" && ar x "${BINUTILS_SRC}/bfd/libbfd.a")
(cd "${GAS_STAGE_DIR}" && ar x "${BINUTILS_SRC}/opcodes/libopcodes.a")
(cd "${GAS_STAGE_DIR}" && ar x "${BINUTILS_SRC}/libiberty/libiberty.a")

GAS_OBJS=(
  targ-cpu.o obj-format.o atof-targ.o app.o as.o atof-generic.o
  bignum-copy.o cond.o expr.o flonum-konst.o flonum-copy.o flonum-mult.o
  frags.o hash.o input-file.o input-scrub.o literal.o messages.o
  output-file.o read.o subsegs.o symbols.o write.o listing.o ecoff.o
  stabs.o sb.o macro.o e-mipself.o
)

cat << 'EOF' > "${GAS_STAGE_DIR}/gas_globalize.txt"
append_insn
macro
macro_build
macro_build_lui
mips_ip
load_register
macro_end
mips_init_after_args
s_change_sec
s_mipsset
imm_expr
offset_expr
imm_reloc
offset_reloc
insn_error
mips_noreorder
prev_insn_valid
mips_no_prev_insn
mips_emit_delays
mips_optimize
mips_debug
mips_isa
file_mips_isa
mips_cpu
mips_trap
EOF

cat << 'EOF' > "${GAS_STAGE_DIR}/gas_redefine.txt"
main gas_main
data_section gas_data_section
text_section gas_text_section
get_run_time gas_get_run_time
spaces gas_spaces
xmalloc gas_xmalloc
xrealloc gas_xrealloc
EOF

for obj in "${GAS_OBJS[@]}"; do
  cp "${BINUTILS_SRC}/gas/${obj}" "${GAS_STAGE_DIR}/gas_${obj}"
  objcopy --globalize-symbols="${GAS_STAGE_DIR}/gas_globalize.txt" \
    "${GAS_STAGE_DIR}/gas_${obj}"
done

for obj in "${GAS_STAGE_DIR}"/*.o; do
  objcopy --redefine-syms="${GAS_STAGE_DIR}/gas_redefine.txt" "${obj}"
done

rm -f "${TOOLCHAIN_DIR}/libgas26_harness.a"
(cd "${GAS_STAGE_DIR}" && ar rcs "${TOOLCHAIN_DIR}/libgas26_harness.a" *.o)

echo "[gcc_harness] Successfully built:"
echo "  - ${TOOLCHAIN_DIR}/libgcc272_harness.a"
echo "  - ${TOOLCHAIN_DIR}/libgas26_harness.a"
