#include "splitter/linker_script.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "splitter/config.pb.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

std::string SanitizeIdentifier(std::string_view name) {
  std::string result(name);
  for (char& c : result) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
      c = '_';
    }
  }
  if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0]))) {
    result.insert(result.begin(), '_');
  }
  return result;
}

}  // namespace

LinkerScriptGenerator::LinkerScriptGenerator(LinkerScriptOptions options)
    : options_(std::move(options)) {}

std::string LinkerScriptGenerator::ResolveObjectPath(std::string_view game_name,
                                                     const Segment& segment,
                                                     const Subsegment* subsegment) const {
  std::string prefix = options_.base_build_dir.empty()
                           ? std::string(game_name)
                           : absl::StrCat(options_.base_build_dir, "/", game_name);

  if (subsegment != nullptr) {
    std::string_view sub_name = subsegment->name();
    switch (subsegment->type()) {
      case SUBSEGMENT_C:
        return absl::StrFormat("%s/src/c/%s/%s.o", prefix, game_name, sub_name);
      case SUBSEGMENT_ASM:
      case SUBSEGMENT_HASM:
        return absl::StrFormat("%s/asm/%s/%s.o", prefix, game_name, sub_name);
      case SUBSEGMENT_DATA:
        if (sub_name.find('/') != std::string_view::npos ||
            sub_name.find(".data") != std::string_view::npos) {
          return absl::StrFormat("%s/asm/%s/%s.o", prefix, game_name, sub_name);
        }
        return absl::StrFormat("%s/asm/%s/data/%s.data.o", prefix, game_name, sub_name);
      case SUBSEGMENT_RODATA:
        return absl::StrFormat("%s/asm/%s/%s.o", prefix, game_name, sub_name);
      case SUBSEGMENT_BIN:
        return absl::StrFormat("%s/assets/%s/%s.o", prefix, game_name, sub_name);
      case SUBSEGMENT_BSS:
        if (sub_name.find('/') != std::string_view::npos ||
            sub_name.find(".bss") != std::string_view::npos) {
          return absl::StrFormat("%s/asm/%s/%s.o", prefix, game_name, sub_name);
        }
        return absl::StrFormat("%s/asm/%s/data/%s.bss.o", prefix, game_name, sub_name);
      default:
        return absl::StrFormat("%s/asm/%s/%s.o", prefix, game_name, sub_name);
    }
  }

  // Segment without subsegments
  if (segment.type() == SEGMENT_HEADER) {
    return absl::StrFormat("%s/asm/%s/header.o", prefix, game_name);
  }
  return absl::StrFormat("%s/assets/%s/%s.o", prefix, game_name, segment.name());
}

