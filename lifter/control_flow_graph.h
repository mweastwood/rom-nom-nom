#ifndef LIFTER_CONTROL_FLOW_GRAPH_H_
#define LIFTER_CONTROL_FLOW_GRAPH_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom {

// Type of control flow edge between basic blocks.
enum class EdgeType {
  kFallthrough,  // Fallthrough to the next sequential block
  kBranch,       // Conditional branch taken
  kJump,         // Unconditional jump (j)
  kReturn,       // Function return (jr $ra)
};

// Directed edge connecting two basic blocks.
struct CfgEdge {
  uint32_t from_block_id = 0;
  uint32_t to_block_id = 0;
  EdgeType type = EdgeType::kFallthrough;
};

// A contiguous sequence of instructions with single entry and single exit.
struct BasicBlock {
  uint32_t id = 0;
  uint32_t start_vram = 0;
  uint32_t end_vram = 0;  // VRAM of the last instruction in the block

  std::vector<Instruction> instructions;

  std::vector<uint32_t> predecessors;
  std::vector<uint32_t> successors;
  std::vector<CfgEdge> outgoing_edges;

  // Convenience queries
  bool IsEmpty() const { return instructions.empty(); }
  size_t Size() const { return instructions.size(); }
  const Instruction* Terminator() const;
  bool HasBranch() const;
  bool HasReturn() const;
};

// Represents the Control Flow Graph of a decompiled function.
class ControlFlowGraph {
 public:
  ControlFlowGraph() = default;

  // Constructs a CFG from a sequence of decoded instructions.
  static absl::StatusOr<ControlFlowGraph> Build(absl::Span<const Instruction> instructions);

  // Accessors
  uint32_t EntryBlockId() const { return entry_block_id_; }
  const std::vector<BasicBlock>& Blocks() const { return blocks_; }
  const std::vector<CfgEdge>& Edges() const { return edges_; }

  const BasicBlock* GetBlock(uint32_t id) const;
  const BasicBlock* FindBlockByVram(uint32_t vram) const;

  // Graph Traversal Algorithms
  // Returns block IDs in Reverse Post-Order (RPO), ideal for forward dataflow analysis.
  std::vector<uint32_t> ReversePostOrder() const;

  // Returns block IDs in Post-Order.
  std::vector<uint32_t> PostOrder() const;

  // Generates a Graphviz DOT representation for debugging and visualization.
  std::string ToDot() const;

 private:
  uint32_t entry_block_id_ = 0;
  std::vector<BasicBlock> blocks_;
  std::vector<CfgEdge> edges_;
  absl::flat_hash_map<uint32_t, size_t> vram_to_block_index_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CONTROL_FLOW_GRAPH_H_
