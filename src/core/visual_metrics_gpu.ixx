module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <wrl/client.h>

#include "awj_visual_metric_shaders.hpp"

export module awj.visual_metrics_gpu;

import awj.encoding_defaults;
import awj.image;
import awj.visual_metrics;

namespace awj::visual_metrics_gpu_detail {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

std::expected<std::vector<float>, std::string> make_float_buffer(std::size_t pixel_count,
                                                                 std::string_view context) {
  if (pixel_count == 0) {
    return std::unexpected{std::format("{} 输入为空。", context)};
  }
  if (pixel_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    return std::unexpected{std::format("{} luma buffer 尺寸超过运行时限制。", context)};
  }
  const auto byte_count = pixel_count * sizeof(float);
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{std::format("{} luma buffer 超过当前运行时上限。", context)};
  }
  std::vector<float> buffer;
  try {
    buffer.resize(pixel_count);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"luma buffer 内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"luma buffer 尺寸超过运行时限制。"};
  }
  return buffer;
}

std::expected<void, std::string> resize_uint32_buffer(std::vector<std::uint32_t>& buffer,
                                                      std::size_t word_count,
                                                      std::string_view context) {
  if (word_count == 0) {
    return std::unexpected{std::format("{} 输入 buffer 为空。", context)};
  }
  if (word_count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
    return std::unexpected{std::format("{} 输入 buffer 尺寸超过运行时限制。", context)};
  }
  const auto byte_count = word_count * sizeof(std::uint32_t);
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return std::unexpected{std::format("{} 输入 buffer 超过当前运行时上限。", context)};
  }
  try {
    buffer.resize(word_count);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"输入 buffer 内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"输入 buffer 尺寸超过运行时限制。"};
  }
  return {};
}

// 小图的 Direct3D 初始化、上传和 readback 成本通常高于 CPU SIMD/缓存路径收益。
// visual_quality 搜索只在足够大的图片上创建可复用 session；one-shot 路径阈值更高，
// 避免没有 reference/candidate 复用时反复支付 GPU 传输成本。
constexpr std::size_t kMinimumGpuSessionPixelCount = 1ull * 1000ull * 1000ull;
constexpr std::size_t kMinimumGpuOneShotPixelCount = 2ull * 1000ull * 1000ull;

std::uint32_t dispatch_group_count(std::uint32_t dimension) noexcept {
  return dimension / 16u + (dimension % 16u == 0 ? 0u : 1u);
}

bool valid_constant_buffer_size(std::size_t byte_count) noexcept {
  constexpr std::size_t kConstantBufferRegisterBytes = 16;
  constexpr std::size_t kMaxConstantBufferBytes =
      static_cast<std::size_t>(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT) *
      kConstantBufferRegisterBytes;
  return byte_count != 0 && byte_count % kConstantBufferRegisterBytes == 0 &&
         byte_count <= kMaxConstantBufferBytes &&
         byte_count <= static_cast<std::size_t>(std::numeric_limits<UINT>::max());
}

// 视觉指标 shader 在 CMake 构建期预编译并内嵌到可执行文件，
// 最终用户运行时只创建 compute shader，不再承担 HLSL 编译成本。
struct LumaConstants {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride{};
  std::uint32_t channels{};
};

struct GmsdConstants {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t group_count_x{};
  std::uint32_t reserved0{};
};

struct GmsdPartial {
  float sum{};
  float sum_sq{};
  float count{};
  float padding{};
};

struct DownsampleConstants {
  std::uint32_t source_width{};
  std::uint32_t source_height{};
  std::uint32_t output_width{};
  std::uint32_t output_height{};
};

struct SsimConstants {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t group_count_x{};
  std::uint32_t reserved0{};
};

struct SsimPartial {
  float sum_ref{};
  float sum_candidate{};
  float sum_ref_sq{};
  float sum_candidate_sq{};
  float sum_cross{};
  float count{};
  float padding0{};
  float padding1{};
};

struct MetricLevelInfo {
  std::size_t pixel_count{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t groups_x{};
  std::uint32_t groups_y{};
};

struct D3D11MetricContext {
  ComPtr<ID3D11Device> device{};
  ComPtr<ID3D11DeviceContext> immediate_context{};
  ComPtr<ID3D11ComputeShader> luma_shader{};
  ComPtr<ID3D11ComputeShader> gmsd_shader{};
  ComPtr<ID3D11ComputeShader> downsample_shader{};
  ComPtr<ID3D11ComputeShader> ssim_shader{};
  std::mutex mutex{};
};

struct ReusableStructuredBuffer {
  ComPtr<ID3D11Buffer> buffer{};
  ComPtr<ID3D11ShaderResourceView> srv{};
  ComPtr<ID3D11UnorderedAccessView> uav{};
  std::size_t capacity{};
  std::size_t element_size{};
  UINT bind_flags{};
};

struct ReusableReadbackBuffer {
  ComPtr<ID3D11Buffer> buffer{};
  std::size_t byte_count{};
};

struct ReusableConstantBuffer {
  ComPtr<ID3D11Buffer> buffer{};
  std::size_t byte_count{};
};

struct GpuSessionState {
  D3D11MetricContext* gpu{};
  std::size_t width{};
  std::size_t height{};
  ReusableStructuredBuffer luma_input{};
  ReusableStructuredBuffer reference_luma{};
  ReusableStructuredBuffer candidate_luma{};
  std::array<ReusableStructuredBuffer, 4> reference_ms_ssim_levels{};
  std::array<ReusableStructuredBuffer, 4> candidate_ms_ssim_levels{};
  std::array<MetricLevelInfo, 5> ms_ssim_levels{};
  ReusableReadbackBuffer luma_readback{};
  ReusableConstantBuffer luma_constants{};
  ReusableStructuredBuffer gmsd_partial{};
  ReusableReadbackBuffer gmsd_readback{};
  ReusableConstantBuffer gmsd_constants{};
  ReusableConstantBuffer downsample_constants{};
  ReusableStructuredBuffer ssim_partial{};
  ReusableReadbackBuffer ssim_readback{};
  ReusableConstantBuffer ssim_constants{};
  std::vector<std::uint32_t> input_words{};
  bool candidate_luma_ready{};
  bool reference_ms_ssim_ready{};
  std::size_t candidate_batch_size{1};
};

struct OneShotMetricState {
  GpuSessionState session{};
  ReusableStructuredBuffer luma_output{};
};

std::string format_hresult(std::string_view action, HRESULT hr) {
  return std::format("{} (HRESULT=0x{:08X})", action, static_cast<std::uint32_t>(hr));
}

std::expected<ComPtr<ID3D11ComputeShader>, std::string> create_compute_shader_from_bytecode(
    ID3D11Device& device,
    const void* bytecode,
    std::size_t byte_count,
    std::string_view name) {
  ComPtr<ID3D11ComputeShader> shader;
  const HRESULT shader_result = device.CreateComputeShader(bytecode,
                                                           byte_count,
                                                           nullptr,
                                                           shader.GetAddressOf());
  if (FAILED(shader_result)) {
    return std::unexpected{format_hresult(std::format("Direct3D {} shader 创建失败", name), shader_result)};
  }
  return shader;
}

std::expected<std::unique_ptr<D3D11MetricContext>, std::string> create_metric_context() {
  // Retained for the process lifetime, as are the cached device and shaders.
  static const HMODULE library = LoadLibraryExW(L"d3d11.dll", nullptr,
                                                LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library) return std::unexpected{"系统未提供 Direct3D 11，使用 CPU 指标。"};
  const auto create_device = reinterpret_cast<decltype(&D3D11CreateDevice)>(
      GetProcAddress(library, "D3D11CreateDevice"));
  if (!create_device) return std::unexpected{"Direct3D 11 入口不可用，使用 CPU 指标。"};
  auto context = std::make_unique<D3D11MetricContext>();
  D3D_FEATURE_LEVEL feature_level{};
  constexpr D3D_FEATURE_LEVEL feature_levels[]{D3D_FEATURE_LEVEL_11_0};
  const HRESULT device_result = create_device(nullptr,
                                                  D3D_DRIVER_TYPE_HARDWARE,
                                                  nullptr,
                                                  0,
                                                  feature_levels,
                                                  1,
                                                  D3D11_SDK_VERSION,
                                                  context->device.GetAddressOf(),
                                                  &feature_level,
                                                  context->immediate_context.GetAddressOf());
  if (FAILED(device_result)) {
    return std::unexpected{format_hresult("Direct3D 11 设备创建失败", device_result)};
  }

  auto luma_shader = create_compute_shader_from_bytecode(*context->device.Get(),
                                                        visual_metrics_gpu_shaders::kLumaShaderBytecode,
                                                        visual_metrics_gpu_shaders::kLumaShaderBytecodeSize,
                                                        "luma");
  if (!luma_shader) {
    return std::unexpected{luma_shader.error()};
  }
  context->luma_shader = *luma_shader;

  auto gmsd_shader = create_compute_shader_from_bytecode(*context->device.Get(),
                                                        visual_metrics_gpu_shaders::kGmsdShaderBytecode,
                                                        visual_metrics_gpu_shaders::kGmsdShaderBytecodeSize,
                                                        "gmsd");
  if (!gmsd_shader) {
    return std::unexpected{gmsd_shader.error()};
  }
  context->gmsd_shader = *gmsd_shader;

  auto downsample_shader = create_compute_shader_from_bytecode(*context->device.Get(),
                                                              visual_metrics_gpu_shaders::kDownsampleShaderBytecode,
                                                              visual_metrics_gpu_shaders::kDownsampleShaderBytecodeSize,
                                                              "downsample");
  if (!downsample_shader) {
    return std::unexpected{downsample_shader.error()};
  }
  context->downsample_shader = *downsample_shader;

  auto ssim_shader = create_compute_shader_from_bytecode(*context->device.Get(),
                                                        visual_metrics_gpu_shaders::kMsSsimShaderBytecode,
                                                        visual_metrics_gpu_shaders::kMsSsimShaderBytecodeSize,
                                                        "ms-ssim");
  if (!ssim_shader) {
    return std::unexpected{ssim_shader.error()};
  }
  context->ssim_shader = *ssim_shader;

  return context;
}

void assign_gpu_init_error(std::string& target, const std::string& message) noexcept {
  try {
    target = message;
  } catch (...) {
  }
}

void assign_gpu_init_error(std::string& target, const char* message) noexcept {
  try {
    target = message;
  } catch (...) {
  }
}

std::expected<D3D11MetricContext*, std::string> shared_metric_context() {
  static std::once_flag init_once;
  static std::unique_ptr<D3D11MetricContext> context;
  static std::string init_error;

  try {
    std::call_once(init_once, [] {
      try {
        auto created = create_metric_context();
        if (created) {
          context = std::move(*created);
          return;
        }
        assign_gpu_init_error(init_error, created.error());
      } catch (const std::bad_alloc&) {
        assign_gpu_init_error(init_error, "Direct3D 初始化内存不足。");
      } catch (const std::length_error&) {
        assign_gpu_init_error(init_error, "Direct3D 初始化错误信息超过运行时限制。");
      } catch (const std::exception&) {
        assign_gpu_init_error(init_error, "Direct3D 初始化异常。");
      } catch (...) {
        assign_gpu_init_error(init_error, "Direct3D 初始化发生未知异常。");
      }
    });
  } catch (...) {
    return std::unexpected{"Direct3D 初始化失败。"};
  }

  if (!context) {
    return std::unexpected{init_error.empty() ? std::string{"Direct3D 初始化失败。"} : init_error};
  }
  return context.get();
}

std::expected<OneShotMetricState*, std::string> shared_one_shot_metric_state() {
  auto shared_context = shared_metric_context();
  if (!shared_context) {
    return std::unexpected{shared_context.error()};
  }

  static OneShotMetricState state;
  static std::once_flag init_once;
  std::call_once(init_once, [&] {
    state.session.gpu = *shared_context;
  });
  return &state;
}

bool has_minimum_pixel_count(std::size_t width,
                             std::size_t height,
                             std::size_t minimum_pixel_count) noexcept {
  return height != 0 && width <= std::numeric_limits<std::size_t>::max() / height &&
         width * height >= minimum_pixel_count;
}

bool should_use_gpu_session(std::size_t width, std::size_t height) noexcept {
  return has_minimum_pixel_count(width, height, kMinimumGpuSessionPixelCount);
}

bool should_use_gpu_one_shot(std::size_t width, std::size_t height) noexcept {
  return has_minimum_pixel_count(width, height, kMinimumGpuOneShotPixelCount);
}

std::expected<UINT, std::string> checked_dispatch_batch_size(
    std::size_t batch_size,
    std::string_view context) {
  if (batch_size == 0 ||
      batch_size > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      batch_size > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
    return std::unexpected{
        std::format("{} batch size 超过 GPU dispatch 限制。", context)};
  }
  return static_cast<UINT>(batch_size);
}

class MappedSubresourceGuard {
 public:
  MappedSubresourceGuard(ID3D11DeviceContext* context, ID3D11Resource* resource) noexcept
      : context_{context}, resource_{resource} {}
  MappedSubresourceGuard(const MappedSubresourceGuard&) = delete;
  MappedSubresourceGuard& operator=(const MappedSubresourceGuard&) = delete;
  ~MappedSubresourceGuard() noexcept {
    if (context_ != nullptr && resource_ != nullptr) {
      context_->Unmap(resource_, 0);
    }
  }

