#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include AWJ_TEST_TURBO_HEADER
#include <ultrahdr_api.h>
#include <avif/avif.h>
#include <lcms2.h>

import awj.avif_aom_codec;
import awj.codec;
import awj.decoder_registry;
import awj.gain_map;
import awj.heif_codec;
import awj.image;
import awj.jpeg_codec;
import awj.large_image_plan;
#if AWJ_HAS_JPEGLI
import awj.jpegli_codec;
#endif

namespace {
using Bytes = std::vector<std::byte>;
using View = std::span<const std::byte>;
namespace fs = std::filesystem;
void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string{message});
}
template<class T> T take(awj::compat::expected<T, std::string> result) {
  if (!result) throw std::runtime_error(result.error());
  return std::move(*result);
}
void append(Bytes& out, View bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }
Bytes text(std::string_view value) {
  const auto* first = reinterpret_cast<const std::byte*>(value.data());
  return {first, first + value.size()};
}
void u16(Bytes& out, unsigned value, bool little = false) {
  for (unsigned i = 0; i < 2; ++i) out.push_back(std::byte((value >> (little ? i * 8 : (1 - i) * 8)) & 255));
}
void u32(Bytes& out, std::uint32_t value, bool little = false) {
  for (unsigned i = 0; i < 4; ++i) out.push_back(std::byte((value >> (little ? i * 8 : (3 - i) * 8)) & 255));
}
std::uint32_t read(View bytes, std::size_t at, unsigned width) {
  require(at <= bytes.size() && width <= bytes.size() - at, "Test fixture is truncated");
  std::uint32_t value = 0;
  for (unsigned i = 0; i < width; ++i) value = (value << 8) | std::to_integer<unsigned>(bytes[at + i]);
  return value;
}
void put(Bytes& bytes, std::size_t at, std::uint32_t value, unsigned width) {
  require(at <= bytes.size() && width <= bytes.size() - at, "Test fixture write is out of bounds");
  for (unsigned i = 0; i < width; ++i) bytes[at + i] = std::byte((value >> ((width - 1 - i) * 8)) & 255);
}
Bytes load(const fs::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  require(bool(input), "Cannot open dependency test fixture");
  const auto count = input.tellg();
  require(count > 0 && count < 16 * 1024 * 1024, "Unexpected dependency test fixture size");
  Bytes bytes(static_cast<std::size_t>(count));
  input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(bool(input), "Cannot read dependency test fixture");
  return bytes;
}
struct Sandbox {
  fs::path path = fs::temp_directory_path() / ("awj-iso-gain-map-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Sandbox() { require(fs::create_directory(path), "ISO test sandbox must be fresh"); }
  ~Sandbox() {
    // 出错时保留项目 TMPDIR 内的必要复现输入，成功时再清理自有唯一目录。
    if (std::uncaught_exceptions() > 0) {
      try { std::fprintf(stderr, "失败测试输入保留：%s\n", path.string().c_str()); } catch (...) {}
      return;
    }
    std::error_code ec; fs::remove_all(path, ec);
  }
  fs::path save(std::string_view name, View bytes) const {
    auto file = path / name;
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(output), "Cannot write ISO test sandbox fixture");
    return file;
  }
};

