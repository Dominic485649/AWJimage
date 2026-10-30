#pragma once
#include "awj/awj.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

struct awj_context {
  awj_context_options options{};
  std::stop_source stop{};
  std::atomic<uint64_t> admitted_bytes{};
  std::mutex interop_mutex{};
  std::shared_ptr<void> d3d_pool{};
};

struct awj_frame {
  std::atomic<uint64_t> references{1};
  awj_frame_desc desc{};
  awj_owner owner{};
  bool retained{};
  std::shared_ptr<void> storage{};
  awj_diagnostics diagnostics{sizeof(awj_diagnostics), AWJ_ABI_VERSION, 0, 0, 0, 0, 0, 1};
  ~awj_frame() {
    // 外部回调违约抛异常时也不能让异常越过 C ABI 或析构边界。
    if (retained) { try { owner.release(owner.user); } catch (...) {} }
  }
};

struct awj_result {
  std::vector<std::byte> bytes{};
  awj_diagnostics diagnostics{sizeof(awj_diagnostics), AWJ_ABI_VERSION, 0, 0, 0, 0, 0, 1};
};

struct awj_queue {
  std::mutex mutex{};
  std::condition_variable changed{};
  std::deque<awj_frame*> frames{};
  uint32_t capacity{};
  bool closed{};
};

namespace awj_sdk {
struct Layout {
  uint32_t count{};
  std::array<uint64_t, AWJ_MAX_PLANES> row_bytes{};
  std::array<uint32_t, AWJ_MAX_PLANES> rows{};
};

awj_status fail(awj_status status, std::string_view message) noexcept;
void clear_error() noexcept;
awj_status layout(const awj_frame_desc& desc, Layout& output) noexcept;
awj_status validate(const awj_context_options& options, const awj_frame_desc& desc) noexcept;
const uint8_t* row(const awj_plane& plane, uint32_t index) noexcept;
awj_status native_encode(awj_context& context, const awj_frame& frame,
                         const awj_encode_options& options, awj_result& output);
uint64_t native_capabilities() noexcept;
using WireHeader = std::array<uint8_t, 160>;
WireHeader encode_wire_header(const awj_frame_desc& desc, const Layout& shape);
awj_status decode_wire_header(const WireHeader& header, awj_frame_desc& desc,
                              Layout& shape, uint64_t& payload, const awj_context_options& limits);

template<class T> bool valid_header(const T* value) noexcept {
  return value && value->struct_size == sizeof(T) && value->abi_version == AWJ_ABI_VERSION;
}

template<class Function> awj_status boundary(Function&& function) noexcept {
  clear_error();
  try {
    return function();
  } catch (const std::bad_alloc&) {
    return fail(AWJ_OUT_OF_MEMORY, "内存分配失败。");
  } catch (const std::exception& error) {
    return fail(AWJ_INTERNAL_ERROR, error.what());
  } catch (...) {
    return fail(AWJ_INTERNAL_ERROR, "SDK 内部或外部回调出现未处理异常。");
  }
}

inline bool multiply(uint64_t a, uint64_t b, uint64_t& output) noexcept {
  if (b && a > UINT64_MAX / b) return false;
  output = a * b;
  return true;
}

class Admission {
 public:
  Admission(awj_context& context, uint64_t bytes) noexcept : context_{context}, bytes_{bytes} {
    auto current = context_.admitted_bytes.load();
    const auto budget = context_.options.memory_budget_bytes;
    while (bytes <= budget && current <= budget - bytes) {
      if (context_.admitted_bytes.compare_exchange_weak(current, current + bytes)) {
        acquired_ = true;
        break;
      }
    }
  }
  ~Admission() { if (acquired_) context_.admitted_bytes.fetch_sub(bytes_); }
  Admission(const Admission&) = delete;
  Admission& operator=(const Admission&) = delete;
  bool acquired() const noexcept { return acquired_; }
 private:
  awj_context& context_;
  uint64_t bytes_{};
  bool acquired_{};
};
}