 private:
  ID3D11DeviceContext* context_{};
  ID3D11Resource* resource_{};
};

std::expected<ComPtr<ID3D11Buffer>, std::string> create_structured_buffer(
    ID3D11Device& device,
    const void* data,
    std::size_t element_count,
    std::size_t element_size,
    UINT bind_flags,
    std::string_view context) {
  if (element_count == 0 || element_size == 0 ||
      element_size > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) ||
      element_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / element_size) {
    return std::unexpected{std::format("{} buffer 超过 GPU 资源限制。", context)};
  }

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = static_cast<UINT>(element_count * element_size);
  desc.Usage = data == nullptr ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = bind_flags;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  desc.StructureByteStride = static_cast<UINT>(element_size);

  D3D11_SUBRESOURCE_DATA initial_data{};
  initial_data.pSysMem = data;

  ComPtr<ID3D11Buffer> buffer;
  const HRESULT result = device.CreateBuffer(&desc, data == nullptr ? nullptr : &initial_data, buffer.GetAddressOf());
  if (FAILED(result)) {
    return std::unexpected{format_hresult(std::format("{} buffer 创建失败", context), result)};
  }
  return buffer;
}

std::expected<ComPtr<ID3D11Buffer>, std::string> create_constant_buffer(
    ID3D11Device& device,
    const void* data,
    std::size_t byte_count,
    std::string_view context) {
  if (data == nullptr || !valid_constant_buffer_size(byte_count)) {
    return std::unexpected{std::format("{} 常量 buffer 尺寸不符合 Direct3D 限制。", context)};
  }

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = static_cast<UINT>(byte_count);
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

  D3D11_SUBRESOURCE_DATA initial_data{};
  initial_data.pSysMem = data;

  ComPtr<ID3D11Buffer> buffer;
  const HRESULT result = device.CreateBuffer(&desc, &initial_data, buffer.GetAddressOf());
  if (FAILED(result)) {
    return std::unexpected{format_hresult(std::format("{} 常量 buffer 创建失败", context), result)};
  }
  return buffer;
}

std::expected<ComPtr<ID3D11Buffer>, std::string> create_readback_buffer(
    ID3D11Device& device,
    std::size_t byte_count,
    std::string_view context) {
  if (byte_count == 0 || byte_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
    return std::unexpected{std::format("{} 回读 buffer 超过 GPU 资源限制。", context)};
  }

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = static_cast<UINT>(byte_count);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ComPtr<ID3D11Buffer> buffer;
  const HRESULT result = device.CreateBuffer(&desc, nullptr, buffer.GetAddressOf());
  if (FAILED(result)) {
    return std::unexpected{format_hresult(std::format("{} 回读 buffer 创建失败", context), result)};
  }
  return buffer;
}

std::expected<ComPtr<ID3D11ShaderResourceView>, std::string> create_buffer_srv(
    ID3D11Device& device,
    ID3D11Buffer& buffer,
    std::size_t element_count,
    std::string_view context) {
  if (element_count == 0 || element_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
    return std::unexpected{std::format("{} 输入视图超过 GPU 资源限制。", context)};
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = 0;
  desc.Buffer.NumElements = static_cast<UINT>(element_count);

  ComPtr<ID3D11ShaderResourceView> view;
  const HRESULT result = device.CreateShaderResourceView(&buffer, &desc, view.GetAddressOf());
  if (FAILED(result)) {
    return std::unexpected{format_hresult(std::format("{} 输入视图创建失败", context), result)};
  }
  return view;
}

std::expected<ComPtr<ID3D11UnorderedAccessView>, std::string> create_buffer_uav(
    ID3D11Device& device,
    ID3D11Buffer& buffer,
    std::size_t element_count,
    std::string_view context) {
  if (element_count == 0 || element_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
    return std::unexpected{std::format("{} 输出视图超过 GPU 资源限制。", context)};
  }

  D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = 0;
  desc.Buffer.NumElements = static_cast<UINT>(element_count);

  ComPtr<ID3D11UnorderedAccessView> view;
  const HRESULT result = device.CreateUnorderedAccessView(&buffer, &desc, view.GetAddressOf());
  if (FAILED(result)) {
    return std::unexpected{format_hresult(std::format("{} 输出视图创建失败", context), result)};
  }
  return view;
}

struct LumaDispatchInfo {
  std::size_t pixel_count{};
  std::size_t input_byte_count{};
  std::size_t input_word_count{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride{};
  std::uint32_t channels{};
  std::uint32_t groups_x{};
  std::uint32_t groups_y{};
};

std::expected<LumaDispatchInfo, std::string> describe_luma_dispatch(const ImageBuffer& image) {
  if (image.bit_depth != 8 || image.planes.empty() ||
      (image.pixel_format != PixelFormat::rgba && image.pixel_format != PixelFormat::rgb)) {
    return std::unexpected{"Direct3D luma 仅支持 8-bit RGB/RGBA 图像。"};
  }
  if (image.width == 0 || image.height == 0) {
    return std::unexpected{"Direct3D luma 输入图像为空。"};
  }
  if (image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected{"Direct3D luma 输入尺寸超过 GPU 资源限制。"};
  }
  if (image.width > std::numeric_limits<std::size_t>::max() / image.height) {
    return std::unexpected{"Direct3D luma 输入尺寸过大。"};
  }

  const auto& plane = image.planes.front();
  const std::size_t channels = image.pixel_format == PixelFormat::rgba ? 4 : 3;
  if (image.width > std::numeric_limits<std::size_t>::max() / channels) {
    return std::unexpected{"Direct3D luma 输入宽度过大。"};
  }
  const std::size_t min_stride = image.width * channels;
  if (!plane.contains_rows(image.height, min_stride) ||
      plane.stride > std::numeric_limits<std::size_t>::max() / image.height) {
    return std::unexpected{"Direct3D luma 输入 buffer 尺寸无效。"};
  }
  if (plane.stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected{"Direct3D luma stride 超过 GPU 资源限制。"};
  }

  const std::size_t pixel_count = image.width * image.height;
  if (pixel_count == 0 || pixel_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(float)) {
    return std::unexpected{"Direct3D luma 输出 buffer 超过 GPU 资源限制。"};
  }

  const std::uint32_t width = static_cast<std::uint32_t>(image.width);
  const std::uint32_t height = static_cast<std::uint32_t>(image.height);
  const std::uint32_t groups_x = dispatch_group_count(width);
  const std::uint32_t groups_y = dispatch_group_count(height);
  if (groups_x > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) {
    return std::unexpected{"Direct3D luma dispatch 尺寸超过 GPU 资源限制。"};
  }

  const std::size_t input_byte_count = plane.stride * (image.height - 1) + min_stride;
  const std::size_t input_word_count =
      input_byte_count / sizeof(std::uint32_t) +
      (input_byte_count % sizeof(std::uint32_t) == 0 ? 0u : 1u);
  if (input_word_count == 0 || input_word_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) ||
      input_word_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(std::uint32_t)) {
    return std::unexpected{"Direct3D luma 输入 buffer 超过 GPU 资源限制。"};
  }

  return LumaDispatchInfo{.pixel_count = pixel_count,
                          .input_byte_count = input_byte_count,
                          .input_word_count = input_word_count,
                          .width = width,
                          .height = height,
                          .stride = static_cast<std::uint32_t>(plane.stride),
                          .channels = static_cast<std::uint32_t>(channels),
                          .groups_x = groups_x,
                          .groups_y = groups_y};
}

std::expected<void, std::string> ensure_structured_buffer(D3D11MetricContext& gpu,
                                                          ReusableStructuredBuffer& slot,
                                                          std::size_t element_count,
                                                          std::size_t element_size,
                                                          UINT bind_flags,
                                                          std::string_view context) {
  if (element_count == 0 || element_size == 0 ||
      element_size > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) ||
      element_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / element_size) {
    return std::unexpected{std::format("{} buffer 超过 GPU 资源限制。", context)};
  }
  if (slot.buffer && slot.capacity >= element_count && slot.element_size == element_size &&
      slot.bind_flags == bind_flags) {
    return {};
  }

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = static_cast<UINT>(element_count * element_size);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = bind_flags;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  desc.StructureByteStride = static_cast<UINT>(element_size);

  ReusableStructuredBuffer next{};
  const HRESULT result = gpu.device->CreateBuffer(&desc, nullptr, next.buffer.GetAddressOf());
  if (FAILED(result)) {
    return std::unexpected{format_hresult(std::format("{} buffer 创建失败", context), result)};
  }

  if ((bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0u) {
    auto srv = create_buffer_srv(*gpu.device.Get(), *next.buffer.Get(), element_count, context);
    if (!srv) {
      return std::unexpected{srv.error()};
    }
    next.srv = *srv;
  }
  if ((bind_flags & D3D11_BIND_UNORDERED_ACCESS) != 0u) {
    auto uav = create_buffer_uav(*gpu.device.Get(), *next.buffer.Get(), element_count, context);
    if (!uav) {
      return std::unexpected{uav.error()};
    }
    next.uav = *uav;
  }

  next.capacity = element_count;
  next.element_size = element_size;
  next.bind_flags = bind_flags;
  slot = std::move(next);
  return {};
}

std::expected<void, std::string> ensure_readback_buffer(D3D11MetricContext& gpu,
                                                        ReusableReadbackBuffer& slot,
                                                        std::size_t byte_count,
                                                        std::string_view context) {
  if (slot.buffer && slot.byte_count == byte_count) {
    return {};
  }
  auto buffer = create_readback_buffer(*gpu.device.Get(), byte_count, context);
  if (!buffer) {
    return std::unexpected{buffer.error()};
  }
  slot.buffer = *buffer;
  slot.byte_count = byte_count;
  return {};
}

std::expected<void, std::string> ensure_constant_buffer(D3D11MetricContext& gpu,
                                                        ReusableConstantBuffer& slot,
                                                        std::size_t byte_count,
                                                        std::string_view context) {
  if (!valid_constant_buffer_size(byte_count)) {
    return std::unexpected{std::format("{} 常量 buffer 尺寸不符合 Direct3D 限制。", context)};
  }
  if (slot.buffer && slot.byte_count == byte_count) {
    return {};
  }

  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = static_cast<UINT>(byte_count);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

  ComPtr<ID3D11Buffer> buffer;
  const HRESULT result = gpu.device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf());
  if (FAILED(result)) {
    return std::unexpected{format_hresult(std::format("{} 常量 buffer 创建失败", context), result)};
  }
  slot.buffer = buffer;
  slot.byte_count = byte_count;
  return {};
}

