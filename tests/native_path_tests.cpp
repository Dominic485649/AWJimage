#include "../src/ui/linux_process.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

import awj.core;
import awj.config;

int main(int argc, char** argv) try {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / ("awj-native-path-" + std::to_string(getpid()));
  if (fs::exists(root)) throw std::runtime_error("test directory must be fresh");
  fs::create_directory(root);
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
  for (int byte = 0x80; byte <= 0xff; ++byte) {
    const auto native = std::string{"中文-"} + char(byte) + ".png";
    if (awj::path_from_argument_text(awj::argument_text_from_native(native)).native() != native)
      throw std::runtime_error("native argv byte changed");
  }
  for (const auto native : {std::string{"中文-ÿ.png"}, std::string{"\xed\xb3\xbf.png"}}) {
    if (awj::path_from_argument_text(awj::argument_text_from_native(native)).native() != native)
      throw std::runtime_error("Unicode or surrogate argv changed");
  }
  const auto input = root / (std::string{"中文 folder-"} + char(0xff));
  const auto output = root / (std::string{"output-"} + char(0xfe));
  fs::create_directory(input);
  fs::create_directory(output);
  const auto one = input / (std::string{"image-"} + char(0xff) + ".png");
  const auto two = input / (std::string{"image-"} + char(0xfe) + ".png");
  const auto parsed = awj::parse_arguments({L"--input", awj::argument_text_from_native(one.native()),
      L"--output", awj::argument_text_from_native(output.native())});
  if (!parsed || parsed->config.input_path != one || parsed->config.output_dir != output)
    throw std::runtime_error("native CLI path changed");
  const auto shell = awj::parse_arguments({L"--shell-convert", awj::argument_text_from_native(one.native()),
      awj::argument_text_from_native(two.native())});
  if (!shell || shell->shell_inputs.size() != 2 || shell->shell_inputs[0] != one || shell->shell_inputs[1] != two)
    throw std::runtime_error("native shell paths collapsed");
  std::ofstream(one) << "fixture";
  std::ofstream(two) << "fixture";
  std::ofstream(input / (std::string{"ignored."} + char(0xff))) << "unknown extension";
  const auto typed = awj::normalize_path_argument(L"  \"中文 folder/image.png\"  ", "input");
  if (!typed || typed->native() != "中文 folder/image.png")
    throw std::runtime_error("Unicode typed path changed");
  auto cfg = awj::default_app_config();
  cfg.input_path = input;
  cfg.output_dir = output;
  cfg.output_template = L"{name}";
  std::vector<awj::ImageFile> files;
  auto scanned = awj::scan_images(cfg, files);
  if (!scanned) throw std::runtime_error(scanned.error());
  if (files.size() != 2 || awj::normalized_lower_path_key(one) == awj::normalized_lower_path_key(two))
    throw std::runtime_error("distinct native paths collapsed");
  for (auto& file : files) {
    const auto expected = output / (file.path.stem().native() + ".avif");
    if (awj::output_path_for(cfg, file) != expected) throw std::runtime_error("native output name changed");
    std::ofstream(expected) << "existing";
  }
  cfg.collision_mode = awj::CollisionMode::suffix_number;
  auto resolved = awj::resolve_batch_output_paths(cfg, files);
  if (!resolved) throw std::runtime_error(resolved.error());
  for (const auto& file : files) {
    const auto expected = output / (file.path.stem().native() + "(1).avif");
    if (awj::output_path_for(cfg, file) != expected) throw std::runtime_error("native collision name changed");
  }
  if (argc == 3) {
    const auto cli_input = input / (std::string{"cli-"} + char(0xff) + ".jpg");
    fs::copy_file(argv[2], cli_input);
    const auto result = awj::ui_process::capture({argv[1], "--input", cli_input.native(),
        "--output", output.native(), "--format", "png", "--no-log", "--no-summary"});
    if (!result || !fs::is_regular_file(output / (cli_input.stem().native() + ".png")))
      throw std::runtime_error("native CLI conversion failed");
  }
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
