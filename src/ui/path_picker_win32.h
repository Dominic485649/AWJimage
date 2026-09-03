#pragma once

#ifdef _WIN32

#include <filesystem>
#include <optional>

namespace awj::ui_path_picker {

std::optional<std::filesystem::path> choose_path(bool pick_folder);

}  // namespace awj::ui_path_picker

#endif