std::expected<LumaDispatchInfo, std::string> write_luma_buffer(GpuSessionState& session,
                                                               const ImageBuffer& image,
                                                               ReusableStructuredBuffer& output) {
  auto info = describe_luma_dispatch(image);
  if (!info) {
    return std::unexpected{info.error()};
  }

  const auto& plane = image.planes.front();
  auto input_words = resize_uint32_buffer(session.input_words, info->input_word_count,
                                          "Direct3D luma");
  if (!input_words) {
    return std::unexpected{input_words.error()};
  }
  std::fill(session.input_words.begin(), session.input_words.end(), 0u);
  std::memcpy(session.input_words.data(), plane.read_bytes().data(), info->input_byte_count);

  auto& gpu = *session.gpu;
  auto input_buffer = ensure_structured_buffer(gpu,
                                               session.luma_input,
                                               info->input_word_count,
                                               sizeof(std::uint32_t),
                                               D3D11_BIND_SHADER_RESOURCE,
                                               "Direct3D luma 输入");
  if (!input_buffer) {
    return std::unexpected{input_buffer.error()};
  }
  auto output_buffer = ensure_structured_buffer(gpu,
                                                output,
                                                info->pixel_count,
                                                sizeof(float),
                                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                                "Direct3D luma 输出");
  if (!output_buffer) {
    return std::unexpected{output_buffer.error()};
  }
  auto constants_buffer = ensure_constant_buffer(gpu, session.luma_constants, sizeof(LumaConstants), "Direct3D luma");
  if (!constants_buffer) {
    return std::unexpected{constants_buffer.error()};
  }

  const LumaConstants constants{.width = info->width,
                                .height = info->height,
                                .stride = info->stride,
                                .channels = info->channels};
  const D3D11_BOX input_box{.left = 0,
                            .top = 0,
                            .front = 0,
                            .right = static_cast<UINT>(info->input_word_count * sizeof(std::uint32_t)),
                            .bottom = 1,
                            .back = 1};
  gpu.immediate_context->UpdateSubresource(session.luma_input.buffer.Get(), 0, &input_box,
                                           session.input_words.data(), 0, 0);
  gpu.immediate_context->UpdateSubresource(session.luma_constants.buffer.Get(), 0, nullptr,
                                           &constants, 0, 0);

  ID3D11ShaderResourceView* shader_resources[]{session.luma_input.srv.Get()};
  ID3D11UnorderedAccessView* unordered_views[]{output.uav.Get()};
  ID3D11Buffer* constant_buffers[]{session.luma_constants.buffer.Get()};
  gpu.immediate_context->CSSetShader(gpu.luma_shader.Get(), nullptr, 0);
  gpu.immediate_context->CSSetShaderResources(0, 1, shader_resources);
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, unordered_views, nullptr);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, constant_buffers);
  gpu.immediate_context->Dispatch(info->groups_x, info->groups_y, 1);

  ID3D11UnorderedAccessView* null_uavs[]{nullptr};
  ID3D11ShaderResourceView* null_srvs[]{nullptr};
  ID3D11Buffer* null_cbs[]{nullptr};
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
  gpu.immediate_context->CSSetShaderResources(0, 1, null_srvs);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, null_cbs);
  gpu.immediate_context->CSSetShader(nullptr, nullptr, 0);
  session.candidate_batch_size = 1;
  return *info;
}

std::expected<LumaDispatchInfo, std::string> write_luma_buffer_batch(
    GpuSessionState& session,
    std::span<const ImageBuffer> images,
    ReusableStructuredBuffer& output) {
  if (images.empty()) {
    return std::unexpected{"Direct3D luma batch 输入为空。"};
  }
  const auto batch_size = checked_dispatch_batch_size(images.size(), "Direct3D luma");
  if (!batch_size) {
    return std::unexpected{batch_size.error()};
  }

  auto first_info = describe_luma_dispatch(images.front());
  if (!first_info) {
    return std::unexpected{first_info.error()};
  }
  for (std::size_t index = 1; index < images.size(); ++index) {
    auto current = describe_luma_dispatch(images[index]);
    if (!current) {
      return std::unexpected{current.error()};
    }
    if (current->width != first_info->width ||
        current->height != first_info->height ||
        current->stride != first_info->stride ||
        current->channels != first_info->channels ||
        current->input_byte_count != first_info->input_byte_count ||
        current->input_word_count != first_info->input_word_count ||
        current->pixel_count != first_info->pixel_count) {
      return std::unexpected{
          "Direct3D luma batch 只支持尺寸、stride 和像素布局一致的候选。"};
    }
  }

  if (first_info->input_word_count >
          std::numeric_limits<std::size_t>::max() / images.size() ||
      first_info->pixel_count >
          std::numeric_limits<std::size_t>::max() / images.size() ||
      first_info->input_word_count >
          static_cast<std::size_t>(std::numeric_limits<UINT>::max()) /
              images.size() ||
      first_info->pixel_count >
          static_cast<std::size_t>(std::numeric_limits<UINT>::max()) /
              images.size()) {
    return std::unexpected{"Direct3D luma batch buffer 超过 GPU 资源限制。"};
  }
  const auto total_input_words = first_info->input_word_count * images.size();
  const auto total_output_pixels = first_info->pixel_count * images.size();

  auto input_words = resize_uint32_buffer(session.input_words,
                                          total_input_words,
                                          "Direct3D luma batch");
  if (!input_words) {
    return std::unexpected{input_words.error()};
  }
  std::fill(session.input_words.begin(), session.input_words.end(), 0u);
  for (std::size_t index = 0; index < images.size(); ++index) {
    const auto& plane = images[index].planes.front();
    auto* target = session.input_words.data() +
                   index * first_info->input_word_count;
    std::memcpy(target, plane.read_bytes().data(), first_info->input_byte_count);
  }

  auto& gpu = *session.gpu;
  auto input_buffer = ensure_structured_buffer(gpu,
                                               session.luma_input,
                                               total_input_words,
                                               sizeof(std::uint32_t),
                                               D3D11_BIND_SHADER_RESOURCE,
                                               "Direct3D luma batch 输入");
  if (!input_buffer) {
    return std::unexpected{input_buffer.error()};
  }
  auto output_buffer = ensure_structured_buffer(gpu,
                                                output,
                                                total_output_pixels,
                                                sizeof(float),
                                                D3D11_BIND_SHADER_RESOURCE |
                                                    D3D11_BIND_UNORDERED_ACCESS,
                                                "Direct3D luma batch 输出");
  if (!output_buffer) {
    return std::unexpected{output_buffer.error()};
  }
  auto constants_buffer = ensure_constant_buffer(gpu,
                                                 session.luma_constants,
                                                 sizeof(LumaConstants),
                                                 "Direct3D luma batch");
  if (!constants_buffer) {
    return std::unexpected{constants_buffer.error()};
  }

  const LumaConstants constants{.width = first_info->width,
                                .height = first_info->height,
                                .stride = first_info->stride,
                                .channels = first_info->channels};
  const auto input_byte_count = total_input_words * sizeof(std::uint32_t);
  const D3D11_BOX input_box{.left = 0,
                            .top = 0,
                            .front = 0,
                            .right = static_cast<UINT>(input_byte_count),
                            .bottom = 1,
                            .back = 1};
  gpu.immediate_context->UpdateSubresource(session.luma_input.buffer.Get(),
                                           0,
                                           &input_box,
                                           session.input_words.data(),
                                           0,
                                           0);
  gpu.immediate_context->UpdateSubresource(session.luma_constants.buffer.Get(),
                                           0,
                                           nullptr,
                                           &constants,
                                           0,
                                           0);

  ID3D11ShaderResourceView* shader_resources[]{session.luma_input.srv.Get()};
  ID3D11UnorderedAccessView* unordered_views[]{output.uav.Get()};
  ID3D11Buffer* constant_buffers[]{session.luma_constants.buffer.Get()};
  gpu.immediate_context->CSSetShader(gpu.luma_shader.Get(), nullptr, 0);
  gpu.immediate_context->CSSetShaderResources(0, 1, shader_resources);
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, unordered_views, nullptr);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, constant_buffers);
  gpu.immediate_context->Dispatch(first_info->groups_x,
                                  first_info->groups_y,
                                  *batch_size);

  ID3D11UnorderedAccessView* null_uavs[]{nullptr};
  ID3D11ShaderResourceView* null_srvs[]{nullptr};
  ID3D11Buffer* null_cbs[]{nullptr};
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
  gpu.immediate_context->CSSetShaderResources(0, 1, null_srvs);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, null_cbs);
  gpu.immediate_context->CSSetShader(nullptr, nullptr, 0);
  session.candidate_batch_size = images.size();
  return *first_info;
}

