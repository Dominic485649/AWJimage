#include "shell_extension_core.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <set>

namespace awj::shell_extension {
namespace {

constexpr std::array<std::wstring_view, 37> kSupportedExtensions = {
    L".jpg",  L".jpeg", L".jpe", L".jfif", L".png", L".webp",
    L".bmp",  L".dib",  L".rle", L".ico",  L".tif", L".tiff",
    L".gif",  L".jxl",  L".avif", L".awsraw", L".dng", L".cr2",
    L".cr3",  L".nef",  L".arw", L".rw2",  L".orf", L".raf",
    L".pef",  L".srw",  L".x3f", L".3fr",  L".erf", L".kdc",
    L".mrw",  L".raw",  L".heic", L".heif", L".jxr", L".wdp",
    L".hdp"};

constexpr std::size_t kMaximumCommandLineCharacters = 32767;
constexpr std::size_t kMaximumCommands = 64;
constexpr std::size_t kMaximumArgumentsPerCommand = 128;
constexpr std::size_t kMaximumConfigurationCharacters = 256 * 1024;

bool contains_forbidden_character(std::wstring_view value) noexcept {
  return std::ranges::any_of(value, [](wchar_t character) {
    return character == L'\0' || character == L'\r' || character == L'\n';
  });
}

bool valid_label(std::wstring_view value) noexcept {
  return !value.empty() && value.size() <= 256 &&
         std::ranges::none_of(value,
                              [](wchar_t character) { return character < 32; });
}

bool valid_verb(std::wstring_view value) noexcept {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](wchar_t character) {
           return (character >= L'a' && character <= L'z') ||
                  (character >= L'A' && character <= L'Z') ||
                  (character >= L'0' && character <= L'9') ||
                  character == L'.' || character == L'_' ||
                  character == L'-';
         });
}