absl::StatusOr<std::string> LinkerScriptGenerator::GenerateMainScript(
    const SplitConfig& config) const {
  std::string game_name(config.basename().empty() ? config.game_name() : config.basename());
  std::string text;
  absl::StrAppend(&text, "SECTIONS\n{\n    HIDDEN(__romPos = 0);\n");

  std::string prev_segment_with_vram;

  for (const auto& segment : config.segments()) {
    if (segment.type() == SEGMENT_BSS) {
      continue;
    }

    std::string name = SanitizeIdentifier(segment.name());
    absl::StrAppend(&text, absl::StrFormat("    %s_ROM_START = __romPos;\n", name));
    absl::StrAppend(&text, absl::StrFormat("    %s_VRAM = ADDR(.%s);\n", name, name));

    std::string vram_expr;
    if (segment.vram() != 0) {
      vram_expr = absl::StrFormat("0x%08X ", segment.vram());
      prev_segment_with_vram = name;
    } else if (!prev_segment_with_vram.empty()) {
      vram_expr = absl::StrFormat("%s_VRAM_END ", prev_segment_with_vram);
      prev_segment_with_vram = name;
    }

    if (segment.subsegments().empty()) {
      std::string obj_path = ResolveObjectPath(game_name, segment, nullptr);
      absl::StrAppend(&text, absl::StrFormat("    .%s %s: AT(%s_ROM_START) SUBALIGN(%d)\n    {\n",
                                             name, vram_expr, name, options_.subalign));
      absl::StrAppend(&text, absl::StrFormat("        FILL(0x%08X);\n", options_.fill_value));
      absl::StrAppend(&text, absl::StrFormat("        %s_DATA_START = .;\n", name));
      absl::StrAppend(&text, absl::StrFormat("        %s(.data);\n", obj_path));
      absl::StrAppend(&text, absl::StrFormat("        %s_DATA_END = .;\n", name));
      absl::StrAppend(
          &text, absl::StrFormat("        %s_DATA_SIZE = ABSOLUTE(%s_DATA_END - %s_DATA_START);\n",
                                 name, name, name));
      absl::StrAppend(&text, "    }\n");
      absl::StrAppend(&text, absl::StrFormat("    __romPos += SIZEOF(.%s);\n", name));
      absl::StrAppend(&text, absl::StrFormat("    %s_ROM_END = __romPos;\n", name));
      absl::StrAppend(&text, absl::StrFormat("    %s_VRAM_END = .;\n\n", name));
    } else {
      // Subsegments present
      std::vector<std::string> text_objs;
      std::vector<std::string> data_objs;
      std::vector<std::string> rodata_objs;
      std::vector<std::string> bss_objs;

      for (const auto& sub : segment.subsegments()) {
        std::string obj_path = ResolveObjectPath(game_name, segment, &sub);

        if (sub.type() == SUBSEGMENT_C || sub.type() == SUBSEGMENT_ASM ||
            sub.type() == SUBSEGMENT_HASM) {
          text_objs.push_back(obj_path);
          data_objs.push_back(obj_path);
          rodata_objs.push_back(obj_path);
          bss_objs.push_back(obj_path);
        } else if (sub.type() == SUBSEGMENT_DATA) {
          data_objs.push_back(obj_path);
        } else if (sub.type() == SUBSEGMENT_RODATA) {
          rodata_objs.push_back(obj_path);
        } else if (sub.type() == SUBSEGMENT_BIN) {
          data_objs.push_back(obj_path);
        } else if (sub.type() == SUBSEGMENT_BSS) {
          bss_objs.push_back(obj_path);
        }
      }

      absl::StrAppend(&text, absl::StrFormat("    .%s %s: AT(%s_ROM_START) SUBALIGN(%d)\n    {\n",
                                             name, vram_expr, name, options_.subalign));
      absl::StrAppend(&text, absl::StrFormat("        FILL(0x%08X);\n", options_.fill_value));

      if (!text_objs.empty()) {
        absl::StrAppend(&text, absl::StrFormat("        %s_TEXT_START = .;\n", name));
        for (const auto& obj : text_objs) {
          absl::StrAppend(&text, absl::StrFormat("        %s(.text);\n", obj));
        }
        absl::StrAppend(&text, absl::StrFormat("        . = ALIGN(., %d);\n", options_.subalign));
        absl::StrAppend(&text, absl::StrFormat("        %s_TEXT_END = .;\n", name));
        absl::StrAppend(
            &text,
            absl::StrFormat("        %s_TEXT_SIZE = ABSOLUTE(%s_TEXT_END - %s_TEXT_START);\n", name,
                            name, name));
      }

      if (!data_objs.empty()) {
        absl::StrAppend(&text, absl::StrFormat("        %s_DATA_START = .;\n", name));
        for (const auto& obj : data_objs) {
          absl::StrAppend(&text, absl::StrFormat("        %s(.data);\n", obj));
        }
        absl::StrAppend(&text, absl::StrFormat("        . = ALIGN(., %d);\n", options_.subalign));
        absl::StrAppend(&text, absl::StrFormat("        %s_DATA_END = .;\n", name));
        absl::StrAppend(
            &text,
            absl::StrFormat("        %s_DATA_SIZE = ABSOLUTE(%s_DATA_END - %s_DATA_START);\n", name,
                            name, name));
      }

      if (!rodata_objs.empty()) {
        absl::StrAppend(&text, absl::StrFormat("        %s_RODATA_START = .;\n", name));
        for (const auto& obj : rodata_objs) {
          absl::StrAppend(&text, absl::StrFormat("        %s(.rodata);\n", obj));
        }
        absl::StrAppend(&text, absl::StrFormat("        . = ALIGN(., %d);\n", options_.subalign));
        absl::StrAppend(&text, absl::StrFormat("        %s_RODATA_END = .;\n", name));
        absl::StrAppend(
            &text,
            absl::StrFormat("        %s_RODATA_SIZE = ABSOLUTE(%s_RODATA_END - %s_RODATA_START);\n",
                            name, name, name));
      }
      absl::StrAppend(&text, "    }\n");

      if (!bss_objs.empty()) {
        absl::StrAppend(&text, absl::StrFormat("    %s_bss_VRAM = ADDR(.%s_bss);\n", name, name));
        absl::StrAppend(&text, absl::StrFormat("    .%s_bss (NOLOAD) : SUBALIGN(%d)\n    {\n", name,
                                               options_.subalign));
        absl::StrAppend(&text, absl::StrFormat("        FILL(0x%08X);\n", options_.fill_value));
        absl::StrAppend(&text, absl::StrFormat("        %s_BSS_START = .;\n", name));
        for (const auto& obj : bss_objs) {
          absl::StrAppend(&text, absl::StrFormat("        %s(.bss);\n", obj));
        }
        absl::StrAppend(&text, absl::StrFormat("        . = ALIGN(., %d);\n", options_.subalign));
        absl::StrAppend(&text, absl::StrFormat("        %s_BSS_END = .;\n", name));
        absl::StrAppend(
            &text, absl::StrFormat("        %s_BSS_SIZE = ABSOLUTE(%s_BSS_END - %s_BSS_START);\n",
                                   name, name, name));
        absl::StrAppend(&text, "    }\n");
      }

      absl::StrAppend(&text, absl::StrFormat("    __romPos += SIZEOF(.%s);\n", name));
      absl::StrAppend(&text,
                      absl::StrFormat("    __romPos = ALIGN(__romPos, %d);\n", options_.subalign));
      absl::StrAppend(&text, absl::StrFormat("    . = ALIGN(., %d);\n", options_.subalign));
      absl::StrAppend(&text, absl::StrFormat("    %s_ROM_END = __romPos;\n", name));
      absl::StrAppend(&text, absl::StrFormat("    %s_VRAM_END = .;\n\n", name));
    }
  }

  if (options_.emit_discard) {
    absl::StrAppend(&text, "    /DISCARD/ :\n    {\n        *(*);\n    }\n");
  }
  absl::StrAppend(&text, "}\n");
  return text;
}