std::expected<LumaImage, std::string> read_luma_buffer(GpuSessionState& session,
                                                       ReusableStructuredBuffer& output,
                                                       const LumaDispatchInfo& info) {
  auto& gpu = *session.gpu;
  const auto readback_byte_count = info.pixel_count * sizeof(float);
  auto readback = ensure_readback_buffer(gpu,
                                         session.luma_readback,
                                         readback_byte_count,
                                         "Direct3D luma");
  if (!readback) {
    return std::unexpected{readback.error()};
  }

  LumaImage luma{.width = info.width, .height = info.height};
  auto luma_pixels = make_float_buffer(info.pixel_count, "Direct3D luma 回读");
  if (!luma_pixels) {
    return std::unexpected{luma_pixels.error()};
  }
  luma.pixels = std::move(*luma_pixels);

  const D3D11_BOX source_box{.left = 0,
                             .top = 0,
                             .front = 0,
                             .right = static_cast<UINT>(readback_byte_count),
                             .bottom = 1,
                             .back = 1};
  gpu.immediate_context->CopySubresourceRegion(session.luma_readback.buffer.Get(),
                                               0,
                                               0,
                                               0,
                                               0,
                                               output.buffer.Get(),
                                               0,
                                               &source_box);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT map_result = gpu.immediate_context->Map(session.luma_readback.buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(map_result)) {
    return std::unexpected{format_hresult("Direct3D luma 回读失败", map_result)};
  }

  const MappedSubresourceGuard map_guard{gpu.immediate_context.Get(), session.luma_readback.buffer.Get()};

  const auto* pixels = static_cast<const float*>(mapped.pData);
  for (std::size_t index = 0; index < info.pixel_count; ++index) {
    luma.pixels[index] = std::clamp(pixels[index], 0.0f, 1.0f);
  }
  return luma;
}

constexpr std::array<double, 5> kMsSsimWeights{0.0448, 0.2856, 0.3001, 0.2363, 0.1333};

std::expected<MetricLevelInfo, std::string> make_metric_level_info(std::size_t width,
                                                                  std::size_t height,
                                                                  std::string_view context) {
  if (width == 0 || height == 0 || width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      width > std::numeric_limits<std::size_t>::max() / height) {
    return std::unexpected{std::format("Direct3D {} 输入尺寸超过 GPU 资源限制。", context)};
  }
  const std::size_t pixel_count = width * height;
  if (pixel_count == 0 || pixel_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
    return std::unexpected{std::format("Direct3D {} buffer 超过 GPU 资源限制。", context)};
  }

  const std::uint32_t u32_width = static_cast<std::uint32_t>(width);
  const std::uint32_t u32_height = static_cast<std::uint32_t>(height);
  const std::uint32_t groups_x = dispatch_group_count(u32_width);
  const std::uint32_t groups_y = dispatch_group_count(u32_height);
  if (groups_x > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > std::numeric_limits<std::uint32_t>::max() / groups_x) {
    return std::unexpected{std::format("Direct3D {} dispatch 尺寸超过 GPU 资源限制。", context)};
  }

  return MetricLevelInfo{.pixel_count = pixel_count,
                         .width = u32_width,
                         .height = u32_height,
                         .groups_x = groups_x,
                         .groups_y = groups_y};
}

ReusableStructuredBuffer& ms_ssim_level_buffer(GpuSessionState& session,
                                               bool reference,
                                               std::size_t level) {
  if (level == 0) {
    return reference ? session.reference_luma : session.candidate_luma;
  }
  if (session.ms_ssim_levels[level].width == session.ms_ssim_levels[level - 1].width &&
      session.ms_ssim_levels[level].height == session.ms_ssim_levels[level - 1].height) {
    return ms_ssim_level_buffer(session, reference, level - 1);
  }
  return reference ? session.reference_ms_ssim_levels[level - 1]
                   : session.candidate_ms_ssim_levels[level - 1];
}

std::expected<void, std::string> dispatch_downsample(GpuSessionState& session,
                                                     ReusableStructuredBuffer& source,
                                                     ReusableStructuredBuffer& output,
                                                     const MetricLevelInfo& source_info,
                                                     const MetricLevelInfo& output_info,
                                                     std::string_view context,
                                                     std::size_t batch_size = 1) {
  const auto checked_batch_size = checked_dispatch_batch_size(batch_size, context);
  if (!checked_batch_size) {
    return std::unexpected{checked_batch_size.error()};
  }
  if (output_info.pixel_count >
      static_cast<std::size_t>(std::numeric_limits<UINT>::max()) /
          batch_size) {
    return std::unexpected{std::format("{} buffer 超过 GPU 资源限制。", context)};
  }
  auto& gpu = *session.gpu;
  auto output_buffer = ensure_structured_buffer(gpu,
                                                output,
                                                output_info.pixel_count * batch_size,
                                                sizeof(float),
                                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                                context);
  if (!output_buffer) {
    return std::unexpected{output_buffer.error()};
  }
  auto constants_buffer = ensure_constant_buffer(gpu, session.downsample_constants,
                                                 sizeof(DownsampleConstants), context);
  if (!constants_buffer) {
    return std::unexpected{constants_buffer.error()};
  }

  const DownsampleConstants constants{.source_width = source_info.width,
                                      .source_height = source_info.height,
                                      .output_width = output_info.width,
                                      .output_height = output_info.height};
  gpu.immediate_context->UpdateSubresource(session.downsample_constants.buffer.Get(), 0, nullptr,
                                           &constants, 0, 0);

  ID3D11ShaderResourceView* shader_resources[]{source.srv.Get()};
  ID3D11UnorderedAccessView* unordered_views[]{output.uav.Get()};
  ID3D11Buffer* constant_buffers[]{session.downsample_constants.buffer.Get()};
  gpu.immediate_context->CSSetShader(gpu.downsample_shader.Get(), nullptr, 0);
  gpu.immediate_context->CSSetShaderResources(0, 1, shader_resources);
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, unordered_views, nullptr);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, constant_buffers);
  gpu.immediate_context->Dispatch(output_info.groups_x,
                                  output_info.groups_y,
                                  *checked_batch_size);

  ID3D11UnorderedAccessView* null_uavs[]{nullptr};
  ID3D11ShaderResourceView* null_srvs[]{nullptr};
  ID3D11Buffer* null_cbs[]{nullptr};
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
  gpu.immediate_context->CSSetShaderResources(0, 1, null_srvs);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, null_cbs);
  gpu.immediate_context->CSSetShader(nullptr, nullptr, 0);
  return {};
}

std::expected<void, std::string> build_ms_ssim_levels(GpuSessionState& session,
                                                      bool reference,
                                                      std::size_t batch_size = 1) {
  auto level0 = make_metric_level_info(session.width, session.height, "MS-SSIM");
  if (!level0) {
    return std::unexpected{level0.error()};
  }
  session.ms_ssim_levels[0] = *level0;

  for (std::size_t level = 1; level < session.ms_ssim_levels.size(); ++level) {
    const auto& previous = session.ms_ssim_levels[level - 1];
    const bool keep_previous = previous.width <= 1u || previous.height <= 1u;
    const std::size_t next_width = keep_previous ? previous.width : (static_cast<std::size_t>(previous.width) + 1u) / 2u;
    const std::size_t next_height = keep_previous ? previous.height : (static_cast<std::size_t>(previous.height) + 1u) / 2u;
    auto next = make_metric_level_info(next_width, next_height, "MS-SSIM");
    if (!next) {
      return std::unexpected{next.error()};
    }
    session.ms_ssim_levels[level] = *next;
    if (!keep_previous) {
      auto downsample = dispatch_downsample(session,
                                            ms_ssim_level_buffer(session, reference, level - 1),
                                            reference ? session.reference_ms_ssim_levels[level - 1]
                                                      : session.candidate_ms_ssim_levels[level - 1],
                                            previous,
                                            *next,
                                            reference ? "Direct3D MS-SSIM reference downsample"
                                                      : "Direct3D MS-SSIM candidate downsample",
                                            reference ? 1 : batch_size);
      if (!downsample) {
        return std::unexpected{downsample.error()};
      }
    }
  }
  return {};
}

double ssim_from_partials(double sum_ref,
                          double sum_candidate,
                          double sum_ref_sq,
                          double sum_candidate_sq,
                          double sum_cross,
                          double count) noexcept {
  if (count <= 0.0) {
    return 1.0;
  }

  const double mean_ref = sum_ref / count;
  const double mean_candidate = sum_candidate / count;
  const double variance_ref = sum_ref_sq / count - mean_ref * mean_ref;
  const double variance_candidate = sum_candidate_sq / count - mean_candidate * mean_candidate;
  const double covariance = sum_cross / count - mean_ref * mean_candidate;
  constexpr double k1 = 0.01;
  constexpr double k2 = 0.03;
  constexpr double c1 = k1 * k1;
  constexpr double c2 = k2 * k2;
  const double numerator = (2.0 * mean_ref * mean_candidate + c1) * (2.0 * covariance + c2);
  const double denominator = (mean_ref * mean_ref + mean_candidate * mean_candidate + c1) *
                             (variance_ref + variance_candidate + c2);
  if (denominator <= 0.0) {
    return 1.0;
  }
  return std::clamp(numerator / denominator, 0.0, 1.0);
}

