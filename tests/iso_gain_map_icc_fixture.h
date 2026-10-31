#include "../src/compat/stdlib_compat.h"
#pragma once

// 本头仅供 iso_gain_map_tests.cpp 在匿名命名空间中引用，复用该文件的 BMFF 与像素 helper。
namespace icc_fixture {

constexpr unsigned base_id = 1, map_id = 2, tmap_id = 11;
constexpr double headroom = 2.148445;
constexpr unsigned hdr_code_tolerance = 64;
constexpr std::string_view profile_name = "sRGB EOTF with DCI-P3 Color Gamut";
using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;

enum class ProfileKind { p3_srgb, wrong_gamut, wrong_gamma, gray };

cmsCIExyYTRIPLE p3_primaries() {
  return {{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}};
}
Bytes profile_bytes(ProfileKind kind = ProfileKind::p3_srgb) {
  const cmsCIExyY white{0.3127, 0.3290, 1.0};
  auto primaries = p3_primaries();
  if (kind == ProfileKind::wrong_gamut) primaries.Red.x = 0.640;
  const cmsFloat64Number srgb[]{2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
  Curve curve{kind == ProfileKind::wrong_gamma ? cmsBuildGamma(nullptr, 1.73)
      : cmsBuildParametricToneCurve(nullptr, 4, srgb), &cmsFreeToneCurve};
  require(bool(curve), "Cannot create synthetic ICC tone curve");
  cmsToneCurve* curves[]{curve.get(), curve.get(), curve.get()};
  Profile profile{kind == ProfileKind::gray ? cmsCreateGrayProfile(&white, curve.get())
      : cmsCreateRGBProfile(&white, &primaries, curves), &cmsCloseProfile};
  require(bool(profile), "Cannot create synthetic matrix/TRC ICC");
  cmsSetProfileVersion(profile.get(), 4.3);
  std::unique_ptr<cmsMLU, decltype(&cmsMLUfree)> description{cmsMLUalloc(nullptr, 1), &cmsMLUfree};
  require(description && cmsMLUsetASCII(description.get(), "en", "US", profile_name.data()) &&
      cmsWriteTag(profile.get(), cmsSigProfileDescriptionTag, description.get()), "Cannot label synthetic ICC");
  require(!cmsIsTag(profile.get(), static_cast<cmsTagSignature>(0x63696370u)), "Synthetic ICC must not contain a cicp tag");
  cmsUInt32Number size = 0;
  require(cmsSaveProfileToMem(profile.get(), nullptr, &size) && size >= 132, "Cannot size synthetic ICC");
  Bytes bytes(size);
  require(cmsSaveProfileToMem(profile.get(), bytes.data(), &size), "Cannot serialize synthetic ICC");
  bytes.resize(size);
  return bytes;
}
std::string description(View bytes) {
  Profile profile{cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size())), &cmsCloseProfile};
  std::array<char, 128> value{};
  require(profile && cmsGetProfileInfoASCII(profile.get(), cmsInfoDescription, "en", "US", value.data(),
      static_cast<cmsUInt32Number>(value.size())), "Cannot inspect synthetic ICC description");
  return value.data();
}
void check_profile(View bytes) {
  require(read(bytes, 16, 4) == 0x52474220u && read(bytes, 36, 4) == 0x61637370u,
      "Positive fixture ICC is not an RGB profile");
  const auto count = read(bytes, 128, 4);
  require(count <= 32 && bytes.size() >= 132 + count * 12, "Unexpected synthetic ICC tag table");
  bool red = false, green = false, blue = false, red_trc = false, green_trc = false, blue_trc = false, white = false;
  for (unsigned i = 0; i < count; ++i) {
    const auto tag = read(bytes, 132 + i * 12, 4);
    require(tag != 0x63696370u, "ICC-only fixture accidentally supplies cicp");
    red |= tag == 0x7258595au; green |= tag == 0x6758595au; blue |= tag == 0x6258595au;
    red_trc |= tag == 0x72545243u; green_trc |= tag == 0x67545243u; blue_trc |= tag == 0x62545243u;
    white |= tag == 0x77747074u;
  }
  require(red && green && blue && red_trc && green_trc && blue_trc && white,
      "Synthetic ICC lacks matrix/TRC/white-point tags");
}

