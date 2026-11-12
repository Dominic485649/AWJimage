#include "../src/compat/stdlib_compat.h"
#include "../src/ui/studio_config.h"
#include "../src/ui/studio_config_io.h"
#include "../src/ui/studio_json.h"
#include <private/slint_tests_helpers.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error{message};
}
void require(std::expected<void, std::string> result) {
  if (!result) throw std::runtime_error{result.error()};
}
std::string read(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  check(static_cast<bool>(input), "测试配置不可读");
  return {std::istreambuf_iterator<char>{input}, {}};
}
std::array<bool, 6> switches(const AwjStudio& app) {
  return {app.get_strip_metadata(), app.get_write_log(), app.get_delete_source(),
      app.get_preserve_creation_time(), app.get_preserve_modification_time(), app.get_preserve_access_time()};
}
}

int main() try {
  using namespace awj::studio;
  namespace fs = std::filesystem;
  const auto path = studio_config_path();
  check(!path.empty() && path.parent_path().parent_path().filename() == "studio-config-io-contract" &&
      !fs::exists(path), "必须使用空的测试专属配置目录");
  slint::testing::init();
  auto app = AwjStudio::create();
  UiState state;
  const std::array<bool, 6> off{};
  check(switches(*app) == off, "新会话六项开关未全部关闭");
  state.config_defaults = capture_studio_config(*app, &state);
  require(write_file_atomically(path,
      R"({"theme_index":1,"language_index":1,"collision_index":3,"strip_metadata":true,"write_log":true,"write_summary":true,"delete_source":true,"preserve_creation_time":true,"preserve_modification_time":true,"preserve_access_time":true,"menu_avif_strip_metadata":true,"future_extension":"保留","last_verified_manifest_v2_sequence":42,"update_manifest_v2_raw":"signed bytes","update_manifest_v2_signature":"signature"})"));
  require(apply_studio_config_file(*app, state));
  check(switches(*app) == off && app->get_theme_index() == 1 && app->get_language_index() == 1 &&
      app->get_collision_index() == 3 && state.menu_params[0].strip_metadata,
      "旧配置恢复了会话开关或丢失了其他设置");
  const auto original = read(path);
  const auto before = capture_studio_config(*app, &state);
  state.last_config_snapshot = before;
  app->set_strip_metadata(true);
  app->set_write_log(true);
  app->set_delete_source(true);
  app->set_preserve_creation_time(true);
  app->set_preserve_modification_time(true);
  app->set_preserve_access_time(true);
  const std::array selected{true, true, true, true, true, true};
  check(switches(*app) == selected && capture_studio_config(*app, &state) == before,
      "会话开关污染了持久快照或无法正常勾选");
  // 菜单快照始终相等，自动保存不会触发注册表同步。
  require(persist_studio_config_if_changed(*app, state));
  check(read(path) == original && switches(*app) == selected,
      "仅修改会话开关仍触发写盘或重置了选择");
  require(apply_studio_config_file(*app, state));
  check(switches(*app) == selected, "重载旧配置改变了当前会话选择");
  app->set_theme_index(2);
  require(persist_studio_config_if_changed(*app, state));
  const auto saved = awj::studio_json::parse_jsonc_config(read(path));
  check(saved.has_value() && switches(*app) == selected, "保存其他设置失败或清空了会话开关");
  for (const auto key : queue_session_config_keys)
    check(!saved->contains(std::string{key}), "会话根键或旧日志别名被重新写入");
  check(saved->at("theme_index").integer == 2 && saved->at("collision_index").integer == 3 &&
      saved->at("menu_avif_strip_metadata").boolean && saved->at("future_extension").string == "保留" &&
      saved->at("last_verified_manifest_v2_sequence").integer == 42 &&
      saved->at("update_manifest_v2_raw").string == "signed bytes" &&
      saved->at("update_manifest_v2_signature").string == "signature",
      "保存丢失了独立菜单、重名策略、未知扩展或更新器状态");
  const auto saved_bytes = read(path);
  require(persist_studio_config_if_changed(*app, state));
  check(read(path) == saved_bytes && switches(*app) == selected, "自动保存不幂等");
  auto restarted = AwjStudio::create();
  UiState restarted_state;
  require(apply_studio_config_file(*restarted, restarted_state));
  check(switches(*restarted) == off && restarted->get_collision_index() == 3 &&
      restarted_state.menu_params[0].strip_metadata, "新会话恢复了临时开关或丢失独立设置");
  check(fs::remove(path), "测试专属配置未清理");
  std::puts("Windows 生产配置加载/快照/自动保存/重载/新会话六项开关回归通过");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
