#ifndef GCC_HARNESS_SHARED_CORPUS_TRACE_LOADER_H_
#define GCC_HARNESS_SHARED_CORPUS_TRACE_LOADER_H_

#include <cstddef>
#include <string_view>

#include "absl/status/statusor.h"
#include "gcc_harness/function_hooks.h"

namespace rom_nom_nom::gcc_harness {

// Returns the cached `FunctionCompilationTrace` for the shared corpus test case
// named `test_name`, lazily loading only the owning stride-32 shard file from
// Bazel runfiles and verifying the corpus fingerprint header.
absl::StatusOr<const FunctionCompilationTrace*> GetCachedSharedCorpusTrace(
    std::string_view test_name);

// Returns the cached `FunctionCompilationTrace` for shared corpus index
// `case_index` (`0 .. GetSharedRoundTripTestCases().size() - 1`), lazily
// loading only shard `case_index % 32`.
absl::StatusOr<const FunctionCompilationTrace*> GetCachedSharedCorpusTraceByIndex(
    size_t case_index);

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_SHARED_CORPUS_TRACE_LOADER_H_
