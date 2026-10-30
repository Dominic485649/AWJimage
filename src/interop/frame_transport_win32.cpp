#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include "awj/awj_shm.h"
#include "sdk_internal.h"
#include <algorithm>
#include <string>

namespace {
constexpr size_t global_bytes = 128;
constexpr size_t slot_metadata = 176;
constexpr size_t state_offset = 160;
constexpr size_t token_offset = 168;
constexpr uint32_t free_slot = 0, writing_slot = 1, ready_slot = 2, reading_slot = 3;
constexpr std::array<uint8_t, 8> map_magic{'A','W','J','S','H','M','1',0};
uint32_t u32(const uint8_t* p) noexcept {
  uint32_t v = 0; for (size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (i * 8); return v;
}
uint64_t u64(const uint8_t* p) noexcept {
  uint64_t v = 0; for (size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8); return v;
}
void put32(uint8_t* p, uint32_t v) noexcept { for (size_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8)); }
void put64(uint8_t* p, uint64_t v) noexcept { for (size_t i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8)); }
struct Handle {
  HANDLE value{};
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle() = default;
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
struct Security {
  PSECURITY_DESCRIPTOR descriptor{};
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
  ~Security() { if (descriptor) LocalFree(descriptor); }
  bool initialize() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    std::vector<uint8_t> bytes(size);
    if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size)) return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) return false;
    std::wstring acl;
    try { acl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring{sid} + L")"; }
    catch (...) { LocalFree(sid); throw; }
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    attributes.lpSecurityDescriptor = descriptor;
    return true;
  }
};
bool session_name(const char* input, std::wstring& output) {
  if (!input) return false;
  output = L"Local\\AWJimage-1.3-";
  size_t length = 0;
  for (; length <= 64 && input[length]; ++length) {
    const unsigned char c = static_cast<unsigned char>(input[length]);
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    output.push_back(static_cast<wchar_t>(c));
  }
  return length && length <= 64;
}
uint64_t creation_time(HANDLE process) noexcept {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
struct Region {
  Handle mapping, mutex, data_event, space_event, peer;
  bool producer{};
  uint32_t peer_pid{};
  uint64_t peer_created{};
  std::atomic_bool failed{};
  uint8_t* memory{};
  uint32_t slots{};
  uint64_t capacity{}, stride{}, size{};
  awj_context_options limits{};
  std::stop_token stop{};
  ~Region() { if (memory) UnmapViewOfFile(memory); }
  uint8_t* slot(uint32_t index) const noexcept { return memory + global_bytes + static_cast<size_t>(index * stride); }
  bool closed() const noexcept { return u32(memory + 48) != 0; }
  // 调用者已持有通道 mutex；只缓存一次真实句柄，并验证创建时间防止 PID 重用。
  bool peer_alive() {
    const auto pid = u32(memory + (producer ? 56 : 52));
    const auto created = u64(memory + (producer ? 72 : 64));
    if (!pid && producer && !peer.value) return true;
    if (!pid || !created) return false;
    if (!peer.value) {
      peer.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!peer.value || creation_time(peer.value) != created) return false;
      peer_pid = pid; peer_created = created;
    }
    return pid == peer_pid && created == peer_created && WaitForSingleObject(peer.value, 0) == WAIT_TIMEOUT;
  }
  void notify() const noexcept {
    bool free = closed(), ready = closed();
    uint64_t first = UINT64_MAX;
    uint32_t first_state = free_slot;
    for (uint32_t i = 0; i < slots; ++i) {
      const auto* entry = slot(i);
      const auto state = u32(entry + state_offset);
      free |= state == free_slot;
      if ((state == writing_slot || state == ready_slot) && u64(entry + token_offset) < first) {
        first = u64(entry + token_offset); first_state = state;
      }
    }
    ready |= first_state == ready_slot;
    if (free) SetEvent(space_event.value); else ResetEvent(space_event.value);
    if (ready) SetEvent(data_event.value); else ResetEvent(data_event.value);
  }
};
struct Lock {
  Region& region;
  DWORD state;
  Lock(Region& value, uint32_t timeout) : region{value}, state{WaitForSingleObject(value.mutex.value, timeout)} {
    if (state == WAIT_ABANDONED) {
      put32(region.memory + 48, 1); put32(region.memory + 80, 1);
      SetEvent(region.data_event.value); SetEvent(region.space_event.value);
    }
  }
  ~Lock() { if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) ReleaseMutex(region.mutex.value); }
  bool acquired() const noexcept { return state == WAIT_OBJECT_0; }
};
class Budget {
 public:
  explicit Budget(uint32_t value) : timeout_{value}, end_{std::chrono::steady_clock::now() + std::chrono::milliseconds{value}} {}
  uint32_t remaining() const noexcept {
    if (!timeout_) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (now >= end_) return 0;
    return static_cast<uint32_t>(std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(end_ - now).count()));
  }
 private:
  uint32_t timeout_;
  std::chrono::steady_clock::time_point end_;
};
struct SlotLease {
  std::shared_ptr<Region> region;
  uint32_t index{};
  uint64_t token{};
  ~SlotLease() {
    Lock lock{*region, 1000};
    if (!lock.acquired()) {
      region->failed.store(true); SetEvent(region->data_event.value); SetEvent(region->space_event.value);
      awj_sdk::fail(AWJ_IO_ERROR, "归还共享槽位时通道锁失效，必须关闭通道。"); return;
    }
    auto* entry = region->slot(index);
    if (u32(entry + state_offset) == reading_slot && u64(entry + token_offset) == token) {
      put32(entry + state_offset, free_slot);
      region->notify();
    }
  }
};
awj_status invalid_channel() { return awj_sdk::fail(AWJ_IO_ERROR, "共享内存对象、权限、布局或对端状态无效。"); }
}

