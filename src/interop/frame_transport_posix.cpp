#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "awj/awj_shm.h"
#include "sdk_internal.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/magic.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr size_t global_bytes = 256, mutex_offset = 128;
constexpr size_t slot_metadata = 176, state_offset = 160, token_offset = 168;
constexpr uint32_t free_slot = 0, writing_slot = 1, ready_slot = 2, reading_slot = 3;
constexpr uint32_t attach_ack = 0x314b4341;
constexpr int required_seals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
constexpr std::array<uint8_t, 8> map_magic{'A','W','J','S','H','P','1',0};
static_assert(mutex_offset + sizeof(pthread_mutex_t) <= global_bytes);
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr));

uint32_t u32(const uint8_t* p) noexcept {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (i * 8);
  return value;
}
uint64_t u64(const uint8_t* p) noexcept {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(p[i]) << (i * 8);
  return value;
}
void put32(uint8_t* p, uint32_t value) noexcept {
  for (size_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}
void put64(uint8_t* p, uint64_t value) noexcept {
  for (size_t i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}
uint32_t atomic_load(const uint8_t* p) noexcept {
  return __atomic_load_n(reinterpret_cast<const uint32_t*>(p), __ATOMIC_SEQ_CST);
}
void atomic_store(uint8_t* p, uint32_t value) noexcept {
  __atomic_store_n(reinterpret_cast<uint32_t*>(p), value, __ATOMIC_SEQ_CST);
}

struct Fd {
  int value{-1};
  Fd() = default;
  explicit Fd(int fd) noexcept : value{fd} {}
  ~Fd() { reset(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : value{std::exchange(other.value, -1)} {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) { reset(); value = std::exchange(other.value, -1); }
    return *this;
  }
  void reset(int fd = -1) noexcept {
    if (value >= 0) ::close(value);
    value = fd;
  }
};

class Budget {
 public:
  explicit Budget(uint32_t milliseconds) noexcept
      : end_{std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds}} {}
  uint32_t remaining() const noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end_) return 0;
    return static_cast<uint32_t>(std::max<int64_t>(1,
        std::chrono::duration_cast<std::chrono::milliseconds>(end_ - now).count()));
  }
 private:
  std::chrono::steady_clock::time_point end_;
};

awj_status invalid_channel() noexcept {
  return awj_sdk::fail(AWJ_IO_ERROR, "共享内存句柄、布局、权限或对端状态无效。");
}
bool session_address(const char* input, sockaddr_un& address, socklen_t& size) {
  if (!input) return false;
  std::string name = "awjimage-1.3-" + std::to_string(::geteuid()) + "-";
  size_t length = 0;
  for (; length <= 64 && input[length]; ++length) {
    const auto c = static_cast<unsigned char>(input[length]);
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    name.push_back(static_cast<char>(c));
  }
  if (!length || length > 64 || name.size() + 1 > sizeof(address.sun_path)) return false;
  address = {};
  address.sun_family = AF_UNIX;
  // Linux abstract namespace 不建立文件系统节点，最后一个 socket 关闭后自动消失。
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  return true;
}

awj_status peer_identity(int socket, Fd& process, pid_t& pid) noexcept {
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) ||
      size != sizeof(credentials) || credentials.uid != ::geteuid() || credentials.pid <= 0)
    return invalid_channel();
#ifdef SO_PEERPIDFD
  int fd = -1;
  size = sizeof(fd);
  if (::getsockopt(socket, SOL_SOCKET, SO_PEERPIDFD, &fd, &size))
    return awj_sdk::fail(errno == ENOPROTOOPT || errno == EOPNOTSUPP ? AWJ_UNSUPPORTED : AWJ_IO_ERROR,
                         "Linux 共享内存要求内核支持 SO_PEERPIDFD。");
  process.reset(fd);
  const int flags = fd >= 0 ? ::fcntl(fd, F_GETFD) : -1;
  if (size != sizeof(fd) || flags < 0 || !(flags & FD_CLOEXEC)) return invalid_channel();
  pid = credentials.pid;
  return AWJ_OK;
#else
  (void)process; (void)pid;
  return awj_sdk::fail(AWJ_UNSUPPORTED, "Linux 系统头未提供 SO_PEERPIDFD。");
