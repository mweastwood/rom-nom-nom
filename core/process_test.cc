#include "core/process.h"

#include <filesystem>
#include <string>

#include "absl/time/time.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(EnvironmentTest, SetsGetsAndUnsetsVariables) {
  Environment env;
  EXPECT_TRUE(env.Empty());
  EXPECT_FALSE(env.Contains("VAR_A"));
  EXPECT_EQ(env.Get("VAR_A"), nullptr);

  env.Set("VAR_A", "value1");
  EXPECT_FALSE(env.Empty());
  EXPECT_TRUE(env.Contains("VAR_A"));
  ASSERT_NE(env.Get("VAR_A"), nullptr);
  EXPECT_EQ(*env.Get("VAR_A"), "value1");

  env.Set("VAR_A", "value2");
  ASSERT_NE(env.Get("VAR_A"), nullptr);
  EXPECT_EQ(*env.Get("VAR_A"), "value2");

  env.Unset("VAR_A");
  EXPECT_TRUE(env.Empty());
  EXPECT_FALSE(env.Contains("VAR_A"));
  EXPECT_EQ(env.Get("VAR_A"), nullptr);
}

TEST(EnvironmentTest, InheritsCurrentProcessEnvironment) {
  Environment env = Environment::InheritCurrent();
  EXPECT_FALSE(env.Empty());
  EXPECT_TRUE(env.Contains("PATH"));
  ASSERT_NE(env.Get("PATH"), nullptr);
  EXPECT_FALSE(env.Get("PATH")->empty());
}

TEST(ProcessTest, EchoOutputs) {
  ProcessOptions options;
  options.program = "echo";
  options.args = {"hello", "world"};

  auto result_or = RunProcess(options);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->Ok());
  EXPECT_EQ(result_or->exit_code, 0);
  EXPECT_EQ(result_or->stdout_output, "hello world\n");
  EXPECT_TRUE(result_or->stderr_output.empty());
}

TEST(ProcessTest, CapturesExitCode) {
  ProcessOptions options;
  options.program = "sh";
  options.args = {"-c", "exit 42"};

  auto result_or = RunProcess(options);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_FALSE(result_or->Ok());
  EXPECT_EQ(result_or->exit_code, 42);
}

TEST(ProcessTest, CapturesStderr) {
  ProcessOptions options;
  options.program = "sh";
  options.args = {"-c", "echo 'error message' >&2"};

  auto result_or = RunProcess(options);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->Ok());
  EXPECT_EQ(result_or->stderr_output, "error message\n");
}

TEST(ProcessTest, RespectsWorkingDirectory) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  ProcessOptions options;
  options.program = "pwd";
  options.working_directory = temp_dir;

  auto result_or = RunProcess(options);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->Ok());

  // Canonicalize to avoid symlink discrepancies (e.g. /tmp vs /var/tmp)
  std::string stdout_trimmed = result_or->stdout_output;
  while (!stdout_trimmed.empty() &&
         (stdout_trimmed.back() == '\n' || stdout_trimmed.back() == '\r')) {
    stdout_trimmed.pop_back();
  }
  EXPECT_EQ(std::filesystem::canonical(stdout_trimmed), std::filesystem::canonical(temp_dir));
}

TEST(ProcessTest, PassesEnvironmentVariables) {
  ProcessOptions options;
  options.program = "sh";
  options.args = {"-c", "echo \"$CUSTOM_VAR\""};
  options.env.Set("CUSTOM_VAR", "antigravity_builder");

  auto result_or = RunProcess(options);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->Ok());
  EXPECT_EQ(result_or->stdout_output, "antigravity_builder\n");
}

TEST(ProcessTest, FailsOnNonExistentProgram) {
  ProcessOptions options;
  options.program = "/path/to/definitely_non_existent_binary_xyz123";

  auto result_or = RunProcess(options);
  EXPECT_FALSE(result_or.ok());
  EXPECT_TRUE(absl::IsNotFound(result_or.status()));
}

TEST(ProcessTest, TimesOutWhenExceeded) {
  ProcessOptions options;
  options.program = "sleep";
  options.args = {"5"};
  options.timeout = absl::Milliseconds(100);

  auto result_or = RunProcess(options);
  EXPECT_FALSE(result_or.ok());
  EXPECT_TRUE(absl::IsDeadlineExceeded(result_or.status()));
}

}  // namespace
}  // namespace rom_nom_nom
