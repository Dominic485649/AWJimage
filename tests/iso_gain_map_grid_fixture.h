#pragma once

// 仅复用锁定公开 HEVC tile；3×4 主图与半尺寸 2×2 辅助图有独立 item 和 extent。
namespace grid12_fixture {

icc_fixture::Fixture make() {
  const auto original = icc_fixture::make();
  auto top = boxes(original.paired);
  auto meta = boxes(View{find(top, "meta").payload}.subspan(4));
  auto iprp = boxes(find(meta, "iprp").payload);
  auto props = boxes(find(iprp, "ipco").payload);
  require(props.size() == 8, "公开 ICC 夹具属性表发生变化");
  const auto tile_width = read(props[1].payload, 4, 4);
  const auto tile_height = read(props[1].payload, 8, 4);
  const auto coded = icc_fixture::item_data(original.paired, meta, 3);
  const auto metadata = icc_fixture::item_data(original.paired, meta, icc_fixture::tmap_id);
  icc_fixture::Fixture fixture{.icc = original.icc, .width = tile_width * 3,
      .height = tile_height * 4, .coded_bytes = original.coded_bytes};
  Bytes canvas(4); u32(canvas, fixture.width); u32(canvas, fixture.height);
  props[3].payload = canvas;
  Bytes small_canvas(4); u32(small_canvas, fixture.width / 2); u32(small_canvas, fixture.height / 2);
  props.push_back({"ispe", std::move(small_canvas)});
  Bytes base_grid{std::byte{0}, std::byte{0}, std::byte{3}, std::byte{2}};
  u16(base_grid, fixture.width); u16(base_grid, fixture.height);
  Bytes map_grid{std::byte{0}, std::byte{0}, std::byte{1}, std::byte{1}};
  u16(map_grid, fixture.width / 2); u16(map_grid, fixture.height / 2);
  std::array<unsigned, 12> base_tiles{};
  std::array<unsigned, 4> map_tiles{};
  for (unsigned i = 0; i < base_tiles.size(); ++i) base_tiles[i] = 3 + i;
  for (unsigned i = 0; i < map_tiles.size(); ++i) map_tiles[i] = 15 + i;
  const auto container = [&](int role) {
    const bool paired = role == 0;
    const unsigned primary = role == 2 ? 2 : 1;
    std::vector<Box> items, refs;
    std::vector<std::pair<unsigned, Bytes>> associations, payloads;
    const auto add_grid = [&](unsigned id, std::span<const unsigned> tiles, const Bytes& grid, Bytes colors) {
      items.push_back({"infe", icc_fixture::infe(id, "grid", id != primary)});
      payloads.emplace_back(id, grid);
      refs.push_back({"dimg", icc_fixture::reference(id, tiles)});
      associations.emplace_back(id, std::move(colors));
      for (const auto tile : tiles) {
        items.push_back({"infe", icc_fixture::infe(tile, "hvc1", true)});
        payloads.emplace_back(tile, coded);
        associations.emplace_back(tile, Bytes{std::byte{0x81}, std::byte{2}, std::byte{3}});
      }
    };
    if (role != 2) add_grid(1, base_tiles, base_grid, Bytes{std::byte{4}, std::byte{3}, std::byte{5}});
    if (role != 1) {
      Bytes colors{std::byte{9}, std::byte{3}, std::byte{6}};
      if (paired) colors.push_back(std::byte{0x88});
      add_grid(2, map_tiles, map_grid, std::move(colors));
    }
    if (paired) {
      items.push_back({"infe", icc_fixture::infe(19, "tmap", true)});
      payloads.emplace_back(19, metadata);
      associations.emplace_back(19, Bytes{std::byte{4}, std::byte{3}, std::byte{7}});
      const std::array<unsigned, 2> rendered{1, 2};
      const std::array<unsigned, 1> base{1};
      refs.push_back({"dimg", icc_fixture::reference(19, rendered)});
      refs.push_back({"auxl", icc_fixture::reference(2, base)});
    }
    Bytes iinf(4); u16(iinf, static_cast<unsigned>(items.size())); append(iinf, serialize(items));
    Bytes ipma(4); u32(ipma, static_cast<std::uint32_t>(associations.size()));
    for (const auto& [id, data] : associations) {
      u16(ipma, id); ipma.push_back(std::byte(data.size())); append(ipma, data);
    }
    Bytes pitm(4); u16(pitm, primary);
    Bytes iref(4); append(iref, serialize(refs));
    const auto locations = [&](std::uint32_t start) {
      Bytes iloc(4); u16(iloc, 0x4440); u16(iloc, static_cast<unsigned>(payloads.size()));
      for (const auto& [id, data] : payloads) {
        require(data.size() <= UINT32_MAX - start, "12+4 夹具 extent 溢出");
        u16(iloc, id); u16(iloc, 0); u32(iloc, 0); u16(iloc, 1); u32(iloc, start);
        u32(iloc, static_cast<std::uint32_t>(data.size())); start += static_cast<std::uint32_t>(data.size());
      }
      return iloc;
    };
    std::vector<Box> parts{find(meta, "hdlr"), {"pitm", std::move(pitm)}, {"iloc", locations(0)},
        {"iinf", std::move(iinf)}, {"iref", std::move(iref)},
        {"iprp", serialize({{"ipco", serialize(props)}, {"ipma", std::move(ipma)}})}};
    Bytes header(4); append(header, serialize(parts));
    const auto start = serialize({find(top, "ftyp"), {"meta", header}}).size() + 8;
    require(start <= UINT32_MAX, "12+4 夹具元数据溢出");
    find(parts, "iloc").payload = locations(static_cast<std::uint32_t>(start));
    header = Bytes(4); append(header, serialize(parts));
    Bytes mdat;
    for (const auto& [id, data] : payloads) append(mdat, data);
    return serialize({find(top, "ftyp"), {"meta", std::move(header)}, {"mdat", std::move(mdat)}});
  };
  fixture.paired = container(0);
  fixture.sdr_reference = container(1);
  fixture.map_reference = container(2);
  return fixture;
}

void contract(const Sandbox& sandbox) {
  const auto fixture = make();
  auto top = boxes(fixture.paired);
  auto meta = boxes(View{find(top, "meta").payload}.subspan(4));
  const auto items = boxes(View{find(meta, "iinf").payload}.subspan(6));
  require(items.size() == 19 && read(find(meta, "pitm").payload, 4, 2) == 1, "12+4 夹具主图或 item 数量不符");
  const auto refs = boxes(View{find(meta, "iref").payload}.subspan(4));
  unsigned grids = 0;
  for (const auto& ref : refs) if (ref.type == "dimg" && read(ref.payload, 0, 2) != 19) {
    const auto id = read(ref.payload, 0, 2), count = read(ref.payload, 2, 2);
    require((id == 1 && count == 12) || (id == 2 && count == 4), "12+4 tile 引用数量不符");
    for (unsigned i = 0; i < count; ++i)
      require(read(ref.payload, 4 + i * 2, 2) == (id == 1 ? 3u : 15u) + i, "12+4 tile item 被别名复用");
    ++grids;
  }
  require(grids == 2, "12+4 夹具缺少独立 grid");
  for (unsigned id = 3; id <= 18; ++id)
    require(icc_fixture::item_data(fixture.paired, meta, id).size() == fixture.coded_bytes, "公开 tile 载荷不完整");
  const auto file = sandbox.save("public-grid12-gain4.heic", fixture.paired);
  awj::HeifImageDecoder serial(1), parallel(4);
  const auto base = take(serial.decode_memory(fixture.sdr_reference, "12-tile ordinary SDR reference"));
  const auto small = take(serial.decode_memory(fixture.map_reference, "4-tile ordinary map reference"));
  require(base.image.width == fixture.width && base.image.height == fixture.height &&
      small.image.width == fixture.width / 2 && small.image.height == fixture.height / 2, "12+4 实际 grid 尺寸不符");
  const auto dimensions = take(parallel.probe_dimensions(file));
  require(dimensions.width == fixture.width && dimensions.height == fixture.height && dimensions.source_has_gain_map &&
      dimensions.gain_map_width == fixture.width / 2 && dimensions.gain_map_height == fixture.height / 2, "12+4 probe 丢失辅助图尺寸");
  for (const auto mode : {awj::GainMapDecodeMode::sdr_only, awj::GainMapDecodeMode::preserve_both}) {
    const auto one = take(serial.decode_memory(fixture.paired, "serial public 12+4 grid", {.gain_map_mode = mode}));
    awj::DecodeDiagnostics facts;
    const auto four = take(parallel.decode(file, {.gain_map_mode = mode, .diagnostics = &facts}));
    require(same_rgba(one.image, four.image) && !one.used_fallback && !four.used_fallback && facts.stage == "complete",
        "12+4 串行/并行解码像素不同或回退");
    if (mode == awj::GainMapDecodeMode::sdr_only) {
      require(same_rgba(four.image, base.image) && !four.gain_map_alternative, "12+4 原 SDR 被重新 tone-map");
    } else {
      require(four.image.bit_depth == 16 && four.image.source_info && four.image.source_info->has_hdr_metadata &&
          four.gain_map_alternative && same_rgba(four.gain_map_alternative->image, base.image), "12+4 未保留 HDR 与原 SDR 两端");
      require(facts.heif_tile_workers > 0 && facts.heif_codec_threads > 0 &&
          facts.heif_tile_workers * facts.heif_codec_threads <= 4, "12+4 HEIF 线程预算相乘越界");
    }
  }
  auto truncated = fixture.paired;
  truncated.resize(truncated.size() - 64);
  require(!parallel.decode_memory(truncated, "truncated public 12+4 grid"), "截断的 12+4 tile 被接受");
  const auto& locations = find(meta, "iloc").payload;
  require(read(locations, 4, 2) == 0x4440 && read(locations, 8, 2) == 1,
      "12+4 受控 iloc 布局发生变化");
  const auto grid_offset = read(locations, 18, 4);
  require(grid_offset + 8 <= fixture.paired.size() && read(fixture.paired, grid_offset, 4) == 0x00000302,
      "12+4 主 grid extent 不符");
  auto inconsistent = fixture.paired;
  inconsistent[grid_offset + 2] = std::byte{255};
  require(!parallel.decode_memory(inconsistent, "inconsistent public grid tile count"), "grid 行数与 tile 引用不符时没有拒绝");
}

}  // namespace grid12_fixture