enum class Outcome { ordinary, iso, rejected };
void check_image(const awj::compat::expected<awj::ImageDecodeResult, std::string>& result, Outcome expected) {
  if (expected == Outcome::rejected) {
    require(!result && result.error().starts_with("Gain Map:"), "HDR must fail terminally, not fall back to SDR");
    require(result.error().find("ISO 21496-1") != std::string::npos, "HDR error must name ISO 21496-1");
    return;
  }
  if (!result) throw std::runtime_error(result.error());
  require(result->image.width && result->image.height && !result->used_fallback, "Image unexpectedly used a fallback");
  const bool enhanced = result->image.source_info && result->image.source_info->source_has_gain_map;
  require(enhanced == (expected == Outcome::iso), "Incorrect ISO enhancement status");
  if (enhanced) {
    require(result->decoder_id.find("iso21496-1") != std::string::npos, "Decoder diagnostic must name ISO21496-1");
    require(result->image.source_info->has_hdr_metadata && result->image.bit_depth == 16, "ISO output lost HDR precision");
  }
}
void check_dimensions(const awj::compat::expected<awj::ImageDimensions, std::string>& dimensions, Outcome expected) {
  if (expected == Outcome::rejected)
    require(!dimensions && dimensions.error().starts_with("Gain Map:") && dimensions.error().find("ISO 21496-1") != std::string::npos,
        "Dimension probe hid or mislabeled a terminal HDR error");
  else require(bool(dimensions), "Valid image dimension probe failed");
}
void check_codec(awj::ImageDecoder& decoder, View bytes, const fs::path& file, Outcome expected) {
  auto memory = decoder.decode_memory(bytes, "ISO protocol fixture", {.copy_metadata_payloads = true});
  auto disk = decoder.decode(file);
  check_image(memory, expected); check_image(disk, expected);
  auto dimensions = decoder.probe_dimensions(file);
  check_dimensions(dimensions, expected);
  if (expected != Outcome::rejected) {
    require(memory->image.width == disk->image.width && memory->image.height == disk->image.height &&
        dimensions->width == disk->image.width && dimensions->height == disk->image.height,
        "File, memory and dimension geometry differ");
  }
}
void check_registry(const fs::path& file, Outcome expected) {
  for (bool fallback : {false, true}) {
    check_image(awj::decode_image_for_path(file, {.allow_wic_fallback = fallback}), expected);
    check_dimensions(awj::probe_image_dimensions_for_path(file, {.allow_wic_fallback = fallback}), expected);
  }
}
void check_jpeg(const Sandbox& sandbox, std::string_view name, View bytes, Outcome expected) {
  const auto file = sandbox.save(name, bytes);
  auto disk = awj::decode_jpeg_gain_map(file);
  auto memory = awj::decode_jpeg_gain_map(bytes);
  auto dimensions = awj::probe_jpeg_gain_map_dimensions(bytes);
  if (expected == Outcome::rejected) {
    require(!disk && !memory && !dimensions && disk.error().starts_with("Gain Map:") &&
        memory.error().starts_with("Gain Map:"), "Gain map gate returned not-my-format for legacy or invalid HDR");
  } else {
    require(disk && memory && dimensions, "Gain map gate failed for a supported image");
    require(bool(*disk) == (expected == Outcome::iso) && bool(*memory) == bool(*disk) &&
        bool(*dimensions) == bool(*disk), "Gain map file/memory gate disagrees");
  }
  awj::JpegImageDecoder turbo;
  check_codec(turbo, bytes, file, expected);
#if AWJ_HAS_JPEGLI
  awj::JpegliImageDecoder jpegli;
  check_codec(jpegli, bytes, file, expected);
#endif
  check_registry(file, expected);
}
Bytes app(Bytes jpeg, unsigned marker, View payload) {
  require(jpeg.size() >= 2 && payload.size() <= 65533, "Invalid test JPEG APP payload");
  Bytes out(jpeg.begin(), jpeg.begin() + 2);
  out.push_back(std::byte{255}); out.push_back(std::byte(marker)); u16(out, static_cast<unsigned>(payload.size() + 2));
  append(out, payload); append(out, View{jpeg}.subspan(2));
  return out;
}
Bytes xmp(std::string_view xml) {
  auto out = text("http://ns.adobe.com/xap/1.0/"); out.push_back(std::byte{0}); append(out, text(xml)); return out;
}
Bytes extended_xmp_piece(std::string_view xml, std::size_t offset, std::size_t count, bool broken = false) {
  require(offset <= xml.size() && count <= xml.size()-offset, "Invalid Extended XMP test range");
  auto packet = text(std::string_view{"http://ns.adobe.com/xmp/extension/\0",35});
  append(packet,text("0123456789ABCDEF0123456789ABCDEF"));
  u32(packet,static_cast<std::uint32_t>(xml.size()));
  u32(packet,static_cast<std::uint32_t>(broken ? xml.size()+1 : offset));
  append(packet,text(xml.substr(offset,count)));
  return packet;
}
Bytes extended_jpeg(Bytes image, std::string_view xml, std::size_t split, bool reverse = false, bool broken = false) {
  require(split && split < xml.size(), "Extended XMP test needs two chunks");
  image = app(std::move(image),0xe1,xmp(R"(<rdf:Description xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns:xmpNote="http://ns.adobe.com/xmp/note/" xmpNote:HasExtendedXMP="0123456789ABCDEF0123456789ABCDEF"/>)"));
  auto first = extended_xmp_piece(xml,0,split);
  auto second = extended_xmp_piece(xml,split,xml.size()-split,broken);
  image = app(std::move(image),0xe1,reverse ? first : second);
  return app(std::move(image),0xe1,reverse ? second : first);
}
Bytes jpeg(unsigned value, unsigned width = 16, unsigned height = 12,
    int y_h_samp_factor = 1, int y_v_samp_factor = 1) {
  jpeg_compress_struct encoder{}; jpeg_error_mgr error{};
  encoder.err = jpeg_std_error(&error); jpeg_create_compress(&encoder);
  unsigned char* memory = nullptr; unsigned long count = 0;
  jpeg_mem_dest(&encoder, &memory, &count);
  encoder.image_width = width; encoder.image_height = height; encoder.input_components = 3; encoder.in_color_space = JCS_RGB;
  jpeg_set_defaults(&encoder); jpeg_set_quality(&encoder, 100, TRUE);
  for (int c = 0; c < 3; ++c) encoder.comp_info[c].h_samp_factor = encoder.comp_info[c].v_samp_factor = 1;
  encoder.comp_info[0].h_samp_factor = y_h_samp_factor;
  encoder.comp_info[0].v_samp_factor = y_v_samp_factor;
  jpeg_start_compress(&encoder, TRUE);
  std::vector<unsigned char> row(width * 3, static_cast<unsigned char>(value));
  while (encoder.next_scanline < height) { JSAMPROW line = row.data(); jpeg_write_scanlines(&encoder, &line, 1); }
  jpeg_finish_compress(&encoder);
  Bytes out(reinterpret_cast<std::byte*>(memory), reinterpret_cast<std::byte*>(memory) + count);
  std::free(memory); jpeg_destroy_compress(&encoder); return out;
}
void ok(uhdr_error_info_t error) {
  require(error.error_code == UHDR_CODEC_OK, error.has_detail ? error.detail : "ISO fixture engine failed");
}
Bytes package_iso(Bytes base, Bytes gain) {
  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_encoder)> encoder(uhdr_create_encoder(), uhdr_release_encoder);
  require(bool(encoder), "Cannot allocate ISO fixture encoder");
  uhdr_compressed_image_t primary{.data = base.data(), .data_sz = base.size(), .capacity = base.size(),
      .cg = UHDR_CG_BT_709, .ct = UHDR_CT_SRGB, .range = UHDR_CR_FULL_RANGE};
  uhdr_compressed_image_t auxiliary{.data = gain.data(), .data_sz = gain.size(), .capacity = gain.size(),
      .cg = UHDR_CG_BT_709, .ct = UHDR_CT_UNSPECIFIED, .range = UHDR_CR_FULL_RANGE};
  uhdr_gainmap_metadata_t metadata{};
  for (int c = 0; c < 3; ++c) { metadata.min_content_boost[c] = 1; metadata.max_content_boost[c] = 4; metadata.gamma[c] = 1; }
  metadata.hdr_capacity_min = 1; metadata.hdr_capacity_max = 4; metadata.use_base_cg = 1;
  ok(uhdr_enc_set_compressed_image(encoder.get(), &primary, UHDR_BASE_IMG));
  ok(uhdr_enc_set_gainmap_image(encoder.get(), &auxiliary, &metadata)); ok(uhdr_encode(encoder.get()));
  auto* encoded = uhdr_get_encoded_stream(encoder.get());
  require(encoded && encoded->data && encoded->data_sz, "ISO fixture encoder returned no stream");
  auto* first = static_cast<std::byte*>(encoded->data); return {first, first + encoded->data_sz};
}
Bytes engine_pixels(View bytes, bool valid) {
  std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)> decoder(uhdr_create_decoder(), uhdr_release_decoder);
  require(bool(decoder), "Cannot allocate ISO fixture decoder");
  uhdr_compressed_image_t input{.data = const_cast<std::byte*>(bytes.data()), .data_sz = bytes.size(), .capacity = bytes.size(),
      .cg = UHDR_CG_UNSPECIFIED, .ct = UHDR_CT_UNSPECIFIED, .range = UHDR_CR_UNSPECIFIED};
  ok(uhdr_dec_set_image(decoder.get(), &input));
  ok(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat));
  ok(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR));
  const auto probed = uhdr_dec_probe(decoder.get());
  if (!valid) {
    require(probed.error_code != UHDR_CODEC_OK && uhdr_decode(decoder.get()).error_code != UHDR_CODEC_OK,
        "Underlying library accepted legacy XMP or corrupt ISO");
    return {};
  }
  ok(probed); ok(uhdr_decode(decoder.get()));
  auto* image = uhdr_get_decoded_image(decoder.get());
  require(image && image->planes[0], "ISO decoder returned no pixels");
  Bytes out;
  for (unsigned row = 0; row < image->h; ++row)
    append(out, {static_cast<std::byte*>(image->planes[0]) + row * image->stride[0] * 8, image->w * 8});
  return out;
}
Bytes damage_iso(Bytes bytes, bool remove_namespace) {
  constexpr std::string_view ns = "urn:iso:std:iso:ts:21496:-1";
  std::size_t pos = 0; unsigned changed = 0;
  for (;;) {
    const std::string_view raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    pos = raw.find(ns, pos);
    if (pos == raw.npos) break;
    if (remove_namespace) bytes[pos] = std::byte{'a'};
    else { require(pos + ns.size() + 1 < bytes.size(), "Truncated generated ISO namespace"); bytes[pos + ns.size() + 1] = std::byte{255}; }
    pos += ns.size(); ++changed;
  }
  require(changed == 2, "Expected primary and auxiliary ISO records"); return bytes;
}
Bytes mpo(Bytes primary, const Bytes& secondary) {
  const auto primary_size = static_cast<std::uint32_t>(primary.size() + 90);
  Bytes packet = text(std::string_view{"MPF\0II", 6}); u16(packet, 42, true); u32(packet, 8, true); u16(packet, 3, true);
  u16(packet, 0xb000, true); u16(packet, 7, true); u32(packet, 4, true); append(packet, text("0100"));
  u16(packet, 0xb001, true); u16(packet, 4, true); u32(packet, 1, true); u32(packet, 2, true);
  u16(packet, 0xb002, true); u16(packet, 7, true); u32(packet, 32, true); u32(packet, 50, true); u32(packet, 0, true);
  u32(packet, 0x20030000, true); u32(packet, primary_size, true); u32(packet, 0, true); u32(packet, 0, true);
  u32(packet, 0, true); u32(packet, static_cast<std::uint32_t>(secondary.size()), true); u32(packet, primary_size - 10, true); u32(packet, 0, true);
  require(packet.size() == 86, "Invalid ordinary MPF test layout");
  auto out = app(std::move(primary), 0xe2, packet); append(out, secondary); return out;
}
constexpr std::string_view adobe = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" hdrgm:Version="1.0" hdrgm:GainMapMax="2" hdrgm:HDRCapacityMax="2"/></rdf:RDF></x:xmpmeta>)";
void extended_xmp_transport_contract() {
  const auto split = adobe.find(" hdrgm:Version");
  auto core = xmp(R"(<rdf:Description xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns:xmpNote="http://ns.adobe.com/xmp/note/" xmpNote:HasExtendedXMP="0123456789ABCDEF0123456789ABCDEF"/>)");
  auto first = extended_xmp_piece(adobe,0,split), second = extended_xmp_piece(adobe,split,adobe.size()-split);
  using Segment = awj::gain_map_detail::JpegXmpSegment;
  std::array<Segment,3> segments{{{core,0},{second,0},{first,0}}};
  auto joined = awj::gain_map_detail::reassemble_extended_xmp(segments);
  require(joined && joined->size() == 1 && joined->front() == text(adobe), "Extended XMP did not reassemble out-of-order chunks");
  put(first,67,0xffffffffu,4);
  require(!awj::gain_map_detail::reassemble_extended_xmp(segments), "Oversized Extended XMP total was accepted");
  put(first,67,static_cast<std::uint32_t>(adobe.size()+1),4);
  require(!awj::gain_map_detail::reassemble_extended_xmp(segments), "Inconsistent Extended XMP totals were accepted");
  put(first,67,static_cast<std::uint32_t>(adobe.size()),4);
  put(second,71,0,4);
  require(!awj::gain_map_detail::reassemble_extended_xmp(segments), "Overlapping Extended XMP chunks were accepted");
  put(second,71,static_cast<std::uint32_t>(split),4);
  require(!awj::gain_map_detail::reassemble_extended_xmp(std::span<const Segment>{segments}.first(2)), "Incomplete Extended XMP was accepted");
}

void jpeg_matrix(const Sandbox& sandbox) {
  extended_xmp_transport_contract();
  auto base = jpeg(128), gain = jpeg(200, 4, 3);
  require(bool(awj::jpeg_detail::validate_saved_metadata_budget_from_memory(base,"vector budget input",true)) &&
      bool(awj::jpeg_detail::validate_saved_metadata_budget_from_memory(View{base},"span budget input",true)),
      "JPEG metadata budget helper must accept both vector and const span");
  check_jpeg(sandbox, "ordinary.jpg", base, Outcome::ordinary);
  const auto ordinary_xml = R"(<rdf:Description xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/" xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmlns:Item="http://ns.google.com/photos/1.0/container/item/" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" xmlns:GainMapMax="urn:not-a-property" xmlns:apple="http://ns.apple.com/HDRGainMap/1.0/" xmlns:HDRGainMapVersion="urn:not-a-property" xmlns:Container="http://ns.google.com/photos/1.0/container/" xmlns:iso="urn:iso:std:iso:ts:21496:-1" crs:Gamma="2.2" xmp:Version="1" Item:Semantic="Primary" dc:title="GainMap"/>)";
  auto ordinary = app(base, 0xe1, xmp(ordinary_xml));
  std::unique_ptr<void, decltype(&cmsCloseProfile)> profile(cmsCreate_sRGBProfile(), cmsCloseProfile);
  cmsUInt32Number size = 0; require(profile && cmsSaveProfileToMem(profile.get(), nullptr, &size), "Cannot create ordinary ICC fixture");
  auto icc = text(std::string_view{"ICC_PROFILE\0\1\1", 14}); const auto at = icc.size(); icc.resize(at + size);
  require(cmsSaveProfileToMem(profile.get(), icc.data() + at, &size), "Cannot serialize ordinary ICC fixture");
  ordinary = app(std::move(ordinary), 0xe2, icc);
  auto exif = text(std::string_view{"Exif\0\0", 6});
  for (unsigned v : {0x49u,0x49u,0x2au,0u,8u,0u,0u,0u,1u,0u,0x12u,1u,3u,0u,1u,0u,0u,0u,1u,0u,0u,0u,0u,0u,0u,0u}) exif.push_back(std::byte(v));
  ordinary = app(std::move(ordinary), 0xe1, exif);
  check_jpeg(sandbox, "ordinary-metadata.jpg", ordinary, Outcome::ordinary);
  auto yuv420 = jpeg(128,16,12,2,2);
  check_jpeg(sandbox,"ordinary-yuv420.jpg",yuv420,Outcome::ordinary);
  auto check_ordinary_header = [&](awj::ImageDecoder& decoder) {
    auto preserved = take(decoder.decode_memory(ordinary, "ordinary metadata", {.copy_metadata_payloads = true}));
    for (auto kind : {awj::MetadataKind::xmp, awj::MetadataKind::icc, awj::MetadataKind::exif})
      require(std::ranges::any_of(preserved.image.metadata, [=](const auto& b) { return b.kind == kind; }), "Ordinary XMP/ICC/Exif was removed");
    for (bool subsampled : {false,true}) {
      const auto& bytes = subsampled ? yuv420 : base;
      const auto file = sandbox.path / (subsampled ? "ordinary-yuv420.jpg" : "ordinary.jpg");
      const auto format = subsampled ? awj::PixelFormat::yuv420 : awj::PixelFormat::yuv444;
      auto memory = take(decoder.decode_memory(bytes,"ordinary JPEG provenance"));
      auto disk = take(decoder.decode(file));
      for (const auto* result : {&memory,&disk})
        require(result->image.source_info && result->image.source_info->pixel_format == format &&
            result->image.source_info->bit_depth == 8,"Ordinary JPEG source sampling or precision was lost");
      auto dimensions = take(decoder.probe_dimensions(file));
      require(dimensions.opaque_sdr8_yuv420 == subsampled,"Ordinary JPEG direct-YUV eligibility changed");
    }
  };
  awj::JpegImageDecoder turbo;
  check_ordinary_header(turbo);
#if AWJ_HAS_JPEGLI
  awj::JpegliImageDecoder jpegli;
  check_ordinary_header(jpegli);
#endif
  check_jpeg(sandbox, "ordinary-mpo.jpg", mpo(ordinary, base), Outcome::ordinary);
  const auto ordinary_split = std::string_view{ordinary_xml}.find(" crs:Gamma");
  check_jpeg(sandbox,"ordinary-extended-xmp.jpg",extended_jpeg(base,ordinary_xml,ordinary_split),Outcome::ordinary);
  check_jpeg(sandbox,"ordinary-extended-reversed.jpg",extended_jpeg(base,ordinary_xml,ordinary_split,true),Outcome::ordinary);
  check_jpeg(sandbox,"ordinary-bad-extended-xmp.jpg",extended_jpeg(base,ordinary_xml,ordinary_split,false,true),Outcome::ordinary);
  check_jpeg(sandbox,"ordinary-extended-split-uri.jpg",extended_jpeg(base,ordinary_xml,std::string_view{ordinary_xml}.find("http://ns.adobe.com/hdr-gain-map/1.0/")+10),Outcome::ordinary);
  check_jpeg(sandbox,"ordinary-extended-mpo.jpg",mpo(extended_jpeg(base,ordinary_xml,ordinary_split),extended_jpeg(base,ordinary_xml,ordinary_split)),Outcome::ordinary);

  auto pure = package_iso(base, gain);
  auto mixed = package_iso(base, app(gain, 0xe1, xmp(adobe)));
  auto legacy = damage_iso(mixed, true), corrupt = damage_iso(mixed, false);
  check_jpeg(sandbox, "pure-iso.jpg", pure, Outcome::iso);
  check_jpeg(sandbox, "renamed-iso.dat", pure, Outcome::iso);
  check_jpeg(sandbox, "renamed-legacy.dat", legacy, Outcome::rejected);
  check_jpeg(sandbox, "iso-with-legacy-xmp.jpg", mixed, Outcome::iso);
  const auto legacy_split = adobe.find(" hdrgm:Version");
  check_jpeg(sandbox,"legacy-extended-xmp.jpg",extended_jpeg(base,adobe,legacy_split),Outcome::rejected);
  check_jpeg(sandbox,"legacy-extended-reversed.jpg",extended_jpeg(base,adobe,legacy_split,true),Outcome::rejected);
  check_jpeg(sandbox,"legacy-extended-split-uri.jpg",extended_jpeg(base,adobe,adobe.find("http://ns.adobe.com/hdr-gain-map/1.0/")+10),Outcome::rejected);
  check_jpeg(sandbox,"legacy-extended-split-property.jpg",extended_jpeg(base,adobe,adobe.find("GainMapMax")+4),Outcome::rejected);
  auto orphan = extended_jpeg(base,adobe,legacy_split);
  const std::string_view orphan_text{reinterpret_cast<const char*>(orphan.data()),orphan.size()};
  constexpr std::string_view link = "xmpNote:HasExtendedXMP=\"";
  const auto guid_at = orphan_text.find(link);
  require(guid_at != orphan_text.npos, "Missing test Extended XMP link");
  std::fill_n(orphan.begin()+guid_at+link.size(),32,std::byte{'F'});
  check_jpeg(sandbox,"unassociated-extended-xmp.jpg",orphan,Outcome::ordinary);
  auto iso_extended = package_iso(base,extended_jpeg(gain,adobe,legacy_split,true));
  auto iso_bad_extended = package_iso(base,extended_jpeg(gain,adobe,legacy_split,false,true));
  check_jpeg(sandbox,"iso-with-extended-legacy.jpg",iso_extended,Outcome::iso);
  check_jpeg(sandbox,"iso-with-bad-extended-legacy.jpg",iso_bad_extended,Outcome::iso);
  require(engine_pixels(pure,true) == engine_pixels(iso_extended,true) && engine_pixels(pure,true) == engine_pixels(iso_bad_extended,true),
      "Extended legacy XMP changed authoritative ISO pixels");
  check_jpeg(sandbox,"bad-iso-with-extended-legacy.jpg",damage_iso(iso_extended,false),Outcome::rejected);
  require(engine_pixels(pure, true) == engine_pixels(mixed, true), "Legacy XMP changed authoritative ISO reconstruction");
  check_jpeg(sandbox, "legacy-only-pair.jpg", legacy, Outcome::rejected); engine_pixels(legacy, false);
  check_jpeg(sandbox, "corrupt-iso-with-xmp.jpg", corrupt, Outcome::rejected); engine_pixels(corrupt, false);
  check_jpeg(sandbox, "corrupt-iso.jpg", damage_iso(pure, false), Outcome::rejected);
  check_jpeg(sandbox, "legacy-adobe.jpg", app(base, 0xe1, xmp(adobe)), Outcome::rejected);
  check_jpeg(sandbox, "legacy-apple.jpg", app(base, 0xe1, xmp(R"(<rdf:Description xmlns:a="http://ns.apple.com/HDRGainMap/1.0/" a:HDRGainMapVersion="131072" a:HDRGainMapHeadroom="2"/>)")), Outcome::rejected);
  check_jpeg(sandbox, "legacy-google.jpg", app(base, 0xe1, xmp(R"(<rdf:Description xmlns:c="http://ns.google.com/photos/1.0/container/"><rdf:li i:Semantic='GainMap'/></rdf:Description>)")), Outcome::rejected);
  auto truncated = app(base, 0xe2, text("urn:iso:std:iso:ts:21496:-1"));
  check_jpeg(sandbox, "truncated-iso.jpg", truncated, Outcome::rejected);
  for (const auto mode : {awj::GainMapDecodeMode::hdr_only,awj::GainMapDecodeMode::sdr_only,awj::GainMapDecodeMode::preserve_both}) {
    for (const auto* invalid : {&legacy,&corrupt,&truncated})
      check_image(turbo.decode_memory(*invalid,"terminal JPEG rendition gate",{.gain_map_mode = mode}),Outcome::rejected);
    auto untouched = take(turbo.decode_memory(ordinary,"ordinary JPEG rendition mode",{.gain_map_mode = mode}));
    require(!untouched.gain_map_alternative && untouched.image.source_info &&
        !untouched.image.source_info->source_has_gain_map,"Ordinary JPEG was promoted to a paired HDR image");
  }
  // Even when MPF makes entropy scanning necessary, stuffed bytes cannot turn
  // arbitrary entropy text into an ISO APP2 record.
  auto fake = app(Bytes{std::byte{255},std::byte{216}}, 0xe2, text(std::string_view{"MPF\0",4}));
  append(fake, Bytes{std::byte{255},std::byte{218},std::byte{0},std::byte{2},std::byte{255},std::byte{0},std::byte{226}});
  append(fake, text("urn:iso:std:iso:ts:21496:-1")); append(fake, Bytes{std::byte{255},std::byte{217}});
  require(!awj::gain_map_detail::jpeg_gain_map_markers(fake).iso, "Entropy bytes were parsed as an APP segment");
}

// Test-only BMFF fixture construction: reuse a pinned HEVC payload, clone its
// coded item as the map, and add tmap/dimg/auxl/auxC. No HEIF encoder is enabled.
struct Box { std::string type; Bytes payload; };
std::vector<Box> boxes(View bytes) {
  std::vector<Box> out;
  for (std::size_t pos = 0; pos < bytes.size();) {
    const auto size = read(bytes, pos, 4); require(size >= 8 && size <= bytes.size() - pos, "Unsupported test BMFF box");
    out.push_back({std::string(reinterpret_cast<const char*>(bytes.data() + pos + 4), 4),
        Bytes(bytes.begin() + pos + 8, bytes.begin() + pos + size)}); pos += size;
  }
  return out;
}
Bytes serialize(const std::vector<Box>& list) {
  Bytes out;
  for (const auto& box : list) { u32(out, static_cast<std::uint32_t>(box.payload.size() + 8)); append(out, text(box.type)); append(out, box.payload); }
  return out;
}
Box& find(std::vector<Box>& list, std::string_view type) {
  const auto it = std::ranges::find_if(list, [&](const Box& b) { return b.type == type; });
  require(it != list.end(), "Missing pinned HEIF fixture box"); return *it;
}
Bytes iso_tmap_metadata(bool hdr_base = false) {
  Bytes out(6); out[5] = std::byte{64};
  for (auto [n,d] : {std::pair{hdr_base ? 2u : 0u,1u}, {hdr_base ? 0u : 2u,1u}, {0u,1u}, {2u,1u}, {1u,1u}, {0u,1u}, {0u,1u}}) { u32(out,n); u32(out,d); }
  require(out.size() == 62, "Invalid ISO tmap test metadata"); return out;
}
Bytes heif_fixture(bool iso, bool apple, bool corrupt = false, bool rotate = false,
    std::string_view name = "rainbow-451x461.heic", bool tmap_primary = false, Bytes metadata = {},
    int base_transfer = 13, bool crop = false) {
  auto source = load(fs::path(AWJ_ISO_TEST_HEIF_DATA) / name);
  auto top = boxes(source); auto& meta_box = find(top, "meta");
  const auto old_meta_size = meta_box.payload.size() + 8;
  auto meta = boxes(View{meta_box.payload}.subspan(4));
  const auto base_id = read(find(meta,"pitm").payload,4,2);
  auto& iinf = find(meta,"iinf"); require(read(iinf.payload,0,4) == 0, "Pinned HEIF iinf version changed");
  auto items = boxes(View{iinf.payload}.subspan(6));
  unsigned max_id = 0; Box base_info;
  for (const auto& item : items) {
    require(item.type == "infe" && read(item.payload,0,1) == 2, "Pinned HEIF infe version changed");
    const auto id = read(item.payload,4,2); max_id = std::max(max_id,id); if (id == base_id) base_info = item;
  }
  require(!base_info.type.empty(), "Missing HEVC primary infe");
  const unsigned aux_id = max_id + 1, tmap_id = max_id + 2;
  if (tmap_primary) { require(iso, "A tmap primary requires ISO metadata"); put(find(meta,"pitm").payload,4,tmap_id,2); }
  auto aux_info = base_info; put(aux_info.payload,4,aux_id,2); aux_info.payload[3] = std::byte{1}; items.push_back(aux_info);
  if (iso) {
    Bytes p{std::byte{2},std::byte{0},std::byte{0},std::byte{1}}; u16(p,tmap_id); u16(p,0); append(p,text("tmap")); p.push_back(std::byte{0});
    items.push_back({"infe",std::move(p)});
  }
  iinf.payload = Bytes(4); u16(iinf.payload,static_cast<unsigned>(items.size())); append(iinf.payload,serialize(items));

  auto& iprp = find(meta,"iprp"); auto properties = boxes(iprp.payload);
  auto& ipco = find(properties,"ipco"); auto props = boxes(ipco.payload);
  const bool hdr_base = base_transfer == 16 || base_transfer == 18;
  unsigned colr_count = 0;
  for (auto& property : props) if (property.type == "colr") {
    property.payload = text("nclx"); u16(property.payload,hdr_base ? 9 : 1); u16(property.payload,base_transfer);
    u16(property.payload,hdr_base ? 9 : 1); property.payload.push_back(std::byte{128}); ++colr_count;
  }
  require(colr_count == 1, "Pinned HEIF color profile layout changed");
  Bytes auxc(4); append(auxc,text(apple ? "urn:com:apple:photo:2020:aux:hdrgainmap" : "urn:iso:std:iso:ts:21496:-1")); auxc.push_back(std::byte{0});
  props.push_back({"auxC",std::move(auxc)}); const auto aux_property = static_cast<unsigned>(props.size());
  unsigned rotation_property = 0, crop_property = 0, alternate_color_property = 0;
  if (iso && hdr_base) {
    auto color = text("nclx"); u16(color,1); u16(color,13); u16(color,1); color.push_back(std::byte{128});
    props.push_back({"colr",std::move(color)}); alternate_color_property = static_cast<unsigned>(props.size());
  }
  if (crop) {
    require(iso, "Test crop requires a tmap rendition");
    const bool alpha = name.find("alpha") != std::string_view::npos;
    Bytes clap;
    for (const unsigned value : {alpha ? 64u : 63u, 1u, alpha ? 48u : 47u, 1u, 0u, 1u, 0u, 1u}) u32(clap,value);
    props.push_back({"clap",std::move(clap)}); crop_property = static_cast<unsigned>(props.size());
  }
  if (rotate) { props.push_back({"irot",Bytes{std::byte{1}}}); rotation_property = static_cast<unsigned>(props.size()); }
  require(props.size() < 128, "Test HEIF property index overflow"); ipco.payload = serialize(props);
  auto& ipma = find(properties,"ipma"); require(read(ipma.payload,0,4) == 0, "Pinned HEIF ipma version changed");
  std::vector<std::pair<unsigned,Bytes>> associations;
  std::size_t pos = 8; Bytes base_properties;
  for (unsigned i = 0; i < read(ipma.payload,4,4); ++i) {
    const auto id = read(ipma.payload,pos,2); pos += 2; const auto count = read(ipma.payload,pos++,1);
    require(count <= ipma.payload.size() - pos, "Invalid test property associations");
    Bytes data(ipma.payload.begin()+pos,ipma.payload.begin()+pos+count); pos += count;
    if (id == base_id) base_properties = data; associations.emplace_back(id,std::move(data));
  }
  require(!base_properties.empty(), "Missing primary property associations");
  auto aux_properties = base_properties; aux_properties.push_back(std::byte(128 | aux_property)); associations.emplace_back(aux_id,std::move(aux_properties));
  if (iso) {
    Bytes rendition;
    for (const auto property : {alternate_color_property, crop_property, rotation_property})
      if (property) rendition.push_back(std::byte(128 | property));
    associations.emplace_back(tmap_id,std::move(rendition));
  }
  ipma.payload = Bytes(4); u32(ipma.payload,static_cast<std::uint32_t>(associations.size()));
  for (const auto& [id,data] : associations) { u16(ipma.payload,id); ipma.payload.push_back(std::byte(data.size())); append(ipma.payload,data); }
  iprp.payload = serialize(properties);

  auto& iref = find(meta,"iref"); auto refs = boxes(View{iref.payload}.subspan(4));
  Bytes auxiliary; u16(auxiliary,aux_id); u16(auxiliary,1); u16(auxiliary,base_id); refs.push_back({"auxl",std::move(auxiliary)});
  if (iso) { Bytes derived; u16(derived,tmap_id); u16(derived,2); u16(derived,base_id); u16(derived,aux_id); refs.push_back({"dimg",std::move(derived)}); }
  iref.payload = Bytes(4); append(iref.payload,serialize(refs));
  struct Location { unsigned id,reference; std::uint32_t base; std::vector<std::pair<std::uint32_t,std::uint32_t>> extents; };
  auto& iloc = find(meta,"iloc"); require(read(iloc.payload,0,4) == 0 && read(iloc.payload,4,2) == 0x4440, "Pinned HEIF iloc layout changed");
  std::vector<Location> locations; pos = 8;
  for (unsigned i = 0; i < read(iloc.payload,6,2); ++i) {
    Location loc{read(iloc.payload,pos,2),read(iloc.payload,pos+2,2),read(iloc.payload,pos+4,4),{}}; pos += 8;
    const auto count = read(iloc.payload,pos,2); pos += 2;
    for (unsigned j = 0; j < count; ++j) { loc.extents.emplace_back(read(iloc.payload,pos,4),read(iloc.payload,pos+4,4)); pos += 8; }
    locations.push_back(std::move(loc));
  }
  auto base_location = std::ranges::find_if(locations,[&](const auto& loc) { return loc.id == base_id; });
  require(base_location != locations.end(), "Missing primary HEVC location");
  auto auxiliary_location = *base_location; auxiliary_location.id = aux_id; locations.push_back(std::move(auxiliary_location));
  if (metadata.empty()) metadata = iso_tmap_metadata(hdr_base);
  if (corrupt) metadata[0] = std::byte{1};
  auto write_locations = [&](std::int64_t delta, std::uint32_t offset) {
    Bytes payload; append(payload,View{iloc.payload}.first(6)); u16(payload,static_cast<unsigned>(locations.size()) + (iso ? 1 : 0));
    for (const auto& loc : locations) {
      const auto shifted = static_cast<std::int64_t>(loc.base) + delta;
      require(loc.base >= old_meta_size && shifted >= 0 && shifted <= 0xffffffffLL, "Invalid relocated test HEIF extent");
      u16(payload,loc.id); u16(payload,loc.reference); u32(payload,static_cast<std::uint32_t>(shifted));
      u16(payload,static_cast<unsigned>(loc.extents.size())); for (auto [start,length] : loc.extents) { u32(payload,start); u32(payload,length); }
    }
    if (iso) { u16(payload,tmap_id); u16(payload,0); u32(payload,offset); u16(payload,1); u32(payload,0); u32(payload,static_cast<std::uint32_t>(metadata.size())); }
    return payload;
  };
  iloc.payload = write_locations(0,0);
  Bytes new_meta(4); append(new_meta,serialize(meta));
  // Replacing the large ICC with nclx may shrink meta, so the delta is signed.
  const auto delta = static_cast<std::int64_t>(new_meta.size()+8) - static_cast<std::int64_t>(old_meta_size);
  const auto metadata_offset = static_cast<std::int64_t>(source.size()) + delta + 8;
  require(metadata_offset >= 0 && metadata_offset <= 0xffffffffLL, "Relocated test tmap offset overflow");
  iloc.payload = write_locations(delta,static_cast<std::uint32_t>(metadata_offset));
  new_meta = Bytes(4); append(new_meta,serialize(meta)); meta_box.payload = std::move(new_meta);
  if (iso) top.push_back({"mdat",std::move(metadata)});
  return serialize(top);
}
std::uint16_t rgba_sample(const awj::ImageBuffer& image, std::size_t x, std::size_t y, std::size_t channel) {
  const auto at = awj::image_sample_coordinates(image,x,y);
  const auto* pixel = image.planes[0].bytes.data() + at[1] * image.planes[0].stride + (at[0] * 4 + channel) * (image.bit_depth / 8);
  if (image.bit_depth == 8) return std::to_integer<std::uint8_t>(*pixel);
  std::uint16_t value{}; std::memcpy(&value,pixel,2); return value;
}
bool same_rgba(const awj::ImageBuffer& a, const awj::ImageBuffer& b) {
  if (a.width != b.width || a.height != b.height || a.bit_depth != b.bit_depth ||
      a.pixel_format != awj::PixelFormat::rgba || b.pixel_format != awj::PixelFormat::rgba ||
      a.sample_representation != b.sample_representation) return false;
  for (std::size_t y = 0; y < a.height; ++y) for (std::size_t x = 0; x < a.width; ++x)
    for (std::size_t c = 0; c < 4; ++c) if (rgba_sample(a,x,y,c) != rgba_sample(b,x,y,c)) return false;
  return true;
}
void same_alpha(const awj::ImageBuffer& a, const awj::ImageBuffer& b) {
  require(a.width == b.width && a.height == b.height && a.alpha_mode != awj::AlphaMode::none &&
      b.alpha_mode != awj::AlphaMode::none,"Paired alpha dimensions/mode differ");
  for (std::size_t y = 0; y < a.height; ++y) for (std::size_t x = 0; x < a.width; ++x) {
    const auto pa = int(rgba_sample(a,x,y,3)) * (a.bit_depth == 8 ? 257 : 1);
    const auto pb = int(rgba_sample(b,x,y,3)) * (b.bit_depth == 8 ? 257 : 1);
    require(std::abs(pa-pb) <= 257,"Gain-map rendition changed source alpha");
  }
}

#include "iso_gain_map_icc_fixture.h"
#include "iso_gain_map_grid_fixture.h"

awj::ImageBuffer rgba_rendition(bool hdr, int transfer) {
  const int depth = hdr ? 16 : 8;
  awj::ImageBuffer image{.width = 4, .height = 3, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::straight, .bit_depth = depth,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = depth,
          .color_primaries = hdr ? 9 : 1, .transfer_characteristics = hdr ? transfer : 13,
          .matrix_coefficients = 0, .color_range = 1, .has_hdr_metadata = hdr}};
  image.planes.push_back({Bytes(4 * 3 * 4 * (depth / 8)), std::size_t(4 * 4 * (depth / 8))});
  const unsigned sdr[]{24,170,40};
  for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 4; ++x) for (std::size_t c = 0; c < 4; ++c) {
    const auto value = c == 3 ? std::min(hdr ? 65535u : 255u, unsigned(x+1) * (hdr ? 16384u : 64u))
        : hdr ? 57000u + unsigned(c) * 1000u : sdr[c];
    auto* out = image.planes[0].bytes.data() + y * image.planes[0].stride + (x * 4 + c) * (depth / 8);
    if (depth == 8) *out = std::byte(value);
    else { const auto v = static_cast<std::uint16_t>(value); std::memcpy(out,&v,2); }
  }
  return image;
}
void fp16_original_space_contract() {
  // 旧 map 只能在原运算域使用；安全 FP16 子集与同域整数参照应一致。
  std::unique_ptr<avifGainMap, decltype(&avifGainMapDestroy)> metadata{avifGainMapCreate(), avifGainMapDestroy};
  require(metadata && bool(awj::heif_detail::parse_iso_metadata(iso_tmap_metadata(), *metadata)),
      "Cannot create FP16 gain-map metadata");
  awj::ImageBuffer base{.width = 1, .height = 1, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = awj::AlphaMode::none, .bit_depth = 16,
      .sample_representation = awj::SampleRepresentation::ieee_half_float,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 12, .transfer_characteristics = 8, .matrix_coefficients = 0, .color_range = 1,
          .linear_reference_white_nits = 203.0F}};
  const std::array<std::uint16_t, 4> half{0x3400, 0x3800, 0x3c00, 0x3c00};
  base.planes.push_back({Bytes(8), 8});
  std::memcpy(base.planes[0].bytes.data(), half.data(), 8);
  awj::ImageBuffer map{.width = 1, .height = 1, .pixel_format = awj::PixelFormat::rgba};
  map.planes.push_back({Bytes{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}}, 4});
  auto reference = base;
  reference.sample_representation = awj::SampleRepresentation::unorm;
  const std::array<std::uint16_t, 4> integer{16384, 32768, 65535, 65535};
  std::memcpy(reference.planes[0].bytes.data(), integer.data(), 8);
  auto rendered = take(awj::compose_iso_gain_map(base, map, *metadata));
  auto expected = take(awj::compose_iso_gain_map(reference, map, *metadata));
  require(rendered.width == 1 && rendered.height == 1 && rendered.bit_depth == 16,
      "FP16 original-domain rendering changed layout");
  for (std::size_t c = 0; c < 4; ++c) {
    std::uint16_t a{}, b{};
    std::memcpy(&a, rendered.planes[0].bytes.data() + c * 2, 2);
    std::memcpy(&b, expected.planes[0].bytes.data() + c * 2, 2);
    require(std::abs(int(a) - int(b)) <= 4, "FP16 gain map changed the original RGB operation space");
  }
  for (const auto bits : {0x8000u, 0xb400u, 0x4000u, 0x7c00u, 0x7e00u}) {
    auto invalid = base;
    const auto value = static_cast<std::uint16_t>(bits);
    std::memcpy(invalid.planes[0].bytes.data(), &value, 2);
    require(!awj::compose_iso_gain_map(invalid, map, *metadata),
        "Invalid or extended FP16 samples were silently clipped by the old gain map");
  }
  auto unknown_white = base;
  unknown_white.source_info->linear_reference_white_nits.reset();
  require(!awj::compose_iso_gain_map(unknown_white, map, *metadata), "Unknown FP16 white scale was guessed");
  unknown_white.source_info->linear_reference_white_nits = 80.0F;
  require(!awj::compose_iso_gain_map(unknown_white, map, *metadata), "scRGB 80-nit samples used the ISO 203-nit scale");
  auto geometry = base;
  geometry.geometry = awj::ImageGeometry{.stored_width = 1, .stored_height = 1, .crop_width = 1, .crop_height = 1};
  require(!awj::compose_iso_gain_map(geometry, map, *metadata), "FP16 geometry changed color space before the old map");
}

void bidirectional_render_contract() {
  using Mode = awj::GainMapDecodeMode;
  awj::ImageBuffer gain{.width = 2, .height = 2, .pixel_format = awj::PixelFormat::rgba,
      .planes = {{Bytes(16,std::byte{255}),8}}};
  for (const bool hdr_base : {false,true}) for (const int transfer : {16,18}) {
    auto base = rgba_rendition(hdr_base,transfer);
    std::unique_ptr<avifGainMap,decltype(&avifGainMapDestroy)> metadata(avifGainMapCreate(),avifGainMapDestroy);
    require(metadata && bool(awj::heif_detail::parse_iso_metadata(iso_tmap_metadata(hdr_base),*metadata)),"Cannot create bidirectional ISO metadata");
    metadata->altColorPrimaries = hdr_base ? AVIF_COLOR_PRIMARIES_BT709 : AVIF_COLOR_PRIMARIES_BT2020;
    metadata->altTransferCharacteristics = hdr_base ? AVIF_TRANSFER_CHARACTERISTICS_SRGB : static_cast<avifTransferCharacteristics>(transfer);
    auto high = take(awj::render_iso_gain_map(base,gain,*metadata,Mode::hdr_only));
    auto low = take(awj::render_iso_gain_map(base,gain,*metadata,Mode::sdr_only));
    auto both = take(awj::render_iso_gain_map(base,gain,*metadata,Mode::preserve_both));
    require(!high.gain_map_alternative && !low.gain_map_alternative && both.gain_map_alternative,
        "Common renderer ignored rendition selection");
    require(high.image.source_info && low.image.source_info && high.image.source_info->has_hdr_metadata &&
        !low.image.source_info->has_hdr_metadata && high.image.source_info->color_primaries == 9 &&
        high.image.source_info->transfer_characteristics == (hdr_base ? transfer : 16) &&
        low.image.source_info->color_primaries == 1 && low.image.source_info->transfer_characteristics == 13,
        "Common renderer lost bidirectional CICP interpretation");
    require(same_rgba(hdr_base ? high.image : low.image,base),"Renderer brightened an existing HDR base or changed an existing SDR base");
    require(same_rgba(both.image,high.image) && same_rgba(both.gain_map_alternative->image,low.image) &&
        both.gain_map_alternative->main_hdr_headroom_log2 == 2 &&
        both.gain_map_alternative->alternate_hdr_headroom_log2 == 0,"Common renderer lost or reversed a headroom endpoint");
    same_alpha(high.image,low.image);
    // 两个输入方向与所有呈现模式都必须保留取消契约，不能因保留原端点而绕过。
    awj::compat::stop_source cancellation; cancellation.request_stop();
    for (const auto mode : {Mode::hdr_only,Mode::sdr_only,Mode::preserve_both}) {
      auto canceled = awj::render_iso_gain_map(base,gain,*metadata,mode,cancellation.get_token());
      require(!canceled && canceled.error().find("取消") != std::string::npos,"Common ISO renderer ignored cancellation");
    }
    auto canceled_compute = awj::compute_iso_gain_map(both.image,*both.gain_map_alternative,2,2,8,cancellation.get_token());
    require(!canceled_compute && canceled_compute.error().find("取消") != std::string::npos,"ISO gain-map computation ignored cancellation");
    auto rebuilt = take(awj::compute_iso_gain_map(both.image,*both.gain_map_alternative,2,2,8));
    require(rebuilt->image && rebuilt->image->width == 2 && rebuilt->image->height == 2 &&
        std::uint64_t(rebuilt->baseHdrHeadroom.n) * rebuilt->alternateHdrHeadroom.d >
            std::uint64_t(rebuilt->alternateHdrHeadroom.n) * rebuilt->baseHdrHeadroom.d &&
        rebuilt->altColorPrimaries == AVIF_COLOR_PRIMARIES_BT709 &&
        rebuilt->altTransferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_SRGB,
        "Recomputed ISO map is not HDR-main/SDR-alternate");
    auto reserved_transfer = base;
    reserved_transfer.source_info->transfer_characteristics = 3;
    for (const auto mode : {Mode::hdr_only,Mode::sdr_only,Mode::preserve_both})
      require(!awj::render_iso_gain_map(reserved_transfer,gain,*metadata,mode),"Reserved CICP transfer silently fell back to BT.709");
    metadata->gainMapGamma[0].d = 0;
    for (const auto mode : {Mode::hdr_only,Mode::sdr_only,Mode::preserve_both})
      require(!awj::render_iso_gain_map(base,gain,*metadata,mode),"Rendition selection bypassed invalid ISO metadata");
  }
}
void heif_rendition_modes(const Sandbox& sandbox) {
  using Mode = awj::GainMapDecodeMode;
  struct Case { int transfer; const char* name; bool rotate; bool crop; };
  for (const auto test : {Case{16,"rainbow-451x461.heic",false,false},
      Case{18,"rainbow-451x461.heic",true,false}, Case{16,"rainbow-451x461.heic",false,true},
      Case{18,"with-alpha-512x512.heic",false,true}}) {
    const auto bytes = heif_fixture(true,false,false,test.rotate,test.name,false,{},test.transfer,test.crop);
    const auto file = sandbox.save("hdr-base-" + std::to_string(test.transfer) + "-" + std::to_string(test.rotate) +
        "-" + std::to_string(test.crop) + ".heic",bytes);
    awj::HeifImageDecoder decoder;
    auto high = take(decoder.decode_memory(bytes,"HEIF HDR base",{.copy_metadata_payloads = false,.gain_map_mode = Mode::hdr_only}));
    auto low = take(decoder.decode_memory(bytes,"HEIF SDR rendition",{.copy_metadata_payloads = false,.gain_map_mode = Mode::sdr_only}));
    auto both = take(decoder.decode_memory(bytes,"HEIF paired renditions",{.copy_metadata_payloads = false,.gain_map_mode = Mode::preserve_both}));
    auto retained = take(decoder.decode_memory(bytes,"HEIF optional metadata",{.copy_metadata_payloads = true,.gain_map_mode = Mode::preserve_both}));
    require(!high.gain_map_alternative && !low.gain_map_alternative && both.gain_map_alternative && retained.gain_map_alternative,
        "HEIF ignored hdr_only/sdr_only/preserve_both or stripped the essential pair");
    require(high.image.source_info && low.image.source_info && high.image.source_info->source_has_gain_map &&
        high.image.source_info->has_hdr_metadata && high.image.source_info->color_primaries == 9 &&
        high.image.source_info->transfer_characteristics == test.transfer &&
        !low.image.source_info->has_hdr_metadata && low.image.source_info->color_primaries == 1 &&
        low.image.source_info->transfer_characteristics == 13,"HEIF HDR-base/SDR-alternate CICP reversed or guessed");
    require(same_rgba(high.image,both.image) && same_rgba(low.image,both.gain_map_alternative->image) &&
        same_rgba(both.image,retained.image) && same_rgba(both.gain_map_alternative->image,retained.gain_map_alternative->image),
        "HEIF rendition/optional metadata mode changed endpoint pixels or geometry");
    require(both.gain_map_alternative->main_hdr_headroom_log2 == 2 &&
        both.gain_map_alternative->alternate_hdr_headroom_log2 == 0,"HEIF reversed high-to-low headroom");
    auto dimensions = take(decoder.probe_dimensions(file));
    const bool alpha = std::string_view{test.name}.find("alpha") != std::string_view::npos;
    const auto expected_width = test.crop ? (alpha ? 64u : 63u) : test.rotate ? 461u : 451u;
    const auto expected_height = test.crop ? (alpha ? 48u : 47u) : test.rotate ? 451u : 461u;
    require(high.image.width == expected_width && high.image.height == expected_height &&
        low.image.width == expected_width && low.image.height == expected_height &&
        dimensions.width == expected_width && dimensions.height == expected_height,
        "HEIF crop/rotation did not transform both renditions and the probe together");
    if (alpha) same_alpha(high.image,low.image);
    awj::ImageDecoder& file_api = decoder;
    auto disk = take(file_api.decode(file,{.copy_metadata_payloads = false,.gain_map_mode = Mode::preserve_both}));
    require(disk.gain_map_alternative && same_rgba(disk.image,both.image) &&
        same_rgba(disk.gain_map_alternative->image,both.gain_map_alternative->image),"HEIF file mode dropped an endpoint");
  }
}
void heif_matrix(const Sandbox& sandbox) {
  awj::HeifImageDecoder decoder;
  auto pure = heif_fixture(true,false), apple_iso = heif_fixture(true,true);
  auto pure_file = sandbox.save("iso.heic",pure), apple_file = sandbox.save("iso-apple-urn.heic",apple_iso);
  check_codec(decoder,pure,pure_file,Outcome::iso); check_codec(decoder,apple_iso,apple_file,Outcome::iso);
  check_registry(pure_file,Outcome::iso); check_registry(apple_file,Outcome::iso);
  auto first = take(decoder.decode_memory(pure,"pure ISO HEIF")); auto second = take(decoder.decode_memory(apple_iso,"ISO with Apple auxiliary URN"));
  require(first.image.planes.front().bytes == second.image.planes.front().bytes, "Apple URN changed the ISO reconstruction");
  auto legacy = heif_fixture(false,true), corrupt = heif_fixture(true,true,true);
  const auto legacy_file = sandbox.save("legacy.heic",legacy), corrupt_file = sandbox.save("corrupt-iso-apple.heic",corrupt);
  check_codec(decoder,legacy,legacy_file,Outcome::rejected); check_registry(legacy_file,Outcome::rejected);
  check_codec(decoder,corrupt,corrupt_file,Outcome::rejected); check_registry(corrupt_file,Outcome::rejected);
  for (const auto mode : {awj::GainMapDecodeMode::sdr_only,awj::GainMapDecodeMode::preserve_both})
    for (const auto* invalid : {&legacy,&corrupt})
      check_image(decoder.decode_memory(*invalid,"terminal HEIF rendition gate",{.gain_map_mode = mode}),Outcome::rejected);
  auto rotated = heif_fixture(true,true,false,true);
  const auto rotated_file = sandbox.save("iso-tmap-rotated.heic",rotated);
  check_codec(decoder,rotated,rotated_file,Outcome::iso); check_registry(rotated_file,Outcome::iso);
  auto rotated_image = take(decoder.decode_memory(rotated,"ISO tmap rotation"));
  require(rotated_image.image.width == first.image.height && rotated_image.image.height == first.image.width,
      "ISO HEIF tmap rotation geometry was lost");
  auto primary_tmap = heif_fixture(true,true,false,false,"rainbow-451x461.heic",true);
  const auto primary_tmap_file = sandbox.save("iso-primary-tmap.heic",primary_tmap);
  check_codec(decoder,primary_tmap,primary_tmap_file,Outcome::iso); check_registry(primary_tmap_file,Outcome::iso);
  auto iso_alpha = heif_fixture(true,true,false,false,"with-alpha-512x512.heic");
  const auto iso_alpha_file = sandbox.save("iso-alpha.heic",iso_alpha);
  check_codec(decoder,iso_alpha,iso_alpha_file,Outcome::iso); check_registry(iso_alpha_file,Outcome::iso);
  auto enhanced_alpha = take(decoder.decode_memory(iso_alpha,"ISO HEIF alpha"));
  require(enhanced_alpha.image.alpha_mode != awj::AlphaMode::none, "ISO HEIF alpha was lost");
  auto alpha = load(fs::path(AWJ_ISO_TEST_HEIF_DATA)/"with-alpha-512x512.heic"); const auto alpha_file = sandbox.save("ordinary-alpha.heic",alpha);
  check_codec(decoder,alpha,alpha_file,Outcome::ordinary);
  auto decoded = take(decoder.decode_memory(alpha,"ordinary alpha HEIF"));
  require(decoded.image.alpha_mode != awj::AlphaMode::none, "Ordinary HEIF alpha was lost");
  auto data = iso_tmap_metadata();
  std::unique_ptr<avifGainMap,decltype(&avifGainMapDestroy)> map(avifGainMapCreate(),avifGainMapDestroy);
  require(map && bool(awj::heif_detail::parse_iso_metadata(data,*map)), "Valid ISO HEIF metadata failed");
  put(data,42,0,4); // Per-channel gamma denominator.
  require(!awj::heif_detail::parse_iso_metadata(data,*map), "Invalid ISO HEIF fraction was accepted");
  auto versioned = [&](std::string_view name, const Bytes& payload, Outcome expected) {
    auto bytes = heif_fixture(true,true,false,false,"rainbow-451x461.heic",false,payload);
    const auto file = sandbox.save(name,bytes);
    check_codec(decoder,bytes,file,expected); check_registry(file,expected);
  };
  auto future_writer = iso_tmap_metadata(); put(future_writer,3,1,2);
  require(bool(awj::heif_detail::parse_iso_metadata(future_writer,*map)), "minimum=0 writer=1 must remain readable");
  versioned("iso-heif-writer1.heic",future_writer,Outcome::iso);
  future_writer.resize(162,std::byte{0xaa});
  require(bool(awj::heif_detail::parse_iso_metadata(future_writer,*map)), "Future ISO writer extension was rejected");
  versioned("iso-heif-writer1-tail.heic",future_writer,Outcome::iso);
  auto known_writer_tail = iso_tmap_metadata(); known_writer_tail.push_back(std::byte{0});
  require(!awj::heif_detail::parse_iso_metadata(known_writer_tail,*map), "Known ISO writer must require exact EOF");
  versioned("iso-heif-writer0-tail.heic",known_writer_tail,Outcome::rejected);
  auto minimum_one = iso_tmap_metadata(); put(minimum_one,1,1,2); put(minimum_one,3,1,2);
  require(!awj::heif_detail::parse_iso_metadata(minimum_one,*map), "Unsupported ISO minimum version was accepted");
  versioned("iso-heif-minimum1.heic",minimum_one,Outcome::rejected);
  future_writer.resize(61);
  require(!awj::heif_detail::parse_iso_metadata(future_writer,*map), "Truncated future-writer mandatory fields were accepted");
}
void avif_diagnostic_contract() {
  avifDecoder decoder{};
  auto diagnostic = [&](std::string_view value) {
    require(value.size() < sizeof(decoder.diag.error), "Diagnostic test input too long");
    std::memset(decoder.diag.error,0,sizeof(decoder.diag.error));
    std::memcpy(decoder.diag.error,value.data(),value.size());
  };
  diagnostic("Ordinary AVIF diagnostic unrelated to gain maps");
  require(!awj::gain_map_detail::avif_iso_gain_map_failure(AVIF_RESULT_OK,&decoder) &&
      bool(awj::gain_map_detail::validate_avif_iso_gain_map_parse(AVIF_RESULT_OK,decoder)),
      "An unrelated ordinary AVIF diagnostic triggered the ISO gate");
  require(!awj::gain_map_detail::avif_iso_gain_map_failure(AVIF_RESULT_INVALID_ARGUMENT,&decoder),
      "An ordinary AVIF error was mislabeled as HDR");
  diagnostic("Box[meta] ordinary AVIF diagnostic unrelated to tmap");
  require(bool(awj::gain_map_detail::validate_avif_iso_gain_map_parse(AVIF_RESULT_OK,decoder)), "An unrelated BMFF diagnostic triggered ISO rejection");
  diagnostic("Box[tmap] has unsupported minimum version [1]");
  for (auto result : {AVIF_RESULT_OK, AVIF_RESULT_NOT_IMPLEMENTED}) {
    auto invalid = awj::gain_map_detail::validate_avif_iso_gain_map_parse(result,decoder);
    require(!invalid && invalid.error().starts_with("Gain Map:") && invalid.error().find("ISO 21496-1") != std::string::npos,
        "Ignored future ISO tmap metadata was accepted as SDR");
  }
  diagnostic("Box[tmap] has unsupported version [1]");
  require(!awj::gain_map_detail::validate_avif_iso_gain_map_parse(AVIF_RESULT_OK,decoder), "Unknown ISO tmap box version was ignored");
  for (auto message : {"Box[tmap] missing mandatory ispe property", "Box[tmap] ispe size mismatch"}) {
    diagnostic(message);
    auto invalid = awj::gain_map_detail::validate_avif_iso_gain_map_parse(AVIF_RESULT_BMFF_PARSE_FAILED,decoder);
    require(!decoder.image && !invalid && invalid.error().starts_with("Gain Map:") &&
        invalid.error().find("ISO 21496-1") != std::string::npos, "Pre-allocation tmap failure escaped ISO classification");
  }
}

Bytes declared_alpha_avif_fixture(Bytes bytes) {
  // The pinned structural grid/alpha fixture deliberately leaves HDR base
  // primaries/matrix unspecified. Give this synthetic positive fixture an
  // explicit CICP using a strict, unique, same-size test-data anchor. This is
  // not application parsing or a color-space guess for user input.
  Bytes anchor;
  for (unsigned v : {0u,0u,0u,19u,'c'+0u,'o'+0u,'l'+0u,'r'+0u,'n'+0u,'c'+0u,'l'+0u,'x'+0u,0u,2u,0u,16u,0u,2u})
    anchor.push_back(std::byte(v));
  const auto found = std::search(bytes.begin(),bytes.end(),anchor.begin(),anchor.end());
  require(found != bytes.end(), "Pinned alpha AVIF color anchor changed");
  require(std::search(found + anchor.size(),bytes.end(),anchor.begin(),anchor.end()) == bytes.end(), "Ambiguous alpha AVIF color anchor");
  found[13] = std::byte{1}; found[17] = std::byte{6};
  return bytes;
}

Bytes avif_tmap_ispe_fixture(Bytes bytes, bool missing) {
  // Fixed pinned fixture ipma: item 2 is tmap, its ispe is index 1, and the
  // smaller auxiliary's ispe is index 8. Change only that association byte.
  Bytes anchor = text("ipma");
  for (unsigned v : {0u,0u,0u,0u,0u,0u,0u,3u,0u,1u,5u,1u,2u,0x83u,4u,5u,
                    0u,2u,3u,1u,6u,7u,0u,3u,4u,8u,6u,0x89u,10u}) anchor.push_back(std::byte(v));
  const auto found = std::search(bytes.begin(),bytes.end(),anchor.begin(),anchor.end());
  require(found != bytes.end() && std::search(found+anchor.size(),bytes.end(),anchor.begin(),anchor.end()) == bytes.end(),
      "Pinned tmap ispe association anchor changed");
  found[23] = std::byte(missing ? 0 : 8);
  return bytes;
}

void avif_matrix(const Sandbox& sandbox) {
  auto decoder = awj::make_avif_image_decoder(1); require(bool(decoder), "No AVIF decoder");
  for (const auto* name : {"seine_sdr_gainmap_srgb.avif", "seine_hdr_gainmap_small_srgb.avif", "color_grid_alpha_grid_gainmap_nogrid.avif"}) {
    auto file = fs::path(AWJ_ISO_TEST_AVIF_DATA)/name; auto bytes = load(file);
    if (std::string_view{name}.find("alpha") != std::string_view::npos) {
      check_image(decoder->decode_memory(bytes,name),Outcome::rejected);
      check_image(decoder->decode(file),Outcome::rejected);
      bytes = declared_alpha_avif_fixture(std::move(bytes));
      file = sandbox.save("declared-iso-alpha.avif",bytes);
    }
    check_codec(*decoder,bytes,file,Outcome::iso); check_registry(file,Outcome::iso);
    auto result = decoder->decode_memory(bytes,name); if (!result) throw std::runtime_error(result.error());
    auto retained = take(decoder->decode_memory(bytes,name,{.copy_metadata_payloads = true}));
    require(result->image.metadata.empty() && result->image.width == retained.image.width && result->image.height == retained.image.height &&
        result->image.planes.front().bytes == retained.image.planes.front().bytes, "Dropping optional metadata changed ISO AVIF pixels or orientation");
    std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)> primary(avifDecoderCreate(),avifDecoderDestroy);
    require(bool(primary), "Cannot create primary color parser");
    primary->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA | AVIF_IMAGE_CONTENT_GAIN_MAP;
    require(avifDecoderSetIOMemory(primary.get(),reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()) == AVIF_RESULT_OK &&
        avifDecoderParse(primary.get()) == AVIF_RESULT_OK && primary->image && primary->image->gainMap,
        "Cannot inspect pinned ISO AVIF primary color metadata");
    const auto& source = *primary->image;
    const auto& map = *source.gainMap;
    require(map.baseHdrHeadroom.d && map.alternateHdrHeadroom.d, "Invalid primary headroom fraction");
    const bool preserve_hdr_base = (source.transferCharacteristics == 16 || source.transferCharacteristics == 18) &&
        std::uint64_t(map.baseHdrHeadroom.n) * map.alternateHdrHeadroom.d >=
            std::uint64_t(map.alternateHdrHeadroom.n) * map.baseHdrHeadroom.d;
    // HDR 主图保留原 CICP；只有从另一端重建 HDR 时才规范为 BT.2020/PQ。
    const int expected_primaries = preserve_hdr_base ? source.colorPrimaries : 9;
    const int expected_transfer = preserve_hdr_base ? source.transferCharacteristics : 16;
    require(result->image.source_info && result->image.source_info->source_has_gain_map &&
        result->image.source_info->color_primaries == expected_primaries &&
        result->image.source_info->transfer_characteristics == expected_transfer,
        "ISO AVIF gain map or selected HDR endpoint color tags were lost");
    if (std::string_view{name}.find("alpha") != std::string_view::npos)
      require(result->image.alpha_mode != awj::AlphaMode::none, "ISO AVIF alpha was lost");
  }
  for (bool missing : {true,false}) {
    auto bytes = avif_tmap_ispe_fixture(load(fs::path(AWJ_ISO_TEST_AVIF_DATA)/"seine_hdr_gainmap_small_srgb.avif"),missing);
    auto file = sandbox.save(missing ? "iso-tmap-missing-ispe.avif" : "iso-tmap-ispe-mismatch.avif",bytes);
    check_codec(*decoder,bytes,file,Outcome::rejected); check_registry(file,Outcome::rejected);
  }
  for (const auto* name : {"seine_sdr_gainmap_gammazero.avif", "unsupported_gainmap_minimum_version.avif", "unsupported_gainmap_version.avif"}) {
    const auto file = fs::path(AWJ_ISO_TEST_AVIF_DATA)/name; auto bytes = load(file);
    check_codec(*decoder,bytes,file,Outcome::rejected); check_registry(file,Outcome::rejected);
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view{argv[1]} == "--validate-icc-metadata") {
      // 只接受独立色彩配置；此入口不打开图像、不创建像素或执行转换。
      const auto bytes = load(fs::path{argv[2]});
      const auto color = take(awj::gain_map_detail::icc_iso_color(bytes, {}));
      std::printf("{\"kind\":\"icc-metadata-only\",\"bytes\":%zu,\"primaries\":%d,\"transfer\":%d,\"pixel_decode\":false,\"image_conversion\":false}\n",
          bytes.size(), color.primaries, color.transfer);
      return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--export-icc-fixture") {
      icc_fixture::export_only(fs::path{argv[2]});
      return 0;
    }
    const bool icc_only = argc == 2 && std::string_view{argv[1]} == "--icc-only";
    const bool grid_only = argc == 2 && std::string_view{argv[1]} == "--grid-12-4-only";
    if (argc != 1 && !icc_only && !grid_only) {
      std::fprintf(stderr, "用法：awj_iso_gain_map_tests [--icc-only | --grid-12-4-only | --export-icc-fixture <全新目录> | --validate-icc-metadata <独立ICC>]\n");
      return 2;
    }
    const Sandbox sandbox;
    const auto stage = [](const char* name) { std::fprintf(stderr, "ISO 验证阶段：%s\n", name); std::fflush(stderr); };
    if (grid_only) {
      stage("heif-12-plus-4-grid"); grid12_fixture::contract(sandbox);
      return 0;
    }
    if (icc_only) {
      stage("heif-icc-grid"); icc_fixture::contract(sandbox);
      return 0;
    }
    stage("fp16-original-space"); fp16_original_space_contract();
    stage("bidirectional-render"); bidirectional_render_contract();
    stage("avif-diagnostics"); avif_diagnostic_contract();
    stage("jpeg"); jpeg_matrix(sandbox);
    stage("heif"); heif_matrix(sandbox);
    stage("heif-renditions"); heif_rendition_modes(sandbox);
    stage("heif-icc-grid"); icc_fixture::contract(sandbox);
    stage("avif"); avif_matrix(sandbox);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr,"ISO 21496-1 contract: %s\n",error.what());
    return 1;
  }
}
