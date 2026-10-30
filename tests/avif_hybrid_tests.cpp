#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <avif/avif.h>
#include "../src/codecs/avif_primary_item_adapter.h"

import awj.animation;
import awj.animation_decoder;
import awj.avif_aom_codec;
import awj.codec;
import awj.image;
import awj.large_image_plan;

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;
using Image = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
using Encoder = std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)>;
using Decoder = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;
constexpr std::uint32_t track_width = 8, track_height = 6;
constexpr std::uint32_t primary_width = 12, primary_height = 8;
constexpr std::uint64_t finite_budget = 64ull * 1024 * 1024;
struct Color { std::uint8_t r, g, b; };
constexpr Color red{255, 0, 0}, blue{0, 0, 255}, green{0, 255, 0};

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}
void avif_ok(avifResult result, std::string_view operation) {
  if (result != AVIF_RESULT_OK)
    throw std::runtime_error(std::string(operation) + ": " + avifResultToString(result));
}
template<class T>
T& value(std::expected<T, std::string>& result, std::string_view operation) {
  if (!result) throw std::runtime_error(std::string(operation) + ": " + result.error());
  return *result;
}
template<class T>
void rejected(const std::expected<T, std::string>& result, std::string_view operation) {
  require(!result, std::string(operation) + " silently accepted an invalid primary/association");
  require(!result.error().empty(), std::string(operation) + " omitted its error");
}

Image solid(Color color, std::uint32_t width, std::uint32_t height) {
  Image image{avifImageCreate(width, height, 8, AVIF_PIXEL_FORMAT_YUV444), avifImageDestroy};
  require(bool(image), "avifImageCreate failed");
  image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
  image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
  image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  image->yuvRange = AVIF_RANGE_FULL;
  avif_ok(avifImageAllocatePlanes(image.get(), AVIF_PLANES_YUV), "allocate fixture planes");
  // RGB identity AV1 stores G, B, R in the Y, U, V planes, without RGB rounding.
  const std::array<std::uint8_t, 3> channels{color.g, color.b, color.r};
  for (unsigned plane = 0; plane < 3; ++plane)
    for (std::uint32_t y = 0; y < height; ++y)
      std::memset(image->yuvPlanes[plane] + std::size_t(y) * image->yuvRowBytes[plane], channels[plane], width);
  return image;
}
Encoder encoder() {
  Encoder result{avifEncoderCreate(), avifEncoderDestroy};
  require(bool(result), "avifEncoderCreate failed");
  result->codecChoice = AVIF_CODEC_CHOICE_AOM;
  result->quality = AVIF_QUALITY_LOSSLESS;
  result->qualityAlpha = AVIF_QUALITY_LOSSLESS;
  result->speed = 10;
  result->maxThreads = 1;
  result->keyframeInterval = 1;
  result->timescale = 100;
  result->repetitionCount = 2;
  return result;
}
Bytes finish(avifEncoder* enc) {
  struct Output {
    avifRWData data{};
    ~Output() { avifRWDataFree(&data); }
  } output;
  avif_ok(avifEncoderFinish(enc, &output.data), "avifEncoderFinish");
  require(output.data.data && output.data.size, "libavif produced an empty fixture");
  const auto* first = reinterpret_cast<const std::byte*>(output.data.data);
  return Bytes(first, first + output.data.size);
}
Bytes encode_still(Color color, std::uint32_t width, std::uint32_t height) {
  auto enc = encoder();
  auto image = solid(color, width, height);
  avif_ok(avifEncoderAddImage(enc.get(), image.get(), 1, AVIF_ADD_IMAGE_FLAG_SINGLE), "encode real still");
  return finish(enc.get());
}
Bytes encode_sequence() {
  auto enc = encoder();
  auto first = solid(blue, track_width, track_height);
  auto second = solid(green, track_width, track_height);
  avif_ok(avifEncoderAddImage(enc.get(), first.get(), 10, AVIF_ADD_IMAGE_FLAG_FORCE_KEYFRAME), "encode blue sequence frame");
  avif_ok(avifEncoderAddImage(enc.get(), second.get(), 20, AVIF_ADD_IMAGE_FLAG_FORCE_KEYFRAME), "encode green sequence frame");
  return finish(enc.get());
}
Bytes encode_grid() {
  auto enc = encoder();
  auto left = solid(red, 64, 64), right = solid(red, 64, 64);
  const std::array<const avifImage*, 2> cells{left.get(), right.get()};
  avif_ok(avifEncoderAddImageGrid(enc.get(), 2, 1, cells.data(), AVIF_ADD_IMAGE_FLAG_SINGLE), "encode real grid");
  return finish(enc.get());
}