using Vector3 = std::array<double, 3>;
using Matrix3 = std::array<Vector3, 3>;
Vector3 multiply3(const Matrix3& matrix, const Vector3& vector) {
  Vector3 result{};
  for (unsigned row = 0; row < 3; ++row) for (unsigned column = 0; column < 3; ++column)
    result[row] += matrix[row][column] * vector[column];
  return result;
}
Matrix3 multiply3(const Matrix3& left, const Matrix3& right) {
  Matrix3 result{};
  for (unsigned row = 0; row < 3; ++row) for (unsigned column = 0; column < 3; ++column)
    for (unsigned inner = 0; inner < 3; ++inner) result[row][column] += left[row][inner] * right[inner][column];
  return result;
}
Matrix3 inverse3(const Matrix3& matrix) {
  std::array<std::array<double, 6>, 3> rows{};
  for (unsigned row = 0; row < 3; ++row) {
    for (unsigned column = 0; column < 3; ++column) rows[row][column] = matrix[row][column];
    rows[row][row + 3] = 1;
  }
  for (unsigned column = 0; column < 3; ++column) {
    unsigned pivot = column;
    for (unsigned row = column + 1; row < 3; ++row)
      if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
    require(std::isfinite(rows[pivot][column]) && std::abs(rows[pivot][column]) > 1e-10,
        "Synthetic standard color matrix is singular");
    std::swap(rows[pivot], rows[column]);
    const auto divisor = rows[column][column];
    for (auto& value : rows[column]) value /= divisor;
    for (unsigned row = 0; row < 3; ++row) if (row != column) {
      const auto factor = rows[row][column];
      for (unsigned entry = 0; entry < 6; ++entry) rows[row][entry] -= factor * rows[column][entry];
    }
  }
  Matrix3 inverse{};
  for (unsigned row = 0; row < 3; ++row) for (unsigned column = 0; column < 3; ++column)
    inverse[row][column] = rows[row][column + 3];
  return inverse;
}
Vector3 xy_white(double x, double y) {
  require(x > 0 && y > 0 && x + y < 1, "Invalid synthetic standard white point");
  return {x / y, 1.0, (1.0 - x - y) / y};
}
Matrix3 native_p3_matrix() {
  const auto primaries = p3_primaries();
  const std::array<cmsCIExyY, 3> colors{primaries.Red, primaries.Green, primaries.Blue};
  Matrix3 matrix{};
  for (unsigned column = 0; column < 3; ++column) {
    matrix[0][column] = colors[column].x / colors[column].y;
    matrix[1][column] = 1;
    matrix[2][column] = (1.0 - colors[column].x - colors[column].y) / colors[column].y;
  }
  const auto scales = multiply3(inverse3(matrix), xy_white(0.3127, 0.3290));
  for (unsigned row = 0; row < 3; ++row) for (unsigned column = 0; column < 3; ++column)
    matrix[row][column] *= scales[column];
  return matrix;
}
Matrix3 d50_xy_bradford() {
  // 标准 Bradford 锥响应矩阵；目标来自公开标准 D50 xy，而非私图 ICC 或生产校验器。
  constexpr Matrix3 bradford{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
  const auto source = multiply3(bradford, xy_white(0.3127, 0.3290));
  const auto destination = multiply3(bradford, xy_white(0.34567, 0.35850));
  auto scaled = bradford;
  for (unsigned row = 0; row < 3; ++row) for (unsigned column = 0; column < 3; ++column)
    scaled[row][column] *= destination[row] / source[row];
  return multiply3(inverse3(bradford), scaled);
}
View profile_tag(View bytes, cmsTagSignature signature) {
  const auto count = read(bytes, 128, 4);
  require(count <= 32 && bytes.size() >= 132 + count * 12, "Invalid synthetic ICC tag table");
  for (unsigned index = 0; index < count; ++index) {
    const auto at = 132 + index * 12;
    if (read(bytes, at, 4) != static_cast<std::uint32_t>(signature)) continue;
    const auto offset = read(bytes, at + 4, 4), size = read(bytes, at + 8, 4);
    require(offset <= bytes.size() && size <= bytes.size() - offset, "Synthetic ICC tag exceeds profile");
    return bytes.subspan(offset, size);
  }
  throw std::runtime_error("Synthetic ICC lacks a required tag");
}
double profile_fixed(View bytes, std::size_t at) {
  const auto value = read(bytes, at, 4);
  return (value & 0x80000000u ? static_cast<double>(value) - 4294967296.0 : static_cast<double>(value)) / 65536.0;
}
Bytes d50_xy_profile_bytes(View original) {
  // 只改适配后的 RGB→XYZ 列和 chad；ICC 固定 PCS header/wtpt、TRC、描述均沿用默认合成 profile。
  check_profile(original);
  const auto native = native_p3_matrix(), adaptation = d50_xy_bradford();
  const auto adapted = multiply3(adaptation, native);
  Profile profile{cmsOpenProfileFromMem(original.data(), static_cast<cmsUInt32Number>(original.size())), &cmsCloseProfile};
  require(bool(profile), "Cannot open synthetic profile for D50 xy variant");
  const std::array<cmsTagSignature, 3> signatures{cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};
  for (unsigned column = 0; column < 3; ++column) {
    const cmsCIEXYZ color{adapted[0][column], adapted[1][column], adapted[2][column]};
    require(cmsWriteTag(profile.get(), signatures[column], &color), "Cannot write standard D50 xy colorant");
  }
  auto chad = text("sf32"); u32(chad, 0);
  for (const auto& row : adaptation) for (const auto value : row) {
    require(std::isfinite(value) && std::abs(value) < 16, "Standard Bradford coefficient exceeds test encoding range");
    u32(chad, static_cast<std::uint32_t>(std::lround(value * 65536.0)));
  }
  require(cmsWriteRawTag(profile.get(), cmsSigChromaticAdaptationTag, chad.data(), static_cast<cmsUInt32Number>(chad.size())),
      "Cannot write synthetic D50 xy chromatic adaptation");
  cmsUInt32Number size = 0;
  require(cmsSaveProfileToMem(profile.get(), nullptr, &size) && size >= 132, "Cannot size D50 xy ICC variant");
  Bytes bytes(size);
  require(cmsSaveProfileToMem(profile.get(), bytes.data(), &size), "Cannot serialize D50 xy ICC variant");
  bytes.resize(size);
  check_profile(bytes);
  require(std::ranges::equal(original.subspan(68, 12), View{bytes}.subspan(68, 12)) &&
      std::ranges::equal(profile_tag(original, cmsSigMediaWhitePointTag), profile_tag(bytes, cmsSigMediaWhitePointTag)),
      "D50 xy variant changed the fixed ICC PCS header or wtpt");
  for (const auto tag : {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag})
    require(std::ranges::equal(profile_tag(original, tag), profile_tag(bytes, tag)), "D50 xy variant changed its sRGB TRC");
  const auto serialized_chad = profile_tag(bytes, cmsSigChromaticAdaptationTag);
  require(serialized_chad.size() == 44 && read(serialized_chad, 0, 4) == 0x73663332u &&
      !std::ranges::equal(profile_tag(original, cmsSigChromaticAdaptationTag), serialized_chad),
      "D50 xy variant still uses the LCMS default adaptation");
  Matrix3 stored_adaptation{}, stored_colorants{};
  bool changed_colorant = false;
  for (unsigned column = 0; column < 3; ++column) {
    const auto color = profile_tag(bytes, signatures[column]);
    require(color.size() == 20 && read(color, 0, 4) == 0x58595a20u, "Invalid D50 xy XYZ colorant");
    changed_colorant |= !std::ranges::equal(profile_tag(original, signatures[column]), color);
    for (unsigned row = 0; row < 3; ++row) {
      stored_colorants[row][column] = profile_fixed(color, 8 + row * 4);
      stored_adaptation[row][column] = profile_fixed(serialized_chad, 8 + (row * 3 + column) * 4);
    }
  }
  require(changed_colorant, "D50 xy variant did not change its adapted colorant columns");
  const auto recovered = multiply3(inverse3(stored_adaptation), stored_colorants);
  // ICC 16.16 分别量化 chad 和颜色列；反演后只允许三个量化步长以内的累计误差。
  for (unsigned row = 0; row < 3; ++row) for (unsigned column = 0; column < 3; ++column)
    require(std::abs(recovered[row][column] - native[row][column]) <= 3.0 / 65536.0,
        "Serialized D50 xy variant does not recover the standard native P3 matrix");
  const auto recovered_white = multiply3(stored_adaptation, xy_white(0.3127, 0.3290));
  const auto requested_white = xy_white(0.34567, 0.35850);
  for (unsigned channel = 0; channel < 3; ++channel)
    require(std::abs(recovered_white[channel] - requested_white[channel]) <= 2.0 / 65536.0,
        "Serialized Bradford matrix does not adapt D65 to the requested D50 xy");
  return bytes;
}

struct Nclx { unsigned primaries, transfer, matrix; bool full = true; };
Bytes nclx(Nclx color) {
  auto bytes = text("nclx");
  u16(bytes, color.primaries); u16(bytes, color.transfer); u16(bytes, color.matrix);
  bytes.push_back(std::byte(color.full ? 128 : 0));
  return bytes;
}
Bytes item_data(View file, const std::vector<Box>& meta, unsigned wanted) {
  const auto item = std::ranges::find_if(meta, [](const Box& box) { return box.type == "iloc"; });
  require(item != meta.end(), "Fixture has no iloc");
  const auto& loc = item->payload;
  require(read(loc, 0, 4) == 0 && read(loc, 4, 2) == 0x4440, "Pinned fixture iloc layout changed");
  Bytes out;
  std::size_t at = 8;
  bool found = false;
  for (unsigned i = 0; i < read(loc, 6, 2); ++i) {
    const auto id = read(loc, at, 2), reference = read(loc, at + 2, 2), base = read(loc, at + 4, 4);
    const auto count = read(loc, at + 8, 2); at += 10;
    require(reference == 0, "Fixture cannot use external item data");
    if (id == wanted) { require(!found, "Duplicate fixture iloc item"); found = true; }
    for (unsigned j = 0; j < count; ++j) {
      const auto offset = std::uint64_t(base) + read(loc, at, 4), length = std::uint64_t(read(loc, at + 4, 4));
      at += 8;
      require(offset <= file.size() && length <= file.size() - offset, "Fixture extent exceeds its file");
      if (id == wanted) append(out, file.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)));
    }
  }
  require(found && !out.empty() && at == loc.size(), "Missing or malformed fixture item extent");
  return out;
}
Bytes infe(unsigned id, std::string_view type, bool hidden) {
  Bytes payload{std::byte{2}, std::byte{0}, std::byte{0}, std::byte(hidden ? 1 : 0)};
  u16(payload, id); u16(payload, 0); append(payload, text(type)); payload.push_back(std::byte{0});
  return payload;
}
Bytes reference(unsigned from, std::span<const unsigned> targets) {
  Bytes payload; u16(payload, from); u16(payload, static_cast<unsigned>(targets.size()));
  for (const auto target : targets) u16(payload, target);
  return payload;
}
struct Fixture {
  Bytes paired, sdr_reference, map_reference, icc;
  unsigned width{}, height{}, coded_bytes{};
};
Fixture make(Bytes icc = profile_bytes(), std::optional<Nclx> base_nclx = {}) {
  // 只取锁定依赖的公开 HEVC 数据，不继承它的 ICC、NCLX、XMP 或其他呈现属性。
  auto source = load(fs::path(AWJ_ISO_TEST_HEIF_DATA) / "rainbow-451x461.heic");
  auto original_top = boxes(source);
  auto original_meta = boxes(View{find(original_top, "meta").payload}.subspan(4));
  const auto original_id = read(find(original_meta, "pitm").payload, 4, 2);
  auto original_items = boxes(View{find(original_meta, "iinf").payload}.subspan(6));
  const auto primary = std::ranges::find_if(original_items, [&](const Box& item) {
    return item.type == "infe" && read(item.payload, 4, 2) == original_id;
  });
  require(primary != original_items.end() && read(primary->payload, 8, 4) == 0x68766331u,
      "Pinned public fixture primary is no longer a coded HEVC item");
  auto original_properties = boxes(find(original_meta, "iprp").payload);
  auto properties = boxes(find(original_properties, "ipco").payload);
  const auto hvc = find(properties, "hvcC"), ispe = find(properties, "ispe");
  const auto tile_width = read(ispe.payload, 4, 4), tile_height = read(ispe.payload, 8, 4);
  // 原文件用 clap 将 452x462 裁为 451x461；合成 grid 使用完整编码 tile，不继承该裁切。
  require(tile_width == 452 && tile_height == 462 && (read(hvc.payload, 16, 1) & 3) == 1 &&
      (read(hvc.payload, 17, 1) & 7) == 0 && (read(hvc.payload, 18, 1) & 7) == 0,
      "Pinned public tile dimensions, chroma or precision changed");
  Box pixi{"pixi", Bytes(4)};
  append(pixi.payload, Bytes{std::byte{3}, std::byte{8}, std::byte{8}, std::byte{8}});
  const auto coded = item_data(source, original_meta, original_id);
  Fixture fixture{.icc = std::move(icc), .width = tile_width * 2, .height = tile_height * 2,
      .coded_bytes = static_cast<unsigned>(coded.size())};
  Bytes grid{std::byte{0}, std::byte{0}, std::byte{1}, std::byte{1}};
  u16(grid, fixture.width); u16(grid, fixture.height);
  Bytes canvas_ispe(4); u32(canvas_ispe, fixture.width); u32(canvas_ispe, fixture.height);
  auto color = text("rICC"); append(color, fixture.icc);
  Bytes auxc(4); append(auxc, text("urn:iso:std:iso:ts:21496:-1")); auxc.push_back(std::byte{0});
  std::vector<Box> props{hvc, ispe, pixi, {"ispe", std::move(canvas_ispe)}, {"colr", std::move(color)},
      {"colr", nclx({2, 2, 5})}, {"colr", nclx({9, 18, 9, false})}, {"auxC", std::move(auxc)}};
  if (base_nclx) props.push_back({"colr", nclx(*base_nclx)});
  auto metadata = iso_tmap_metadata();
  put(metadata, 10, 1'000'000, 4); put(metadata, 14, 2'148'445, 4); put(metadata, 18, 1'000'000, 4);
  put(metadata, 30, 2'148'445, 4); put(metadata, 34, 1'000'000, 4);
  const std::array<unsigned, 4> base_tiles{3, 4, 5, 6}, map_tiles{7, 8, 9, 10};
  enum class Role { paired, base, map };
  const auto container = [&](Role role) {
    const bool paired = role == Role::paired;
    const bool include_base = role != Role::map, include_map = role != Role::base;
    const unsigned primary_id = role == Role::map ? map_id : base_id;
    std::vector<Box> items, refs;
    std::vector<std::pair<unsigned, Bytes>> associations, payloads;
    const auto add_grid = [&](unsigned id, const std::array<unsigned, 4>& tiles, Bytes colors) {
      items.push_back({"infe", infe(id, "grid", id != primary_id)});
      payloads.emplace_back(id, grid);
      refs.push_back({"dimg", reference(id, tiles)});
      associations.emplace_back(id, std::move(colors));
      for (const auto tile : tiles) {
        items.push_back({"infe", infe(tile, "hvc1", true)});
        payloads.emplace_back(tile, coded);
        associations.emplace_back(tile, Bytes{std::byte{0x81}, std::byte{2}, std::byte{3}});
      }
    };
    if (include_base) {
      Bytes colors{std::byte{4}, std::byte{3}, std::byte{5}};
      if (base_nclx) colors.push_back(std::byte{9});
      add_grid(base_id, base_tiles, std::move(colors));
    }
    if (include_map) {
      Bytes colors{std::byte{4}, std::byte{3}, std::byte{6}};
      if (paired) colors.push_back(std::byte{0x88});
      add_grid(map_id, map_tiles, std::move(colors));
    }
    if (paired) {
      items.push_back({"infe", infe(tmap_id, "tmap", true)});
      payloads.emplace_back(tmap_id, metadata);
      associations.emplace_back(tmap_id, Bytes{std::byte{4}, std::byte{3}, std::byte{7}});
      const std::array<unsigned, 2> rendered{base_id, map_id};
      const std::array<unsigned, 1> base{base_id};
      refs.push_back({"dimg", reference(tmap_id, rendered)});
      refs.push_back({"auxl", reference(map_id, base)});
    }
    Bytes iinf(4); u16(iinf, static_cast<unsigned>(items.size())); append(iinf, serialize(items));
    Bytes ipma(4); u32(ipma, static_cast<std::uint32_t>(associations.size()));
    for (const auto& [id, data] : associations) {
      u16(ipma, id); ipma.push_back(std::byte(data.size())); append(ipma, data);
    }
    Bytes pitm(4); u16(pitm, primary_id);
    Bytes iref(4); append(iref, serialize(refs));
    const auto locations = [&](std::uint32_t data_start) {
      Bytes iloc(4); u16(iloc, 0x4440); u16(iloc, static_cast<unsigned>(payloads.size()));
      std::uint32_t offset = data_start;
      for (const auto& [id, data] : payloads) {
        require(data.size() <= UINT32_MAX - offset, "Synthetic HEIC item offset overflow");
        u16(iloc, id); u16(iloc, 0); u32(iloc, 0); u16(iloc, 1); u32(iloc, offset);
        u32(iloc, static_cast<std::uint32_t>(data.size())); offset += static_cast<std::uint32_t>(data.size());
      }
      return iloc;
    };
    std::vector<Box> meta{find(original_meta, "hdlr"), {"pitm", std::move(pitm)}, {"iloc", locations(0)},
        {"iinf", std::move(iinf)}, {"iref", std::move(iref)},
        {"iprp", serialize({{"ipco", serialize(props)}, {"ipma", std::move(ipma)}})}};
    Bytes meta_bytes(4); append(meta_bytes, serialize(meta));
    const auto prefix = serialize({find(original_top, "ftyp"), {"meta", std::move(meta_bytes)}}).size() + 8;
    require(prefix <= UINT32_MAX, "Synthetic HEIC metadata offset overflow");
    find(meta, "iloc").payload = locations(static_cast<std::uint32_t>(prefix));
    meta_bytes = Bytes(4); append(meta_bytes, serialize(meta));
    Bytes mdat;
    for (const auto& [id, data] : payloads) append(mdat, data);
    return serialize({find(original_top, "ftyp"), {"meta", std::move(meta_bytes)}, {"mdat", std::move(mdat)}});
  };
  fixture.paired = container(Role::paired);
  fixture.sdr_reference = container(Role::base);
  fixture.map_reference = container(Role::map);
  return fixture;
}