std::expected<std::size_t, std::string> ssim_partial_count_for_level(const MetricLevelInfo& level_info) {
  const std::size_t partial_count = static_cast<std::size_t>(level_info.groups_x) * level_info.groups_y;
  if (partial_count == 0 ||
      partial_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(SsimPartial)) {
    return std::unexpected{"Direct3D MS-SSIM partial buffer 超过 GPU 资源限制。"};
  }
  return partial_count;
}

std::expected<std::size_t, std::string> dispatch_ssim_level_session(GpuSessionState& session,
                                                                    std::size_t level,
                                                                    std::size_t readback_byte_offset,
                                                                    std::size_t batch_size = 1) {
  const auto& level_info = session.ms_ssim_levels[level];
  const auto checked_batch_size = checked_dispatch_batch_size(batch_size, "Direct3D MS-SSIM");
  if (!checked_batch_size) {
    return std::unexpected{checked_batch_size.error()};
  }
  auto partial_count = ssim_partial_count_for_level(level_info);
  if (!partial_count) {
    return std::unexpected{partial_count.error()};
  }
  if (*partial_count >
      static_cast<std::size_t>(std::numeric_limits<UINT>::max()) /
          batch_size / sizeof(SsimPartial)) {
    return std::unexpected{"Direct3D MS-SSIM partial buffer 超过 GPU 资源限制。"};
  }
  const std::size_t total_partial_count = *partial_count * batch_size;
  const std::size_t byte_count = total_partial_count * sizeof(SsimPartial);
  if (!session.ssim_readback.buffer ||
      readback_byte_offset > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) ||
      byte_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) - readback_byte_offset) {
    return std::unexpected{"Direct3D MS-SSIM 回读 buffer 超过 GPU 资源限制。"};
  }

  auto& gpu = *session.gpu;
  auto partial_buffer = ensure_structured_buffer(gpu,
                                                 session.ssim_partial,
                                                 total_partial_count,
                                                 sizeof(SsimPartial),
                                                 D3D11_BIND_UNORDERED_ACCESS,
                                                 "Direct3D MS-SSIM partial");
  if (!partial_buffer) {
    return std::unexpected{partial_buffer.error()};
  }
  auto constants_buffer = ensure_constant_buffer(gpu, session.ssim_constants,
                                                 sizeof(SsimConstants), "Direct3D MS-SSIM");
  if (!constants_buffer) {
    return std::unexpected{constants_buffer.error()};
  }

  const SsimConstants constants{.width = level_info.width,
                                .height = level_info.height,
                                .group_count_x = level_info.groups_x,
                                .reserved0 = static_cast<std::uint32_t>(batch_size)};
  gpu.immediate_context->UpdateSubresource(session.ssim_constants.buffer.Get(), 0, nullptr,
                                           &constants, 0, 0);

  ID3D11ShaderResourceView* shader_resources[]{ms_ssim_level_buffer(session, true, level).srv.Get(),
                                               ms_ssim_level_buffer(session, false, level).srv.Get()};
  ID3D11UnorderedAccessView* unordered_views[]{session.ssim_partial.uav.Get()};
  ID3D11Buffer* constant_buffers[]{session.ssim_constants.buffer.Get()};
  gpu.immediate_context->CSSetShader(gpu.ssim_shader.Get(), nullptr, 0);
  gpu.immediate_context->CSSetShaderResources(0, 2, shader_resources);
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, unordered_views, nullptr);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, constant_buffers);
  gpu.immediate_context->Dispatch(level_info.groups_x,
                                  level_info.groups_y,
                                  *checked_batch_size);

  ID3D11UnorderedAccessView* null_uavs[]{nullptr};
  ID3D11ShaderResourceView* null_srvs[]{nullptr, nullptr};
  ID3D11Buffer* null_cbs[]{nullptr};
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
  gpu.immediate_context->CSSetShaderResources(0, 2, null_srvs);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, null_cbs);
  gpu.immediate_context->CSSetShader(nullptr, nullptr, 0);

  const D3D11_BOX source_box{.left = 0,
                             .top = 0,
                             .front = 0,
                             .right = static_cast<UINT>(byte_count),
                             .bottom = 1,
                             .back = 1};
  gpu.immediate_context->CopySubresourceRegion(session.ssim_readback.buffer.Get(),
                                               0,
                                               static_cast<UINT>(readback_byte_offset),
                                               0,
                                               0,
                                               session.ssim_partial.buffer.Get(),
                                               0,
                                               &source_box);
  return *partial_count;
}

