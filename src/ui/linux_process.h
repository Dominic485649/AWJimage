#pragma once

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <utility>

extern char** environ;

namespace awj::ui_process {

inline int wait(pid_t child) noexcept {
  int status{};
  while (::waitpid(child, &status, 0) == -1) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// All descriptors owned by this pipe are above stderr, including when the
// parent starts with one of its standard streams closed.
struct Pipe {
  std::array<int, 2> fds{-1, -1};
  Pipe() = default;
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;
  ~Pipe() { for (int fd : fds) if (fd >= 0) ::close(fd); }
  bool open() noexcept {
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) return false;
    for (auto& fd : fds) {
      if (fd > STDERR_FILENO) continue;
      const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (copy < 0) return false;
      ::close(fd);
      fd = copy;
    }
    return true;
  }
  void close_writer() noexcept { ::close(fds[1]); fds[1] = -1; }
};

inline std::optional<pid_t> start(std::vector<std::string> args,
                                int output = -1) {
  if (args.empty() || args.front().empty()) return std::nullopt;
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    if (arg.find('\0') != std::string::npos) return std::nullopt;
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) return std::nullopt;
  int error = ::posix_spawn_file_actions_addopen(
      &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!error) error = ::posix_spawn_file_actions_addopen(
      &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  if (!error) {
    error = output < 0
        ? ::posix_spawn_file_actions_addopen(
              &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0)
        : ::posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
  }
  pid_t child{};
  if (!error) error = ::posix_spawnp(
      &child, argv.front(), &actions, nullptr, argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  return error ? std::nullopt : std::optional{child};
}

inline std::optional<std::string> capture(std::vector<std::string> args) {
  Pipe pipe;
  if (!pipe.open()) return std::nullopt;
  const auto child = start(std::move(args), pipe.fds[1]);
  if (!child) return std::nullopt;
  pipe.close_writer();
  std::string output;
  std::array<char, 4096> buffer;
  bool read_ok = true;
  try {
    for (;;) {
      const auto count = ::read(pipe.fds[0], buffer.data(), buffer.size());
      if (count > 0) output.append(buffer.data(), static_cast<std::size_t>(count));
      else if (count == 0) break;
      else if (errno != EINTR) { read_ok = false; break; }
    }
  } catch (...) {
    // A failed allocation must not leave the child blocked on a full pipe.
    ::kill(*child, SIGKILL);
    wait(*child);
    throw;
  }
  if (!read_ok) ::kill(*child, SIGKILL);
  const int status = wait(*child);
  if (!read_ok || status != 0) return std::nullopt;
  return output;
}

inline bool launch(std::vector<std::string> args) {
  const auto child = start(std::move(args));
  if (!child) return false;
  try {
    std::thread{[pid = *child] { wait(pid); }}.detach();
  } catch (...) {
    ::kill(*child, SIGKILL);
    wait(*child);
    return false;
  }
  return true;
}

}  // namespace awj::ui_process