void check_structure(const Fixture& fixture, std::optional<Nclx> extra_base_nclx = {}) {
  // 从最终序列化字节重新检查关联，不能只对 builder 的局部变量作自证。
  auto top = boxes(fixture.paired);
  auto meta = boxes(View{find(top, "meta").payload}.subspan(4));
  require(read(find(meta, "pitm").payload, 4, 2) == base_id, "ICC fixture primary is not the base grid");
  auto items = boxes(View{find(meta, "iinf").payload}.subspan(6));
  require(items.size() == 11, "ICC fixture did not retain distinct grids and eight coded tiles");
  const auto item_type = [&](unsigned id) {
    const auto item = std::ranges::find_if(items, [&](const Box& box) { return read(box.payload, 4, 2) == id; });
    require(item != items.end(), "ICC fixture item is missing");
    return read(item->payload, 8, 4);
  };
  require(item_type(base_id) == 0x67726964u && item_type(map_id) == 0x67726964u &&
      item_type(tmap_id) == 0x746d6170u, "ICC fixture is not grid/grid/tmap");
  for (unsigned tile = 3; tile <= 10; ++tile) require(item_type(tile) == 0x68766331u, "Grid child is not coded HEVC");
  auto iprp = boxes(find(meta, "iprp").payload);
  auto props = boxes(find(iprp, "ipco").payload);
  const auto& ipma = find(iprp, "ipma").payload;
  require(read(ipma, 0, 4) == 0, "Unexpected ICC fixture ipma encoding");
  std::size_t at = 8;
  unsigned base_colors = 0, map_colors = 0, tmap_colors = 0, base_iccs = 0;
  for (unsigned i = 0; i < read(ipma, 4, 4); ++i) {
    const auto id = read(ipma, at, 2), count = read(ipma, at + 2, 1); at += 3;
    for (unsigned j = 0; j < count; ++j) {
      const auto index = read(ipma, at++, 1) & 127u;
      require(index && index <= props.size(), "ICC fixture property index is invalid");
      const auto& property = props[index - 1];
      if (property.type != "colr") continue;
      require(id == base_id || id == map_id || id == tmap_id, "A coded tile inherited a rendering color property");
      if (id == base_id) {
        ++base_colors;
        if (read(property.payload, 0, 4) == 0x72494343u) {
          ++base_iccs;
          require(View{property.payload}.subspan(4).size() == fixture.icc.size() &&
              std::ranges::equal(View{property.payload}.subspan(4), fixture.icc), "Base grid ICC was not preserved");
        } else require(extra_base_nclx && property.payload == nclx(*extra_base_nclx), "ICC-only base borrowed another item's NCLX");
      } else if (id == map_id) {
        ++map_colors; require(property.payload == nclx({2, 2, 5}), "Map sample grid must retain unspecified primaries/transfer");
      } else {
        ++tmap_colors; require(property.payload == nclx({9, 18, 9, false}), "tmap must retain its own BT.2020/HLG property");
      }
    }
  }
  require(at == ipma.size() && base_iccs == 1 && base_colors == (extra_base_nclx ? 2u : 1u) && map_colors == 1 && tmap_colors == 1,
      "ICC fixture color-property associations differ from the intended structure");
  auto refs = boxes(View{find(meta, "iref").payload}.subspan(4));
  unsigned dimg_count = 0;
  for (const auto& ref : refs) if (ref.type == "dimg") {
    ++dimg_count;
    const auto id = read(ref.payload, 0, 2), count = read(ref.payload, 2, 2);
    require(ref.payload.size() == 4 + count * 2, "Invalid synthetic dimg extent");
    if (id == tmap_id) {
      require(count == 2 && read(ref.payload, 4, 2) == base_id && read(ref.payload, 6, 2) == map_id,
          "tmap must reference [base,map] in that order");
    } else {
      require((id == base_id || id == map_id) && count == 4, "Fixture grid is not a four-tile derived image");
      for (unsigned tile = 0; tile < count; ++tile)
        require(read(ref.payload, 4 + tile * 2, 2) == (id == base_id ? 3u : 7u) + tile, "Grid tile references were aliased");
    }
  }
  require(dimg_count == 3, "Missing independent grid/tmap dimg relationships");
  for (unsigned tile = 3; tile <= 10; ++tile)
    require(item_data(fixture.paired, meta, tile).size() == fixture.coded_bytes, "Synthetic tile lost its public HEVC payload");
  for (const auto id : {base_id, map_id}) {
    const auto grid = item_data(fixture.paired, meta, id);
    require(grid.size() == 8 && read(grid, 0, 4) == 0x00000101u && read(grid, 4, 2) == fixture.width &&
        read(grid, 6, 2) == fixture.height, "Serialized fixture grid canvas is not 2x2");
  }
  const auto metadata = item_data(fixture.paired, meta, tmap_id);
  require(metadata.size() == 62 && read(metadata, 5, 1) == 64 && read(metadata, 6, 4) == 0 &&
      read(metadata, 10, 4) == 1'000'000 && read(metadata, 14, 4) == 2'148'445 && read(metadata, 18, 4) == 1'000'000,
      "ICC fixture headroom or useBaseColorSpace changed");
}