std::expected<double, std::string> compute_ms_ssim_session(GpuSessionState& session) {
  if (!session.reference_luma.srv || !session.candidate_luma.srv || !session.candidate_luma_ready) {
    return std::unexpected{"Direct3D MS-SSIM 输入为空。"};
  }
  if (!session.reference_ms_ssim_ready) {
    auto reference_levels = build_ms_ssim_levels(session, true);
    if (!reference_levels) {
      return std::unexpected{reference_levels.error()};
    }
    session.reference_ms_ssim_ready = true;
  }
  auto candidate_levels = build_ms_ssim_levels(session, false);
  if (!candidate_levels) {
    return std::unexpected{candidate_levels.error()};
  }

  std::array<std::size_t, kMsSsimWeights.size()> partial_offsets{};
  std::array<std::size_t, kMsSsimWeights.size()> partial_counts{};
  std::size_t total_partial_count = 0;
  for (std::size_t level = 0; level < kMsSsimWeights.size(); ++level) {
    auto partial_count = ssim_partial_count_for_level(session.ms_ssim_levels[level]);
    if (!partial_count) {
      return std::unexpected{partial_count.error()};
    }
    partial_offsets[level] = total_partial_count;
    partial_counts[level] = *partial_count;
    if (total_partial_count > std::numeric_limits<std::size_t>::max() - *partial_count) {
      return std::unexpected{"Direct3D MS-SSIM 回读 buffer 超过 GPU 资源限制。"};
    }
    total_partial_count += *partial_count;
  }
  if (total_partial_count == 0 ||
      total_partial_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(SsimPartial)) {
    return std::unexpected{"Direct3D MS-SSIM 回读 buffer 超过 GPU 资源限制。"};
  }

  auto& gpu = *session.gpu;
  auto readback = ensure_readback_buffer(gpu,
                                         session.ssim_readback,
                                         total_partial_count * sizeof(SsimPartial),
                                         "Direct3D MS-SSIM");
  if (!readback) {
    return std::unexpected{readback.error()};
  }

  for (std::size_t level = 0; level < kMsSsimWeights.size(); ++level) {
    auto dispatched = dispatch_ssim_level_session(session, level, partial_offsets[level] * sizeof(SsimPartial));
    if (!dispatched) {
      return std::unexpected{dispatched.error()};
    }
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT map_result = gpu.immediate_context->Map(session.ssim_readback.buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(map_result)) {
    return std::unexpected{format_hresult("Direct3D MS-SSIM 回读失败", map_result)};
  }

  const MappedSubresourceGuard map_guard{gpu.immediate_context.Get(), session.ssim_readback.buffer.Get()};
  const auto* partials = static_cast<const SsimPartial*>(mapped.pData);

  double value = 1.0;
  for (std::size_t level = 0; level < kMsSsimWeights.size(); ++level) {
    double sum_ref = 0.0;
    double sum_candidate = 0.0;
    double sum_ref_sq = 0.0;
    double sum_candidate_sq = 0.0;
    double sum_cross = 0.0;
    double count = 0.0;
    for (std::size_t index = 0; index < partial_counts[level]; ++index) {
      const auto& partial = partials[partial_offsets[level] + index];
      sum_ref += static_cast<double>(partial.sum_ref);
      sum_candidate += static_cast<double>(partial.sum_candidate);
      sum_ref_sq += static_cast<double>(partial.sum_ref_sq);
      sum_candidate_sq += static_cast<double>(partial.sum_candidate_sq);
      sum_cross += static_cast<double>(partial.sum_cross);
      count += static_cast<double>(partial.count);
    }
    const double ssim = ssim_from_partials(sum_ref, sum_candidate, sum_ref_sq, sum_candidate_sq, sum_cross, count);
    value *= std::pow(std::clamp(ssim, 1e-9, 1.0), kMsSsimWeights[level]);
  }
  return std::clamp(value, 0.0, 1.0);
}

std::expected<std::vector<double>, std::string> compute_ms_ssim_session_batch(
    GpuSessionState& session,
    std::size_t batch_size) {
  if (!session.reference_luma.srv || !session.candidate_luma.srv || !session.candidate_luma_ready) {
    return std::unexpected{"Direct3D MS-SSIM 输入为空。"};
  }
  const auto checked_batch_size = checked_dispatch_batch_size(batch_size, "Direct3D MS-SSIM");
  if (!checked_batch_size) {
    return std::unexpected{checked_batch_size.error()};
  }
  if (!session.reference_ms_ssim_ready) {
    auto reference_levels = build_ms_ssim_levels(session, true);
    if (!reference_levels) {
      return std::unexpected{reference_levels.error()};
    }
    session.reference_ms_ssim_ready = true;
  }
  auto candidate_levels = build_ms_ssim_levels(session, false, batch_size);
  if (!candidate_levels) {
    return std::unexpected{candidate_levels.error()};
  }

  std::array<std::size_t, kMsSsimWeights.size()> partial_offsets{};
  std::array<std::size_t, kMsSsimWeights.size()> partial_counts{};
  std::size_t total_partial_count = 0;
  for (std::size_t level = 0; level < kMsSsimWeights.size(); ++level) {
    auto partial_count = ssim_partial_count_for_level(session.ms_ssim_levels[level]);
    if (!partial_count) {
      return std::unexpected{partial_count.error()};
    }
    if (*partial_count >
        std::numeric_limits<std::size_t>::max() / batch_size) {
      return std::unexpected{"Direct3D MS-SSIM 回读 buffer 超过 GPU 资源限制。"};
    }
    const auto level_total = *partial_count * batch_size;
    partial_offsets[level] = total_partial_count;
    partial_counts[level] = *partial_count;
    if (total_partial_count >
        std::numeric_limits<std::size_t>::max() - level_total) {
      return std::unexpected{"Direct3D MS-SSIM 回读 buffer 超过 GPU 资源限制。"};
    }
    total_partial_count += level_total;
  }
  if (total_partial_count == 0 ||
      total_partial_count >
          static_cast<std::size_t>(std::numeric_limits<UINT>::max()) /
              sizeof(SsimPartial)) {
    return std::unexpected{"Direct3D MS-SSIM 回读 buffer 超过 GPU 资源限制。"};
  }

  auto& gpu = *session.gpu;
  auto readback = ensure_readback_buffer(gpu,
                                         session.ssim_readback,
                                         total_partial_count * sizeof(SsimPartial),
                                         "Direct3D MS-SSIM");
  if (!readback) {
    return std::unexpected{readback.error()};
  }

  for (std::size_t level = 0; level < kMsSsimWeights.size(); ++level) {
    auto dispatched = dispatch_ssim_level_session(
        session,
        level,
        partial_offsets[level] * sizeof(SsimPartial),
        batch_size);
    if (!dispatched) {
      return std::unexpected{dispatched.error()};
    }
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT map_result = gpu.immediate_context->Map(session.ssim_readback.buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(map_result)) {
    return std::unexpected{format_hresult("Direct3D MS-SSIM 回读失败", map_result)};
  }

  const MappedSubresourceGuard map_guard{gpu.immediate_context.Get(),
                                         session.ssim_readback.buffer.Get()};
  const auto* partials = static_cast<const SsimPartial*>(mapped.pData);

  std::vector<double> values;
  try {
    values.assign(batch_size, 1.0);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"Direct3D MS-SSIM batch 结果内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"Direct3D MS-SSIM batch 结果数量超过运行时限制。"};
  }

  for (std::size_t candidate_index = 0; candidate_index < batch_size; ++candidate_index) {
    double value = 1.0;
    for (std::size_t level = 0; level < kMsSsimWeights.size(); ++level) {
      double sum_ref = 0.0;
      double sum_candidate = 0.0;
      double sum_ref_sq = 0.0;
      double sum_candidate_sq = 0.0;
      double sum_cross = 0.0;
      double count = 0.0;
      const auto base = partial_offsets[level] +
                        candidate_index * partial_counts[level];
      for (std::size_t index = 0; index < partial_counts[level]; ++index) {
        const auto& partial = partials[base + index];
        sum_ref += static_cast<double>(partial.sum_ref);
        sum_candidate += static_cast<double>(partial.sum_candidate);
        sum_ref_sq += static_cast<double>(partial.sum_ref_sq);
        sum_candidate_sq += static_cast<double>(partial.sum_candidate_sq);
        sum_cross += static_cast<double>(partial.sum_cross);
        count += static_cast<double>(partial.count);
      }
      const double ssim = ssim_from_partials(sum_ref,
                                             sum_candidate,
                                             sum_ref_sq,
                                             sum_candidate_sq,
                                             sum_cross,
                                             count);
      value *= std::pow(std::clamp(ssim, 1e-9, 1.0), kMsSsimWeights[level]);
    }
    values[candidate_index] = std::clamp(value, 0.0, 1.0);
  }
  return values;
}

std::expected<std::vector<double>, std::string> compute_gmsd_session_batch(
    GpuSessionState& session,
    std::size_t batch_size) {
  if (!session.reference_luma.srv || !session.candidate_luma.srv ||
      session.width == 0 || session.height == 0 || !session.candidate_luma_ready) {
    return std::unexpected{"Direct3D GMSD 输入为空。"};
  }
  const auto checked_batch_size = checked_dispatch_batch_size(batch_size, "Direct3D GMSD");
  if (!checked_batch_size) {
    return std::unexpected{checked_batch_size.error()};
  }
  if (session.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      session.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      session.width > std::numeric_limits<std::size_t>::max() / session.height) {
    return std::unexpected{"Direct3D GMSD 输入尺寸超过 GPU 资源限制。"};
  }

  const std::size_t pixel_count = session.width * session.height;
  if (pixel_count == 0 ||
      pixel_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(float)) {
    return std::unexpected{"Direct3D GMSD 输入 buffer 超过 GPU 资源限制。"};
  }

  const std::uint32_t width = static_cast<std::uint32_t>(session.width);
  const std::uint32_t height = static_cast<std::uint32_t>(session.height);
  const std::uint32_t groups_x = dispatch_group_count(width);
  const std::uint32_t groups_y = dispatch_group_count(height);
  if (groups_x > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > std::numeric_limits<std::uint32_t>::max() / groups_x) {
    return std::unexpected{"Direct3D GMSD dispatch 尺寸超过 GPU 资源限制。"};
  }

  const std::size_t partial_count = static_cast<std::size_t>(groups_x) * groups_y;
  if (partial_count == 0 ||
      partial_count >
          static_cast<std::size_t>(std::numeric_limits<UINT>::max()) /
              batch_size / sizeof(GmsdPartial)) {
    return std::unexpected{"Direct3D GMSD partial buffer 超过 GPU 资源限制。"};
  }
  const auto total_partial_count = partial_count * batch_size;

  auto& gpu = *session.gpu;
  auto partial_buffer = ensure_structured_buffer(gpu,
                                                 session.gmsd_partial,
                                                 total_partial_count,
                                                 sizeof(GmsdPartial),
                                                 D3D11_BIND_UNORDERED_ACCESS,
                                                 "Direct3D GMSD partial");
  if (!partial_buffer) {
    return std::unexpected{partial_buffer.error()};
  }
  const auto readback_byte_count = total_partial_count * sizeof(GmsdPartial);
  auto readback = ensure_readback_buffer(gpu,
                                         session.gmsd_readback,
                                         readback_byte_count,
                                         "Direct3D GMSD");
  if (!readback) {
    return std::unexpected{readback.error()};
  }
  auto constants_buffer = ensure_constant_buffer(gpu,
                                                 session.gmsd_constants,
                                                 sizeof(GmsdConstants),
                                                 "Direct3D GMSD");
  if (!constants_buffer) {
    return std::unexpected{constants_buffer.error()};
  }

  const GmsdConstants constants{.width = width,
                                .height = height,
                                .group_count_x = groups_x,
                                .reserved0 = static_cast<std::uint32_t>(batch_size)};
  gpu.immediate_context->UpdateSubresource(session.gmsd_constants.buffer.Get(),
                                           0,
                                           nullptr,
                                           &constants,
                                           0,
                                           0);

  ID3D11ShaderResourceView* shader_resources[]{session.reference_luma.srv.Get(),
                                               session.candidate_luma.srv.Get()};
  ID3D11UnorderedAccessView* unordered_views[]{session.gmsd_partial.uav.Get()};
  ID3D11Buffer* constant_buffers[]{session.gmsd_constants.buffer.Get()};
  gpu.immediate_context->CSSetShader(gpu.gmsd_shader.Get(), nullptr, 0);
  gpu.immediate_context->CSSetShaderResources(0, 2, shader_resources);
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, unordered_views, nullptr);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, constant_buffers);
  gpu.immediate_context->Dispatch(groups_x, groups_y, *checked_batch_size);

  ID3D11UnorderedAccessView* null_uavs[]{nullptr};
  ID3D11ShaderResourceView* null_srvs[]{nullptr, nullptr};
  ID3D11Buffer* null_cbs[]{nullptr};
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
  gpu.immediate_context->CSSetShaderResources(0, 2, null_srvs);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, null_cbs);
  gpu.immediate_context->CSSetShader(nullptr, nullptr, 0);

  const D3D11_BOX source_box{.left = 0,
                             .top = 0,
                             .front = 0,
                             .right = static_cast<UINT>(readback_byte_count),
                             .bottom = 1,
                             .back = 1};
  gpu.immediate_context->CopySubresourceRegion(session.gmsd_readback.buffer.Get(),
                                               0,
                                               0,
                                               0,
                                               0,
                                               session.gmsd_partial.buffer.Get(),
                                               0,
                                               &source_box);

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT map_result = gpu.immediate_context->Map(session.gmsd_readback.buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(map_result)) {
    return std::unexpected{format_hresult("Direct3D GMSD 回读失败", map_result)};
  }

  const MappedSubresourceGuard map_guard{gpu.immediate_context.Get(),
                                         session.gmsd_readback.buffer.Get()};
  const auto* partials = static_cast<const GmsdPartial*>(mapped.pData);

  std::vector<double> values;
  try {
    values.resize(batch_size);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"Direct3D GMSD batch 结果内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"Direct3D GMSD batch 结果数量超过运行时限制。"};
  }
  for (std::size_t candidate_index = 0; candidate_index < batch_size; ++candidate_index) {
    double sum = 0.0;
    double sum_sq = 0.0;
    double count = 0.0;
    const auto base = candidate_index * partial_count;
    for (std::size_t index = 0; index < partial_count; ++index) {
      const auto& partial = partials[base + index];
      sum += static_cast<double>(partial.sum);
      sum_sq += static_cast<double>(partial.sum_sq);
      count += static_cast<double>(partial.count);
    }
    if (count <= 0.0) {
      return std::unexpected{"Direct3D GMSD 输入为空。"};
    }
    const double mean = sum / count;
    const double variance = std::max(0.0, sum_sq / count - mean * mean);
    values[candidate_index] = std::sqrt(variance);
  }
  return values;
}

