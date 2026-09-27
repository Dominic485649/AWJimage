module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

export module awj.studio_defaults;

export namespace awj::studio_defaults {

inline constexpr std::string_view config_file_name = "AWJ.jsonc";
inline constexpr unsigned long worker_force_stop_exit_code = 130;

inline constexpr std::uint64_t bytes_per_gib = 1024ull * 1024ull * 1024ull;

inline constexpr std::uint32_t default_window_width = 1220;
inline constexpr std::uint32_t default_window_height = 800;
// At this width six 68px template buttons end at the collision selector's right edge.
inline constexpr int min_window_width = 796;
inline constexpr int min_window_height = 560;
inline constexpr int max_window_width = 16384;
inline constexpr int max_window_height = 16384;

inline constexpr std::size_t max_task_rows = 5000;
inline constexpr std::size_t max_pending_events = 2048;

inline constexpr auto queue_double_click_delay = std::chrono::milliseconds{450};

inline constexpr auto theme_refresh_interval = std::chrono::seconds{3};
inline constexpr auto config_save_interval = std::chrono::milliseconds{800};

}  // namespace awj::studio_defaults
