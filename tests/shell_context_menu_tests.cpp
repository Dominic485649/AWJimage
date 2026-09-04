#include <iostream>
#include <string_view>

#include "shell_context_menu.h"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using awj::ui::shell_command_store_verb_key;
  using awj::ui::shell_command_store_verb_name;
  using awj::ui::shell_context_menu_subcommands;

  if (shell_command_store_verb_name(L"avif") != L"AWJImage.avif") {
    return fail("CommandStore verb name changed unexpectedly.");
  }
  if (shell_command_store_verb_key(L"avif") !=
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\AWJImage.avif") {
    return fail("CommandStore verb registry path changed unexpectedly.");
  }

  constexpr std::wstring_view without_png_suffix =
      L"AWJImage.png;AWJImage.webp;AWJImage.avif;AWJImage.jxl;AWJImage.jpgli";
  constexpr std::wstring_view with_png_suffix =
      L"AWJImage.png;AWJImage.webp;AWJImage.avif;AWJImage.avif-png;AWJImage.jxl;AWJImage.jpgli";
  if (shell_context_menu_subcommands(false) != without_png_suffix ||
      shell_context_menu_subcommands(true) != with_png_suffix) {
    return fail("SubCommands list does not match the enabled formats.");
  }
  return 0;
}
