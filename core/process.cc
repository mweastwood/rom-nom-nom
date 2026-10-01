#include "core/process.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace rom_nom_nom {
namespace {

int SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

Environment Environment::InheritCurrent() {
  Environment env;
  if (environ == nullptr) {
    return env;
  }
  for (char** current = environ; *current != nullptr; ++current) {
    std::string_view entry(*current);
    size_t eq_pos = entry.find('=');
    if (eq_pos != std::string_view::npos) {
      env.Set(std::string(entry.substr(0, eq_pos)), std::string(entry.substr(eq_pos + 1)));
    }
  }
  return env;
}

absl::StatusOr<ProcessResult> RunProcess(const ProcessOptions& options) {
  if (options.program.empty()) {
    return absl::InvalidArgumentError("RunProcess: Program path must not be empty.");
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};

  if (pipe2(stdout_pipe, O_CLOEXEC) < 0 || pipe2(stderr_pipe, O_CLOEXEC) < 0 ||
      pipe2(err_pipe, O_CLOEXEC) < 0) {
    int saved_errno = errno;
    if (stdout_pipe[0] >= 0) {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
    }
    if (stderr_pipe[0] >= 0) {
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
    }
    if (err_pipe[0] >= 0) {
      close(err_pipe[0]);
      close(err_pipe[1]);
    }
    return absl::InternalError(
        absl::StrFormat("RunProcess: Failed to create pipes: %s", std::strerror(saved_errno)));
  }

  pid_t pid = fork();
  if (pid < 0) {
    int saved_errno = errno;
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return absl::InternalError(
        absl::StrFormat("RunProcess: Fork failed: %s", std::strerror(saved_errno)));
  }

  if (pid == 0) {
    // Child process
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    close(err_pipe[0]);

    if (!options.working_directory.empty()) {
      if (chdir(options.working_directory.c_str()) != 0) {
        int err = errno;
        (void)write(err_pipe[1], &err, sizeof(err));
        _exit(127);
      }
    }

    for (const auto& [key, value] : options.env.Variables()) {
      setenv(key.c_str(), value.c_str(), 1);
    }

    if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
      int err = errno;
      (void)write(err_pipe[1], &err, sizeof(err));
      _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(options.program.c_str()));
    for (const auto& arg : options.args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execvp(options.program.c_str(), argv.data());

    int err = errno;
    (void)write(err_pipe[1], &err, sizeof(err));
    _exit(127);
  }

  // Parent process
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  close(err_pipe[1]);

  // Check if execvp failed immediately
  int exec_errno = 0;
  ssize_t err_bytes = read(err_pipe[0], &exec_errno, sizeof(exec_errno));
  close(err_pipe[0]);

  if (err_bytes == sizeof(exec_errno)) {
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    waitpid(pid, nullptr, 0);
    return absl::NotFoundError(absl::StrFormat("RunProcess: Failed to execute '%s': %s",
                                               options.program, std::strerror(exec_errno)));
  }

  SetNonBlocking(stdout_pipe[0]);
  SetNonBlocking(stderr_pipe[0]);

  ProcessResult result;
  std::vector<pollfd> fds = {
      {stdout_pipe[0], POLLIN, 0},
      {stderr_pipe[0], POLLIN, 0},
  };

  auto start_time = absl::Now();
  bool stdout_open = true;
  bool stderr_open = true;
  char buffer[4096];

  while (stdout_open || stderr_open) {
    int timeout_ms = -1;
    if (options.timeout > absl::ZeroDuration()) {
      absl::Duration elapsed = absl::Now() - start_time;
      if (elapsed >= options.timeout) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        return absl::DeadlineExceededError(absl::StrFormat(
            "RunProcess: Process '%s' exceeded timeout of %v", options.program, options.timeout));
      }
      timeout_ms = static_cast<int>(absl::ToInt64Milliseconds(options.timeout - elapsed));
      if (timeout_ms < 0) {
        timeout_ms = 0;
      }
    }

    fds[0].events = stdout_open ? POLLIN : 0;
    fds[1].events = stderr_open ? POLLIN : 0;

    int ret = poll(fds.data(), fds.size(), timeout_ms);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (stdout_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = read(stdout_pipe[0], buffer, sizeof(buffer));
      if (n > 0) {
        result.stdout_output.append(buffer, n);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        stdout_open = false;
        close(stdout_pipe[0]);
      }
    }

    if (stderr_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = read(stderr_pipe[0], buffer, sizeof(buffer));
      if (n > 0) {
        result.stderr_output.append(buffer, n);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        stderr_open = false;
        close(stderr_pipe[0]);
      }
    }
  }

  if (stdout_open) {
    close(stdout_pipe[0]);
  }
  if (stderr_open) {
    close(stderr_pipe[0]);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return absl::InternalError(
        absl::StrFormat("RunProcess: waitpid failed: %s", std::strerror(errno)));
  }

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }

  return result;
}

}  // namespace rom_nom_nom
