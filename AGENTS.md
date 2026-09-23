# AGENTS.md — Development Guidelines & Decompilation Workflow

This document establishes the architecture rules, coding standards, and step-by-step decompilation workflow for AI agents and human contributors working on `rom-nom-nom`.

---

## 1. Core Principles & Constraints

1. **100% Bit-Exact Match**: Every C decompilation change must preserve a byte-for-byte SHA-1 bit-exact match against the original retail ROM (`bazel test //...`).
2. **Dual-Track Architecture**:
   - `src/c/<game>/`: Pure C implementation compiled by KMC GCC 2.7.2 targeting the N64 hardware.
   - `src/cc/<game>/`: Modern C++20 implementation targeting modern host architectures, mirroring the C functionality.
   - `mocks/ultra64/`: Host-based mocks for Libultra and N64 hardware registers.
   - `tests/<game>/`: Shared GoogleTest suites verifying that both C and C++ implementations behave identically.
3. **Google Style Guide Compliance**:
   - **Functions & Methods**: Strict `CamelCase` without underscores (e.g. `MessageInit`, `MessageClipSpan`, `MockUltra64Reset`).
   - **Variables & Members**: `lower_snake_case` (e.g. `box_index`, `bank_id`, `scale_x`).
   - **Constants & Enums**: `kCamelCase` with `k` prefix (e.g. `kIdle`, `kMainProc`).
   - Exceptions are restricted to N64 hardware/OS entrypoints (`main`, `idle`, `mainproc`, `os*`), assembly labels (`func_*`, `D_*`), and test macros (`TEST`, `TEST_F`).
4. **Git Protocol**: **NEVER** commit or push changes without explicit user request.

---

## 2. Essential Tool Commands

| Command | Purpose |
| :--- | :--- |
| `bazel run //:progress` | Check overall `.text` executable code decompilation progress and module list. |
| `bazel run //:m2c -- <func_name>` | Decompile a function from assembly to C using dynamic on-the-fly context. |
| `bazel run //:m2c -- --dump-context` | Export fresh preprocessed C context (e.g. for pasting into [decomp.me](https://decomp.me)). |
| `bazel run //:diff -- <func_name>` | Side-by-side assembly diff comparing target ROM instructions against compiled C. |
| `bazel run //:diff -- <func_name> --watch` | Live-reloading diff watcher that updates on file save. |
| `bazel run //:format` | Format all C/C++ files across the codebase using `clang-format`. |
| `bazel run //:tidy -- --check` | Verify naming conventions and language safety with `clang-tidy`. |
| `bazel test //...` | Run all bit-exact ROM verification tests and unit test suites. |

---

## 3. Step-by-Step Decompilation Workflow

Follow this procedure when decompiling a new function or module:

### Step 1: Identify Target Function & Inspect Assembly
Check current progress with `bazel run //:progress`. Identify the target function in `bazel-bin/asm/<game>/*.s` (e.g. `func_800266C0`).

Inspect the target instructions and length:
```bash
bazel run //:diff -- func_800266C0
```

### Step 2: Generate Initial C Draft with m2c
Run `m2c` to produce an initial C draft:
```bash
bazel run //:m2c -- func_800266C0
```
`m2c` automatically compiles the latest context from `common.h` and project headers in memory, maps struct fields, and auto-formats the C draft with `clang-format`.

### Step 3: Implement C Code & Iterate with diff
1. Place or append the C code into the appropriate module file under `src/c/<game>/`.
2. Name the function using Google `CamelCase`. Add an alias linking it to the ROM symbol name:
   ```c
   s32 MyFunction(s32 arg0) { ... }
   s32 func_800266C0(s32 arg0) __attribute__((alias("MyFunction")));
   ```
3. Start the live-reload diff watcher:
   ```bash
   bazel run //:diff -- MyFunction --watch
   ```
4. Adjust types, loop constructs, variable order, and expressions until the diff displays:
   ```
   *** [MATCH 100%] N/N instructions match bit-exact! ***
   ```

### Step 4: Update Splat Segment Split (When File is Complete)
When all functions in an assembly split range are decompiled into a C file:
1. Open `splat/<game>.yaml`.
2. Replace the `asm` subsegment with your new `c` entry:
   ```yaml
   - [0x1AC0, c, my_module]
   ```
3. Rebuild the ROM:
   ```bash
   bazel test //:harvest_moon_64_rom_bitexact_test
   ```

### Step 5: Verification & Style Gates
Always run the full verification battery:
```bash
bazel run //:format
bazel run //:tidy -- --check
bazel test //...
```
Ensure 0 format violations, 0 clang-tidy errors, and 100% bit-exact ROM test passing.

### Step 6: Baseline Unit Testing (C Implementation)
Before writing any C++ code, establish behavioral tests against the decompiled C implementation:
1. Define the component's abstract test interface in `tests/<game>/<component>_test_interface.h`.
2. Implement the C adapter in `tests/<game>/<component>_c_adapter.cc` calling the C functions and linking against `mocks/ultra64/`.
3. Author GoogleTest cases in `tests/<game>/<component>_test.cc` that thoroughly exercise all code paths, edge cases, and hardware interactions.
4. Execute the C test target to verify the baseline:
   ```bash
   bazel test //tests/<game>:<component>_c_test
   ```

### Step 7: Dual-Track C++ Port & Equivalency Verification
Once the C implementation passes all unit tests, port the logic to modern C++:
1. Re-implement the functionality using modern C++20 patterns (RAII, type safety, namespaces) in `src/cc/<game>/`.
2. Implement the C++ adapter in `tests/<game>/<component>_cc_adapter.cc`.
3. Run the C++ unit test target:
   ```bash
   bazel test //tests/<game>:<component>_cc_test
   ```
4. Verify complete equivalency across both tracks:
   ```bash
   bazel test //tests/<game>/...
   ```
   Because both `<component>_c_test` and `<component>_cc_test` execute the exact same shared test suite, passing both proves 100% behavioral equivalency between the C and C++ codebases.

