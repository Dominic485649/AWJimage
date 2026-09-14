#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "shell_extension_core.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shell = awj::shell_extension;

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error{std::string{message}};
}

void check_quote_round_trip(std::wstring_view argument) {
  const auto quoted = L"probe " + shell::quote_windows_argument(argument, true);
  int count = 0;
  LPWSTR* parsed = CommandLineToArgvW(quoted.c_str(), &count);
  check(parsed != nullptr, "CommandLineToArgvW failed");
  struct LocalArguments {
    LPWSTR* value{};
    ~LocalArguments() {
      if (value != nullptr) LocalFree(value);
    }
  } cleanup{parsed};
  check(count == 2 && std::wstring_view{parsed[1]} == argument,
        "quoted argument did not round-trip");
}

std::vector<std::wstring> parse_command_line(const std::wstring& command_line) {
  int count = 0;
  LPWSTR* parsed = CommandLineToArgvW(command_line.c_str(), &count);
  check(parsed != nullptr, "could not parse generated command line");
  std::vector<std::wstring> result;
  for (int index = 0; index < count; ++index) result.emplace_back(parsed[index]);
  LocalFree(parsed);
  return result;
}

}  // namespace

int wmain() try {
  check(shell::is_supported_extension(L".PNG"), "uppercase PNG rejected");
  check(shell::is_supported_extension(L".awsraw"), "AWSRAW rejected");
  check(!shell::is_supported_extension(L".txt"), "TXT accepted");

  const std::array supported{
      shell::SelectionItem{L"C:\\图片 空格\\a.PNG", false},
      shell::SelectionItem{L"C:\\图片 空格\\b.webp", false}};
  check(shell::is_supported_selection(supported), "supported files rejected");
  const std::array directories{
      shell::SelectionItem{L"C:\\图片 空格\\目录一", true},
      shell::SelectionItem{L"C:\\图片 空格\\目录二", true}};
  check(shell::is_supported_selection(directories), "directories rejected");
  const std::array mixed{
      shell::SelectionItem{L"C:\\图片 空格\\a.PNG", false},
      shell::SelectionItem{L"C:\\图片 空格\\目录", true}};
  check(!shell::is_supported_selection(mixed), "mixed file/directory selection accepted");
  const std::array unsupported{
      shell::SelectionItem{L"C:\\图片 空格\\a.txt", false}};
  check(!shell::is_supported_selection(unsupported), "unsupported selection accepted");
  check(!shell::is_supported_selection({}), "empty selection accepted");

  for (const std::wstring value : {
           L"", L"plain", L"space value", L"C:\\trailing slash\\",
           L"embedded\"quote", L"slashes\\\\\"quote", L"Unicode Ω 图片"}) {
    check_quote_round_trip(value);
  }

  const auto defaults = shell::default_menu_commands();
  check(defaults.size() == 5, "fallback command count is not five");
  check(defaults.front().canonical_verb == L"AWJimage.Convert.10.png",
        "fallback command ordering changed");

  const auto built = shell::build_awj_command_line(
      std::filesystem::path{L"C:\\Program Files\\AWJimage\\AWJ.exe"},
      defaults.front().arguments, supported);
  check(built.has_value(), "valid command line was rejected");
  const auto parsed = parse_command_line(*built);
  const std::vector<std::wstring> expected{
      L"C:\\Program Files\\AWJimage\\AWJ.exe", L"--shell-window",
      L"--shell-convert", L"--format", L"png", L"--collision", L"number",
      L"-i", L"C:\\图片 空格\\a.PNG", L"C:\\图片 空格\\b.webp"};
  check(parsed == expected, "generated command arguments changed");

  shell::RuntimeConfiguration configuration{
      .executable = L"C:\\Program Files\\AWJimage\\AWJ.exe",
      .menu_label = L"AWJimage 转换",
      .commands = defaults};
  auto preset = defaults.front();
  preset.group_label = L"网页预设";
  preset.group_canonical_verb = L"AWJimage.Preset.00";
  preset.canonical_verb = L"AWJimage.Preset.00.png";
  preset.arguments.insert(preset.arguments.begin(), {L"--preset", L"网页预设"});
  configuration.commands.push_back(std::move(preset));
  const auto encoded = shell::encode_configuration(configuration);
  check(encoded.has_value(), "valid shell configuration was not encoded");
  const auto decoded = shell::decode_configuration(*encoded);
  check(decoded.has_value() && *decoded == configuration,
        "shell configuration did not round-trip");
  auto truncated = *encoded;
  truncated.pop_back();
  check(!shell::decode_configuration(truncated),
        "truncated shell configuration was accepted");
  auto duplicate = configuration;
  duplicate.commands.back().canonical_verb =
      duplicate.commands.front().canonical_verb;
  check(!shell::encode_configuration(duplicate),
        "duplicate canonical verbs were accepted");

  auto invalid_arguments = defaults.front().arguments;
  invalid_arguments.push_back(L"line\nbreak");
  check(!shell::build_awj_command_line(
             L"C:\\AWJ\\AWJ.exe", invalid_arguments, supported),
        "control character in configured argument accepted");
  check(!shell::build_awj_command_line(L"AWJ.exe", defaults.front().arguments,
                                       supported),
        "relative executable path accepted");

  const std::array huge_selection{shell::SelectionItem{
      std::filesystem::path{L"C:\\" + std::wstring(33000, L'a') + L".png"},
      false}};
  check(!shell::build_awj_command_line(L"C:\\AWJ\\AWJ.exe",
                                       defaults.front().arguments,
                                       huge_selection),
        "oversized CreateProcess command line accepted");

  std::puts("Shell extension core selection and command-line tests passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
