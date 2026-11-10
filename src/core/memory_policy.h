#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/sysinfo.h>
#endif

namespace awj::memory_policy {

struct MemoryStatus {
  std::uint64_t total_bytes{};
  std::uint64_t available_bytes{};
};

// Integer floor(4*n/5), including UINT64_MAX, without a multiply overflow or
// floating-point rounding. Zero is unknown, never an unlimited sentinel.
constexpr std::uint64_t physical_input_limit(std::uint64_t total) noexcept {
  return (total / 5) * 4 + ((total % 5) * 4) / 5;
}

constexpr bool fits_sum(std::uint64_t used, std::uint64_t extra,
                        std::uint64_t limit) noexcept {
  return used <= limit && extra <= limit - used;
}

constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  return fits_sum(a, b, UINT64_MAX) ? a + b : UINT64_MAX;
}

constexpr std::uint64_t saturating_multiply(std::uint64_t a, std::uint64_t b) noexcept {
  return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

inline MemoryStatus current_memory_status() noexcept {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return {};
  return {status.ullTotalPhys, status.ullAvailPhys};
#else
  struct sysinfo status {};
  if (sysinfo(&status) != 0) return {};
  const auto unit = static_cast<std::uint64_t>(status.mem_unit ? status.mem_unit : 1);
  return {saturating_multiply(status.totalram, unit),
          saturating_multiply(saturating_add(status.freeram, status.bufferram), unit)};
#endif
}

inline constexpr std::size_t default_metadata_limit = 64u * 1024u * 1024u;
inline constexpr const char* unavailable_message =
    "无法获取物理总内存，无法确定总内存 80% 的输入/缓冲区保护上限。";

// A value snapshot, not mutable process state. The legacy input-only opt-out
// does not disable working-set or metadata/library allocation protection.
struct Policy {
  std::uint64_t input_bytes{};
  bool unrestricted_input{};
  bool unrestricted_memory{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return unrestricted_input || input_bytes != 0;
  }
  [[nodiscard]] constexpr std::uint64_t input_limit() const noexcept {
    return unrestricted_input ? UINT64_MAX : input_bytes;
  }
  [[nodiscard]] constexpr std::size_t metadata_limit(std::size_t overhead = 0) const noexcept {
    if (unrestricted_memory) return SIZE_MAX;
    return overhead > SIZE_MAX - default_metadata_limit ? SIZE_MAX
                                                        : default_metadata_limit + overhead;
  }
  friend constexpr bool operator==(const Policy&, const Policy&) = default;
};

constexpr std::optional<Policy> resolve(bool unlock_memory, bool unlock_input,
                                        std::uint64_t physical_total) noexcept {
  const bool unrestricted = unlock_memory || unlock_input;
  const auto limit = unrestricted ? UINT64_MAX : physical_input_limit(physical_total);
  if (!limit) return std::nullopt;
  return Policy{limit, unrestricted, unlock_memory};
}

inline std::optional<Policy> resolve_system(bool unlock_memory = false,
                                           bool unlock_input = false) noexcept {
  // An opt-out must also work when the physical-memory query is unavailable.
  return resolve(unlock_memory, unlock_input,
                 unlock_memory || unlock_input ? 0 : current_memory_status().total_bytes);
}

namespace detail {
inline thread_local const Policy* active_policy = nullptr;
}

// Own the snapshot: callers may pass temporaries. Save/restore is thread-local,
// so nested operations restore correctly and overlapping sessions never race.
// A scope is neither movable nor copyable and must die on its creating thread.
class Scope final {
 public:
  explicit Scope(Policy policy) noexcept : policy_(policy), previous_(detail::active_policy) {
    detail::active_policy = &policy_;
  }
  ~Scope() { detail::active_policy = previous_; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;
 private:
  const Policy policy_;
  const Policy* previous_;
};

inline Policy current() {
  if (detail::active_policy) return *detail::active_policy;
  const auto policy = resolve_system();
  if (!policy) throw std::runtime_error{unavailable_message};
  return *policy;
}

// Metadata-only foreign callbacks need no OS query, allocation or exception.
// Library callbacks that may run on other threads carry their own Policy/limit
// in opaque state rather than reading this thread's scope.
inline std::size_t metadata_limit(std::size_t overhead = 0) noexcept {
  return detail::active_policy ? detail::active_policy->metadata_limit(overhead)
                               : Policy{}.metadata_limit(overhead);
}

inline bool unrestricted_memory() noexcept {
  return detail::active_policy && detail::active_policy->unrestricted_memory;
}

}  // namespace awj::memory_policy
