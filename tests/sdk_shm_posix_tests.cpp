#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "awj/awj_shm.h"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行 Linux 共享内存测试失败: %s; %s\n", __LINE__, #value, awj_last_error()); return 1; } } while (false)
namespace {
constexpr int child_ready = 100, child_finish = 101;
void fill_desc(awj_frame_desc& desc, const awj_shm_write& slot, uint64_t id = 77) {
  awj_frame_desc_init(&desc); desc.width = 2; desc.height = 2; desc.frame_id = id;
  desc.planes[0] = {slot.data, slot.capacity, 0, 8};
  desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 0; desc.color.range = 1;
}
bool receive_byte(int fd, char expected) {
  pollfd event{fd, POLLIN, 0};
  int ready;
  do { ready = ::poll(&event, 1, 10000); } while (ready < 0 && errno == EINTR);
  char value{};
  return ready > 0 && (event.revents & POLLIN) && ::read(fd, &value, 1) == 1 && value == expected;
}
bool send_byte(int fd, char value) { return ::write(fd, &value, 1) == 1; }
struct Child {
  pid_t pid{-1};
  int ready{-1}, finish{-1}, process{-1};
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  ~Child() {
    if (ready >= 0) ::close(ready);
    if (finish >= 0) ::close(finish);
    if (pid > 0) {
      // 只清理本测试刚创建且未 waitpid 回收的子进程，PID 此时不能被复用。
      ::kill(pid, SIGKILL);
      int status;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (process >= 0) ::close(process);
  }
  bool wait() {
    pollfd event{process, POLLIN, 0};
    int ready_count;
    do { ready_count = ::poll(&event, 1, 10000); } while (ready_count < 0 && errno == EINTR);
    if (ready_count <= 0 || !(event.revents & POLLIN)) return false;
    int status{};
    pid_t result;
    do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    if (result != pid) return false;
    pid = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
};
bool spawn_child(const std::string& executable, const char* role, const std::string& name, Child& child) {
  int ready[2]{-1, -1}, finish[2]{-1, -1};
  if (::pipe2(ready, O_CLOEXEC)) return false;
  if (::pipe2(finish, O_CLOEXEC)) { ::close(ready[0]); ::close(ready[1]); return false; }
  child.ready = ready[0]; child.finish = finish[1];
  posix_spawn_file_actions_t actions;
  int error = ::posix_spawn_file_actions_init(&actions);
  if (!error) {
    error = ::posix_spawn_file_actions_adddup2(&actions, ready[1], child_ready);
    if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, finish[0], child_finish);
    if (!error) {
      char* arguments[]{const_cast<char*>(executable.c_str()), const_cast<char*>("--child"),
                        const_cast<char*>(role), const_cast<char*>(name.c_str()), nullptr};
      error = ::posix_spawn(&child.pid, executable.c_str(), &actions, nullptr, arguments, environ);
    }
    ::posix_spawn_file_actions_destroy(&actions);
  }
  ::close(ready[1]); ::close(finish[0]);
  if (error) return false;
  child.process = static_cast<int>(::syscall(SYS_pidfd_open, child.pid, 0));
  return child.process >= 0;
}
int child_main(const char* role, const char* name) {
  awj_context* context{}; CHECK(awj_context_create(nullptr, &context) == AWJ_OK);
  awj_shm_channel* channel{};
  const bool producer = std::strcmp(role, "abrupt-producer") == 0;
  CHECK((producer ? awj_shm_create(context, name, 2, 4096, &channel)
                  : awj_shm_open(context, name, &channel)) == AWJ_OK);
  awj_frame* frame{};
  if (producer) {
    awj_shm_write slot{sizeof(slot), AWJ_ABI_VERSION};
    CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_OK);
    std::memset(slot.data, 42, 16); awj_frame_desc desc; fill_desc(desc, slot);
    CHECK(awj_shm_commit(channel, slot.token, &desc) == AWJ_OK);
    CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_OK); // 留下 WRITING 槽位。
  } else CHECK(awj_shm_receive(channel, 5000, &frame) == AWJ_OK);
  CHECK(send_byte(child_ready, 'R'));
  if (std::strcmp(role, "consumer") != 0) {
    CHECK(receive_byte(child_finish, 'X'));
    ::_exit(0); // 不清理通道/帧，覆盖真实异常退出与 READING 槽位遗留。
  }
  awj_shm_destroy(channel); awj_context_destroy(context);
  const uint8_t* row{}; uint64_t size{};
  CHECK(awj_frame_row(frame, 0, 0, &row, &size) == AWJ_OK && size == 8 && row[0] == 42);
  awj_frame_release(frame);
  return 0;
}
int abrupt_test(const std::string& executable, const std::string& name,
                bool child_producer, awj_context* context) {
  awj_shm_channel* channel{};
  awj_shm_write slot{sizeof(slot), AWJ_ABI_VERSION};
  if (!child_producer) {
    CHECK(awj_shm_create(context, name.c_str(), 1, 4096, &channel) == AWJ_OK);
    CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_OK);
    std::memset(slot.data, 42, 16); awj_frame_desc desc; fill_desc(desc, slot);
    CHECK(awj_shm_commit(channel, slot.token, &desc) == AWJ_OK);
  }
  Child child;
  CHECK(spawn_child(executable, child_producer ? "abrupt-producer" : "abrupt-consumer", name, child));
  CHECK(receive_byte(child.ready, 'R'));
  awj_frame* held{};
  if (child_producer) {
    CHECK(awj_shm_open(context, name.c_str(), &channel) == AWJ_OK);
    CHECK(awj_shm_receive(channel, 0, &held) == AWJ_OK);
  } else CHECK(awj_shm_begin_write(channel, 0, &slot) == AWJ_BUSY);
  CHECK(send_byte(child.finish, 'X') && child.wait());
  awj_frame* next{};
  CHECK((child_producer ? awj_shm_receive(channel, 5000, &next)
                       : awj_shm_begin_write(channel, 5000, &slot)) == AWJ_IO_ERROR);
  awj_shm_destroy(channel);
  if (held) {
    const uint8_t* row{}; uint64_t size{};
    CHECK(awj_frame_row(held, 0, 0, &row, &size) == AWJ_OK && row[0] == 42);
    awj_frame_release(held);
  }
  return 0;
}
}
int main(int argc, char** argv) {
  if (argc == 4 && std::strcmp(argv[1], "--child") == 0) return child_main(argv[2], argv[3]);
  std::array<char, 4096> executable_path{};
  const auto length = ::readlink("/proc/self/exe", executable_path.data(), executable_path.size() - 1);
  CHECK(length > 0 && static_cast<size_t>(length) < executable_path.size() - 1);
  const std::string executable{executable_path.data(), static_cast<size_t>(length)};
  awj_context_options options; awj_context_options_init(&options);
  options.max_frame_bytes = 4096; options.memory_budget_bytes = 65536;
  awj_context* context{}; CHECK(awj_context_create(&options, &context) == AWJ_OK);
  const auto name = "sdk-shm-posix-" + std::to_string(::getpid());
  awj_shm_channel* producer{}; awj_shm_channel* reader{}; awj_shm_channel* duplicate{};
  CHECK(awj_shm_create(context, "../invalid", 1, 4096, &producer) == AWJ_INVALID_ARGUMENT);
  CHECK(awj_shm_create(context, name.c_str(), 17, 4096, &producer) == AWJ_INVALID_ARGUMENT);
  CHECK(awj_shm_create(context, name.c_str(), 1, 4096, &producer) == AWJ_OK);
  CHECK(awj_shm_create(context, name.c_str(), 1, 4096, &duplicate) == AWJ_BUSY && !duplicate);

  // 失败的消费者预算检查发生在 ACK 之前，不能永久抢占唯一消费者身份。
  auto small_options = options; small_options.memory_budget_bytes = 4096;
  awj_context* small{}; CHECK(awj_context_create(&small_options, &small) == AWJ_OK);
  CHECK(awj_shm_open(small, name.c_str(), &duplicate) != AWJ_OK && !duplicate);
  awj_context_destroy(small);
  CHECK(awj_shm_open(context, name.c_str(), &reader) == AWJ_OK);
  CHECK(awj_shm_open(context, name.c_str(), &duplicate) != AWJ_OK && !duplicate);
  awj_shm_write slot{sizeof(slot), AWJ_ABI_VERSION};
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_OK);
  auto* original = slot.data; const auto first_token = slot.token;
  for (uint32_t i = 0; i < 16; ++i) slot.data[i] = static_cast<uint8_t>(i);
  awj_frame_desc desc; fill_desc(desc, slot);
  const uint8_t external[16]{};
  auto invalid = desc; invalid.planes[0].data = external; invalid.planes[0].size = 16;
  CHECK(awj_shm_commit(producer, slot.token, &invalid) == AWJ_INVALID_ARGUMENT);
  CHECK(awj_shm_commit(producer, slot.token, &desc) == AWJ_OK);
  CHECK(awj_shm_commit(producer, slot.token, &desc) == AWJ_INVALID_ARGUMENT);
  awj_frame* frame{}; CHECK(awj_shm_receive(reader, 0, &frame) == AWJ_OK);
  const uint8_t* row{}; uint64_t size{};
  awj_diagnostics diagnostics{sizeof(diagnostics), AWJ_ABI_VERSION};
  CHECK(awj_frame_diagnostics(frame, &diagnostics) == AWJ_OK && diagnostics.input_copy_bytes == 0);
  CHECK(awj_frame_row(frame, 0, 1, &row, &size) == AWJ_OK && size == 8 && row[0] == 8);
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_BUSY);
  CHECK(awj_shm_begin_write(producer, 10, &slot) == AWJ_TIMEOUT);
  awj_frame_release(frame);
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_OK && slot.data == original && slot.token != first_token);
  CHECK(awj_shm_abort_write(producer, first_token) == AWJ_INVALID_ARGUMENT);
  CHECK(awj_shm_abort_write(producer, slot.token) == AWJ_OK);
  awj_shm_close(producer);
  CHECK(awj_shm_receive(reader, 0, &frame) == AWJ_END_OF_STREAM);
  awj_shm_destroy(reader); awj_shm_destroy(producer);
  CHECK(awj_shm_create(context, name.c_str(), 2, 4096, &producer) == AWJ_OK);
  CHECK(awj_shm_open(context, name.c_str(), &reader) == AWJ_OK);

  // 后提交的 READY 不能越过较早的 WRITING；关闭时丢弃未提交槽位并允许 drain。
  awj_shm_write first{sizeof(first), AWJ_ABI_VERSION}, second{sizeof(second), AWJ_ABI_VERSION};
  CHECK(awj_shm_begin_write(producer, 0, &first) == AWJ_OK);
  CHECK(awj_shm_begin_write(producer, 0, &second) == AWJ_OK);
  std::memset(first.data, 11, 16); std::memset(second.data, 22, 16);
  fill_desc(desc, second, 82); CHECK(awj_shm_commit(producer, second.token, &desc) == AWJ_OK);
  CHECK(awj_shm_receive(reader, 0, &frame) == AWJ_BUSY && !frame);
  fill_desc(desc, first, 81); CHECK(awj_shm_commit(producer, first.token, &desc) == AWJ_OK);
  CHECK(awj_shm_receive(reader, 0, &frame) == AWJ_OK);
  awj_frame_desc described; awj_frame_desc_init(&described);
  CHECK(awj_frame_describe(frame, &described) == AWJ_OK && described.frame_id == 81);
  awj_frame_release(frame);
  CHECK(awj_shm_begin_write(producer, 0, &first) == AWJ_OK);
  awj_shm_close(producer);
  CHECK(awj_shm_receive(reader, 0, &frame) == AWJ_OK);
  CHECK(awj_frame_describe(frame, &described) == AWJ_OK && described.frame_id == 82);
  awj_shm_destroy(reader); awj_shm_destroy(producer);
  CHECK(awj_frame_row(frame, 0, 1, &row, &size) == AWJ_OK && row[0] == 22);
  awj_frame_release(frame);

  const auto cross = name + "-cross";
  CHECK(awj_shm_create(context, cross.c_str(), 1, 4096, &producer) == AWJ_OK);
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_OK);
  std::memset(slot.data, 42, 16); fill_desc(desc, slot);
  CHECK(awj_shm_commit(producer, slot.token, &desc) == AWJ_OK);
  {
    Child child; CHECK(spawn_child(executable, "consumer", cross, child));
    CHECK(receive_byte(child.ready, 'R') && child.wait());
  }
  CHECK(awj_shm_begin_write(producer, 0, &slot) == AWJ_END_OF_STREAM);
  awj_shm_destroy(producer);
  CHECK(abrupt_test(executable, name + "-dead-producer", true, context) == 0);
  CHECK(abrupt_test(executable, name + "-dead-consumer", false, context) == 0);
  awj_context_destroy(context);
  std::puts("Linux 匿名共享内存：背压、槽位复用、FIFO、失败连接恢复、跨进程传输、异常退出与帧保活通过。");
  return 0;
}
