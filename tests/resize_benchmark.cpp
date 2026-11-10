#include "../src/compat/stdlib_compat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

import awj.config;
import awj.codec;
import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.resource_planner;
import awj.pixel_conversion;
import awj.decoder_registry;
import awj.png_codec;
import awj.jpegli_codec;
import awj.webp_codec;
import awj.jxl_codec;
import awj.avif_aom_codec;

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr int measured_repeats = 3;
constexpr std::uint64_t memory_budget = 1024ull * 1024 * 1024;

void require(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string{message});
}

template<class Expected> auto take(Expected&& result) {
  if (!result) throw std::runtime_error(result.error());
  return std::move(*result);
}

double process_cpu_seconds() {
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0,
      "读取进程 CPU 时间失败。");
  const auto value = [](FILETIME time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return static_cast<double>(value(kernel) + value(user)) / 10'000'000.0;
#else
  timespec value{};
  require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "读取进程 CPU 时间失败。");
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1e9;
#endif
}

struct Measurement { double wall{}, cpu{}; };
struct Timer {
  double cpu{process_cpu_seconds()};
  Clock::time_point started{Clock::now()};
  Measurement finish() const {
    const auto ended = Clock::now();
    return {std::chrono::duration<double>(ended - started).count(), process_cpu_seconds() - cpu};
  }
};

awj::ResourcePlan resources(int threads) {
  return {.file_parallelism = 1, .encoder_threads_per_file = threads,
      .global_thread_budget = threads, .memory_limit_bytes = memory_budget};
}

std::uint64_t pixel_fingerprint(const awj::ImageBuffer& image) {
  std::uint64_t result = 14695981039346656037ull;
  for (const auto& plane : image.planes) for (auto byte : plane.read_bytes()) {
    result ^= std::to_integer<unsigned char>(byte);
    result *= 1099511628211ull;
  }
  return result;
}

std::string_view layout_name(awj::PixelFormat format) {
  switch (format) {
    case awj::PixelFormat::rgba: return "rgba";
    case awj::PixelFormat::rgb: return "rgb";
    case awj::PixelFormat::gray: return "gray";
    case awj::PixelFormat::yuv444: return "yuv444";
    case awj::PixelFormat::yuv422: return "yuv422";
    case awj::PixelFormat::yuv420: return "yuv420";
    default: return "other";
  }
}

awj::NativeEncodeSettings settings(awj::OutputFormat format, int threads, const awj::ImageBuffer& image) {
  awj::NativeEncodeSettings result{};
  result.output_format = format;
  result.quality = awj::default_quality_for(format);
  result.speed = awj::default_speed_for(format);
  result.bit_depth = take(awj::resolve_rendition_bit_depth(format, image, false));
  result.resources = resources(threads);
  if (format == awj::OutputFormat::jpgli) result.chroma_mode = awj::jpeg_auto_chroma(image);
  return result;
}

struct Sample { std::string_view name; std::size_t width, height; int fixture_depth; };
constexpr std::array<Sample, 3> samples{{{"small-rgb8", 1280, 720, 8},
    {"medium-rgba8", 1920, 1080, 8}, {"large-rgb16", 3840, 2160, 16}}};

struct Csv {
  std::ofstream stream;
  std::size_t rows{};
  explicit Csv(const fs::path& path) : stream(path) {
    require(static_cast<bool>(stream), "创建测量 CSV 失败。");
    stream << "phase,sample,source,codec,width,height,fixture_depth,input_depth,input_layout,scale_percent,thread_budget,repeat,warmup,wall_seconds,cpu_seconds,cpu_sample_long_enough,resize_workers,scratch_bytes,encoded_bytes,encoder_threads,check\n";
    stream << std::fixed << std::setprecision(9);
  }
  void row(std::string_view phase, const Sample& sample, std::string_view source, std::string_view codec,
      const awj::ImageBuffer& input, int scale, int threads, int repeat, Measurement measured,
      const awj::GeometryTransformDiagnostics& geometry = {}, std::uint64_t encoded_bytes = 0,
      int encoder_threads = 0) {
    stream << phase << ',' << sample.name << ',' << source << ',' << codec << ',' << input.width << ','
        << input.height << ',' << sample.fixture_depth << ',' << input.bit_depth << ',' << layout_name(input.pixel_format)
        << ',' << scale << ',' << threads << ',' << repeat << ',' << (repeat == 0 ? 1 : 0)
        << ',' << measured.wall << ',' << measured.cpu << ',' << (measured.wall >= 0.05 ? 1 : 0)
        << ',' << geometry.workers << ',' << geometry.scratch_bytes << ',' << encoded_bytes << ','
        << encoder_threads << ",pass\n";
    stream.flush();
    require(static_cast<bool>(stream), "写入测量 CSV 失败。");
    ++rows;
  }
};