std::string LinkerScriptGenerator::GenerateSymbolsScript(const SymbolIndex& symbols) const {
  std::string text;
  absl::StrAppend(&text, "/* Auto-generated global symbol definitions */\n");
  for (const auto* entry : symbols.SortedEntries()) {
    absl::StrAppend(&text, absl::StrFormat("%s = 0x%08X;\n", entry->name(), entry->address()));
  }
  return text;
}

std::string LinkerScriptGenerator::GenerateHardwareRegsScript() const {
  return R"(leoBootID = 0x800001A0;
osTvType = 0x80000300;
osRomType = 0x80000304;
osRomBase = 0x80000308;
osResetType = 0x8000030C;
osCicId = 0x80000310;
osVersion = 0x80000314;
osMemSize = 0x80000318;
osAppNMIBuffer = 0x8000031C;
SP_MEM_ADDR_REG = 0xA4040000;
SP_DRAM_ADDR_REG = 0xA4040004;
SP_RD_LEN_REG = 0xA4040008;
SP_WR_LEN_REG = 0xA404000C;
SP_STATUS_REG = 0xA4040010;
SP_DMA_FULL_REG = 0xA4040014;
SP_DMA_BUSY_REG = 0xA4040018;
SP_SEMAPHORE_REG = 0xA404001C;
SP_PC = 0xA4080000;
DPC_START_REG = 0xA4100000;
DPC_END_REG = 0xA4100004;
DPC_CURRENT_REG = 0xA4100008;
DPC_STATUS_REG = 0xA410000C;
DPC_CLOCK_REG = 0xA4100010;
DPC_BUFBUSY_REG = 0xA4100014;
DPC_PIPEBUSY_REG = 0xA4100018;
DPC_TMEM_REG = 0xA410001C;
DPS_TBIST_REG = 0xA4200000;
DPS_TEST_MODE_REG = 0xA4200004;
DPS_BUFTEST_ADDR_REG = 0xA4200008;
DPS_BUFTEST_DATA_REG = 0xA420000C;
MI_MODE_REG = 0xA4300000;
MI_VERSION_REG = 0xA4300004;
MI_INTR_REG = 0xA4300008;
MI_INTR_MASK_REG = 0xA430000C;
MI_SK_EXCEPTION_REG = 0xA4300014;
MI_SK_WATCHDOG_TIMER = 0xA4300018;
MI_RANDOM_BIT = 0xA430002C;
MI_HW_INTR_REG = 0xA4300038;
MI_HW_INTR_MASK_REG = 0xA430003C;
VI_STATUS_REG = 0xA4400000;
VI_DRAM_ADDR_REG = 0xA4400004;
VI_WIDTH_REG = 0xA4400008;
VI_INTR_REG = 0xA440000C;
VI_CURRENT_REG = 0xA4400010;
VI_BURST_REG = 0xA4400014;
VI_V_SYNC_REG = 0xA4400018;
VI_H_SYNC_REG = 0xA440001C;
VI_LEAP_REG = 0xA4400020;
VI_H_START_REG = 0xA4400024;
VI_V_START_REG = 0xA4400028;
VI_V_BURST_REG = 0xA440002C;
VI_X_SCALE_REG = 0xA4400030;
VI_Y_SCALE_REG = 0xA4400034;
AI_DRAM_ADDR_REG = 0xA4500000;
AI_LEN_REG = 0xA4500004;
AI_CONTROL_REG = 0xA4500008;
AI_STATUS_REG = 0xA450000C;
AI_DACRATE_REG = 0xA4500010;
AI_BITRATE_REG = 0xA4500014;
PI_DRAM_ADDR_REG = 0xA4600000;
PI_CART_ADDR_REG = 0xA4600004;
PI_RD_LEN_REG = 0xA4600008;
PI_WR_LEN_REG = 0xA460000C;
PI_STATUS_REG = 0xA4600010;
PI_BSD_DOM1_LAT_REG = 0xA4600014;
PI_BSD_DOM1_PWD_REG = 0xA4600018;
PI_BSD_DOM1_PGS_REG = 0xA460001C;
PI_BSD_DOM1_RLS_REG = 0xA4600020;
PI_BSD_DOM2_LAT_REG = 0xA4600024;
PI_BSD_DOM2_PWD_REG = 0xA4600028;
PI_BSD_DOM2_PGS_REG = 0xA460002C;
PI_BSD_DOM2_RLS_REG = 0xA4600030;
RI_MODE_REG = 0xA4700000;
RI_CONFIG_REG = 0xA4700004;
RI_CURRENT_LOAD_REG = 0xA4700008;
RI_SELECT_REG = 0xA470000C;
RI_REFRESH_REG = 0xA4700010;
RI_LATENCY_REG = 0xA4700014;
RI_RERROR_REG = 0xA4700018;
RI_WERROR_REG = 0xA470001C;
SI_DRAM_ADDR_REG = 0xA4800000;
SI_PIF_ADDR_RD64B_REG = 0xA4800004;
SI_PIF_ADDR_WR64B_REG = 0xA4800010;
SI_STATUS_REG = 0xA4800018;
)";
}

}  // namespace rom_nom_nom