std::expected<double, std::string> compute_gmsd_session(GpuSessionState& session) {
  if (!session.reference_luma.srv || !session.candidate_luma.srv ||
      session.width == 0 || session.height == 0 || !session.candidate_luma_ready) {
    return std::unexpected{"Direct3D GMSD 输入为空。"};
  }
  if (session.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      session.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      session.width > std::numeric_limits<std::size_t>::max() / session.height) {
    return std::unexpected{"Direct3D GMSD 输入尺寸超过 GPU 资源限制。"};
  }

  const std::size_t pixel_count = session.width * session.height;
  if (pixel_count == 0 || pixel_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(float)) {
    return std::unexpected{"Direct3D GMSD 输入 buffer 超过 GPU 资源限制。"};
  }

  const std::uint32_t width = static_cast<std::uint32_t>(session.width);
  const std::uint32_t height = static_cast<std::uint32_t>(session.height);
  const std::uint32_t groups_x = dispatch_group_count(width);
  const std::uint32_t groups_y = dispatch_group_count(height);
  if (groups_x > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > std::numeric_limits<std::uint32_t>::max() / groups_x) {
    return std::unexpected{"Direct3D GMSD dispatch 尺寸超过 GPU 资源限制。"};
  }

  const std::size_t partial_count = static_cast<std::size_t>(groups_x) * groups_y;
  if (partial_count == 0 ||
      partial_count > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(GmsdPartial)) {
    return std::unexpected{"Direct3D GMSD partial buffer 超过 GPU 资源限制。"};
  }

  auto& gpu = *session.gpu;
  auto partial_buffer = ensure_structured_buffer(gpu,
                                                 session.gmsd_partial,
                                                 partial_count,
                                                 sizeof(GmsdPartial),
                                                 D3D11_BIND_UNORDERED_ACCESS,
                                                 "Direct3D GMSD partial");
  if (!partial_buffer) {
    return std::unexpected{partial_buffer.error()};
  }
  const auto readback_byte_count = partial_count * sizeof(GmsdPartial);
  auto readback = ensure_readback_buffer(gpu,
                                         session.gmsd_readback,
                                         readback_byte_count,
                                         "Direct3D GMSD");
  if (!readback) {
    return std::unexpected{readback.error()};
  }
  auto constants_buffer = ensure_constant_buffer(gpu, session.gmsd_constants, sizeof(GmsdConstants), "Direct3D GMSD");
  if (!constants_buffer) {
    return std::unexpected{constants_buffer.error()};
  }

  const GmsdConstants constants{.width = width,
                                .height = height,
                                .group_count_x = groups_x,
                                .reserved0 = 0};
  gpu.immediate_context->UpdateSubresource(session.gmsd_constants.buffer.Get(), 0, nullptr,
                                           &constants, 0, 0);

  ID3D11ShaderResourceView* shader_resources[]{session.reference_luma.srv.Get(), session.candidate_luma.srv.Get()};
  ID3D11UnorderedAccessView* unordered_views[]{session.gmsd_partial.uav.Get()};
  ID3D11Buffer* constant_buffers[]{session.gmsd_constants.buffer.Get()};
  gpu.immediate_context->CSSetShader(gpu.gmsd_shader.Get(), nullptr, 0);
  gpu.immediate_context->CSSetShaderResources(0, 2, shader_resources);
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, unordered_views, nullptr);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, constant_buffers);
  gpu.immediate_context->Dispatch(groups_x, groups_y, 1);

  ID3D11UnorderedAccessView* null_uavs[]{nullptr};
  ID3D11ShaderResourceView* null_srvs[]{nullptr, nullptr};
  ID3D11Buffer* null_cbs[]{nullptr};
  gpu.immediate_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
  gpu.immediate_context->CSSetShaderResources(0, 2, null_srvs);
  gpu.immediate_context->CSSetConstantBuffers(0, 1, null_cbs);
  gpu.immediate_context->CSSetShader(nullptr, nullptr, 0);
  const D3D11_BOX source_box{.left = 0,
                             .top = 0,
                             .front = 0,
                             .right = static_cast<UINT>(readback_byte_count),
                             .bottom = 1,
                             .back = 1};
  gpu.immediate_context->CopySubresourceRegion(session.gmsd_readback.buffer.Get(),
                                               0,
                                               0,
                                               0,
                                               0,
                                               session.gmsd_partial.buffer.Get(),
                                               0,
                                               &source_box);

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT map_result = gpu.immediate_context->Map(session.gmsd_readback.buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(map_result)) {
    return std::unexpected{format_hresult("Direct3D GMSD 回读失败", map_result)};
  }

  const MappedSubresourceGuard map_guard{gpu.immediate_context.Get(), session.gmsd_readback.buffer.Get()};

  const auto* partials = static_cast<const GmsdPartial*>(mapped.pData);
  double sum = 0.0;
  double sum_sq = 0.0;
  double count = 0.0;
  for (std::size_t index = 0; index < partial_count; ++index) {
    sum += static_cast<double>(partials[index].sum);
    sum_sq += static_cast<double>(partials[index].sum_sq);
    count += static_cast<double>(partials[index].count);
  }

  if (count <= 0.0) {
    return std::unexpected{"Direct3D GMSD 输入为空。"};
  }

  const double mean = sum / count;
  const double variance = std::max(0.0, sum_sq / count - mean * mean);
  return std::sqrt(variance);
}

std::expected<LumaImage, std::string> make_luma_image_d3d11(OneShotMetricState& one_shot,
                                                            const ImageBuffer& image) {
  auto& session = one_shot.session;
  auto info = write_luma_buffer(session, image, one_shot.luma_output);
  if (!info) {
    return std::unexpected{info.error()};
  }
  return read_luma_buffer(session, one_shot.luma_output, *info);
}

std::expected<void, std::string> prepare_luma_pair_d3d11(OneShotMetricState& one_shot,
                                                         const LumaImage& reference,
                                                         const LumaImage& candidate,
                                                         std::string_view context,
                                                         std::string_view reference_context,
                                                         std::string_view candidate_context) {
  if (reference.empty() || candidate.empty()) {
    return std::unexpected{std::format("{} 输入为空。", context)};
  }
  if (reference.width != candidate.width || reference.height != candidate.height ||
      reference.pixels.size() != candidate.pixels.size()) {
    return std::unexpected{std::format("{} 输入尺寸不一致。", context)};
  }
  if (reference.width > std::numeric_limits<std::size_t>::max() / reference.height ||
      reference.width * reference.height != reference.pixels.size()) {
    return std::unexpected{std::format("{} 输入尺寸无效。", context)};
  }
  if (reference.pixels.size() > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) / sizeof(float)) {
    return std::unexpected{std::format("{} 输入 buffer 超过 GPU 资源限制。", context)};
  }

  auto& session = one_shot.session;
  auto& gpu = *session.gpu;
  auto reference_buffer = ensure_structured_buffer(gpu,
                                                   session.reference_luma,
                                                   reference.pixels.size(),
                                                   sizeof(float),
                                                   D3D11_BIND_SHADER_RESOURCE,
                                                   reference_context);
  if (!reference_buffer) {
    return std::unexpected{reference_buffer.error()};
  }
  auto candidate_buffer = ensure_structured_buffer(gpu,
                                                   session.candidate_luma,
                                                   candidate.pixels.size(),
                                                   sizeof(float),
                                                   D3D11_BIND_SHADER_RESOURCE,
                                                   candidate_context);
  if (!candidate_buffer) {
    return std::unexpected{candidate_buffer.error()};
  }

  const auto byte_count = reference.pixels.size() * sizeof(float);
  const D3D11_BOX update_box{.left = 0,
                             .top = 0,
                             .front = 0,
                             .right = static_cast<UINT>(byte_count),
                             .bottom = 1,
                             .back = 1};
  gpu.immediate_context->UpdateSubresource(session.reference_luma.buffer.Get(), 0, &update_box,
                                           reference.pixels.data(), 0, 0);
  gpu.immediate_context->UpdateSubresource(session.candidate_luma.buffer.Get(), 0, &update_box,
                                           candidate.pixels.data(), 0, 0);

  session.width = reference.width;
  session.height = reference.height;
  session.candidate_luma_ready = true;
  session.reference_ms_ssim_ready = false;
  return {};
}

std::expected<double, std::string> compute_gmsd_d3d11(OneShotMetricState& one_shot,
                                                      const LumaImage& reference,
                                                      const LumaImage& candidate) {
  if (auto prepared = prepare_luma_pair_d3d11(one_shot,
                                              reference,
                                              candidate,
                                              "Direct3D GMSD",
                                              "Direct3D GMSD reference",
                                              "Direct3D GMSD candidate");
      !prepared) {
    return std::unexpected{prepared.error()};
  }
  return compute_gmsd_session(one_shot.session);
}

std::expected<double, std::string> compute_ms_ssim_d3d11(OneShotMetricState& one_shot,
                                                         const LumaImage& reference,
                                                         const LumaImage& candidate) {
  if (auto prepared = prepare_luma_pair_d3d11(one_shot,
                                              reference,
                                              candidate,
                                              "Direct3D MS-SSIM",
                                              "Direct3D MS-SSIM reference",
                                              "Direct3D MS-SSIM candidate");
      !prepared) {
    return std::unexpected{prepared.error()};
  }
  return compute_ms_ssim_session(one_shot.session);
}

std::expected<VisualMetricResult, std::string> calculate_visual_metrics_d3d11(
    OneShotMetricState& one_shot,
    const LumaImage& reference,
    const LumaImage& candidate) {
  if (auto prepared = prepare_luma_pair_d3d11(one_shot,
                                              reference,
                                              candidate,
                                              "Direct3D visual metric",
                                              "Direct3D visual metric reference",
                                              "Direct3D visual metric candidate");
      !prepared) {
    return std::unexpected{prepared.error()};
  }
  auto gmsd = compute_gmsd_session(one_shot.session);
  if (!gmsd) {
    return std::unexpected{gmsd.error()};
  }
  auto ms_ssim = compute_ms_ssim_session(one_shot.session);
  if (!ms_ssim) {
    return std::unexpected{ms_ssim.error()};
  }
  return make_visual_metric_result(*gmsd, *ms_ssim);
}
}  // namespace awj::visual_metrics_gpu_detail

export namespace awj {

struct AcceleratedVisualMetricTiming {
  double luma_seconds{};
  double gmsd_seconds{};
  double ms_ssim_seconds{};
};

class AcceleratedVisualMetricSession {
 public:
  AcceleratedVisualMetricSession() noexcept = default;
  AcceleratedVisualMetricSession(const AcceleratedVisualMetricSession&) = delete;
  AcceleratedVisualMetricSession& operator=(const AcceleratedVisualMetricSession&) = delete;
  AcceleratedVisualMetricSession(AcceleratedVisualMetricSession&&) noexcept = default;
  AcceleratedVisualMetricSession& operator=(AcceleratedVisualMetricSession&&) noexcept = default;
  ~AcceleratedVisualMetricSession() = default;

  static std::expected<AcceleratedVisualMetricSession, std::string> create(const ImageBuffer& reference_image) {
    if (!visual_metrics_gpu_detail::should_use_gpu_session(reference_image.width, reference_image.height)) {
      return std::unexpected{"Direct3D visual metric session 已对小图禁用。"};
    }

    auto shared_context = visual_metrics_gpu_detail::shared_metric_context();
    if (!shared_context) {
      return std::unexpected{shared_context.error()};
    }

    AcceleratedVisualMetricSession session;
    std::unique_ptr<visual_metrics_gpu_detail::GpuSessionState> state_holder;
    try {
      state_holder = std::make_unique<visual_metrics_gpu_detail::GpuSessionState>();
    } catch (const std::bad_alloc&) {
      return std::unexpected{"Direct3D visual metric session 内存不足。"};
    }
    auto& state = *state_holder;
    state.gpu = *shared_context;
    state.width = reference_image.width;
    state.height = reference_image.height;

    std::lock_guard lock{state.gpu->mutex};
    auto info = visual_metrics_gpu_detail::write_luma_buffer(state, reference_image, state.reference_luma);
    if (!info) {
      return std::unexpected{info.error()};
    }
    session.state_.reset(state_holder.release());
    return std::move(session);
  }