double srgb_eotf(double code) {
  return code <= 0.04045 ? code / 12.92 : std::pow((code + 0.055) / 1.055, 2.4);
}
std::uint16_t pq_code(double linear) {
  // ISO 合成工作白为 203 nit；ST 2084 的 1.0 是 10000 nit，不是 HLG 或 sRGB。
  const auto p = std::pow(std::max(0.0, linear) * (203.0 / 10000.0), 2610.0 / 16384.0);
  const auto encoded = std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
      (1.0 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
  return static_cast<std::uint16_t>(std::lround(std::clamp(encoded, 0.0, 1.0) * 65535.0));
}
struct References { awj::ImageBuffer sdr, hdr; };
References references(const Fixture& fixture) {
  awj::HeifImageDecoder decoder;
  auto base = take(decoder.decode_memory(fixture.sdr_reference, "synthetic independent SDR grid", {.copy_metadata_payloads = true}));
  auto gain = take(decoder.decode_memory(fixture.map_reference, "synthetic independent sample grid", {.copy_metadata_payloads = false}));
  require(!base.gain_map_alternative && !gain.gain_map_alternative && base.image.width == fixture.width &&
      base.image.height == fixture.height && gain.image.width == fixture.width && gain.image.height == fixture.height &&
      base.image.bit_depth == 8 && gain.image.bit_depth == 8 && !base.used_fallback && !gain.used_fallback,
      "Independent fixture references are not ordinary eight-bit grids");
  require(std::ranges::any_of(base.image.metadata, [&](const awj::MetadataBlock& block) {
    return block.kind == awj::MetadataKind::icc && block.bytes == fixture.icc;
  }), "Independent SDR reference lost the synthetic ICC");
  // 由 P3-D65 与 BT.2020 标准 xy 原色推导的线性 RGB→RGB 矩阵；不调用被测 CICP/ICC 映射代码。
  constexpr std::array<std::array<double, 3>, 3> p3_to_bt2020{{
      {0.753833034361722, 0.198597369052377, 0.047569596585901},
      {0.045743848965359, 0.941777219811693, 0.012478931222947},
      {-0.001210340354519, 0.017601717301090, 0.983608623053429}}};
  awj::ImageBuffer hdr{.width = fixture.width, .height = fixture.height, .pixel_format = awj::PixelFormat::rgba,
      .alpha_mode = base.image.alpha_mode, .bit_depth = 16,
      .source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
          .color_primaries = 9, .transfer_characteristics = 16, .matrix_coefficients = 9, .color_range = 1, .has_hdr_metadata = true}};
  hdr.planes.push_back({Bytes(std::size_t(fixture.width) * fixture.height * 8), std::size_t(fixture.width) * 8});
  std::vector<double> input(std::size_t(fixture.width) * 3), output(input.size());
  bool colored = false, unequal_gain = false;
  for (unsigned y = 0; y < fixture.height; ++y) {
    for (unsigned x = 0; x < fixture.width; ++x) {
      const auto red = rgba_sample(base.image, x, y, 0), green = rgba_sample(base.image, x, y, 1), blue = rgba_sample(base.image, x, y, 2);
      colored |= std::max({red, green, blue}) - std::min({red, green, blue}) > 80;
      unequal_gain |= std::abs(int(rgba_sample(gain.image, x, y, 0)) - int(rgba_sample(gain.image, x, y, 1))) > 40;
      for (unsigned c = 0; c < 3; ++c) {
        const auto encoded = rgba_sample(base.image, x, y, c) / 255.0;
        const auto map_code = rgba_sample(gain.image, x, y, c) / 255.0;
        // useBaseColorSpace=true：先在 P3 线性通道中应用增益，之后才转输出色域。
        input[std::size_t(x) * 3 + c] = srgb_eotf(encoded) * std::exp2(headroom * map_code);
      }
    }
    for (unsigned x = 0; x < fixture.width; ++x) for (unsigned c = 0; c < 3; ++c) {
      double linear = 0;
      for (unsigned source_channel = 0; source_channel < 3; ++source_channel)
        linear += p3_to_bt2020[c][source_channel] * input[std::size_t(x) * 3 + source_channel];
      output[std::size_t(x) * 3 + c] = linear;
    }
    for (unsigned x = 0; x < fixture.width; ++x) for (unsigned c = 0; c < 4; ++c) {
      const auto value = c == 3 ? static_cast<std::uint16_t>(rgba_sample(base.image, x, y, 3) * 257u)
          : pq_code(output[std::size_t(x) * 3 + c]);
      std::memcpy(hdr.planes[0].bytes.data() + y * hdr.planes[0].stride + (std::size_t(x) * 4 + c) * 2, &value, 2);
    }
  }
  require(colored && unequal_gain, "Reference samples cannot detect gamut or gain-map color-space ordering errors");
  return {std::move(base.image), std::move(hdr)};
}
void check_sdr(const awj::ImageBuffer& image, const References& expected, bool retain_metadata) {
  require(same_rgba(image, expected.sdr) && image.alpha_mode == expected.sdr.alpha_mode,
      "Existing ICC SDR pixels were tone-mapped, converted or requantized");
  require(image.source_info && image.source_info->source_has_gain_map && !image.source_info->has_hdr_metadata &&
      image.source_info->color_primaries == 12 && image.source_info->transfer_characteristics == 13,
      "ICC SDR interpretation was lost or borrowed from the BT.2020/HLG tmap");
  if (!retain_metadata) require(image.metadata.empty(), "Strip mode retained optional SDR metadata");
}
void check_hdr(const awj::ImageBuffer& image, const References& expected, bool retain_metadata) {
  require(image.width == expected.hdr.width && image.height == expected.hdr.height && image.bit_depth == 16 &&
      image.pixel_format == awj::PixelFormat::rgba && image.sample_representation == awj::SampleRepresentation::unorm &&
      image.alpha_mode == expected.hdr.alpha_mode && image.source_info && image.source_info->source_has_gain_map && image.source_info->has_hdr_metadata &&
      image.source_info->color_primaries == 9 && image.source_info->transfer_characteristics == 16 &&
      image.source_info->matrix_coefficients == 9 && image.source_info->color_range == 1,
      "ICC reconstruction did not produce explicitly interpreted BT.2020/PQ HDR");
  require(std::ranges::none_of(image.metadata, [](const awj::MetadataBlock& block) { return block.kind == awj::MetadataKind::icc; }),
      "Reconstructed HDR retained the SDR ICC");
  if (!retain_metadata) require(image.metadata.empty(), "Strip mode retained optional HDR metadata");
  unsigned maximum_error = 0;
  for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
    for (unsigned c = 0; c < 3; ++c) maximum_error = std::max(maximum_error,
        static_cast<unsigned>(std::abs(int(rgba_sample(image, x, y, c)) - int(rgba_sample(expected.hdr, x, y, c)))));
    require(rgba_sample(image, x, y, 3) == rgba_sample(expected.hdr, x, y, 3), "ICC reconstruction changed alpha");
  }
  if (maximum_error > hdr_code_tolerance) throw std::runtime_error("Independent ICC HDR reference differs by " +
      std::to_string(maximum_error) + " codes; limit is " + std::to_string(hdr_code_tolerance));
}
void check_result(const awj::ImageDecodeResult& result, const References& expected, awj::GainMapDecodeMode mode, bool retain_metadata) {
  require(!result.used_fallback && result.decoder_id.find("iso21496-1") != std::string::npos,
      "ICC fixture used ordinary-image fallback");
  if (mode == awj::GainMapDecodeMode::sdr_only) check_sdr(result.image, expected, retain_metadata);
  else check_hdr(result.image, expected, retain_metadata);
  require(bool(result.gain_map_alternative) == (mode == awj::GainMapDecodeMode::preserve_both), "ICC rendition selection dropped or added an endpoint");
  if (result.gain_map_alternative) {
    const auto& alternate = *result.gain_map_alternative;
    check_sdr(alternate.image, expected, retain_metadata);
    require(std::abs(alternate.main_hdr_headroom_log2 - headroom) < 0.000001 && alternate.alternate_hdr_headroom_log2 == 0,
        "ICC fixture headroom endpoint direction changed");
  }
}
void check_rejection(const awj::compat::expected<awj::ImageDecodeResult, std::string>& result) {
  require(!result && result.error().starts_with("Gain Map:") && result.error().find("ISO 21496-1") != std::string::npos &&
      result.error().find("ICC") != std::string::npos, "Invalid or conflicting ICC was not explicitly rejected at the ISO boundary");
}
void check_failure_facts(const awj::DecodeDiagnostics& facts, std::optional<Nclx> explicit_color) {
  const auto signaled = [](unsigned value) -> std::optional<int> {
    return value == 2 ? std::optional<int>{} : std::optional<int>{static_cast<int>(value)};
  };
  require(facts.stage == "heif.gain-map.render" && facts.decoder_id.find("libheif") != std::string::npos &&
      facts.source_has_icc && !facts.source_has_alpha_channel && facts.source_alpha_mode == awj::AlphaMode::none &&
      facts.source_info && facts.source_info->source_has_gain_map && facts.source_info->has_hdr_metadata,
      "Failed ICC decode lost confirmed input facts or failed at an unexpected stage");
  require(facts.source_info->color_primaries == (explicit_color ? signaled(explicit_color->primaries) : std::optional<int>{}) &&
      facts.source_info->transfer_characteristics == (explicit_color ? signaled(explicit_color->transfer) : std::optional<int>{}),
      "Failure diagnostics replaced the primary container ICC facts with decoded defaults or tmap HDR tags");
}
void contract(const Sandbox& sandbox) {
  using Mode = awj::GainMapDecodeMode;
  const auto fixture = make();
  check_profile(fixture.icc); check_structure(fixture);
  const auto expected = references(fixture);
  const auto file = sandbox.save("icc-only-sdr-grid-gain-map.heic", fixture.paired);
  awj::HeifImageDecoder decoder;
  const auto dimensions = take(decoder.probe_dimensions(file));
  require(dimensions.width == fixture.width && dimensions.height == fixture.height && dimensions.source_has_gain_map &&
      dimensions.gain_map_width == fixture.width && dimensions.gain_map_height == fixture.height &&
      !dimensions.opaque_sdr8_yuv420, "ICC grid dimension probe hid the independent gain map");
  for (const bool retain_metadata : {false, true}) for (const auto mode : {Mode::hdr_only, Mode::sdr_only, Mode::preserve_both}) {
    awj::DecodeDiagnostics memory_facts, file_facts;
    const auto memory = take(decoder.decode_memory(fixture.paired, "synthetic ICC-only grid",
        {.copy_metadata_payloads = retain_metadata, .gain_map_mode = mode, .diagnostics = &memory_facts}));
    const auto disk = take(decoder.decode(file,
        {.copy_metadata_payloads = retain_metadata, .gain_map_mode = mode, .diagnostics = &file_facts}));
    check_result(memory, expected, mode, retain_metadata); check_result(disk, expected, mode, retain_metadata);
    require(same_rgba(memory.image, disk.image), "ICC grid file and memory endpoint pixels differ");
    for (const auto* facts : {&memory_facts, &file_facts}) require(facts->stage == "complete" && facts->source_has_icc &&
        facts->source_info && facts->source_info->source_has_gain_map && facts->source_info->has_hdr_metadata &&
        !facts->source_info->color_primaries && !facts->source_info->transfer_characteristics,
        "Successful ICC decode confused original container facts with normalized endpoint CICP");
  }
  // 常见 D50 xy 近似约定不能被误判为另一色域；像素 oracle 仍只依据原生 D65 P3。
  std::fprintf(stderr, "ICC grid 正向用例：standard-D50-xy-Bradford\n");
  const auto xy_profile = d50_xy_profile_bytes(fixture.icc);
  require(description(xy_profile) == description(fixture.icc), "D50 xy variant changed the synthetic profile description");
  const auto xy_fixture = make(xy_profile);
  check_structure(xy_fixture);
  const auto xy_expected = references(xy_fixture);
  require(same_rgba(xy_expected.sdr, expected.sdr) && same_rgba(xy_expected.hdr, expected.hdr),
      "D50 PCS representation changed the independent native-D65 pixel oracle");
  const auto xy_file = sandbox.save("icc-only-sdr-grid-d50-xy-gain-map.heic", xy_fixture.paired);
  const auto xy_memory = take(decoder.decode_memory(xy_fixture.paired, "synthetic D50 xy ICC-only grid",
      {.copy_metadata_payloads = false, .gain_map_mode = Mode::preserve_both}));
  const auto xy_disk = take(decoder.decode(xy_file, {.copy_metadata_payloads = true, .gain_map_mode = Mode::preserve_both}));
  check_result(xy_memory, xy_expected, Mode::preserve_both, false);
  check_result(xy_disk, xy_expected, Mode::preserve_both, true);
  require(same_rgba(xy_memory.image, xy_disk.image) &&
      same_rgba(xy_memory.gain_map_alternative->image, xy_disk.gain_map_alternative->image),
      "D50 xy ICC grid file/memory endpoint pixels differ");
  // 明确且相符的 NCLX 可以共存；unspecified 不能覆盖已验证的 ICC。
  for (const auto color : {Nclx{12, 13, 1}, Nclx{2, 2, 5}}) {
    const auto compatible = make(fixture.icc, color);
    check_structure(compatible, color);
    const auto low = take(decoder.decode_memory(compatible.paired, "compatible ICC/NCLX", {.gain_map_mode = Mode::sdr_only}));
    const auto ordinary = take(decoder.decode_memory(compatible.sdr_reference, "compatible independent base"));
    require(same_rgba(low.image, ordinary.image) && low.image.source_info && low.image.source_info->color_primaries == 12 &&
        low.image.source_info->transfer_characteristics == 13 && !low.image.source_info->has_hdr_metadata,
        "Compatible or unspecified NCLX changed the existing ICC SDR endpoint");
  }
  auto corrupt = fixture.icc; corrupt[36] = std::byte{0};
  const auto wrong_gamut = profile_bytes(ProfileKind::wrong_gamut), wrong_gamma = profile_bytes(ProfileKind::wrong_gamma);
  require(description(fixture.icc) == profile_name && description(wrong_gamut) == profile_name &&
      description(wrong_gamma) == profile_name && fixture.icc != wrong_gamut && fixture.icc != wrong_gamma,
      "Same-name ICC negative fixtures are not distinct valid profiles");
  struct Invalid { const char* name; Bytes profile; std::optional<Nclx> color; };
  const std::array<Invalid, 6> invalid{{{"corrupt", std::move(corrupt), {}},
      {"non-rgb", profile_bytes(ProfileKind::gray), {}}, {"same-name-wrong-gamut", wrong_gamut, {}},
      {"same-name-wrong-gamma", wrong_gamma, {}}, {"conflicting-primaries", fixture.icc, Nclx{1, 13, 1}},
      {"conflicting-hdr-transfer", fixture.icc, Nclx{12, 18, 9}}}};
  for (const auto& test : invalid) {
    std::fprintf(stderr, "ICC grid 拒绝用例：%s\n", test.name);
    const auto broken = make(test.profile, test.color);
    check_structure(broken, test.color);
    const auto path = sandbox.save(std::string("icc-grid-") + test.name + ".heic", broken.paired);
    for (const auto mode : {Mode::hdr_only, Mode::sdr_only, Mode::preserve_both}) {
      awj::DecodeDiagnostics memory_facts, file_facts;
      check_rejection(decoder.decode_memory(broken.paired, test.name,
          {.copy_metadata_payloads = false, .gain_map_mode = mode, .diagnostics = &memory_facts}));
      check_failure_facts(memory_facts, test.color);
      check_rejection(decoder.decode(path,
          {.copy_metadata_payloads = true, .gain_map_mode = mode, .diagnostics = &file_facts}));
      check_failure_facts(file_facts, test.color);
    }
  }
  awj::compat::stop_source cancellation; cancellation.request_stop();
  awj::HeifImageDecoder canceled{1, cancellation.get_token()};
  for (const auto mode : {Mode::hdr_only, Mode::sdr_only, Mode::preserve_both}) {
    const auto memory = canceled.decode_memory(fixture.paired, "canceled synthetic ICC grid", {.gain_map_mode = mode});
    const auto disk = canceled.decode(file, {.gain_map_mode = mode});
    require(!memory && !disk && memory.error().find("取消") != std::string::npos && disk.error().find("取消") != std::string::npos,
        "ICC grid rendition selection ignored cancellation");
  }
}