#endif
}

bool process_alive(int fd) noexcept {
  if (fd < 0) return false;
  pollfd event{fd, POLLIN, 0};
  const int result = ::poll(&event, 1, 0);
  return result == 0 || (result < 0 && errno == EINTR);
}

bool send_packet(int socket, uint32_t value, int right = -1) noexcept {
  std::array<uint8_t, 4> bytes{};
  put32(bytes.data(), value);
  iovec data{bytes.data(), bytes.size()};
  alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
  msghdr message{};
  message.msg_iov = &data; message.msg_iovlen = 1;
  if (right >= 0) {
    message.msg_control = control.data(); message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &right, sizeof(right));
  }
  return ::sendmsg(socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
}

// 所有传入 fd 都先接管或关闭，包括格式错误的多 fd 消息；不把 ancillary 截断当作成功。
awj_status receive_packet(int socket, uint32_t& value, Fd& right,
                          uint32_t timeout, std::stop_token stop) noexcept {
  Budget budget{timeout};
  for (;;) {
    if (stop.stop_requested()) return AWJ_CANCELLED;
    std::array<uint8_t, 4> bytes{};
    iovec data{bytes.data(), bytes.size()};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 2)> control{};
    msghdr message{};
    message.msg_iov = &data; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    const auto count = ::recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count >= 0) {
      unsigned rights = 0;
      bool valid = count == static_cast<ssize_t>(bytes.size()) && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
      for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len < CMSG_LEN(0)) {
          valid = false;
          continue;
        }
        const size_t payload = header->cmsg_len - CMSG_LEN(0);
        if (payload % sizeof(int)) valid = false;
        for (size_t i = 0; i < payload / sizeof(int); ++i) {
          int fd = -1;
          std::memcpy(&fd, CMSG_DATA(header) + i * sizeof(fd), sizeof(fd));
          if (++rights == 1) right.reset(fd);
          else { if (fd >= 0) ::close(fd); valid = false; }
        }
      }
      if (!valid) { right.reset(); return AWJ_IO_ERROR; }
      value = u32(bytes.data());
      return AWJ_OK;
    }
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return AWJ_IO_ERROR;
    const auto remaining = budget.remaining();
    if (!remaining) return AWJ_TIMEOUT;
    pollfd event{socket, POLLIN, 0};
    if (::poll(&event, 1, static_cast<int>(std::min<uint32_t>(remaining, 25))) < 0 && errno != EINTR)
      return AWJ_IO_ERROR;
  }
}

