#ifndef DIFFER_DIFFER_PIPELINE_H_
#define DIFFER_DIFFER_PIPELINE_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "differ/diff_formatter.h"
#include "differ/sequence_aligner.h"

namespace rom_nom_nom {

// Configuration options for the DifferPipeline.
struct DifferPipelineOptions {
  std::string game_name = "harvest-moon-64";
  std::filesystem::path repo_root;
  std::filesystem::path source_file;
  std::filesystem::path config_path;
  std::filesystem::path symbols_path;
  std::filesystem::path rom_path;
  DiffFormatterOptions format_options;
};

// Result of executing the diff pipeline on a single function.
struct DifferResult {
  std::string func_name;
  std::string canonical_name;
  uint32_t vram = 0;
  size_t target_instructions_count = 0;
  size_t compiled_instructions_count = 0;
  std::optional<std::filesystem::path> source_file;
  AlignmentResult alignment;
  std::string formatted_output;
  bool is_bit_exact = false;
};

// DifferPipeline coordinates target extraction from retail ROM/assembly,
// compilation of C sources via KMC GCC 2.7.2, object file parsing,
// relocation patching, instruction normalization, sequence alignment,
// and side-by-side terminal formatting.
class DifferPipeline {
 public:
  explicit DifferPipeline(DifferPipelineOptions options);

  // Executes the complete diff pipeline for the requested function.
  absl::StatusOr<DifferResult> Diff(std::string_view func_name) const;

  const DifferPipelineOptions& Options() const { return options_; }

 private:
  DifferPipelineOptions options_;
  std::filesystem::path repo_root_;
};

}  // namespace rom_nom_nom

#endif  // DIFFER_DIFFER_PIPELINE_H_
