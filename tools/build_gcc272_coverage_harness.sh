#!/usr/bin/env bash
# Builds an isolated, gcov-instrumented GCC 2.7.2 harness static archive
# (`toolchain/libgcc272_harness_cov.a`) for on-demand per-pass line and branch
# coverage analysis without modifying `toolchain/gcc-2.7.2-src/` or
# `toolchain/libgcc272_harness.a`.
#
# Only the 14 GCC 2.7.2 RTL/backend pass files are compiled with
# `--coverage -fprofile-abs-path` into `toolchain/gcc-2.7.2-cov-build/`; all
# remaining `.o` members are copied verbatim from `toolchain/libgcc272_harness.a`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN_DIR="${REPO_ROOT}/toolchain"
GCC_SRC="${TOOLCHAIN_DIR}/gcc-2.7.2-src"
BASE_ARCHIVE="${TOOLCHAIN_DIR}/libgcc272_harness.a"
COV_ARCHIVE="${TOOLCHAIN_DIR}/libgcc272_harness_cov.a"
COV_BUILD="${TOOLCHAIN_DIR}/gcc-2.7.2-cov-build"
STAGE_DIR="${COV_BUILD}/stage"
SETUP_SCRIPT="${REPO_ROOT}/gcc_harness/setup_sources.sh"

if [[ ! -f "${BASE_ARCHIVE}" || ! -f "${GCC_SRC}/tm.h" ]]; then
  echo "[gcc_pass_coverage] Missing ${BASE_ARCHIVE} or ${GCC_SRC}/tm.h." >&2
  echo "[gcc_pass_coverage] Run ./tools/setup_gcc_harness.sh first." >&2
  exit 1
fi

HARNESS_CFLAGS="-O1 -fno-inline -fno-inline-functions -fno-inline-small-functions -fno-inline-functions-called-once -fgnu89-inline -fcommon -w"
CC_CMD=(gcc -std=gnu89 -DCROSS_COMPILE -DIN_GCC ${HARNESS_CFLAGS} --coverage -fprofile-abs-path "-I${GCC_SRC}" "-I${GCC_SRC}/config")

# Clean and recreate isolated coverage build directory.
rm -rf "${COV_BUILD}"
mkdir -p "${STAGE_DIR}"

# 1. Extract all baseline `.o` objects from `libgcc272_harness.a` (read-only).
(cd "${STAGE_DIR}" && ar x "${BASE_ARCHIVE}")

# 2. Extract the authoritative globalize symbol list from `setup_sources.sh`.
GLOBALIZE_LIST="${COV_BUILD}/gcc_globalize.txt"
python3 - "${SETUP_SCRIPT}" "${GLOBALIZE_LIST}" <<'PYEOF'
import sys

src_path, dst_path = sys.argv[1], sys.argv[2]
with open(src_path, "r", encoding="utf-8") as f:
  lines = f.readlines()

marker = 'cat << \'EOF\' > "${GCC_STAGE_DIR}/gcc_globalize.txt"'
start = None
for i, line in enumerate(lines):
  if marker in line:
    start = i + 1
    break
if start is None:
  raise RuntimeError(f"Could not locate gcc_globalize.txt block in {src_path}")

syms = []
for line in lines[start:]:
  if line.strip() == "EOF":
    break
  syms.append(line)

with open(dst_path, "w", encoding="utf-8") as f:
  f.writelines(syms)
PYEOF

# 3. Compile ONLY the 14 GCC 2.7.2 pass `.c` files into `${COV_BUILD}/`
#    (leaving `.gcno` files in `${COV_BUILD}/` and touching nothing in `${GCC_SRC}/`).
PASS_SPECS=(
  "final.c:final.o"
  "reorg.c:reorg.o"
  "jump.c:jump.o"
  "sched.c:sched.o"
  "reload.c:reload.o"
  "reload1.c:reload1.o"
  "caller-save.c:caller-save.o"
  "global.c:global.o"
  "local-alloc.c:local-alloc.o"
  "flow.c:flow.o"
  "combine.c:combine.o"
  "cse.c:cse.o"
  "loop.c:loop.o"
  "config/mips/mips.c:mips.o"
)

echo "[gcc_pass_coverage] Compiling 14 instrumented GCC 2.7.2 pass files into ${COV_BUILD}..."
for spec in "${PASS_SPECS[@]}"; do
  src_rel="${spec%%:*}"
  obj_name="${spec##*:}"
  "${CC_CMD[@]}" -c "${GCC_SRC}/${src_rel}" -o "${COV_BUILD}/${obj_name}"
done

# 4. Apply exact `objcopy --redefine-sym` and `--globalize-symbols` transforms
#    to copies in `${STAGE_DIR}/` (keeping `.gcno` intact in `${COV_BUILD}/`).
for spec in "${PASS_SPECS[@]}"; do
  obj_name="${spec##*:}"
  cp "${COV_BUILD}/${obj_name}" "${STAGE_DIR}/${obj_name}"
done

objcopy --redefine-sym max_uid=reorg_max_uid "${STAGE_DIR}/reorg.o"
objcopy --redefine-sym uid_cuid=combine_uid_cuid "${STAGE_DIR}/combine.o"
objcopy \
  --redefine-sym reg_qty=lra_reg_qty \
  --redefine-sym next_qty=lra_next_qty \
  --redefine-sym qty_first_reg=lra_qty_first_reg \
  --redefine-sym qty_mode=lra_qty_mode \
  --redefine-sym this_insn=lra_this_insn \
  "${STAGE_DIR}/local-alloc.o"
objcopy \
  --redefine-sym max_uid=cse_max_uid \
  --redefine-sym uid_cuid=cse_uid_cuid \
  --redefine-sym table=cse_table \
  "${STAGE_DIR}/cse.o"

for spec in "${PASS_SPECS[@]}"; do
  obj_name="${spec##*:}"
  objcopy --globalize-symbols="${GLOBALIZE_LIST}" "${STAGE_DIR}/${obj_name}"
done

# 5. Archive `toolchain/libgcc272_harness_cov.a` preserving member order from baseline.
mapfile -t BASE_MEMBERS < <(ar t "${BASE_ARCHIVE}")
rm -f "${COV_ARCHIVE}"
(cd "${STAGE_DIR}" && ar rcs "${COV_ARCHIVE}" "${BASE_MEMBERS[@]}")

# 6. Ensure `toolchain/BUILD.bazel` exposes manual `gcc272_harness_cov_lib`.
if [[ -f "${TOOLCHAIN_DIR}/BUILD.bazel" ]] && ! grep -q 'name = "gcc272_harness_cov_lib"' "${TOOLCHAIN_DIR}/BUILD.bazel"; then
  cat <<'EOF' >> "${TOOLCHAIN_DIR}/BUILD.bazel"

cc_import(
    name = "gcc272_harness_cov_lib",
    testonly = True,
    static_library = "libgcc272_harness_cov.a",
    tags = ["manual"],
)
EOF
fi

echo "[gcc_pass_coverage] Built ${COV_ARCHIVE} (14 instrumented passes, .gcno in ${COV_BUILD})."