struct Region {
  Fd mapping, listener, peer, server_stop;
  std::jthread server;
  bool producer{};
  pid_t peer_pid{};
  std::atomic_bool failed{};
  uint8_t* memory{};
  uint32_t slots{};
  uint64_t capacity{}, stride{}, size{};
  awj_context_options limits{};
  std::stop_token stop{};
  ~Region() {
    // server 只借用 this；先停止并 join，再解除映射，不能让线程反过来持有 Region。
    if (server.joinable()) { server.request_stop(); server.join(); }
    if (memory) ::munmap(memory, static_cast<size_t>(size));
  }
  pthread_mutex_t* mutex() const noexcept { return reinterpret_cast<pthread_mutex_t*>(memory + mutex_offset); }
  uint8_t* slot(uint32_t index) const noexcept { return memory + global_bytes + static_cast<size_t>(index * stride); }
  bool closed() const noexcept { return atomic_load(memory + 48) != 0; }
  bool broken() const noexcept { return failed.load() || atomic_load(memory + 80) != 0; }
  uint32_t sequence() const noexcept { return atomic_load(memory + 84); }
  bool peer_alive() const noexcept {
    const auto pid = u32(memory + (producer ? 56 : 52));
    if (producer && !pid && peer.value < 0) return true;
    return pid && pid == static_cast<uint32_t>(peer_pid) && process_alive(peer.value);
  }
  void notify() const noexcept {
    auto* sequence = reinterpret_cast<uint32_t*>(memory + 84);
    __atomic_add_fetch(sequence, 1, __ATOMIC_SEQ_CST);
    ::syscall(SYS_futex, sequence, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
  }
  void mark_failed() noexcept {
    failed.store(true);
    atomic_store(memory + 80, 1);
    atomic_store(memory + 48, 1);
    notify();
  }
  void serve(std::stop_token token) noexcept;
};

struct Lock {
  Region& region;
  int state{EINVAL};
  bool owns{};
  explicit Lock(Region& value, uint32_t timeout) noexcept : region{value} {
    if (!timeout) state = ::pthread_mutex_trylock(region.mutex());
    else {
      timespec deadline{};
      if (!::clock_gettime(CLOCK_MONOTONIC, &deadline)) {
        deadline.tv_sec += timeout / 1000;
        deadline.tv_nsec += static_cast<long>(timeout % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
        state = ::pthread_mutex_clocklock(region.mutex(), CLOCK_MONOTONIC, &deadline);
      }
    }
    owns = state == 0 || state == EOWNERDEAD;
    if (state == EOWNERDEAD) {
      region.mark_failed();
      ::pthread_mutex_consistent(region.mutex());
    }
  }
  ~Lock() { if (owns) ::pthread_mutex_unlock(region.mutex()); }
  bool acquired() const noexcept { return state == 0; }
};

void Region::serve(std::stop_token token) noexcept {
  try {
    std::stop_callback wake{token, [this] {
      const uint64_t one = 1;
      (void)::write(server_stop.value, &one, sizeof(one));
    }};
    while (!token.stop_requested()) {
      pollfd events[2]{{listener.value, POLLIN, 0}, {server_stop.value, POLLIN, 0}};
      const auto ready = ::poll(events, 2, -1);
      if (ready < 0) { if (errno == EINTR) continue; mark_failed(); return; }
      if (token.stop_requested()) return;
      if (!(events[0].revents & POLLIN)) { mark_failed(); return; }
      Fd connection{::accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
      if (connection.value < 0) { if (errno == EINTR || errno == EAGAIN) continue; mark_failed(); return; }
      Fd process;
      pid_t pid{};
      auto status = peer_identity(connection.value, process, pid);
      if (status != AWJ_OK) { send_packet(connection.value, status); continue; }
      {
        Lock lock{*this, 1000};
        if (!lock.acquired() || broken()) status = AWJ_IO_ERROR;
        else if (closed()) status = AWJ_END_OF_STREAM;
        else if (u32(memory + 56)) status = AWJ_BUSY;
      }
      if (status != AWJ_OK) { send_packet(connection.value, status); continue; }
      if (!send_packet(connection.value, AWJ_OK, mapping.value)) continue;
      uint32_t ack{};
      Fd unexpected;
      status = receive_packet(connection.value, ack, unexpected, 1000, token);
      if (status != AWJ_OK || ack != attach_ack || unexpected.value >= 0) continue;
      {
        Lock lock{*this, 1000};
        if (!lock.acquired() || broken() || !process_alive(process.value)) status = AWJ_IO_ERROR;
        else if (closed()) status = AWJ_END_OF_STREAM;
        else if (u32(memory + 56)) status = AWJ_BUSY;
        else {
          peer = std::move(process); peer_pid = pid;
          put32(memory + 56, static_cast<uint32_t>(pid));
          notify();
        }
      }
      if (!send_packet(connection.value, status) && status == AWJ_OK) mark_failed();
    }
  } catch (...) {
    mark_failed();
  }
}

struct SlotLease {
  std::shared_ptr<Region> region;
  uint32_t index{};
  uint64_t token{};
  ~SlotLease() {
    Lock lock{*region, 1000};
    if (!lock.acquired()) { region->mark_failed(); return; }
    auto* entry = region->slot(index);
    if (u32(entry + state_offset) == reading_slot && u64(entry + token_offset) == token) {
      put32(entry + state_offset, free_slot);
      region->notify();
    }
  }
};
}

struct awj_shm_channel { std::shared_ptr<Region> region; bool producer{}; };

namespace {
template<class Action> awj_status wait_channel(awj_shm_channel* channel, uint32_t timeout, Action&& action) {
  if (!channel || timeout > 60000) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "无效通道或等待上限。");
  auto& region = *channel->region;
  Budget budget{timeout};
  for (;;) {
    if (region.stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "共享内存等待已取消。");
    if (region.broken()) return invalid_channel();
    auto sequence = region.sequence();
    {
      Lock lock{region, std::min<uint32_t>(budget.remaining(), 25)};
      if (lock.acquired()) {
        if (region.broken()) return invalid_channel();
        if (!region.closed() && !region.peer_alive()) { region.mark_failed(); return invalid_channel(); }
        const auto status = action(region);
        if (status != AWJ_BUSY) return status;
        sequence = region.sequence();
      } else if (lock.state != ETIMEDOUT && lock.state != EBUSY) return invalid_channel();
    }
    const auto remaining = budget.remaining();
    if (!remaining) return awj_sdk::fail(timeout ? AWJ_TIMEOUT : AWJ_BUSY, "共享内存通道暂无可用槽位。");
    const auto milliseconds = std::min<uint32_t>(remaining, 25);
    timespec interval{0, static_cast<long>(milliseconds) * 1000000};
    // 必须使用进程共享 futex，不加 FUTEX_PRIVATE_FLAG；周期检查 pidfd 和取消。
    const auto result = ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(region.memory + 84),
        FUTEX_WAIT, sequence, &interval, nullptr, 0);
    if (result < 0 && errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT) return invalid_channel();
  }
}
void close_channel(awj_shm_channel* channel) noexcept {
  if (!channel) return;
  auto& region = *channel->region;
  Lock lock{region, 1000};
  if (!lock.acquired()) { region.mark_failed(); return; }
  atomic_store(region.memory + 48, 1);
  for (uint32_t i = 0; i < region.slots; ++i)
    if (u32(region.slot(i) + state_offset) == writing_slot) put32(region.slot(i) + state_offset, free_slot);
  region.notify();
}
}