Bytes ppm(const awj::ImageBuffer& image) {
  require(image.pixel_format == awj::PixelFormat::rgba && (image.bit_depth == 8 || image.bit_depth == 16), "Cannot export reference pixels");
  auto bytes = text("P6\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n" +
      (image.bit_depth == 8 ? "255\n" : "65535\n"));
  for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) for (unsigned c = 0; c < 3; ++c) {
    const auto value = rgba_sample(image, x, y, c);
    if (image.bit_depth == 8) bytes.push_back(std::byte(value)); else u16(bytes, value);
  }
  return bytes;
}
void export_only(const fs::path& requested) {
  // 显式导出不执行回归矩阵，也不调用被测 Gain Map 合成器生成 HDR 期望。
  require(!requested.empty(), "--export-icc-fixture requires an explicit output directory");
  const auto directory = fs::absolute(requested).lexically_normal();
  require(directory.has_filename() && fs::is_directory(directory.parent_path()) && !fs::exists(directory),
      "ICC export requires a fresh directory under an existing parent; existing files are never overwritten");
  const auto fixture = make(); check_profile(fixture.icc); check_structure(fixture);
  const auto expected = references(fixture);
  const auto d50_fixture = make(d50_xy_profile_bytes(fixture.icc));
  check_structure(d50_fixture);
  auto invalid_icc = fixture.icc;
  invalid_icc[36] = std::byte{0};
  const auto invalid_fixture = make(invalid_icc);
  const auto sdr = ppm(expected.sdr), hdr = ppm(expected.hdr);
  const auto manifest = text("{\n  \"kind\": \"synthetic-icc-only-sdr-grid-iso21496-1\",\n"
      "  \"private_input_used\": false,\n  \"source\": \"locked libheif tests/data/rainbow-451x461.heic HEVC payload only\",\n"
      "  \"libheif_commit\": \"f81f28ac014b1c28dee483ac45b72c6b05bc421a\",\n"
      "  \"width\": " + std::to_string(fixture.width) + ",\n  \"height\": " + std::to_string(fixture.height) + ",\n"
      "  \"base\": {\"id\": 1, \"type\": \"grid\", \"tiles\": [3,4,5,6], \"layout\": [2,2], \"colr\": \"rICC only\", \"nclx\": null, \"icc\": \"synthetic LCMS RGB matrix/TRC Display-P3 D65 + sRGB EOTF; no cicp tag\"},\n"
      "  \"map\": {\"id\": 2, \"type\": \"grid\", \"tiles\": [7,8,9,10], \"layout\": [2,2], \"nclx\": [2,2,5], \"full_range\": true},\n"
      "  \"tmap\": {\"id\": 11, \"dimg\": [1,2], \"nclx\": [9,18,9], \"full_range\": false},\n"
      "  \"metadata\": {\"base_headroom\": [0,1000000], \"alternate_headroom\": [2148445,1000000], \"useBaseColorSpace\": true, \"gain_min\": 0, \"gain_max\": 2.148445, \"gamma\": 1, \"offsets\": [0,0]},\n"
      "  \"files\": {\"input\": \"icc-only-sdr-grid-gain-map.heic\", \"d50_xy_input\": \"icc-only-d50-xy-grid-gain-map.heic\", \"invalid_icc_input\": \"invalid-icc-grid-gain-map.heic\", \"independent_sdr_container\": \"sdr-reference.heic\", \"sdr_rgb\": \"sdr-reference-p3-srgb.ppm\", \"hdr_rgb\": \"hdr-reference-bt2020-pq.ppm\"},\n"
      "  \"reference_method\": \"ordinary base/map grid decode; independent sRGB EOTF, per-channel exp2 gain in P3, standard D65 P3-to-BT2020 matrix, ST2084 at 203 nit diffuse white\",\n"
      "  \"sdr_expected_cicp\": [12,13], \"hdr_expected_cicp\": [9,16,9],\n"
      "  \"assertions\": {\"sdr\": \"exact existing decoded RGB samples, no tone mapping\", \"hdr_max_abs_16bit_code_error\": 64, \"strip\": \"same endpoint pixels and explicit CICP with optional metadata removed\"}\n}\n");
  require(fs::create_directory(directory), "Cannot create fresh ICC fixture export directory");
  const auto save = [&](std::string_view name, View bytes) {
    std::ofstream output(directory / name, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); output.close();
    require(bool(output), "Cannot write synthetic ICC fixture export");
  };
  save("icc-only-sdr-grid-gain-map.heic", fixture.paired);
  save("icc-only-d50-xy-grid-gain-map.heic", d50_fixture.paired);
  save("invalid-icc-grid-gain-map.heic", invalid_fixture.paired);
  save("sdr-reference.heic", fixture.sdr_reference);
  save("sdr-reference-p3-srgb.ppm", sdr);
  save("hdr-reference-bt2020-pq.ppm", hdr);
  save("manifest.json", manifest);
  std::fprintf(stderr, "合成 ICC grid fixture 已导出到：%s\n", directory.string().c_str());
}

} // namespace icc_fixture
