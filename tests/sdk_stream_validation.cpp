#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
constexpr size_t limit = 8 * 1024 * 1024;
uint64_t little(const uint8_t* data, size_t bytes) {
  uint64_t value{};
  for (size_t i = 0; i < bytes; ++i) value |= uint64_t{data[i]} << (8 * i);
  return value;
}
void put(std::vector<uint8_t>& data, size_t offset, uint64_t value, size_t bytes = 4) {
  for (size_t i = 0; i < bytes; ++i) data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
bool png(const std::vector<uint8_t>& bytes, uint32_t width, uint32_t height) {
  constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  if (bytes.size() < 33 || std::memcmp(bytes.data(), signature.data(), signature.size())) return false;
  auto big = [&](size_t offset) {
    uint32_t value{};
    for (size_t i = 0; i < 4; ++i) value = value * 256 + bytes[offset + i];
    return value;
  };
  return big(8) == 13 && !std::memcmp(bytes.data() + 12, "IHDR", 4) &&
         big(16) == width && big(20) == height && bytes[24] == 8;
}
bool end() { return std::fgetc(stdin) == EOF && !std::ferror(stdin); }
}
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  const std::string_view mode{argv[1]};
  if (mode == "wire" || mode == "truncated") {
    std::vector<uint8_t> wire(176, 255);
    std::memset(wire.data(), 0, 160);
    std::memcpy(wire.data(), "AWJFRM1", 8);
    put(wire, 8, 1); put(wire, 12, 160); put(wire, 16, 2); put(wire, 20, 2);
    put(wire, 24, 1); put(wire, 28, 8); put(wire, 32, 1);
    put(wire, 56, 1); put(wire, 60, 1000); put(wire, 64, 1); put(wire, 68, 13);
    put(wire, 76, 1); put(wire, 80, UINT32_MAX); put(wire, 96, 16, 8);
    put(wire, 104, 8, 8); put(wire, 136, 2);
    if (mode == "truncated") wire.resize(171);
    return std::fwrite(wire.data(), 1, wire.size(), stdout) == wire.size() ? 0 : 1;
  }
  if (mode == "empty") return end() ? 0 : 1;
  if (mode == "multi") {
    for (uint64_t frame = 0; frame < 2; ++frame) {
      std::array<uint8_t, 40> header{};
      if (std::fread(header.data(), 1, header.size(), stdin) != header.size() ||
          std::memcmp(header.data(), "AWJRES1", 8) || little(header.data() + 8, 4) != 1 ||
          little(header.data() + 12, 4) != 40 || little(header.data() + 16, 8) != frame) return 1;
      const auto size = little(header.data() + 32, 8);
      if (!size || size > limit) return 1;
      std::vector<uint8_t> payload(static_cast<size_t>(size));
      if (std::fread(payload.data(), 1, payload.size(), stdin) != payload.size() || !png(payload, 8, 6)) return 1;
    }
    return end() ? 0 : 1;
  }
  if (mode == "png") {
    std::vector<uint8_t> payload;
    std::array<uint8_t, 4096> buffer{};
    for (;;) {
      const auto count = std::fread(buffer.data(), 1, buffer.size(), stdin);
      if (payload.size() > limit - count) return 1;
      payload.insert(payload.end(), buffer.begin(), buffer.begin() + count);
      if (count != buffer.size()) break;
    }
    return !std::ferror(stdin) && png(payload, 2, 2) ? 0 : 1;
  }
  return 1;
}
