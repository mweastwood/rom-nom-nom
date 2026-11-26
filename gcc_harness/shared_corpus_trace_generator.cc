#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/strip.h"
#include "core/round_trip_test_cases.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/trace_serializer.h"

namespace {

using ::rom_nom_nom::GetSharedRoundTripTestCases;
using ::rom_nom_nom::RoundTripTestCase;
using ::rom_nom_nom::gcc_harness::ComputeCorpusFingerprint;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::IndexedFunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::kDefaultSharedCorpusTraceShards;
using ::rom_nom_nom::gcc_harness::SerializedTraceShardHeader;
using ::rom_nom_nom::gcc_harness::SerializeTraceShard;

struct GeneratorArgs {
  uint32_t shard_index = 0;
  uint32_t num_shards = kDefaultSharedCorpusTraceShards;
  std::string output_path;
};

bool ParseArgs(int argc, char** argv, GeneratorArgs* args) {
  bool saw_shard_index = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (absl::ConsumePrefix(&arg, "--shard_index=")) {
      if (!absl::SimpleAtoi(arg, &args->shard_index)) {
        std::fprintf(stderr, "Invalid --shard_index value: %s\n", argv[i]);
        return false;
      }
      saw_shard_index = true;
    } else if (absl::ConsumePrefix(&arg, "--num_shards=")) {
      if (!absl::SimpleAtoi(arg, &args->num_shards)) {
        std::fprintf(stderr, "Invalid --num_shards value: %s\n", argv[i]);
        return false;
      }
    } else if (absl::ConsumePrefix(&arg, "--output=")) {
      args->output_path = std::string(arg);
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      return false;
    }
  }
  if (!saw_shard_index || args->output_path.empty() || args->num_shards == 0 ||
      args->shard_index >= args->num_shards) {
    std::fprintf(stderr, "Usage: %s --shard_index=<i> --num_shards=<N> --output=<path>\n", argv[0]);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  GeneratorArgs args;
  if (!ParseArgs(argc, argv, &args)) {
    return 1;
  }

  const std::vector<RoundTripTestCase>& all_cases = GetSharedRoundTripTestCases();
  GccContext ctx;
  FunctionHooks hooks(&ctx);

  std::vector<IndexedFunctionCompilationTrace> records;
  records.reserve(all_cases.size() / args.num_shards + 1);
  for (size_t idx = args.shard_index; idx < all_cases.size(); idx += args.num_shards) {
    const RoundTripTestCase& tc = all_cases[idx];
    ctx.BeginFunction("harness_init");
    hooks.ApplyCompilationProfile({2, tc.options.c_flags, {}});
    regclass_init();

    IndexedFunctionCompilationTrace rec;
    rec.case_index = static_cast<uint32_t>(idx);
    rec.trace = hooks.TraceForwardCompilation(tc);
    if (rec.trace.stage0_input.machine_words.empty()) {
      std::fprintf(stderr, "TraceForwardCompilation produced empty machine_words for %s\n",
                   tc.test_name.c_str());
      return 1;
    }
    records.push_back(std::move(rec));
  }

  SerializedTraceShardHeader header;
  header.shard_index = args.shard_index;
  header.num_shards = args.num_shards;
  header.total_corpus_cases = static_cast<uint32_t>(all_cases.size());
  header.corpus_fingerprint = ComputeCorpusFingerprint(all_cases);

  const std::string encoded = SerializeTraceShard(header, records);
  std::ofstream out(args.output_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    std::fprintf(stderr, "Failed to open output file: %s\n", args.output_path.c_str());
    return 1;
  }
  out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
  out.flush();
  if (!out.good()) {
    std::fprintf(stderr, "Failed writing trace shard to: %s\n", args.output_path.c_str());
    return 1;
  }
  return 0;
}