// The following bounded BMFF helpers are fixture construction ONLY. Inputs are
// the tiny files just encoded above by the pinned libavif, never user files.
// They relocate existing coded payloads; no AV1 packet or pixel is synthesized.
// Production classification must continue to use libavif's parsed structures.
std::uint64_t read_be(std::span<const std::byte> bytes, std::size_t offset, unsigned width) {
  require(width <= 8 && offset <= bytes.size() && width <= bytes.size() - offset, "fixture integer out of bounds");
  std::uint64_t result = 0;
  for (unsigned i = 0; i < width; ++i) result = (result << 8) | std::to_integer<unsigned>(bytes[offset + i]);
  return result;
}
std::uint64_t take(std::span<const std::byte> bytes, std::size_t& offset, unsigned width) {
  const auto result = read_be(bytes, offset, width);
  offset += width;
  return result;
}
void append_be(Bytes& bytes, std::uint64_t number, unsigned width) {
  require(width <= 8 && (width == 8 || (number >> (width * 8)) == 0), "fixture integer does not fit");
  for (unsigned i = 0; i < width; ++i) bytes.push_back(std::byte((number >> ((width - i - 1) * 8)) & 255));
}
void set_be(Bytes& bytes, std::size_t offset, std::uint64_t number, unsigned width) {
  require(offset <= bytes.size() && width <= bytes.size() - offset, "fixture integer patch out of bounds");
  Bytes encoded; append_be(encoded, number, width);
  std::copy(encoded.begin(), encoded.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}
struct Atom {
  std::string type;
  Bytes payload;
  std::size_t origin_begin{}, origin_payload{};
};
std::vector<Atom> atoms(std::span<const std::byte> bytes) {
  require(bytes.size() <= 1024 * 1024, "unexpectedly large generated fixture");
  std::vector<Atom> result;
  for (std::size_t offset = 0; offset < bytes.size();) {
    require(result.size() < 32 && bytes.size() - offset >= 8, "unexpected generated box structure");
    auto size = read_be(bytes, offset, 4);
    const std::size_t header = size == 1 ? 16 : 8;
    if (size == 1) size = read_be(bytes, offset + 8, 8);
    require(size >= header && size <= bytes.size() - offset, "generated box length is invalid");
    Atom atom{.type = std::string(reinterpret_cast<const char*>(bytes.data() + offset + 4), 4),
              .origin_begin = offset, .origin_payload = offset + header};
    const auto payload = bytes.subspan(offset + header, static_cast<std::size_t>(size) - header);
    atom.payload.assign(payload.begin(), payload.end());
    result.push_back(std::move(atom));
    offset += static_cast<std::size_t>(size);
  }
  return result;
}
Atom* optional_atom(std::vector<Atom>& list, std::string_view type) {
  Atom* found = nullptr;
  for (auto& atom : list) if (atom.type == type) {
    require(!found, std::string("duplicate generated box: ") + std::string(type));
    found = &atom;
  }
  return found;
}
Atom& one(std::vector<Atom>& list, std::string_view type) {
  auto* found = optional_atom(list, type);
  require(found != nullptr, std::string("generated box missing: ") + std::string(type));
  return *found;
}
Bytes pack(const Atom& atom) {
  require(atom.type.size() == 4 && atom.payload.size() <= UINT32_MAX - 8, "fixture box cannot be serialized");
  Bytes bytes;
  append_be(bytes, atom.payload.size() + 8, 4);
  for (char c : atom.type) bytes.push_back(std::byte(static_cast<unsigned char>(c)));
  bytes.insert(bytes.end(), atom.payload.begin(), atom.payload.end());
  return bytes;
}
Bytes pack(const std::vector<Atom>& list) {
  Bytes bytes;
  for (const auto& atom : list) {
    const auto encoded = pack(atom);
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
  }
  return bytes;
}
struct Extent { std::uint64_t offset{}, size{}; };
struct Location {
  std::uint32_t id{};
  unsigned method{};
  std::vector<Extent> extents;
};
std::vector<Location> locations(std::span<const std::byte> bytes) {
  const auto version = read_be(bytes, 0, 1);
  require(version <= 2 && (read_be(bytes, 0, 4) & 0xffffff) == 0, "generated iloc version/flags changed");
  const unsigned offset_width = static_cast<unsigned>(read_be(bytes, 4, 1) >> 4);
  const unsigned length_width = static_cast<unsigned>(read_be(bytes, 4, 1) & 15);
  const unsigned base_width = static_cast<unsigned>(read_be(bytes, 5, 1) >> 4);
  const unsigned index_width = version ? static_cast<unsigned>(read_be(bytes, 5, 1) & 15) : 0;
  for (unsigned width : {offset_width, length_width, base_width, index_width})
    require(width == 0 || width == 4 || width == 8, "generated iloc integer width changed");
  std::size_t cursor = 6;
  const auto count = take(bytes, cursor, version < 2 ? 2 : 4);
  require(count && count <= 16, "unexpected generated item count");
  std::vector<Location> result;
  for (std::uint64_t i = 0; i < count; ++i) {
    Location item{.id = static_cast<std::uint32_t>(take(bytes, cursor, version < 2 ? 2 : 4))};
    item.method = version ? static_cast<unsigned>(take(bytes, cursor, 2)) : 0;
    require(item.id && item.method <= 1 && take(bytes, cursor, 2) == 0, "unexpected generated item reference/storage");
    const auto base = take(bytes, cursor, base_width);
    const auto extent_count = take(bytes, cursor, 2);
    require(extent_count && extent_count <= 16, "unexpected generated extent count");
    for (std::uint64_t j = 0; j < extent_count; ++j) {
      if (version && index_width) require(take(bytes, cursor, index_width) == 0, "generated item-offset reference unsupported");
      const auto offset = take(bytes, cursor, offset_width), length = take(bytes, cursor, length_width);
      require(offset <= UINT64_MAX - base, "generated iloc offset overflow");
      item.extents.push_back({base + offset, length});
    }
    result.push_back(std::move(item));
  }
  require(cursor == bytes.size(), "unexpected generated iloc trailing fields");
  return result;
}
Bytes pack_locations(const std::vector<Location>& list) {
  // Normalize only these test fixtures to v2, 64-bit offset/length, no base/index.
  Bytes bytes{std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x88}, std::byte{0}};
  append_be(bytes, list.size(), 4);
  for (const auto& item : list) {
    append_be(bytes, item.id, 4); append_be(bytes, item.method, 2); append_be(bytes, 0, 2);
    append_be(bytes, item.extents.size(), 2);
    for (const auto& extent : item.extents) { append_be(bytes, extent.offset, 8); append_be(bytes, extent.size, 8); }
  }
  return bytes;
}
struct Parts {
  Atom ftyp, media;
  std::vector<Atom> meta;
  std::vector<Location> items;
  std::uint32_t primary_id{};
  std::optional<Atom> movie;
};
Parts split(const Bytes& bytes) {
  auto root = atoms(bytes);
  Parts result{.ftyp = one(root, "ftyp"), .media = one(root, "mdat")};
  const auto& meta = one(root, "meta");
  require(read_be(meta.payload, 0, 4) == 0, "generated meta version/flags changed");
  result.meta = atoms(std::span(meta.payload).subspan(4));
  const auto& pitm = one(result.meta, "pitm").payload;
  const auto version = read_be(pitm, 0, 1);
  require(version <= 1 && pitm.size() == (version ? 8u : 6u), "generated pitm changed");
  result.primary_id = static_cast<std::uint32_t>(read_be(pitm, 4, version ? 4 : 2));
  result.items = locations(one(result.meta, "iloc").payload);
  if (auto* movie = optional_atom(root, "moov")) result.movie = *movie;
  return result;
}
Location& primary(Parts& file) {
  Location* found = nullptr;
  for (auto& item : file.items) if (item.id == file.primary_id) { require(!found, "duplicate primary location"); found = &item; }
  require(found != nullptr, "generated primary location missing");
  return *found;
}
Atom meta_atom(Parts& file) {
  one(file.meta, "iloc").payload = pack_locations(file.items);
  Bytes payload(4, std::byte{0});
  const auto children = pack(file.meta);
  payload.insert(payload.end(), children.begin(), children.end());
  return Atom{.type = "meta", .payload = std::move(payload)};
}
void walk_movie(Atom& atom, const std::function<void(Atom&)>& visit, unsigned depth = 0) {
  require(depth < 8, "unexpected generated movie nesting");
  visit(atom);
  if (atom.type != "moov" && atom.type != "trak" && atom.type != "mdia" && atom.type != "minf" && atom.type != "stbl") return;
  auto children = atoms(atom.payload);
  for (auto& child : children) walk_movie(child, visit, depth + 1);
  atom.payload = pack(children);
}
std::uint64_t relocate(std::uint64_t offset, std::uint64_t size, const Atom& media, std::uint64_t new_begin) {
  require(offset >= media.origin_payload, "generated extent precedes mdat");
  const auto relative = offset - media.origin_payload;
  require(relative <= media.payload.size() && size <= media.payload.size() - relative && relative <= UINT64_MAX - new_begin,
          "generated extent is outside its owned mdat");
  return new_begin + relative;
}
Bytes compose(Parts still, const Parts* sequence = nullptr, bool same_media = false) {
  require(!same_media || (sequence && still.media.payload == sequence->media.payload &&
          still.media.origin_payload == sequence->media.origin_payload), "shared fixture media mismatch");
  auto ftyp = sequence ? sequence->ftyp : still.ftyp;
  std::optional<Atom> movie;
  if (sequence) {
    require(sequence->movie.has_value(), "generated sequence has no movie");
    movie = sequence->movie;
    walk_movie(*movie, [](Atom&) {}); // Normalize container headers before calculating offsets.
  }
  const auto initial_meta = meta_atom(still);
  const auto media_begin = pack(ftyp).size() + pack(initial_meta).size() + (movie ? pack(*movie).size() : 0) + 8;
  const auto primary_begin = media_begin + (sequence && !same_media ? sequence->media.payload.size() : 0);
  for (auto& item : still.items) if (!item.method)
    for (auto& extent : item.extents) extent.offset = relocate(extent.offset, extent.size, still.media, primary_begin);
  if (movie) walk_movie(*movie, [&](Atom& atom) {
    if (atom.type != "stco" && atom.type != "co64") return;
    require(read_be(atom.payload, 0, 4) == 0, "generated chunk table version/flags changed");
    const auto count = read_be(atom.payload, 4, 4);
    const unsigned width = atom.type == "co64" ? 8 : 4;
    require(count && count <= 16 && atom.payload.size() == 8 + count * width, "generated chunk table changed");
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto position = 8 + static_cast<std::size_t>(i) * width;
      set_be(atom.payload, position, relocate(read_be(atom.payload, position, width), 1, sequence->media, media_begin), width);
    }
  });
  std::vector<Atom> root{std::move(ftyp), meta_atom(still)};
  if (movie) root.push_back(std::move(*movie));
  Bytes media = sequence ? sequence->media.payload : Bytes{};
  if (!sequence || !same_media) media.insert(media.end(), still.media.payload.begin(), still.media.payload.end());
  root.push_back(Atom{.type = "mdat", .payload = std::move(media)});
  auto result = pack(root);
  auto check = atoms(result);
  require(one(check, "mdat").origin_payload == media_begin, "fixture relocation changed container lengths");
  return result;
}
void store_primary_in_idat(Parts& file) {
  auto& item = primary(file);
  Bytes data;
  for (const auto& extent : item.extents) {
    const auto* old_idat = optional_atom(file.meta, "idat");
    if (item.method) require(old_idat != nullptr, "generated idat missing");
    const auto& source = item.method ? old_idat->payload : file.media.payload;
    const auto base = item.method ? 0 : file.media.origin_payload;
    require(extent.offset >= base, "generated primary extent precedes its storage");
    const auto offset = extent.offset - base;
    require(offset <= source.size() && extent.size <= source.size() - offset, "generated primary extent exceeds its storage");
    const auto part = std::span(source).subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(extent.size));
    data.insert(data.end(), part.begin(), part.end());
  }
  auto* idat = optional_atom(file.meta, "idat");
  if (!idat) { file.meta.push_back(Atom{.type = "idat"}); idat = &file.meta.back(); }
  const auto offset = idat->payload.size();
  idat->payload.insert(idat->payload.end(), data.begin(), data.end());
  item.method = 1;
  item.extents = {{offset, data.size()}};
}
Bytes pure_sequence(Bytes bytes) {
  auto root = atoms(bytes);
  const auto& ftyp = one(root, "ftyp");
  require(ftyp.payload.size() >= 8 && (ftyp.payload.size() - 8) % 4 == 0 &&
          std::memcmp(ftyp.payload.data(), "avis", 4) == 0, "generated sequence brand changed");
  // Keep all file offsets unchanged, including an extended-size mdat header.
  // With no avif brand/meta, this is a timed sequence without any primary item.
  for (std::size_t i = 8; i < ftyp.payload.size(); i += 4)
    if (std::memcmp(ftyp.payload.data() + i, "avif", 4) == 0)
      std::memcpy(bytes.data() + ftyp.origin_payload + i, "msf1", 4);
  const auto& meta = one(root, "meta");
  std::memcpy(bytes.data() + meta.origin_begin + 4, "free", 4);
  return bytes;
}
Bytes mutate_primary(Bytes bytes, const std::function<void(Location&)>& mutate) {
  auto file = split(bytes);
  mutate(primary(file));
  auto root = atoms(bytes);
  one(root, "meta") = meta_atom(file);
  auto result = pack(root);
  auto before = atoms(bytes), after = atoms(result);
  require(result.size() == bytes.size() && one(before, "mdat").origin_payload == one(after, "mdat").origin_payload,
          "same-size fixture mutation moved media");
  return result;
}
Bytes bad_sample_description(Bytes bytes) {
  auto root = atoms(bytes);
  unsigned changed = 0;
  walk_movie(one(root, "moov"), [&](Atom& atom) {
    if (atom.type != "stsc") return;
    require(read_be(atom.payload, 4, 4) != 0 && atom.payload.size() >= 20, "generated stsc is missing");
    set_be(atom.payload, 16, 0, 4); // Invalid 1-based sample_description_index, ignored by upstream Fill().
    ++changed;
  });
  require(changed == 1, "unexpected generated track count");
  auto result = pack(root);
  require(result.size() == bytes.size(), "stsc fixture mutation moved media");
  return result;
}