extern "C" {
awj_status AWJ_CALL awj_shm_create(awj_context* context, const char* name,
                                 uint32_t slots, uint64_t capacity, awj_shm_channel** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少共享通道输出。");
    *output = nullptr;
    sockaddr_un address{}; socklen_t address_size{};
    if (!context || !session_address(name, address, address_size) || !slots || slots > 16 || !capacity ||
        capacity > context->options.max_frame_bytes || capacity > UINT64_MAX - slot_metadata - 63)
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "共享内存名称、容量或槽位数无效。");
    if constexpr (std::endian::native != std::endian::little)
      return awj_sdk::fail(AWJ_UNSUPPORTED, "当前本机共享内存布局要求小端平台。");
    const uint64_t stride = (capacity + slot_metadata + 63) & ~UINT64_C(63);
    uint64_t bytes{};
    if (!awj_sdk::multiply(stride, slots, bytes) || bytes > UINT64_MAX - global_bytes ||
        bytes + global_bytes > context->options.memory_budget_bytes || bytes + global_bytes > SIZE_MAX ||
        bytes + global_bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
      return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "共享内存总容量超过 context 预算。");
    auto region = std::make_shared<Region>();
    region->size = bytes + global_bytes; region->stride = stride; region->capacity = capacity; region->slots = slots;
    region->limits = context->options; region->stop = context->stop.get_token(); region->producer = true;
    region->listener.reset(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (region->listener.value < 0) return invalid_channel();
    if (::bind(region->listener.value, reinterpret_cast<sockaddr*>(&address), address_size))
      return awj_sdk::fail(errno == EADDRINUSE ? AWJ_BUSY : AWJ_IO_ERROR, "共享会话已存在或不可创建。");
    if (::listen(region->listener.value, 4)) return invalid_channel();
    region->server_stop.reset(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (region->server_stop.value < 0) return invalid_channel();
    region->mapping.reset(::memfd_create("AWJimage-frame-channel", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (region->mapping.value < 0 || ::fchmod(region->mapping.value, S_IRUSR | S_IWUSR) ||
        ::ftruncate(region->mapping.value, static_cast<off_t>(region->size))) return invalid_channel();
    const int allocated = ::posix_fallocate(region->mapping.value, 0, static_cast<off_t>(region->size));
    if (allocated) return awj_sdk::fail(allocated == ENOSPC || allocated == ENOMEM ? AWJ_OUT_OF_MEMORY : AWJ_IO_ERROR,
                                      "无法预留匿名共享内存的实际 backing，拒绝延迟 SIGBUS 风险。");
    if (::fcntl(region->mapping.value, F_ADD_SEALS, required_seals)) return invalid_channel();
    void* mapping = ::mmap(nullptr, static_cast<size_t>(region->size), PROT_READ | PROT_WRITE, MAP_SHARED, region->mapping.value, 0);
    if (mapping == MAP_FAILED) return invalid_channel();
    region->memory = static_cast<uint8_t*>(mapping);
    std::copy(map_magic.begin(), map_magic.end(), region->memory);
    put32(region->memory + 8, 1); put32(region->memory + 12, slots);
    put64(region->memory + 16, capacity); put64(region->memory + 24, stride); put64(region->memory + 32, region->size);
    put64(region->memory + 40, 1); put32(region->memory + 52, static_cast<uint32_t>(::getpid()));
    put32(region->memory + 60, sizeof(pthread_mutex_t)); put32(region->memory + 64, static_cast<uint32_t>(::geteuid()));
    put32(region->memory + 96, global_bytes);
    pthread_mutexattr_t attributes{};
    int error = ::pthread_mutexattr_init(&attributes);
    if (error) return invalid_channel();
    error = ::pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    if (!error) error = ::pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
    if (!error) error = ::pthread_mutex_init(region->mutex(), &attributes);
    ::pthread_mutexattr_destroy(&attributes);
    if (error) return awj_sdk::fail(AWJ_UNSUPPORTED, "平台不支持 process-shared robust pthread mutex。");
    auto channel = std::make_unique<awj_shm_channel>();
    channel->region = region; channel->producer = true;
    region->server = std::jthread([pointer = region.get()](std::stop_token token) { pointer->serve(token); });
    *output = channel.release();
    return AWJ_OK;
  });
}

awj_status AWJ_CALL awj_shm_open(awj_context* context, const char* name, awj_shm_channel** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少共享通道输出。");
    *output = nullptr;
    sockaddr_un address{}; socklen_t address_size{};
    if (!context || !session_address(name, address, address_size))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "共享会话名称无效。");
    Fd connection{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (connection.value < 0 || ::connect(connection.value, reinterpret_cast<sockaddr*>(&address), address_size))
      return invalid_channel();
    auto region = std::make_shared<Region>();
    region->limits = context->options; region->stop = context->stop.get_token();
    auto status = peer_identity(connection.value, region->peer, region->peer_pid);
    if (status != AWJ_OK) return status;
    Fd received;
    uint32_t reply{};
    status = receive_packet(connection.value, reply, received, 1000, region->stop);
    if (status != AWJ_OK) return awj_sdk::fail(status, "共享会话握手未完成。");
    if (reply != AWJ_OK) return awj_sdk::fail(reply <= AWJ_COPY_REQUIRED ? reply : AWJ_IO_ERROR, "共享会话拒绝连接。");
    if (received.value < 0) return invalid_channel();
    struct stat information{};
    struct statfs filesystem{};
    const auto seals = ::fcntl(received.value, F_GET_SEALS);
    if (::fstat(received.value, &information) || ::fstatfs(received.value, &filesystem) ||
        !S_ISREG(information.st_mode) || information.st_uid != ::geteuid() ||
        (information.st_mode & 0777) != 0600 || information.st_size < static_cast<off_t>(global_bytes) ||
        static_cast<uint64_t>(information.st_size) > context->options.memory_budget_bytes ||
        static_cast<uint64_t>(information.st_size) > SIZE_MAX ||
        filesystem.f_type != TMPFS_MAGIC || seals < 0 || (seals & required_seals) != required_seals)
      return invalid_channel();
    region->size = static_cast<uint64_t>(information.st_size);
    region->mapping = std::move(received);
    void* mapping = ::mmap(nullptr, static_cast<size_t>(region->size), PROT_READ | PROT_WRITE, MAP_SHARED, region->mapping.value, 0);
    if (mapping == MAP_FAILED) return invalid_channel();
    region->memory = static_cast<uint8_t*>(mapping);
    if (!std::equal(map_magic.begin(), map_magic.end(), region->memory) || u32(region->memory + 8) != 1 ||
        u32(region->memory + 60) != sizeof(pthread_mutex_t) || u32(region->memory + 96) != global_bytes ||
        u32(region->memory + 64) != static_cast<uint32_t>(::geteuid())) return invalid_channel();
    region->slots = u32(region->memory + 12); region->capacity = u64(region->memory + 16);
    region->stride = u64(region->memory + 24);
    uint64_t bytes{};
    if (!region->slots || region->slots > 16 || !region->capacity || region->capacity > context->options.max_frame_bytes ||
        region->capacity > UINT64_MAX - slot_metadata - 63 ||
        region->stride != ((region->capacity + slot_metadata + 63) & ~UINT64_C(63)) ||
        !awj_sdk::multiply(region->stride, region->slots, bytes) || bytes > UINT64_MAX - global_bytes ||
        region->size != bytes + global_bytes || u64(region->memory + 32) != region->size)
      return invalid_channel();
    {
      Lock lock{*region, 1000};
      if (!lock.acquired() || region->broken() || region->closed() || u32(region->memory + 56) || !region->peer_alive())
        return invalid_channel();
    }
    auto channel = std::make_unique<awj_shm_channel>();
    channel->region = region;
    if (!send_packet(connection.value, attach_ack)) return invalid_channel();
    // ACK 后才占用唯一消费者身份；不完整的连接不能留下一个“活着但不存在”的消费者。
    struct AttachGuard {
      Region& region;
      bool complete{};
      ~AttachGuard() { if (!complete) region.mark_failed(); }
    } guard{*region};
    Fd unexpected;
    status = receive_packet(connection.value, reply, unexpected, 1000, region->stop);
    if (status != AWJ_OK || reply != AWJ_OK || unexpected.value >= 0) return invalid_channel();
    {
      Lock lock{*region, 1000};
      if (!lock.acquired() || region->broken() ||
          u32(region->memory + 56) != static_cast<uint32_t>(::getpid()) || !region->peer_alive()) return invalid_channel();
    }
    guard.complete = true;
    *output = channel.release();
    return AWJ_OK;
  });
}

