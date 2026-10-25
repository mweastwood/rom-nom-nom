#ifndef LIFTER_CFG_DE_REORG_H_
#define LIFTER_CFG_DE_REORG_H_

#include "core/control_flow_graph.h"

namespace rom_nom_nom {

// Reverses MIPS GCC 2.7.2 delay-slot scheduling (`reorg.c`) on the Control Flow
// Graph prior to dominator analysis, loop analysis, and region structuring.
class CfgDeReorg {
 public:
  // Unschedules branch and jump delay-slot instructions by hoisting safe
  // same-block delay slots above the terminator and sinking stolen fallthrough
  // or taken delay slots (`fill_eager_delay_slots`) into their single-predecessor
  // destination blocks when defined registers are dead along the opposite edge.
  static void UnscheduleDelaySlots(ControlFlowGraph* cfg);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CFG_DE_REORG_H_
