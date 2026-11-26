#ifndef GCC_HARNESS_TRACE_SERIALIZER_H_
#define GCC_HARNESS_TRACE_SERIALIZER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/function_hooks.h"

namespace rom_nom_nom::gcc_harness {

inline constexpr uint32_t kTraceShardMagic = 0x544E4E52u;  // "RNNT"
inline constexpr uint32_t kTraceShardFormatVersion = 2u;
inline constexpr uint32_t kDefaultSharedCorpusTraceShards = 32u;

// File header stored at the beginning of each sharded trace archive.
struct SerializedTraceShardHeader {
  uint32_t magic = kTraceShardMagic;
  uint32_t format_version = kTraceShardFormatVersion;
  uint32_t shard_index = 0;
  uint32_t num_shards = kDefaultSharedCorpusTraceShards;
  uint32_t total_corpus_cases = 0;
  uint64_t corpus_fingerprint = 0;
};

// Per-record byte-offset index entry stored right after the v2 shard header.
struct TraceShardIndexEntry {
  uint32_t case_index = 0;
  uint32_t byte_offset = 0;  // Offset relative to start of record payload section.
  uint32_t byte_length = 0;  // Exact byte length of serialized trace blob.
};

// Parsed header and validated per-record byte-offset index table of a v2 trace
// shard file.
struct ParsedTraceShardIndex {
  SerializedTraceShardHeader header;
  size_t payload_base_offset = 0;
  std::vector<TraceShardIndexEntry> entries;
};

// A single corpus trace paired with its 0-based index in
// `GetSharedRoundTripTestCases()`.
struct IndexedFunctionCompilationTrace {
  uint32_t case_index = 0;
  FunctionCompilationTrace trace;
};

// Complete deserialized trace shard file.
struct DeserializedTraceShard {
  SerializedTraceShardHeader header;
  std::vector<IndexedFunctionCompilationTrace> records;
};

// Computes a deterministic 64-bit FNV-1a fingerprint over all fields of every
// `RoundTripTestCase` in `test_cases`.
uint64_t ComputeCorpusFingerprint(absl::Span<const RoundTripTestCase> test_cases);

// Deterministically serializes a single `FunctionCompilationTrace` to a
// lossless little-endian binary buffer (with per-trace string interning and
// byte-buffer content-addressed `RtlControlFlowGraph` deduplication).
std::string SerializeFunctionCompilationTrace(const FunctionCompilationTrace& trace);

// Deserializes a single `FunctionCompilationTrace` from `bytes`.
absl::StatusOr<FunctionCompilationTrace> DeserializeFunctionCompilationTrace(
    std::string_view bytes);

// Deterministically serializes a v2 sharded trace file containing `header`,
// the per-record `(case_index, byte_offset, byte_length)` index table, and
// concatenated trace payloads for `records`.
std::string SerializeTraceShard(const SerializedTraceShardHeader& header,
                                absl::Span<const IndexedFunctionCompilationTrace> records);

// Parses and validates the v2 trace shard header and per-record byte-offset
// index table without deserializing individual trace payloads. Validates magic,
// format version 2, shard coordinates, optional corpus fingerprint, strictly
// ascending `case_index`, overflow-safe bounds, and exact payload tiling.
absl::StatusOr<ParsedTraceShardIndex> ParseTraceShardIndex(
    std::string_view bytes, std::optional<uint64_t> expected_corpus_fingerprint = std::nullopt);

// Deserializes a sharded trace file from `bytes`, validating magic, format
// version, shard bounds, index bounds, pool indices, and (when provided) corpus
// fingerprint.
absl::StatusOr<DeserializedTraceShard> DeserializeTraceShard(
    std::string_view bytes, std::optional<uint64_t> expected_corpus_fingerprint = std::nullopt);

// Compares two `RtlControlFlowGraph` values across every single field (blocks,
// instructions, `RtlInsn`, `RtlExpr`, `Instruction`, and `RtlSidebandState`).
bool RtlControlFlowGraphEqualsExact(const RtlControlFlowGraph& lhs, const RtlControlFlowGraph& rhs,
                                    std::string* diff_detail = nullptr);

// Compares two `FunctionCompilationTrace` values across every single field.
bool FunctionCompilationTraceEqualsExact(const FunctionCompilationTrace& lhs,
                                         const FunctionCompilationTrace& rhs,
                                         std::string* diff_detail = nullptr);

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_TRACE_SERIALIZER_H_