Decoder parsed(const Bytes& bytes, avifDecoderSource source = AVIF_DECODER_SOURCE_AUTO) {
  Decoder decoder{avifDecoderCreate(), avifDecoderDestroy};
  require(bool(decoder), "avifDecoderCreate failed");
  decoder->maxThreads = 1;
  decoder->imageCountLimit = 32;
  avif_ok(avifDecoderSetSource(decoder.get(), source), "set fixture decoder source");
  avif_ok(avifDecoderSetIOMemory(decoder.get(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), "set fixture memory");
  avif_ok(avifDecoderParse(decoder.get()), "parse real fixture");
  return decoder; // Borrows bytes, which each caller keeps alive through decoder destruction.
}
avifExtent frame_extent(const Bytes& bytes, unsigned frame) {
  auto decoder = parsed(bytes, AVIF_DECODER_SOURCE_TRACKS);
  require(decoder->imageCount == 2 && frame < 2, "unexpected generated sequence sample count");
  // NthImageMaxExtent is a dependency envelope, NOT the individual sample.
  // In particular, without stss this pinned decoder marks only frame zero sync,
  // so its frame-one envelope also includes the preceding blue packet. Read the
  // exact sample positions from our owned two-sample, one-chunk fixture instead.
  auto file = split(bytes);
  require(file.movie.has_value(), "generated sequence has no movie");
  std::optional<std::uint64_t> chunk_offset;
  std::vector<std::uint64_t> sample_sizes;
  unsigned mappings = 0;
  walk_movie(*file.movie, [&](Atom& atom) {
    if (atom.type == "stco" || atom.type == "co64") {
      const unsigned width = atom.type == "co64" ? 8 : 4;
      require(!chunk_offset && read_be(atom.payload, 4, 4) == 1 && atom.payload.size() == 8 + width,
              "fixture sample helper requires exactly one chunk");
      chunk_offset = read_be(atom.payload, 8, width);
    } else if (atom.type == "stsc") {
      require(!mappings && atom.payload.size() == 20 && read_be(atom.payload, 4, 4) == 1 &&
              read_be(atom.payload, 8, 4) == 1 && read_be(atom.payload, 12, 4) == 2 && read_be(atom.payload, 16, 4) == 1,
              "fixture sample helper requires one two-sample color chunk");
      ++mappings;
    } else if (atom.type == "stsz") {
      const auto all_samples_size = read_be(atom.payload, 4, 4);
      require(sample_sizes.empty() && read_be(atom.payload, 8, 4) == 2 &&
              atom.payload.size() == (all_samples_size ? 12u : 20u), "generated sample sizes changed");
      for (unsigned i = 0; i < 2; ++i)
        sample_sizes.push_back(all_samples_size ? all_samples_size : read_be(atom.payload, 12 + i * 4, 4));
    }
  });
  require(chunk_offset && mappings == 1 && sample_sizes.size() == 2, "generated sample tables missing");
  auto offset = *chunk_offset;
  for (unsigned i = 0; i < frame; ++i) {
    require(offset <= bytes.size() && sample_sizes[i] <= bytes.size() - offset, "generated prior sample exceeds file");
    offset += sample_sizes[i];
  }
  const auto size = sample_sizes[frame];
  require(size > 1 && offset <= bytes.size() && size <= bytes.size() - offset, "generated sample extent is invalid");
  offset = relocate(offset, size, file.media, file.media.origin_payload);
  return avifExtent{.offset = offset, .size = static_cast<std::size_t>(size)};
}
void pixels(std::span<const std::byte> bytes, std::size_t stride, unsigned width, unsigned height, Color expected) {
  require(stride >= std::size_t(width) * 4 && bytes.size() >= stride * height, "decoded RGBA storage invalid");
  const std::array<unsigned, 4> wanted{expected.r, expected.g, expected.b, 255};
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) for (unsigned c = 0; c < 4; ++c) {
    const auto sample = std::to_integer<unsigned>(bytes[std::size_t(y) * stride + x * 4 + c]);
    const auto delta = sample > wanted[c] ? sample - wanted[c] : wanted[c] - sample;
    require(delta <= 1, "decoded pixels came from the wrong presentation/color");
  }
}
void pixels(const awj::ImageBuffer& image, unsigned width, unsigned height, Color expected) {
  require(image.width == width && image.height == height && image.pixel_format == awj::PixelFormat::rgba &&
          image.bit_depth == 8 && image.sample_representation == awj::SampleRepresentation::unorm && !image.planes.empty(),
          "factory selected the wrong dimensions/representation");
  pixels(image.planes.front().bytes, image.planes.front().stride, width, height, expected);
}
void pixels(const avifImage& image, unsigned width, unsigned height, Color expected) {
  require(image.width == width && image.height == height && image.depth == 8, "raw decoder dimensions/depth changed");
  avifRGBImage rgb{};
  avifRGBImageSetDefaults(&rgb, &image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;
  rgb.rowBytes = width * 4;
  Bytes rgba(std::size_t(rgb.rowBytes) * height);
  rgb.pixels = reinterpret_cast<std::uint8_t*>(rgba.data());
  avif_ok(avifImageYUVToRGB(&image, &rgb), "convert decoded real AV1 pixels");
  pixels(rgba, rgb.rowBytes, width, height, expected);
}
void verify_tracks(const Bytes& bytes) {
  auto decoder = parsed(bytes, AVIF_DECODER_SOURCE_TRACKS);
  require(decoder->imageSequenceTrackPresent && decoder->imageCount == 2, "fixture lost its real sequence track");
  for (Color color : {blue, green}) {
    avif_ok(avifDecoderNextImage(decoder.get()), "decode real sequence sample");
    pixels(*decoder->image, track_width, track_height, color);
  }
}
void write_fixture(const fs::path& path, const Bytes& bytes) {
  std::ofstream file(path, std::ios::binary);
  require(bool(file), "cannot create generated AVIF fixture");
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file.close();
  require(!file.fail(), "cannot finish generated AVIF fixture");
}
void verify_factory(const fs::path& path, const Bytes& bytes, unsigned width, unsigned height, Color color) {
  auto decoder = awj::make_avif_image_decoder(1);
  require(bool(decoder), "factory omitted AVIF decoder");
  auto dimensions = decoder->probe_dimensions(path);
  const auto& size = value(dimensions, "factory probe_dimensions");
  require(size.width == width && size.height == height, "factory probe selected sequence dimensions instead of primary");
  auto file = decoder->decode(path);
  pixels(value(file, "factory file decode").image, width, height, color);
  auto memory = decoder->decode_memory(bytes, "generated AVIF hybrid regression");
  pixels(value(memory, "factory memory decode").image, width, height, color);
}
void verify_still(const fs::path& path, const Bytes& bytes, unsigned width, unsigned height, Color color, bool has_tracks = false) {
  write_fixture(path, bytes);
  auto raw = parsed(bytes);
  require(awjAvifHasIndependentPrimary(raw.get()) == 1, "independent primary was not classified as static");
  avif_ok(avifDecoderSetSource(raw.get(), AVIF_DECODER_SOURCE_PRIMARY_ITEM), "select independent primary");
  avif_ok(avifDecoderNextImage(raw.get()), "decode independent primary");
  pixels(*raw->image, width, height, color);
  if (has_tracks) verify_tracks(bytes);
  verify_factory(path, bytes, width, height, color);
  auto container = awj::parse_avif_container_info(path);
  // 此入口专用于原样直通。混合容器虽选择静态主图，仍不能把附带 motion 一起复制到输出。
  if (has_tracks) rejected(container, "hybrid passthrough must re-encode only the still");
  else {
    const auto& info = value(container, "static container info");
    require(info.width == width && info.height == height, "passthrough probe selected the sequence");
  }
  auto sequence = awj::avif_has_sequence(path);
  require(!value(sequence, "static avif_has_sequence"), "independent primary routed to a sequence");
  auto route = awj::input_uses_animation_reader(path);
  require(!value(route, "static animation route"), "static primary used generic animation routing");
  for (std::uint64_t budget : {std::uint64_t{0}, finite_budget}) {
    auto direct = awj::open_avif_animation(path, budget, {});
    require(!value(direct, "static direct animation open"), "direct animation open discarded the primary");
    auto generic = awj::open_animation(path, budget);
    require(!value(generic, "static generic animation open"), "generic animation open discarded the primary");
  }
}
void verify_reader(awj::AnimationReader& reader) {
  const auto& info = reader.info();
  require(info.width == track_width && info.height == track_height && info.durations.size() == 2 && info.repetitions == 2,
          "sequence reader changed dimensions, frame count or repetitions");
  require(info.durations[0].numerator == 1 && info.durations[0].denominator == 10 &&
          info.durations[1].numerator == 1 && info.durations[1].denominator == 5, "sequence durations changed");
  for (Color color : {blue, green}) {
    auto next = reader.next();
    require(value(next, "read real animation frame"), "sequence ended before its green frame");
    pixels(reader.frame(), track_width, track_height, color);
  }
  auto end = reader.next();
  require(!value(end, "sequence end"), "sequence produced extra frames");
}
void verify_animation(const fs::path& path, const Bytes& bytes, std::optional<Color> cover) {
  write_fixture(path, bytes);
  auto raw = parsed(bytes, AVIF_DECODER_SOURCE_TRACKS);
  require(awjAvifHasIndependentPrimary(raw.get()) == 0, "sequence/shared cover was mistaken for independent static content");
  const auto selected = avifDecoderSetSource(raw.get(), AVIF_DECODER_SOURCE_PRIMARY_ITEM);
  if (cover) {
    avif_ok(selected, "select actual shared cover");
    avif_ok(avifDecoderNextImage(raw.get()), "decode actual shared cover");
    pixels(*raw->image, track_width, track_height, *cover);
  } else require(selected != AVIF_RESULT_OK, "pure sequence unexpectedly retained a primary item");
  verify_tracks(bytes);
  verify_factory(path, bytes, track_width, track_height, blue);
  auto container = awj::parse_avif_container_info(path);
  rejected(container, "sequence passthrough");
  auto sequence = awj::avif_has_sequence(path);
  require(value(sequence, "sequence avif_has_sequence"), "pure animation was routed as a still");
  auto route = awj::input_uses_animation_reader(path);
  require(value(route, "sequence generic route"), "pure animation bypassed the reader");
  for (std::uint64_t budget : {std::uint64_t{0}, finite_budget}) {
    auto direct = awj::open_avif_animation(path, budget, {});
    auto& direct_reader = value(direct, "sequence direct open");
    require(bool(direct_reader), "direct sequence open returned an empty reader");
    verify_reader(*direct_reader);
    auto generic = awj::open_animation(path, budget);
    auto& generic_reader = value(generic, "sequence generic open");
    require(bool(generic_reader), "generic sequence open returned an empty reader");
    verify_reader(*generic_reader);
  }
}
void verify_rejected(const fs::path& path, const Bytes& bytes) {
  write_fixture(path, bytes);
  auto decoder = awj::make_avif_image_decoder(1);
  require(bool(decoder), "factory omitted decoder for rejection test");
  rejected(decoder->probe_dimensions(path), "invalid primary probe");
  rejected(decoder->decode(path), "invalid primary file decode");
  rejected(decoder->decode_memory(bytes, "invalid AVIF primary association"), "invalid primary memory decode");
  rejected(awj::parse_avif_container_info(path), "invalid primary passthrough");
  rejected(awj::avif_has_sequence(path), "invalid primary avif_has_sequence");
  rejected(awj::input_uses_animation_reader(path), "invalid primary generic route");
  for (std::uint64_t budget : {std::uint64_t{0}, finite_budget}) {
    rejected(awj::open_avif_animation(path, budget, {}), "invalid primary direct animation open");
    rejected(awj::open_animation(path, budget), "invalid primary generic animation open");
  }
}
void verify_bad_association(const fs::path& path, const Bytes& bytes) {
  auto raw = parsed(bytes, AVIF_DECODER_SOURCE_TRACKS);
  require(awjAvifHasIndependentPrimary(raw.get()) < 0, "unsafe parsed association was silently treated as animation");
  verify_rejected(path, bytes);
}

void regressions(const fs::path& root) {
  const auto red_still = encode_still(red, primary_width, primary_height);
  const auto blue_still = encode_still(blue, track_width, track_height);
  const auto sequence = encode_sequence();
  auto tracks = split(sequence);
  const auto first_sample = frame_extent(sequence, 0), second_sample = frame_extent(sequence, 1);
  const auto& generated_cover = primary(tracks);
  require(generated_cover.method == 0 && generated_cover.extents.size() == 1 &&
          generated_cover.extents[0].offset == first_sample.offset && generated_cover.extents[0].size == first_sample.size,
          "libavif no longer generated the expected shared coded-sample cover");
  verify_animation(root / "shared-cover.avif", sequence, blue);
  verify_animation(root / "pure-sequence.avifs", pure_sequence(sequence), std::nullopt);

  auto later_cover = tracks;
  primary(later_cover).extents = {{second_sample.offset, second_sample.size}};
  verify_animation(root / "shared-second-sample-cover.avif", compose(later_cover, &tracks, true), green);
  auto fragmented_cover = tracks;
  const auto half = first_sample.size / 2;
  primary(fragmented_cover).extents = {{first_sample.offset, half}, {first_sample.offset + half, first_sample.size - half}};
  verify_animation(root / "shared-contiguous-extents.avif", compose(fragmented_cover, &tracks, true), blue);

  verify_still(root / "pure-static-red.avif", red_still, primary_width, primary_height, red);
  const auto independent = compose(split(red_still), &tracks);
  verify_still(root / "independent-red-blue-green.avif", independent, primary_width, primary_height, red, true);
  // Same visible blue as frame zero, but physically separate coded storage is
  // still independent. No pixel-similarity/author-intent heuristic is acceptable.
  verify_still(root / "independent-identical-blue.avif", compose(split(blue_still), &tracks), track_width, track_height, blue, true);

  const auto hybrid_sample = frame_extent(independent, 0);
  verify_bad_association(root / "partial-sample-overlap.avif", mutate_primary(independent, [&](Location& item) {
    item.extents = {{hybrid_sample.offset + 1, hybrid_sample.size - 1}};
  }));
  verify_bad_association(root / "out-of-file-primary.avif", mutate_primary(independent, [&](Location& item) {
    item.extents.front().offset = independent.size() + 1;
  }));
  verify_bad_association(root / "overflowing-primary.avif", mutate_primary(independent, [](Location& item) {
    item.extents = {{UINT64_MAX - 1, 8}};
  }));
  verify_bad_association(root / "invalid-sample-description.avif", bad_sample_description(independent));

  auto coded_idat = split(red_still);
  store_primary_in_idat(coded_idat);
  verify_still(root / "pure-static-coded-idat.avif", compose(coded_idat), primary_width, primary_height, red);
  verify_bad_association(root / "ambiguous-coded-idat-and-track.avif", compose(coded_idat, &tracks));

  auto grid = split(encode_grid());
  store_primary_in_idat(grid);
  require(primary(grid).method == 1, "grid fixture is not really IDAT-stored");
  verify_still(root / "pure-grid-idat.avif", compose(grid), 128, 64, red);
  verify_still(root / "independent-grid-idat-and-track.avif", compose(grid, &tracks), 128, 64, red, true);
  auto bad_grid = grid;
  const auto& location = primary(bad_grid);
  require(location.extents.size() == 1 && location.extents[0].size >= 8, "generated grid descriptor changed");
  auto& idat = one(bad_grid.meta, "idat").payload;
  const auto descriptor = static_cast<std::size_t>(location.extents[0].offset);
  require(read_be(idat, descriptor, 1) == 0 && read_be(idat, descriptor + 2, 1) == 0 &&
          read_be(idat, descriptor + 3, 1) == 1, "generated 2x1 grid descriptor changed");
  set_be(idat, descriptor + 3, 2, 1); // Claims three columns, but dimg has only two tiles.
  verify_rejected(root / "invalid-pure-grid-idat.avif", compose(bad_grid));
  const auto invalid_grid_hybrid = compose(bad_grid, &tracks);
  auto grid_candidate = parsed(invalid_grid_hybrid, AVIF_DECODER_SOURCE_TRACKS);
  require(awjAvifHasIndependentPrimary(grid_candidate.get()) == 1, "grid candidate was incorrectly discarded before authoritative validation");
  require(avifDecoderSetSource(grid_candidate.get(), AVIF_DECODER_SOURCE_PRIMARY_ITEM) != AVIF_RESULT_OK,
          "PRIMARY_ITEM bypassed strict grid/dimg validation after adapter returned 1");
  verify_rejected(root / "invalid-grid-idat-with-valid-track.avif", invalid_grid_hybrid);
}
} // namespace

int main(int argc, char** argv) {
  fs::path run_root;
  try {
    require(argc == 2 && fs::path(argv[1]).is_absolute(), "Pass one absolute, project-specific results root as argv[1]");
    const fs::path results_root{argv[1]};
    fs::create_directories(results_root);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 32 && run_root.empty(); ++attempt) {
      auto candidate = results_root / ("avif-hybrid-" + std::to_string(stamp) + "-" + std::to_string(attempt));
      std::error_code error;
      if (fs::create_directory(candidate, error)) run_root = std::move(candidate);
      else require(!error || error == std::errc::file_exists, "cannot create isolated AVIF fixture directory");
    }
    require(!run_root.empty(), "cannot reserve a fresh AVIF fixture directory");
    // These small, genuinely encoded inputs are retained under the caller's
    // results root for reproducing failures and tracing the tested container.
    regressions(run_root);
    std::fprintf(stdout, "AVIF hybrid routing and real-pixel regressions passed; fixtures: %s\n", run_root.string().c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "AVIF hybrid regression failed: %s\nFixtures retained at: %s\n", error.what(), run_root.string().c_str());
    return 1;
  }
}
