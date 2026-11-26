#ifndef LIFTER_STAGE0_TEST_UTILS_H_
#define LIFTER_STAGE0_TEST_UTILS_H_

#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Controls how `BakeLinkedRomStage0Input` lays out simulated linked N64 ROM
// addresses and metadata for Stage 0..N verification tests.
struct LinkedRomBakeOptions {
  bool same_hi_page_for_globals = true;
  bool reverse_jump_tables = true;
  bool strip_table_and_pool_labels = true;
};

// Simulates linking a relocatable `.o` `Stage0FunctionInput` into a fixed-VRAM
// N64 ROM slice (`base_vram = 0x80020000`, `gp_vram = 0x80060000`, empty
// `relocations`, populated `vram_to_symbol` / `vram_to_symbol_size`, and
// relocated machine words).
Stage0FunctionInput BakeLinkedRomStage0Input(const Stage0FunctionInput& reloc_input,
                                             const LinkedRomBakeOptions& options = {});

// Returns true iff baking `reloc_input` into `linked` can produce any
// observable difference in Stage 0..N lifting or suffix verification.
bool HasDistinctLinkedRomRepresentation(const Stage0FunctionInput& reloc_input,
                                        const Stage0FunctionInput& linked);

}  // namespace rom_nom_nom::lifter

#endif  // LIFTER_STAGE0_TEST_UTILS_H_