std::unique_ptr<awj::ImageEncoder> encoder(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::png: return std::make_unique<awj::PngImageEncoder>();
    case awj::OutputFormat::jpgli: return std::make_unique<awj::JpegliImageEncoder>();
    case awj::OutputFormat::webp: return std::make_unique<awj::WebPImageEncoder>();
    case awj::OutputFormat::jxl: return std::make_unique<awj::JXLImageEncoder>();
    case awj::OutputFormat::avif: return awj::make_avif_image_encoder(awj::AvifEncoderMode::aom);
    default: throw std::runtime_error("不支持的测量格式。");
  }
}

std::unique_ptr<awj::ImageDecoder> decoder(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::png: return std::make_unique<awj::PngImageDecoder>();
    case awj::OutputFormat::jpgli: return std::make_unique<awj::JpegliImageDecoder>();
    case awj::OutputFormat::webp: return std::make_unique<awj::WebPImageDecoder>();
    case awj::OutputFormat::jxl: return std::make_unique<awj::JXLImageDecoder>(8);
    case awj::OutputFormat::avif: return awj::make_avif_image_decoder(8);
    default: throw std::runtime_error("不支持的测量格式。");
  }
}

void write_bytes(const fs::path& path, const std::vector<std::byte>& bytes) {
  require(!fs::exists(path), "拒绝覆盖已有编码产物。");
  std::ofstream stream{path, std::ios::binary};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(stream), "写入编码产物失败。");
}

