#include "../src/ui/linux_process.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string_view>

int main(int argc, char** argv) {
  using awj::ui_process::capture;
  using awj::ui_process::launch;
  const std::string self = std::filesystem::read_symlink("/proc/self/exe").native();
  if (argc > 1) {
    const std::string_view mode{argv[1]};
    if (mode == "--echo") { std::fputs(argv[2], stdout); return 0; }
    if (mode == "--failure") { std::fputs("partial", stdout); return 7; }
    if (mode == "--signal") { ::raise(SIGTERM); return 1; }
    if (mode == "--large") {
      const std::string block(4096, 'x');
      for (int i = 0; i < 256; ++i) std::fputs(block.c_str(), stdout);
      return 0;
    }
    if (mode == "--closed-stdio") {
      ::close(0); ::close(1); ::close(2);
      return capture({self, "--echo", "closed"}) == "closed" ? 0 : 1;
    }
    return 0;
  }
  const auto fail = [](const char* text) { std::fprintf(stderr, "%s\n", text); return 1; };
  const auto fd_count = [] {
    return std::distance(std::filesystem::directory_iterator{"/proc/self/fd"},
                         std::filesystem::directory_iterator{});
  };
  const auto initial_fds = fd_count();
  const std::string text = std::string{" 中文 ' \" ; $(false)\n "} + char(0xff);
  if (capture({self, "--echo", text}) != text) return fail("argv bytes changed");
  if (capture({self, "--failure"}) || capture({self, "--signal"}))
    return fail("failed child stdout accepted");
  if (capture({"/awj-no-such-command"}) || capture({}) ||
      capture({self, "--echo", std::string{"a\0b", 3}}))
    return fail("invalid command accepted");
  const auto large = capture({self, "--large"});
  if (!large || *large != std::string(1024 * 1024, 'x'))
    return fail("pipe output truncated");
  if (!capture({self, "--closed-stdio"})) return fail("closed stdio broke pipe setup");
  for (int i = 0; i < 20; ++i) {
    if (capture({self, "--echo", text}) != text || !launch({self, "--exit"}))
      return fail("repeated spawn failed");
  }
  if (launch({"/awj-no-such-command"})) return fail("missing opener accepted");
  // WNOWAIT observes exit without stealing the detached reaper's child.
  bool reaped = false;
  for (int i = 0; i < 200; ++i) {
    siginfo_t info{};
    if (::waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) == -1 && errno == ECHILD) {
      reaped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (!reaped || fd_count() != initial_fds) return fail("child or descriptor leaked");
  return 0;
}