  std::expected<void, std::string> prepare_candidate_luma(const ImageBuffer& candidate_image) {
    if (state_ == nullptr) {
      return std::unexpected{"Direct3D visual metric session 不可用。"};
    }
    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    state.candidate_luma_ready = false;
    if (candidate_image.width != state.width || candidate_image.height != state.height) {
      return std::unexpected{"Direct3D metric 输入尺寸不一致。"};
    }

    auto info = visual_metrics_gpu_detail::write_luma_buffer(state, candidate_image, state.candidate_luma);
    if (!info) {
      return std::unexpected{info.error()};
    }
    state.candidate_luma_ready = true;
    return {};
  }

  std::expected<LumaImage, std::string> make_candidate_luma(const ImageBuffer& candidate_image) {
    if (state_ == nullptr) {
      return std::unexpected{"Direct3D visual metric session 不可用。"};
    }
    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    state.candidate_luma_ready = false;
    if (candidate_image.width != state.width || candidate_image.height != state.height) {
      return std::unexpected{"Direct3D GMSD 输入尺寸不一致。"};
    }

    auto info = visual_metrics_gpu_detail::write_luma_buffer(state, candidate_image, state.candidate_luma);
    if (!info) {
      return std::unexpected{info.error()};
    }
    auto luma = visual_metrics_gpu_detail::read_luma_buffer(state, state.candidate_luma, *info);
    if (!luma) {
      return std::unexpected{luma.error()};
    }
    state.candidate_luma_ready = true;
    return luma;
  }

  std::expected<double, std::string> compute_gmsd() {
    if (state_ == nullptr) {
      return std::unexpected{"Direct3D visual metric session 不可用。"};
    }
    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    return visual_metrics_gpu_detail::compute_gmsd_session(state);
  }

  std::expected<double, std::string> compute_ms_ssim() {
    if (state_ == nullptr) {
      return std::unexpected{"Direct3D visual metric session 不可用。"};
    }
    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    return visual_metrics_gpu_detail::compute_ms_ssim_session(state);
  }

  std::expected<VisualMetricResult, std::string> calculate_candidate_metrics(
      const ImageBuffer& candidate_image,
      AcceleratedVisualMetricTiming* timing = nullptr) {
    if (state_ == nullptr) {
      return std::unexpected{"Direct3D visual metric session 不可用。"};
    }
    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    state.candidate_luma_ready = false;
    if (candidate_image.width != state.width || candidate_image.height != state.height) {
      return std::unexpected{"Direct3D metric 输入尺寸不一致。"};
    }

    auto started = visual_metrics_gpu_detail::Clock::now();
    auto info = visual_metrics_gpu_detail::write_luma_buffer(state, candidate_image, state.candidate_luma);
    if (timing != nullptr) {
      timing->luma_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    }
    if (!info) {
      return std::unexpected{info.error()};
    }
    // candidate luma 常驻 GPU：后续 GMSD 与 MS-SSIM 直接复用同一 structured buffer，
    // 不回读到 CPU；只有 GPU 失败并由调用方 fallback 时才重新走 CPU luma/metric。
    state.candidate_luma_ready = true;

    started = visual_metrics_gpu_detail::Clock::now();
    auto gmsd = visual_metrics_gpu_detail::compute_gmsd_session(state);
    if (timing != nullptr) {
      timing->gmsd_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    }
    if (!gmsd) {
      return std::unexpected{gmsd.error()};
    }

    started = visual_metrics_gpu_detail::Clock::now();
    auto ms_ssim = visual_metrics_gpu_detail::compute_ms_ssim_session(state);
    if (timing != nullptr) {
      timing->ms_ssim_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    }
    if (!ms_ssim) {
      return std::unexpected{ms_ssim.error()};
    }
    return make_visual_metric_result(*gmsd, *ms_ssim);
  }

  std::expected<std::vector<VisualMetricResult>, std::string>
  calculate_candidate_metrics_batch(
      std::span<const ImageBuffer> candidate_images,
      AcceleratedVisualMetricTiming* timing = nullptr) {
    if (candidate_images.empty()) {
      return std::unexpected{"Direct3D visual metric batch 输入为空。"};
    }
    if (state_ == nullptr) {
      return std::unexpected{"Direct3D visual metric session 不可用。"};
    }
    if (candidate_images.size() == 1) {
      auto single = calculate_candidate_metrics(candidate_images.front(), timing);
      if (!single) {
        return std::unexpected{single.error()};
      }
      std::vector<VisualMetricResult> results;
      try {
        results.push_back(*single);
      } catch (const std::bad_alloc&) {
        return std::unexpected{"Direct3D visual metric batch 结果内存不足。"};
      } catch (const std::length_error&) {
        return std::unexpected{"Direct3D visual metric batch 结果数量超过运行时限制。"};
      }
      return results;
    }

    auto& state = *static_cast<visual_metrics_gpu_detail::GpuSessionState*>(state_.get());
    std::lock_guard lock{state.gpu->mutex};
    state.candidate_luma_ready = false;
    for (const auto& image : candidate_images) {
      if (image.width != state.width || image.height != state.height) {
        return std::unexpected{"Direct3D metric batch 输入尺寸不一致。"};
      }
    }

    auto started = visual_metrics_gpu_detail::Clock::now();
    auto info = visual_metrics_gpu_detail::write_luma_buffer_batch(
        state,
        candidate_images,
        state.candidate_luma);
    if (timing != nullptr) {
      timing->luma_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    }
    if (!info) {
      return std::unexpected{info.error()};
    }
    state.candidate_luma_ready = true;

    started = visual_metrics_gpu_detail::Clock::now();
    auto gmsd = visual_metrics_gpu_detail::compute_gmsd_session_batch(
        state,
        candidate_images.size());
    if (timing != nullptr) {
      timing->gmsd_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    }
    if (!gmsd) {
      return std::unexpected{gmsd.error()};
    }

    started = visual_metrics_gpu_detail::Clock::now();
    auto ms_ssim = visual_metrics_gpu_detail::compute_ms_ssim_session_batch(
        state,
        candidate_images.size());
    if (timing != nullptr) {
      timing->ms_ssim_seconds += visual_metrics_gpu_detail::elapsed_seconds(started);
    }
    if (!ms_ssim) {
      return std::unexpected{ms_ssim.error()};
    }
    if (gmsd->size() != candidate_images.size() ||
        ms_ssim->size() != candidate_images.size()) {
      return std::unexpected{"Direct3D visual metric batch 结果数量不一致。"};
    }

    std::vector<VisualMetricResult> results;
    try {
      results.reserve(candidate_images.size());
      for (std::size_t index = 0; index < candidate_images.size(); ++index) {
        results.push_back(make_visual_metric_result((*gmsd)[index], (*ms_ssim)[index]));
      }
    } catch (const std::bad_alloc&) {
      return std::unexpected{"Direct3D visual metric batch 结果内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"Direct3D visual metric batch 结果数量超过运行时限制。"};
    }
    return results;
  }

 private:
  struct StateDeleter {
    void operator()(void* value) const noexcept;
  };
  using StatePtr = std::unique_ptr<void, StateDeleter>;

  StatePtr state_{};
};

void AcceleratedVisualMetricSession::StateDeleter::operator()(void* value) const noexcept {
  delete static_cast<visual_metrics_gpu_detail::GpuSessionState*>(value);
}

std::expected<LumaImage, std::string> make_luma_image_accelerated(const ImageBuffer& image) {
  if (!visual_metrics_gpu_detail::should_use_gpu_one_shot(image.width, image.height)) {
    return make_luma_image(image);
  }
  auto one_shot = visual_metrics_gpu_detail::shared_one_shot_metric_state();
  if (one_shot) {
    auto& state = **one_shot;
    std::lock_guard lock{state.session.gpu->mutex};
    if (auto gpu_luma = visual_metrics_gpu_detail::make_luma_image_d3d11(state, image)) {
      return gpu_luma;
    }
  }
  return make_luma_image(image);
}

std::expected<double, std::string> compute_gmsd_accelerated(const LumaImage& reference,
                                                            const LumaImage& candidate) {
  if (!visual_metrics_gpu_detail::should_use_gpu_one_shot(reference.width, reference.height)) {
    return compute_gmsd(reference, candidate);
  }
  auto one_shot = visual_metrics_gpu_detail::shared_one_shot_metric_state();
  if (one_shot) {
    auto& state = **one_shot;
    std::lock_guard lock{state.session.gpu->mutex};
    if (auto gmsd = visual_metrics_gpu_detail::compute_gmsd_d3d11(state, reference, candidate)) {
      return gmsd;
    }
  }
  return compute_gmsd(reference, candidate);
}

std::expected<double, std::string> compute_ms_ssim_accelerated(const LumaImage& reference,
                                                               const LumaImage& candidate) {
  if (!visual_metrics_gpu_detail::should_use_gpu_one_shot(reference.width, reference.height)) {
    return compute_ms_ssim(reference, candidate);
  }
  auto one_shot = visual_metrics_gpu_detail::shared_one_shot_metric_state();
  if (one_shot) {
    auto& state = **one_shot;
    std::lock_guard lock{state.session.gpu->mutex};
    if (auto ms_ssim = visual_metrics_gpu_detail::compute_ms_ssim_d3d11(state, reference, candidate)) {
      return ms_ssim;
    }
  }
  return compute_ms_ssim(reference, candidate);
}

std::expected<VisualMetricResult, std::string> calculate_visual_metrics_accelerated(
    const LumaImage& reference,
    const LumaImage& candidate) {
  if (visual_metrics_gpu_detail::should_use_gpu_one_shot(reference.width, reference.height)) {
    auto one_shot = visual_metrics_gpu_detail::shared_one_shot_metric_state();
    if (one_shot) {
      auto& state = **one_shot;
      std::lock_guard lock{state.session.gpu->mutex};
      if (auto metrics = visual_metrics_gpu_detail::calculate_visual_metrics_d3d11(state, reference, candidate)) {
        return metrics;
      }
    }
  }
  auto gmsd = compute_gmsd(reference, candidate);
  if (!gmsd) {
    return std::unexpected{gmsd.error()};
  }
  auto ms_ssim = compute_ms_ssim(reference, candidate);
  if (!ms_ssim) {
    return std::unexpected{ms_ssim.error()};
  }
  return make_visual_metric_result(*gmsd, *ms_ssim);
}

}  // namespace awj