int run(const fs::path& fixtures, const fs::path& output) {
  require(fs::is_directory(fixtures), "固定输入目录不存在。");
  require(!fs::exists(output), "测量输出目录必须为全新路径。");
  fs::create_directories(output / "codecs");
  awj::set_process_low_priority();
#ifdef _WIN32
  require(GetPriorityClass(GetCurrentProcess()) == BELOW_NORMAL_PRIORITY_CLASS, "进程优先级不是低于正常。");
#endif
  const auto policy = awj::encoding_defaults::system_memory_policy();
  require(policy.has_value(), "无法确定本次内存策略。");
  const awj::encoding_defaults::ScopedMemoryPolicy memory_scope{*policy};
  Csv csv{output / "measurements.csv"};
  for (const auto& sample : samples) {
    for (const auto source : {std::string_view{"png"}, std::string_view{"jpg"}}) {
      const auto path = fixtures / (std::string{sample.name} + "." + std::string{source});
      require(fs::is_regular_file(path), "缺少固定输入文件。");
      awj::ImageDecodeResult decoded;
      for (int repeat = 0; repeat <= measured_repeats; ++repeat) {
        const Timer timer;
        auto value = awj::decode_image_for_path(path, {.allow_wic_fallback = false, .decode_threads = 8,
            .native_pixels = true, .gain_map_mode = awj::GainMapDecodeMode::sdr_only});
        const auto measured = timer.finish();
        decoded = take(std::move(value));
        require(decoded.image.width == sample.width && decoded.image.height == sample.height, "解码尺寸不匹配。");
        csv.row("decode_file", sample, source, decoded.decoder_id, decoded.image, 100, 8, repeat, measured);
      }
      std::uint64_t scaled_fingerprint{};
      for (int threads : {1, 8}) {
        awj::ImageBuffer scaled;
        awj::AppConfig cfg{};
        cfg.image_size_limit = {.mode = awj::ImageSizeLimitMode::fixed, .scale_percent = 50};
        const auto geometry_plan = take(awj::plan_image_geometry(decoded.image.width, decoded.image.height, cfg));
        for (int repeat = 0; repeat <= measured_repeats; ++repeat) {
          awj::GeometryTransformDiagnostics geometry{};
          const Timer timer;
          auto value = awj::apply_image_geometry(decoded.image, geometry_plan, false, {}, resources(threads), &geometry);
          const auto measured = timer.finish();
          scaled = take(std::move(value));
          require(scaled.width == sample.width / 2 && scaled.height == sample.height / 2, "缩放尺寸不匹配。");
          require(geometry.workers >= 1 && geometry.workers <= threads, "缩放实际 worker 超出预算。");
          const auto fingerprint = pixel_fingerprint(scaled);
          if (!scaled_fingerprint) scaled_fingerprint = fingerprint;
          require(fingerprint == scaled_fingerprint, "线程预算改变了缩放像素指纹。");
          csv.row("resize_only", sample, source, "spline36", decoded.image, 50, threads, repeat, measured, geometry);
        }
        const std::array<std::pair<std::string_view, const awj::ImageBuffer*>, 2> variants{{
            {"encode_full", &decoded.image}, {"encode_scaled", &scaled}}};
        for (const auto& [phase, image] : variants) {
          const auto options = settings(awj::OutputFormat::jpgli, threads, *image);
          for (int repeat = 0; repeat <= measured_repeats; ++repeat) {
            const Timer timer;
            auto value = awj::JpegliImageEncoder{}.encode(*image, options);
            const auto measured = timer.finish();
            const auto encoded = take(std::move(value));
            require(!encoded.encoded.bytes.empty(), "JPEGli 编码为空。");
            csv.row(phase, sample, source, "jpegli", *image, phase == "encode_full" ? 100 : 50,
                threads, repeat, measured, {}, encoded.encoded.bytes.size(), encoded.diagnostics.encoder_threads);
          }
        }
      }
      std::cout << "分阶段测量完成：" << sample.name << '/' << source << '\n' << std::flush;
    }
  }
  const auto small = take(awj::decode_image_for_path(fixtures / "small-rgb8.png",
      {.allow_wic_fallback = false, .native_pixels = true}));
  const std::array<std::pair<awj::OutputFormat, std::string_view>, 5> formats{{
      {awj::OutputFormat::png, "png"}, {awj::OutputFormat::jpgli, "jpegli"},
      {awj::OutputFormat::webp, "webp"}, {awj::OutputFormat::jxl, "jxl"}, {awj::OutputFormat::avif, "avif-aom"}}};
  for (const auto& [format, name] : formats) {
    auto writer = encoder(format);
    auto reader = decoder(format);
    const auto options = settings(format, 8, small.image);
    awj::NativeEncodeResult encoded;
    for (int repeat = 0; repeat <= measured_repeats; ++repeat) {
      const Timer timer;
      auto value = writer->encode(small.image, options);
      const auto measured = timer.finish();
      encoded = take(std::move(value));
      require(!encoded.encoded.bytes.empty(), "格式编码结果为空。");
      csv.row("codec_encode", samples.front(), "rgba8", name, small.image, 100, 8, repeat,
          measured, {}, encoded.encoded.bytes.size(), encoded.diagnostics.encoder_threads);
    }
    write_bytes(output / "codecs" / ("small-rgb8." + std::string{name}), encoded.encoded.bytes);
    for (int repeat = 0; repeat <= measured_repeats; ++repeat) {
      const Timer timer;
      auto value = reader->decode_memory(encoded.encoded.bytes, "resize-benchmark", {.native_pixels = true});
      const auto measured = timer.finish();
      const auto image = take(std::move(value));
      require(image.image.width == small.image.width && image.image.height == small.image.height, "格式往返尺寸不匹配。");
      if (format == awj::OutputFormat::png)
        require(pixel_fingerprint(image.image) == pixel_fingerprint(small.image), "PNG 无损往返指纹不匹配。");
      csv.row("codec_decode_memory", samples.front(), name, reader->id(), image.image, 100, 8, repeat, measured);
    }
    std::cout << "格式测量完成：" << name << " quality=" << options.quality << " speed=" << options.speed
        << " depth=" << options.bit_depth.value_or(0) << '\n' << std::flush;
  }
  std::ofstream environment{output / "environment.txt"};
  environment << "measured_repeats=" << measured_repeats << "\nwarmups_per_condition=1\nrows=" << csv.rows
      << "\nlogical_processors=" << awj::available_processor_count() << "\nmemory_budget_bytes=" << memory_budget
      << "\npriority=below-normal\ncpu_note=单次不足50ms时不用于细粒度CPU结论\n";
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0,
      "读取峰值内存失败。");
  environment << "compiler_msvc=" << _MSC_FULL_VER << "\npriority_class=" << GetPriorityClass(GetCurrentProcess())
      << "\npeak_working_set_bytes=" << counters.PeakWorkingSetSize
      << "\npeak_commit_bytes=" << counters.PeakPagefileUsage << '\n';
  require(GetPriorityClass(GetCurrentProcess()) == BELOW_NORMAL_PRIORITY_CLASS, "测量期间优先级发生变化。");
#else
  rusage usage{};
  require(getrusage(RUSAGE_SELF, &usage) == 0, "读取峰值内存失败。");
  environment << "compiler=" << __VERSION__ << "\npeak_rss_bytes=" << static_cast<std::uint64_t>(usage.ru_maxrss) * 1024 << '\n';
#endif
  require(static_cast<bool>(environment), "写入环境证据失败。");
  require(csv.rows == 208, "测量记录数量不匹配。");
  std::cout << "208 条测量记录及随附检查完成。\n";
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 3, "用法：awj_resize_benchmark <固定输入目录> <全新结果目录>");
    return run(fs::absolute(argv[1]), fs::absolute(argv[2]));
  } catch (const std::exception& error) {
    std::cerr << "性能测量失败：" << error.what() << '\n';
    return 1;
  }
}
