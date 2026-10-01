#ifndef ROM_NOM_NOM_CORE_PROCESS_H_
#define ROM_NOM_NOM_CORE_PROCESS_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"

namespace rom_nom_nom {

// Structured representation of child process environment variables.
class Environment {
 public:
  Environment() = default;

  // Sets or overrides an environment variable.
  void Set(std::string key, std::string value) { variables_[std::move(key)] = std::move(value); }

  // Removes an environment variable from the set.
  void Unset(std::string_view key) { variables_.erase(std::string(key)); }

  // Retrieves an environment variable if present, or nullptr.
  const std::string* Get(std::string_view key) const {
    auto it = variables_.find(key);
    return it != variables_.end() ? &it->second : nullptr;
  }

  // Returns true if the variable is defined.
  bool Contains(std::string_view key) const { return variables_.contains(key); }

  // Returns true if no environment variables are defined.
  bool Empty() const { return variables_.empty(); }

  // Returns the map of variable name/value pairs.
  const absl::flat_hash_map<std::string, std::string>& Variables() const { return variables_; }

  // Populates an Environment capturing all environment variables from the current process.
  static Environment InheritCurrent();

 private:
  absl::flat_hash_map<std::string, std::string> variables_;
};

// Configuration options for executing a child process.
struct ProcessOptions {
  std::string program;
  std::vector<std::string> args;
  std::filesystem::path working_directory;
  Environment env;
  absl::Duration timeout = absl::ZeroDuration();  // ZeroDuration = no timeout
};

// Captured outputs and termination status of a child process.
struct ProcessResult {
  int exit_code = -1;
  std::string stdout_output;
  std::string stderr_output;

  bool Ok() const { return exit_code == 0; }
};

// Executes a subprocess directly using fork/execvp/pipe without spawning a shell.
// Returns an error status if the process could not be launched or timed out.
absl::StatusOr<ProcessResult> RunProcess(const ProcessOptions& options);

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_CORE_PROCESS_H_