std::wstring normalized_verb(std::wstring_view value) {
  std::wstring normalized{value};
  std::ranges::transform(normalized, normalized.begin(), [](wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  return normalized;
}

std::expected<void, std::string> validate_configuration(
    const RuntimeConfiguration& configuration) {
  const auto executable = configuration.executable.native();
  if (executable.empty() || !configuration.executable.is_absolute() ||
      executable.size() >= kMaximumCommandLineCharacters ||
      contains_forbidden_character(executable)) {
    return std::unexpected{"shell configuration has an invalid executable"};
  }
  if (!valid_label(configuration.menu_label)) {
    return std::unexpected{"shell configuration has an invalid menu label"};
  }
  if (configuration.commands.empty() ||
      configuration.commands.size() > kMaximumCommands) {
    return std::unexpected{"shell configuration command count is invalid"};
  }

  std::set<std::wstring> verbs;
  std::set<std::wstring> groups;
  std::wstring active_group;
  bool grouped_commands_started = false;
  for (const auto& command : configuration.commands) {
    const bool grouped = !command.group_label.empty() ||
                         !command.group_canonical_verb.empty();
    if (grouped != (!command.group_label.empty() &&
                    !command.group_canonical_verb.empty()) ||
        !valid_label(command.label) || !valid_verb(command.canonical_verb) ||
        (grouped && (!valid_label(command.group_label) ||
                     !valid_verb(command.group_canonical_verb))) ||
        command.arguments.size() > kMaximumArgumentsPerCommand) {
      return std::unexpected{"shell configuration contains an invalid command"};
    }
    if (!verbs.insert(normalized_verb(command.canonical_verb)).second) {
      return std::unexpected{"shell configuration contains duplicate verbs"};
    }
    if (!grouped) {
      if (grouped_commands_started) {
        return std::unexpected{
            "ungrouped shell commands must precede grouped commands"};
      }
    } else {
      grouped_commands_started = true;
      const auto normalized = normalized_verb(command.group_canonical_verb);
      if (normalized != active_group) {
        if (!groups.insert(normalized).second) {
          return std::unexpected{
              "shell configuration command groups must be contiguous"};
        }
        active_group = normalized;
      }
    }
    for (const auto& argument : command.arguments) {
      if (argument.size() > 8192 || contains_forbidden_character(argument)) {
        return std::unexpected{
            "shell configuration contains an invalid argument"};
      }
    }
  }
  return {};
}

void append_argument(std::wstring& command_line, std::wstring_view argument,
                     bool force_quotes = false) {
  command_line.push_back(L' ');
  command_line += quote_windows_argument(argument, force_quotes);
}

MenuCommand make_default_command(std::wstring label, std::wstring verb,
                                 std::wstring format) {
  return MenuCommand{
      .label = std::move(label),
      .canonical_verb = std::move(verb),
      .arguments = {L"--shell-window", L"--shell-convert", L"--format",
                    std::move(format), L"--collision", L"number"}};
}

std::expected<std::size_t, std::string> parse_count(
    std::wstring_view value, std::size_t maximum) {
  if (value.empty()) return std::unexpected{"empty configuration count"};
  std::size_t result = 0;
  for (const wchar_t character : value) {
    if (character < L'0' || character > L'9') {
      return std::unexpected{"non-decimal configuration count"};
    }
    const std::size_t digit = static_cast<std::size_t>(character - L'0');
    if (result > (maximum - digit) / 10) {
      return std::unexpected{"configuration count is too large"};
    }
    result = result * 10 + digit;
  }
  if (result > maximum) {
    return std::unexpected{"configuration count exceeds its limit"};
  }
  return result;
}

}  // namespace

std::span<const std::wstring_view> supported_extensions() noexcept {
  return kSupportedExtensions;
}

bool is_supported_extension(std::wstring_view extension) noexcept {
  if (extension.empty() || extension.size() > 16) return false;
  std::array<wchar_t, 17> normalized{};
  std::ranges::transform(extension, normalized.begin(), [](wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  const std::wstring_view value{normalized.data(), extension.size()};
  return std::ranges::find(kSupportedExtensions, value) !=
         kSupportedExtensions.end();
}

bool is_supported_selection(
    std::span<const SelectionItem> selection) noexcept {
  if (selection.empty() || selection.front().path.empty()) return false;
  const bool directories = selection.front().is_directory;
  return std::ranges::all_of(selection, [directories](const SelectionItem& item) {
    if (item.path.empty() || item.is_directory != directories) return false;
    return directories || is_supported_extension(item.path.extension().native());
  });
}

std::wstring quote_windows_argument(std::wstring_view argument,
                                    bool force_quotes) {
  const bool needs_quotes =
      force_quotes || argument.empty() ||
      argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) return std::wstring{argument};

  std::wstring result;
  result.reserve(argument.size() + 2);
  result.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
    } else {
      result.append(backslashes, L'\\');
      result.push_back(character);
    }
    backslashes = 0;
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::expected<std::wstring, std::string> build_awj_command_line(
    const std::filesystem::path& executable,
    std::span<const std::wstring> arguments,
    std::span<const SelectionItem> selection) {
  const auto executable_text = executable.native();
  if (executable_text.empty() || !executable.is_absolute() ||
      contains_forbidden_character(executable_text)) {
    return std::unexpected{"AWJ executable path is not a valid absolute path"};
  }
  if (!is_supported_selection(selection)) {
    return std::unexpected{"shell selection contains an unsupported item"};
  }
  for (const auto& argument : arguments) {
    if (argument.size() >= kMaximumCommandLineCharacters ||
        contains_forbidden_character(argument)) {
      return std::unexpected{"configured AWJ argument is invalid"};
    }
  }

  auto command_line = quote_windows_argument(executable_text, true);
  for (const auto& argument : arguments) append_argument(command_line, argument);
  append_argument(command_line, L"-i");
  for (const auto& item : selection) {
    append_argument(command_line, item.path.native(), true);
  }
  if (command_line.size() + 1 > kMaximumCommandLineCharacters) {
    return std::unexpected{"AWJ command line exceeds the CreateProcessW limit"};
  }
  return command_line;
}

std::expected<std::vector<std::wstring>, std::string> encode_configuration(
    const RuntimeConfiguration& configuration) {
  if (auto valid = validate_configuration(configuration); !valid) {
    return std::unexpected{valid.error()};
  }
  std::vector<std::wstring> encoded;
  encoded.reserve(5 + configuration.commands.size() * 8);
  encoded.emplace_back(configuration_magic);
  encoded.push_back(std::to_wstring(configuration_version));
  encoded.push_back(configuration.executable.native());
  encoded.push_back(configuration.menu_label);
  encoded.push_back(std::to_wstring(configuration.commands.size()));
  std::size_t characters = 0;
  for (const auto& command : configuration.commands) {
    const bool grouped = !command.group_label.empty();
    encoded.push_back(grouped ? L"1" : L"0");
    if (grouped) {
      encoded.push_back(command.group_label);
      encoded.push_back(command.group_canonical_verb);
    }
    encoded.push_back(command.label);
    encoded.push_back(command.canonical_verb);
    encoded.push_back(std::to_wstring(command.arguments.size()));
    encoded.insert(encoded.end(), command.arguments.begin(),
                   command.arguments.end());
  }
  for (const auto& value : encoded) {
    characters += value.size() + 1;
    if (characters > kMaximumConfigurationCharacters) {
      return std::unexpected{"shell configuration is too large"};
    }
  }
  return encoded;
}

std::expected<RuntimeConfiguration, std::string> decode_configuration(
    std::span<const std::wstring> encoded) {
  std::size_t cursor = 0;
  const auto take = [&]() -> std::expected<std::wstring, std::string> {
    if (cursor >= encoded.size()) {
      return std::unexpected{"shell configuration ended unexpectedly"};
    }
    return encoded[cursor++];
  };

  auto magic = take();
  auto version = take();
  auto executable = take();
  auto menu_label = take();
  auto command_count_text = take();
  if (!magic || !version || !executable || !menu_label ||
      !command_count_text || *magic != configuration_magic) {
    return std::unexpected{"shell configuration header is invalid"};
  }
  auto version_number = parse_count(*version,
                                    std::numeric_limits<std::uint32_t>::max());
  if (!version_number || *version_number != configuration_version) {
    return std::unexpected{"shell configuration version is unsupported"};
  }
  auto command_count = parse_count(*command_count_text, kMaximumCommands);
  if (!command_count || *command_count == 0) {
    return std::unexpected{"shell configuration has no commands"};
  }

  RuntimeConfiguration configuration{
      .executable = std::filesystem::path{std::move(*executable)},
      .menu_label = std::move(*menu_label)};
  configuration.commands.reserve(*command_count);
  for (std::size_t index = 0; index < *command_count; ++index) {
    auto grouped_text = take();
    if (!grouped_text || (*grouped_text != L"0" && *grouped_text != L"1")) {
      return std::unexpected{"shell command group flag is invalid"};
    }
    MenuCommand command;
    if (*grouped_text == L"1") {
      auto label = take();
      auto verb = take();
      if (!label || !verb) {
        return std::unexpected{"shell command group is incomplete"};
      }
      command.group_label = std::move(*label);
      command.group_canonical_verb = std::move(*verb);
    }
    auto label = take();
    auto verb = take();
    auto argument_count_text = take();
    if (!label || !verb || !argument_count_text) {
      return std::unexpected{"shell command record is incomplete"};
    }
    auto argument_count =
        parse_count(*argument_count_text, kMaximumArgumentsPerCommand);
    if (!argument_count) return std::unexpected{argument_count.error()};
    command.label = std::move(*label);
    command.canonical_verb = std::move(*verb);
    command.arguments.reserve(*argument_count);
    for (std::size_t argument = 0; argument < *argument_count; ++argument) {
      auto value = take();
      if (!value) return std::unexpected{value.error()};
      command.arguments.push_back(std::move(*value));
    }
    configuration.commands.push_back(std::move(command));
  }
  if (cursor != encoded.size()) {
    return std::unexpected{"shell configuration contains trailing records"};
  }
  if (auto valid = validate_configuration(configuration); !valid) {
    return std::unexpected{valid.error()};
  }
  return configuration;
}

std::vector<MenuCommand> default_menu_commands() {
  std::vector<MenuCommand> commands;
  commands.reserve(5);
  commands.push_back(make_default_command(
      L"转换为 PNG", L"AWJimage.Convert.10.png", L"png"));
  commands.push_back(make_default_command(
      L"转换为 WebP", L"AWJimage.Convert.20.webp", L"webp"));
  commands.push_back(make_default_command(
      L"转换为 AVIF", L"AWJimage.Convert.30.avif", L"avif"));
  commands.push_back(make_default_command(
      L"转换为 JXL", L"AWJimage.Convert.50.jxl", L"jxl"));
  commands.push_back(make_default_command(
      L"转换为 JPGLI", L"AWJimage.Convert.60.jpgli", L"jpgli"));
  return commands;
}

}  // namespace awj::shell_extension
