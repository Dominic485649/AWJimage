#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <string_view>

import awj.update_windows;

namespace {
namespace fs = std::filesystem;

int fail(std::string_view message) {
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
  return 1;
}

void write(const fs::path& path, std::string_view content) {
  fs::create_directories(path.parent_path());
  std::ofstream out{path, std::ios::binary};
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

struct Fixture {
  fs::path parent = fs::temp_directory_path() /
      std::format(L"awj-cleanup-test-{}-{}", GetCurrentProcessId(), GetTickCount64());
  fs::path root = parent / L"AWJimage";
  fs::path stage = root / L"updates" / L"transaction";
  fs::path install = parent / L"install";
  Fixture() {
    fs::create_directories(stage);
    fs::create_directories(install);
    write(root / L"shader_cache" / L"legacy.bin", "old");
    write(root / L"unrelated" / L"nested" / L"asset.bin", "old");
    write(root / L"random.tmp", "old");
  }
  ~Fixture() { std::error_code ec; fs::remove_all(parent, ec); }
  bool intact() const {
    return fs::exists(root / L"shader_cache" / L"legacy.bin") &&
           fs::exists(root / L"unrelated" / L"nested" / L"asset.bin") &&
           fs::exists(root / L"random.tmp") && fs::exists(stage);
  }
};

int test_state(std::string_view state, bool pointer) {
  Fixture fixture;
  if (!state.empty()) write(fixture.stage / L"state.txt", state);
  if (pointer) write(fixture.install / L".awj-update-transaction", "stage");
  auto result = awj::update::windows_detail::cleanup_committed_update(
      fixture.root, fixture.stage, fixture.install);
  if (result || !fixture.intact()) return fail("uncommitted update deleted LocalAppData contents");
  return 0;
}
}  // namespace

int main() {
  for (auto state : {"", "prepared", "files-replaced", "rolled-back", "failed"}) {
    if (test_state(state, false)) return 1;
  }
  if (test_state("committed", true)) return 1;
  Fixture fixture;
  write(fixture.stage / L"state.txt", "committed");
  auto result = awj::update::windows_detail::cleanup_committed_update(
      fixture.root, fixture.stage, fixture.install);
  if (!result) return fail(result.error());
  if (!fs::exists(fixture.root) || !fs::is_empty(fixture.root))
    return fail("committed update did not empty all LocalAppData/AWJimage contents");

  Fixture legacy;
  write(legacy.stage / L"state.txt", "files-replaced");
  write(legacy.stage / L"version.txt", AWJ_BUILD_VERSION);
  const auto stage_utf8 = legacy.stage.u8string();
  write(legacy.install / L".awj-update-transaction",
        {reinterpret_cast<const char*>(stage_utf8.data()), stage_utf8.size()});
  if (!awj::update::windows_detail::legacy_cleanup_preflight(
          legacy.root, legacy.stage, legacy.install)) {
    return fail("legacy health-check preflight rejected a valid pending update");
  }
  write(legacy.stage / L"cleanup-managed-by-helper", "1");
  if (awj::update::windows_detail::legacy_cleanup_preflight(
          legacy.root, legacy.stage, legacy.install)) {
    return fail("new helper must own its own cleanup");
  }
  fs::remove(legacy.stage / L"cleanup-managed-by-helper");
  if (awj::update::windows_detail::cleanup_legacy_committed_update(
          legacy.root, legacy.stage, legacy.install, 0) || !legacy.intact()) {
    return fail("legacy pending transaction deleted LocalAppData contents");
  }
  fs::remove(legacy.install / L".awj-update-transaction");
  write(legacy.stage / L"state.txt", "rolled-back");
  if (awj::update::windows_detail::cleanup_legacy_committed_update(
          legacy.root, legacy.stage, legacy.install, 0) || !legacy.intact()) {
    return fail("legacy rollback deleted LocalAppData contents");
  }
  fs::remove(legacy.stage / L"state.txt");
  if (awj::update::windows_detail::cleanup_legacy_committed_update(
          legacy.root, legacy.stage, legacy.install, 36) || !legacy.intact()) {
    return fail("failed legacy helper deleted LocalAppData contents");
  }
  if (!awj::update::windows_detail::cleanup_legacy_committed_update(
          legacy.root, legacy.stage, legacy.install, 0) ||
      !fs::exists(legacy.root) || !fs::is_empty(legacy.root)) {
    return fail("successful legacy update did not empty LocalAppData contents");
  }
  return 0;
}
