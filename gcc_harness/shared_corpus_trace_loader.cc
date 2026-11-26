#include "gcc_harness/shared_corpus_trace_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/round_trip_test_cases.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/trace_serializer.h"

namespace rom_nom_nom::gcc_harness {
namespace {

std::filesystem::path ResolveShardRunfilePath(uint32_t shard_index) {
  const std::string rel_path =
      absl::StrFormat("gcc_harness/shared_corpus_traces_shard_%02u.bin", shard_index);
  if (std::filesystem::exists(rel_path)) {
    return std::filesystem::absolute(rel_path);
  }
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  if (test_srcdir != nullptr && test_srcdir[0] != '\0') {
    std::filesystem::path candidate(test_srcdir);
    candidate /=
        (test_workspace != nullptr && test_workspace[0] != '\0') ? test_workspace : "_main";
    candidate /= rel_path;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  const char* runfiles_dir = std::getenv("RUNFILES_DIR");
  if (runfiles_dir != nullptr && runfiles_dir[0] != '\0') {
    std::filesystem::path candidate(runfiles_dir);
    candidate /=
        (test_workspace != nullptr && test_workspace[0] != '\0') ? test_workspace : "_main";
    candidate /= rel_path;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return rel_path;
}

class SharedCorpusTraceStore {
 public:
  static SharedCorpusTraceStore& Instance() {
    static auto* const instance = new SharedCorpusTraceStore();
    return *instance;
  }

  absl::StatusOr<const FunctionCompilationTrace*> GetByName(std::string_view test_name) {
    auto it = name_to_index_.find(test_name);
    if (it == name_to_index_.end()) {
      return absl::NotFoundError(
          absl::StrCat("Unknown shared corpus test_name: '", test_name, "'"));
    }
    return GetByIndex(it->second);
  }

  absl::StatusOr<const FunctionCompilationTrace*> GetByIndex(size_t case_index) {
    if (case_index >= total_cases_) {
      return absl::OutOfRangeError(absl::StrFormat(
          "Shared corpus case_index %zu out of range (size %zu)", case_index, total_cases_));
    }
    const uint32_t shard_index =
        static_cast<uint32_t>(case_index % kDefaultSharedCorpusTraceShards);

    std::lock_guard<std::mutex> lock(mu_);
    auto shard_it = loaded_shards_.find(shard_index);
    if (shard_it == loaded_shards_.end()) {
      absl::Status st = LoadShardLocked(shard_index);
      if (!st.ok()) {
        return st;
      }
      shard_it = loaded_shards_.find(shard_index);
    }
    LoadedShard& loaded = *shard_it->second;
    const uint32_t case_id = static_cast<uint32_t>(case_index);
    if (auto decoded_it = loaded.decoded_traces.find(case_id);
        decoded_it != loaded.decoded_traces.end()) {
      return decoded_it->second.get();
    }

    auto entry_it = loaded.case_index_to_entry.find(case_id);
    if (entry_it == loaded.case_index_to_entry.end()) {
      return absl::NotFoundError(
          absl::StrFormat("Case index %zu missing from trace shard %02u", case_index, shard_index));
    }

    const TraceShardIndexEntry& entry = entry_it->second;
    const std::string_view payload =
        std::string_view(loaded.raw_bytes)
            .substr(loaded.index.payload_base_offset + entry.byte_offset, entry.byte_length);
    absl::StatusOr<FunctionCompilationTrace> trace = DeserializeFunctionCompilationTrace(payload);
    if (!trace.ok()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Failed to deserialize cached case_index=%zu from shard %02u: %s",
                          case_index, shard_index, trace.status().message()));
    }

    auto owned = std::make_unique<FunctionCompilationTrace>(std::move(*trace));
    const FunctionCompilationTrace* ptr = owned.get();
    loaded.decoded_traces.emplace(case_id, std::move(owned));
    return ptr;
  }

 private:
  struct LoadedShard {
    std::string raw_bytes;
    ParsedTraceShardIndex index;
    absl::flat_hash_map<uint32_t, TraceShardIndexEntry> case_index_to_entry;
    absl::flat_hash_map<uint32_t, std::unique_ptr<FunctionCompilationTrace>> decoded_traces;
  };

  SharedCorpusTraceStore() {
    const std::vector<RoundTripTestCase>& all_cases = GetSharedRoundTripTestCases();
    total_cases_ = all_cases.size();
    corpus_fingerprint_ = ComputeCorpusFingerprint(all_cases);
    name_to_index_.reserve(total_cases_);
    for (size_t i = 0; i < total_cases_; ++i) {
      name_to_index_.emplace(all_cases[i].test_name, i);
    }
  }

  absl::Status LoadShardLocked(uint32_t shard_index) {
    const std::filesystem::path shard_path = ResolveShardRunfilePath(shard_index);
    std::ifstream in(shard_path, std::ios::binary);
    if (!in.is_open()) {
      return absl::NotFoundError(
          absl::StrCat("Unable to open cached trace shard file: ", shard_path.string()));
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
      return absl::InternalError(
          absl::StrCat("I/O error reading cached trace shard file: ", shard_path.string()));
    }

    absl::StatusOr<ParsedTraceShardIndex> parsed = ParseTraceShardIndex(raw, corpus_fingerprint_);
    if (!parsed.ok()) {
      return parsed.status();
    }
    if (parsed->header.shard_index != shard_index ||
        parsed->header.num_shards != kDefaultSharedCorpusTraceShards ||
        parsed->header.total_corpus_cases != total_cases_) {
      return absl::FailedPreconditionError(absl::StrFormat(
          "Cached trace shard %02u header mismatch: got shard=%u/%u total=%u (expected %u/%u %zu)",
          shard_index, parsed->header.shard_index, parsed->header.num_shards,
          parsed->header.total_corpus_cases, shard_index, kDefaultSharedCorpusTraceShards,
          total_cases_));
    }

    auto loaded = std::make_unique<LoadedShard>();
    loaded->raw_bytes = std::move(raw);
    loaded->index = std::move(*parsed);
    loaded->case_index_to_entry.reserve(loaded->index.entries.size());
    for (const TraceShardIndexEntry& entry : loaded->index.entries) {
      loaded->case_index_to_entry.emplace(entry.case_index, entry);
    }
    loaded_shards_.emplace(shard_index, std::move(loaded));
    return absl::OkStatus();
  }

  size_t total_cases_ = 0;
  uint64_t corpus_fingerprint_ = 0;
  absl::flat_hash_map<std::string, size_t> name_to_index_;
  std::mutex mu_;
  absl::flat_hash_map<uint32_t, std::unique_ptr<LoadedShard>> loaded_shards_;
};

}  // namespace

absl::StatusOr<const FunctionCompilationTrace*> GetCachedSharedCorpusTrace(
    std::string_view test_name) {
  return SharedCorpusTraceStore::Instance().GetByName(test_name);
}

absl::StatusOr<const FunctionCompilationTrace*> GetCachedSharedCorpusTraceByIndex(
    size_t case_index) {
  return SharedCorpusTraceStore::Instance().GetByIndex(case_index);
}

}  // namespace rom_nom_nom::gcc_harness
