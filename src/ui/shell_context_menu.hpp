#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace awj::shell_context_menu {

inline constexpr std::uint32_t schema_version = 4;
inline constexpr std::wstring_view owner_value_name = L"AWJimage.Owner";
inline constexpr std::wstring_view schema_value_name = L"AWJimage.SchemaVersion";
inline constexpr std::wstring_view owner_value = L"AWJimage";
inline constexpr std::wstring_view parent_canonical_verb = L"AWJimage.Convert";
inline constexpr std::wstring_view shared_tree_reference = L"AWJimage.ContextMenu.v4.A";

struct FormatParams {
  std::wstring quality_text{};
  std::wstring bit_depth_text{};
  std::wstring speed_text{};
  int avif_encoder_index{};
  int avif_color_representation_index{};
  int chroma_index{};
  int alpha_policy_index{1};
  int jpegli_progressive_index{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  bool strip_metadata{};
  bool allow_wic_fallback{true};
  bool close_on_finish{true};
  bool install_avif_png_command{};
  int size_limit_index{};
  std::wstring max_width_text{};
  std::wstring max_height_text{};
  std::wstring max_long_edge_text{};
  std::wstring max_short_edge_text{};

  bool operator==(const FormatParams&) const = default;
};

using MenuParams = std::array<FormatParams, 5>;

struct CommandSpec {
  std::wstring_view canonical_verb{};
  std::wstring_view label{};
  std::wstring_view format{};
  std::size_t params_index{};
  bool append_png_suffix{};
};

struct InstallPlan {
  std::vector<std::wstring> extensions{};

  bool operator==(const InstallPlan&) const = default;
};

enum class RegistryValueKind {
  string,
  dword,
};

struct RegistryValueSpec {
  std::wstring key{};
  std::wstring name{};
  RegistryValueKind kind{RegistryValueKind::string};
  std::wstring string_value{};
  std::uint32_t dword_value{};

  bool operator==(const RegistryValueSpec&) const = default;
};

struct RegistrySchema {
  InstallPlan plan{};
  std::vector<std::wstring> parent_roots{};
  std::vector<std::wstring> keys{};
  std::vector<RegistryValueSpec> values{};

  bool operator==(const RegistrySchema&) const = default;
};

std::span<const std::wstring_view> supported_extensions() noexcept;
std::span<const CommandSpec> command_specs() noexcept;
std::wstring image_parent_key();
std::wstring directory_parent_key();
std::wstring extension_parent_key(std::wstring_view extension);
std::wstring shared_tree_key(int slot = 0);
std::wstring legacy_shared_tree_key();
std::vector<std::wstring> legacy_root_keys();
std::vector<std::wstring> owned_root_keys();
InstallPlan build_install_plan();
std::wstring build_convert_command_line(const std::filesystem::path& awj_exe,
                                        std::wstring_view format,
                                        const FormatParams& params,
                                        bool append_png_suffix = false);
RegistrySchema build_registry_schema(const std::filesystem::path& awj_exe,
                                     const MenuParams& menu_params,
                                     const InstallPlan& plan,
                                     std::span<const std::wstring> preset_names = {},
                                     int slot = 0);

std::expected<InstallPlan, std::string> detect_install_plan();
std::expected<void, std::string> install(const std::filesystem::path& awj_exe,
                                         const MenuParams& menu_params,
                                         std::span<const std::wstring> preset_names = {});
std::expected<void, std::string> reconcile(const std::filesystem::path& awj_exe,
                                         const MenuParams& menu_params,
                                         std::span<const std::wstring> preset_names = {},
                                         bool force_install = false);
std::expected<void, std::string> recover();
std::expected<bool, std::string> is_installed();
std::expected<void, std::string> remove();
std::expected<std::optional<std::string>, std::string> warning(
    const std::filesystem::path& awj_exe,
    const MenuParams& menu_params,
    std::span<const std::wstring> preset_names = {});
std::expected<std::vector<std::wstring>, std::string> legacy_machine_commands();
std::expected<void, std::string> remove_legacy_machine_commands();

}  // namespace awj::shell_context_menu