struct awj_shm_channel { std::shared_ptr<Region> region; bool producer{}; };

namespace {
template<class Action> awj_status wait_channel(awj_shm_channel* channel, uint32_t timeout, Action&& action) {
  if (!channel || timeout > 60000) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "无效通道或等待上限。");
  auto& region = *channel->region;
  Budget budget{timeout};
  for (;;) {
    if (region.stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "共享内存等待已取消。");
    {
      Lock lock{region, std::min<uint32_t>(budget.remaining(), 25)};
      if (lock.acquired()) {
        if (region.failed.load() || u32(region.memory + 80)) return invalid_channel();
        if (!region.closed() && !region.peer_alive()) {
          region.failed.store(true); put32(region.memory + 80, 1); put32(region.memory + 48, 1); region.notify();
          return invalid_channel();
        }
        const auto status = action(region);
        if (status != AWJ_BUSY) return status;
      } else if (lock.state != WAIT_TIMEOUT) return invalid_channel();
    }
    const auto remaining = budget.remaining();
    if (!remaining) return awj_sdk::fail(timeout ? AWJ_TIMEOUT : AWJ_BUSY, "共享内存通道暂无可用槽位。");
    const auto event = channel->producer ? region.space_event.value : region.data_event.value;
    if (WaitForSingleObject(event, std::min<uint32_t>(remaining, 25)) == WAIT_FAILED) return invalid_channel();
  }
}
void close_channel(awj_shm_channel* channel) noexcept {
  if (!channel) return;
  auto& region = *channel->region;
  Lock lock{region, 1000};
  if (!lock.acquired()) { awj_sdk::fail(AWJ_IO_ERROR, "共享内存关闭时通道锁失效。"); return; }
  put32(region.memory + 48, 1);
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
    std::wstring prefix;
    if (!context || !session_name(name, prefix) || !slots || slots > 16 || !capacity ||
        capacity > context->options.max_frame_bytes || capacity > (UINT64_MAX - slot_metadata - 63))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "共享内存名称、容量或槽位数无效。");
    const uint64_t stride = (capacity + slot_metadata + 63) & ~UINT64_C(63);
    uint64_t bytes{};
    if (!awj_sdk::multiply(stride, slots, bytes) || bytes > UINT64_MAX - global_bytes ||
        bytes + global_bytes > context->options.memory_budget_bytes || bytes + global_bytes > SIZE_MAX)
      return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "共享内存总容量超过 context 预算。");
    auto region = std::make_shared<Region>();
    region->size = bytes + global_bytes; region->stride = stride; region->capacity = capacity; region->slots = slots;
    region->limits = context->options; region->stop = context->stop.get_token(); region->producer = true;
    Security security;
    if (!security.initialize()) return invalid_channel();
    region->mutex.value = CreateMutexW(&security.attributes, TRUE, (prefix + L"-mutex").c_str());
    if (!region->mutex.value) return invalid_channel();
    if (GetLastError() == ERROR_ALREADY_EXISTS) return awj_sdk::fail(AWJ_BUSY, "共享会话已存在，拒绝覆盖。");
    // 新建的 mutex 最初由本线程持有，失败路径也必须归还。
    struct Unlock { HANDLE mutex; ~Unlock() { ReleaseMutex(mutex); } } unlock{region->mutex.value};
    region->mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, &security.attributes, PAGE_READWRITE,
        static_cast<DWORD>(region->size >> 32), static_cast<DWORD>(region->size), (prefix + L"-map").c_str());
    if (!region->mapping.value || GetLastError() == ERROR_ALREADY_EXISTS) return invalid_channel();
    region->data_event.value = CreateEventW(&security.attributes, TRUE, FALSE, (prefix + L"-data").c_str());
    if (!region->data_event.value || GetLastError() == ERROR_ALREADY_EXISTS) return invalid_channel();
    region->space_event.value = CreateEventW(&security.attributes, TRUE, TRUE, (prefix + L"-space").c_str());
    if (!region->space_event.value || GetLastError() == ERROR_ALREADY_EXISTS) return invalid_channel();
    region->memory = static_cast<uint8_t*>(MapViewOfFile(region->mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(region->size)));
    if (!region->memory) return invalid_channel();
    std::copy(map_magic.begin(), map_magic.end(), region->memory);
    put32(region->memory + 8, 1); put32(region->memory + 12, slots);
    put64(region->memory + 16, capacity); put64(region->memory + 24, stride); put64(region->memory + 32, region->size);
    const auto created = creation_time(GetCurrentProcess());
    if (!created) return invalid_channel();
    put64(region->memory + 40, 1); put32(region->memory + 52, GetCurrentProcessId());
    put64(region->memory + 64, created);
    auto channel = std::make_unique<awj_shm_channel>(); channel->region = std::move(region); channel->producer = true;
    *output = channel.release();
    return AWJ_OK;
  });
}
awj_status AWJ_CALL awj_shm_open(awj_context* context, const char* name, awj_shm_channel** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少共享通道输出。");
    *output = nullptr;
    std::wstring prefix;
    if (!context || !session_name(name, prefix)) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "共享会话名称无效。");
    auto region = std::make_shared<Region>();
    region->limits = context->options; region->stop = context->stop.get_token();
    region->mutex.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (prefix + L"-mutex").c_str());
    region->mapping.value = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (prefix + L"-map").c_str());
    region->data_event.value = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (prefix + L"-data").c_str());
    region->space_event.value = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (prefix + L"-space").c_str());
    if (!region->mutex.value || !region->mapping.value || !region->data_event.value || !region->space_event.value) return invalid_channel();
    region->memory = static_cast<uint8_t*>(MapViewOfFile(region->mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!region->memory) return invalid_channel();
    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(region->memory, &information, sizeof(information)) || information.RegionSize < global_bytes) return invalid_channel();
    Lock lock{*region, 1000};
    if (!lock.acquired()) return invalid_channel();
    if (!std::equal(map_magic.begin(), map_magic.end(), region->memory) || u32(region->memory + 8) != 1 ||
        u32(region->memory + 60) || u32(region->memory + 56) || region->closed()) return invalid_channel();
    region->slots = u32(region->memory + 12); region->capacity = u64(region->memory + 16);
    region->stride = u64(region->memory + 24); region->size = u64(region->memory + 32);
    uint64_t bytes{};
    if (!region->slots || region->slots > 16 || !region->capacity || region->capacity > context->options.max_frame_bytes ||
        region->capacity > UINT64_MAX - slot_metadata - 63 ||
        region->stride != ((region->capacity + slot_metadata + 63) & ~UINT64_C(63)) ||
        !awj_sdk::multiply(region->stride, region->slots, bytes) || bytes > UINT64_MAX - global_bytes ||
        region->size != bytes + global_bytes || region->size > information.RegionSize ||
        region->size > context->options.memory_budget_bytes) return invalid_channel();
    if (!region->peer_alive()) return invalid_channel();
    const auto created = creation_time(GetCurrentProcess());
    if (!created) return invalid_channel();
    auto channel = std::make_unique<awj_shm_channel>();
    put64(region->memory + 72, created);
    put32(region->memory + 56, GetCurrentProcessId());
    channel->region = std::move(region);
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
    const auto checked = awj_sdk::validate(region.limits, *desc);
    if (checked != AWJ_OK) return checked;
    awj_sdk::Layout shape{};
    const auto shape_status = awj_sdk::layout(*desc, shape);
    if (shape_status != AWJ_OK) return shape_status;
    Lock lock{region, 1000};
    if (!lock.acquired()) return invalid_channel();
    if (region.failed.load() || u32(region.memory + 80)) return invalid_channel();
    if (region.closed()) return AWJ_END_OF_STREAM;
    if (!region.peer_alive()) {
      put32(region.memory + 80, 1); put32(region.memory + 48, 1); region.notify();
      return invalid_channel();
    }
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
      put32(entry + state_offset, ready_slot);
      region.notify();
      return AWJ_OK;
    }
    return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "提交 token 已过期或不属于此写槽位。");
  });
}
awj_status AWJ_CALL awj_shm_abort_write(awj_shm_channel* channel, uint64_t token) {
  return awj_sdk::boundary([&] {
    if (!channel || !channel->producer || !token) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "无效生产者或 token。");
    auto& region = *channel->region;
    Lock lock{region, 1000}; if (!lock.acquired()) return invalid_channel();
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
      status = awj_sdk::validate(region.limits, desc); if (status != AWJ_OK) return status;
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
