#include "lifter/function_loader.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "differ/target_extractor.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

FunctionLoader::FunctionLoader(std::unique_ptr<TargetExtractor> extractor)
    : extractor_(std::move(extractor)) {}

absl::StatusOr<std::unique_ptr<FunctionLoader>> FunctionLoader::Create(
    FunctionLoaderOptions options) {
  TargetExtractorOptions extractor_opts;
  extractor_opts.repo_root = options.repo_root;
  extractor_opts.game_name = options.game_name;

  auto extractor_or = TargetExtractor::Create(extractor_opts);
  if (!extractor_or.ok()) {
    return extractor_or.status();
  }
  return std::make_unique<FunctionLoader>(std::move(*extractor_or));
}

absl::StatusOr<LoadedFunction> FunctionLoader::LoadFunction(
    std::string_view func_name_or_addr) const {
  if (extractor_ == nullptr) {
    return absl::FailedPreconditionError("FunctionLoader: No TargetExtractor initialized.");
  }

  auto target_or = extractor_->ExtractTarget(func_name_or_addr);
  if (!target_or.ok()) {
    return target_or.status();
  }

  LoadedFunction loaded;
  loaded.name = target_or->name;
  loaded.vram = target_or->vram;
  loaded.instructions = std::move(target_or->instructions);
  loaded.symbol_index = extractor_->Symbols();
  return loaded;
}

absl::StatusOr<LoadedFunction> FunctionLoader::FromWords(std::string name,
                                                         absl::Span<const uint32_t> words,
                                                         uint32_t base_vram,
                                                         const SymbolIndex* symbol_index) {
  auto insts_or = DecodeSequence(words, base_vram);
  if (!insts_or.ok()) {
    return insts_or.status();
  }

  LoadedFunction loaded;
  loaded.name = std::move(name);
  loaded.vram = base_vram;
  loaded.instructions = std::move(*insts_or);
  loaded.symbol_index = symbol_index;
  return loaded;
}

}  // namespace rom_nom_nom