awj_status AWJ_CALL awj_shm_begin_write(awj_shm_channel* channel, uint32_t timeout, awj_shm_write* output) {
  return awj_sdk::boundary([&] {
    if (!channel || !channel->producer || !awj_sdk::valid_header(output))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "begin_write 需要生产者通道和有效输出 ABI。");
    output->token = 0; output->data = nullptr; output->capacity = 0;
    return wait_channel(channel, timeout, [&](Region& region) {
      if (region.closed()) return AWJ_END_OF_STREAM;
      for (uint32_t i = 0; i < region.slots; ++i) {
        auto* entry = region.slot(i);
        if (u32(entry + state_offset) != free_slot) continue;
        const auto token = u64(region.memory + 40);
        if (!token || token == UINT64_MAX) return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "共享槽位代次耗尽。");
        put64(region.memory + 40, token + 1); put64(entry + token_offset, token); put32(entry + state_offset, writing_slot);
        output->token = token; output->data = entry + slot_metadata; output->capacity = region.capacity;
        region.notify();
        return AWJ_OK;
      }
      return AWJ_BUSY;
    });
  });
}
awj_status AWJ_CALL awj_shm_commit(awj_shm_channel* channel, uint64_t token, const awj_frame_desc* desc) {
  return awj_sdk::boundary([&] {
    if (!channel || !channel->producer || !token || !desc) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "共享提交参数无效。");
    auto& region = *channel->region;
    auto status = awj_sdk::validate(region.limits, *desc);
    if (status != AWJ_OK) return status;
    awj_sdk::Layout shape{};
    status = awj_sdk::layout(*desc, shape);
    if (status != AWJ_OK) return status;
    Lock lock{region, 1000};
    if (!lock.acquired() || region.broken()) return invalid_channel();
    if (region.closed()) return AWJ_END_OF_STREAM;
    if (!region.peer_alive()) { region.mark_failed(); return invalid_channel(); }
    for (uint32_t i = 0; i < region.slots; ++i) {
      auto* entry = region.slot(i);
      if (u32(entry + state_offset) != writing_slot || u64(entry + token_offset) != token) continue;
      uint64_t offset = 0;
      for (uint32_t p = 0; p < shape.count; ++p) {
        uint64_t bytes{};
        if (!awj_sdk::multiply(shape.row_bytes[p], shape.rows[p], bytes) || bytes > region.capacity || offset > region.capacity - bytes ||
            desc->planes[p].stride != static_cast<int64_t>(shape.row_bytes[p]) ||
            awj_sdk::row(desc->planes[p], 0) != entry + slot_metadata + offset)
          return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "共享提交必须使用槽位内紧凑排列的平面，不能隐式复制外部帧。");
        offset += bytes;
      }
      if (desc->color.icc_size > region.capacity - offset ||
          (desc->color.icc_size && desc->color.icc != entry + slot_metadata + offset))
        return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "共享 ICC 不在槽位中。");
      const auto header = awj_sdk::encode_wire_header(*desc, shape);
      std::copy(header.begin(), header.end(), entry);
      put32(entry + state_offset, ready_slot); region.notify();
      return AWJ_OK;
    }
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "提交 token 已过期或不属于此写槽位。");
  });
}
awj_status AWJ_CALL awj_shm_abort_write(awj_shm_channel* channel, uint64_t token) {
  return awj_sdk::boundary([&] {
    if (!channel || !channel->producer || !token) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "无效生产者或 token。");
    auto& region = *channel->region;
    Lock lock{region, 1000};
    if (!lock.acquired()) return invalid_channel();
    for (uint32_t i = 0; i < region.slots; ++i) {
      auto* entry = region.slot(i);
      if (u32(entry + state_offset) == writing_slot && u64(entry + token_offset) == token) {
        put32(entry + state_offset, free_slot); region.notify(); return AWJ_OK;
      }
    }
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "写槽位 token 已过期。");
  });
}
awj_status AWJ_CALL awj_shm_receive(awj_shm_channel* channel, uint32_t timeout, awj_frame** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少共享帧输出。");
    *output = nullptr;
    if (!channel || channel->producer) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "receive 需要消费者通道。");
    return wait_channel(channel, timeout, [&](Region& region) {
      uint32_t index = region.slots; uint64_t first = UINT64_MAX;
      for (uint32_t i = 0; i < region.slots; ++i) {
        const auto* entry = region.slot(i); const auto state = u32(entry + state_offset);
        if (state > reading_slot) return invalid_channel();
        if ((state == ready_slot || state == writing_slot) && u64(entry + token_offset) < first) {
          index = i; first = u64(entry + token_offset);
        }
      }
      if (index == region.slots) return region.closed() ? AWJ_END_OF_STREAM : AWJ_BUSY;
      auto* entry = region.slot(index);
      if (u32(entry + state_offset) != ready_slot) return AWJ_BUSY;
      awj_sdk::WireHeader header{}; std::copy_n(entry, header.size(), header.begin());
      awj_frame_desc desc{}; awj_sdk::Layout shape{}; uint64_t payload{};
      auto status = awj_sdk::decode_wire_header(header, desc, shape, payload, region.limits);
      if (status != AWJ_OK) return status;
      if (payload > region.capacity) return invalid_channel();
      uint64_t offset = 0;
      for (uint32_t p = 0; p < shape.count; ++p) {
        desc.planes[p] = {entry + slot_metadata, payload, offset, static_cast<int64_t>(shape.row_bytes[p])};
        offset += shape.row_bytes[p] * shape.rows[p];
      }
      desc.color.icc = desc.color.icc_size ? entry + slot_metadata + offset : nullptr;
      status = awj_sdk::validate(region.limits, desc);
      if (status != AWJ_OK) return status;
      auto frame = std::make_unique<awj_frame>();
      auto lease = std::make_shared<SlotLease>();
      lease->region = channel->region; lease->index = index; lease->token = first;
      frame->desc = desc; frame->storage = std::move(lease);
      put32(entry + state_offset, reading_slot); region.notify();
      *output = frame.release();
      return AWJ_OK;
    });
  });
}
void AWJ_CALL awj_shm_close(awj_shm_channel* channel) { close_channel(channel); }
void AWJ_CALL awj_shm_destroy(awj_shm_channel* channel) { close_channel(channel); delete channel; }
}
