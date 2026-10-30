#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "awj/awj_d3d11.h"
#include "sdk_internal.h"
#include <algorithm>

using Microsoft::WRL::ComPtr;
namespace {
struct SourceOwner {
  awj_owner owner{};
  bool retained{};
  ~SourceOwner() { if (retained) { try { owner.release(owner.user); } catch (...) {} } }
};
struct Surface {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  ComPtr<ID3D11Texture2D> texture;
  uint32_t width{}, height{};
  DXGI_FORMAT format{};
  uint64_t capacity{};
  bool busy{}; // 仅在 Pool::mutex 下访问。
};
struct Pool {
  std::mutex mutex;
  std::vector<std::shared_ptr<Surface>> surfaces;
};
struct ReadLease {
  std::shared_ptr<Pool> pool;
  std::shared_ptr<Surface> surface;
  std::unique_ptr<SourceOwner> owner;
  std::vector<uint8_t> icc;
  ComPtr<IDXGIKeyedMutex> keyed;
  uint64_t release_key{};
  uint32_t subresource{};
  bool mapped{};
  ~ReadLease() {
    if (mapped) surface->immediate->Unmap(surface->texture.Get(), subresource);
    if (keyed) keyed->ReleaseSync(release_key);
    if (pool && surface) { std::lock_guard lock{pool->mutex}; surface->busy = false; }
    // owner 的外部 release 在 D3D 调用完成且不持 pool 锁之后执行。
  }
};
struct SyncLease {
  ComPtr<IDXGIKeyedMutex> mutex;
  uint64_t key{};
  bool acquired{};
  ~SyncLease() { if (acquired) mutex->ReleaseSync(key); }
};
awj_status gpu_error() { return awj_sdk::fail(AWJ_IO_ERROR, "D3D11 资源、同步、映射或设备状态无效。"); }
awj_status select_surface(awj_context& context, ID3D11Device* device,
                           uint32_t width, uint32_t height, DXGI_FORMAT format,
                           uint64_t estimated, ReadLease& lease) {
  {
    std::lock_guard lock{context.interop_mutex};
    if (!context.d3d_pool) context.d3d_pool = std::make_shared<Pool>();
    lease.pool = std::static_pointer_cast<Pool>(context.d3d_pool);
  }
  std::lock_guard lock{lease.pool->mutex};
  uint64_t total = 0;
  size_t available = lease.pool->surfaces.size();
  for (size_t i = 0; i < lease.pool->surfaces.size(); ++i) {
    const auto& surface = lease.pool->surfaces[i];
    total += surface->capacity;
    if (!surface->busy) {
      if (surface->device.Get() == device && surface->width == width && surface->height == height && surface->format == format) {
        surface->busy = true; lease.surface = surface; return AWJ_OK;
      }
      available = i;
    }
  }
  if (available == lease.pool->surfaces.size() && lease.pool->surfaces.size() >= 3)
    return awj_sdk::fail(AWJ_BUSY, "D3D11 回读槽位仍被借用，必须先释放帧。");
  if (available < lease.pool->surfaces.size()) total -= lease.pool->surfaces[available]->capacity;
  if (estimated > context.options.memory_budget_bytes || total > context.options.memory_budget_bytes - estimated)
    return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "D3D11 缓存容量超过 context 预算。");
  auto surface = std::make_shared<Surface>();
  surface->device = device; device->GetImmediateContext(surface->immediate.GetAddressOf());
  surface->width = width; surface->height = height; surface->format = format; surface->capacity = estimated;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
  desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, surface->texture.GetAddressOf()))) return gpu_error();
  if (available == lease.pool->surfaces.size()) lease.pool->surfaces.push_back(surface);
  else lease.pool->surfaces[available] = surface;
  surface->busy = true; lease.surface = std::move(surface);
  return AWJ_OK;
}
}

extern "C" {
void AWJ_CALL awj_d3d11_options_init(awj_d3d11_options* options) {
  if (!options) return;
  *options = {};
  options->struct_size = sizeof(*options); options->abi_version = AWJ_ABI_VERSION;
  options->timeout_ms = 1000;
  awj_frame_desc desc; awj_frame_desc_init(&desc); options->color = desc.color;
}
awj_status AWJ_CALL awj_d3d11_import(awj_context* context, const awj_d3d11_options* options, awj_frame** output) {
  return awj_sdk::boundary([&] {
    if (!output) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "缺少 D3D11 帧输出。");
    *output = nullptr;
    if (!context || !awj_sdk::valid_header(options) || options->reserved ||
        options->copy_policy > AWJ_COPY_REQUIRE_BORROWED || options->sync_kind > AWJ_D3D11_SYNC_KEYED_MUTEX ||
        options->timeout_ms > 60000 || (options->flags & ~7u) ||
        (!!options->owner.retain != !!options->owner.release) ||
        (!!options->texture == !!options->shared_nt_handle) ||
        (options->shared_nt_handle && !options->device))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "D3D11 ABI、来源、所有权或同步选项无效。");
    if (context->stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "context 已取消。");
    if (options->sync_kind == AWJ_D3D11_SYNC_EXTERNAL && !(options->flags & AWJ_D3D11_SOURCE_READY))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "必须明确完成来源同步，或使用 keyed mutex。");
    ComPtr<ID3D11Texture2D> texture;
    if (options->texture) {
      if (FAILED(static_cast<IUnknown*>(options->texture)->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) return gpu_error();
    } else {
      ComPtr<ID3D11Device1> opener;
      if (FAILED(static_cast<IUnknown*>(options->device)->QueryInterface(IID_PPV_ARGS(opener.GetAddressOf()))) ||
          FAILED(opener->OpenSharedResource1(options->shared_nt_handle, IID_PPV_ARGS(texture.GetAddressOf())))) return gpu_error();
    }
    D3D11_TEXTURE2D_DESC resource{}; texture->GetDesc(&resource);
    if (!resource.MipLevels || !resource.ArraySize || resource.SampleDesc.Count != 1 ||
        options->subresource >= static_cast<uint64_t>(resource.MipLevels) * resource.ArraySize)
      return awj_sdk::fail(AWJ_UNSUPPORTED, "不支持此 mip/array/MSAA 资源。");
    const auto mip = options->subresource % resource.MipLevels;
    const auto width = std::max(1u, resource.Width >> std::min(mip, 31u));
    const auto height = std::max(1u, resource.Height >> std::min(mip, 31u));
    awj_frame_desc desc; awj_frame_desc_init(&desc);
    desc.width = options->width ? options->width : width;
    desc.height = options->height ? options->height : height;
    if (desc.width > width || desc.height > height) return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "有效区域超出纹理。");
    desc.color = options->color;
    uint32_t bytes_per_pixel = 4;
    bool semiplanar = false;
    switch (resource.Format) {
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
        desc.format = AWJ_RGBA16F; desc.bit_depth = 16; bytes_per_pixel = 8;
        if ((options->flags & AWJ_D3D11_SCRGB) && desc.color.primaries < 0 && desc.color.transfer < 0 &&
            desc.color.matrix < 0 && desc.color.range < 0 && !desc.color.icc_size) {
          desc.color.primaries = 1; desc.color.transfer = 8; desc.color.matrix = 0; desc.color.range = 1;
          desc.color.linear_reference_white_nits = 80;
        }
        break;
      case DXGI_FORMAT_R8G8B8A8_UNORM: desc.format = AWJ_RGBA8; break;
      case DXGI_FORMAT_B8G8R8A8_UNORM: desc.format = AWJ_BGRA8; break;
      case DXGI_FORMAT_NV12: desc.format = AWJ_NV12; desc.plane_count = 2; bytes_per_pixel = 1; semiplanar = true; break;
      case DXGI_FORMAT_P010: desc.format = AWJ_P010; desc.bit_depth = 10; desc.plane_count = 2; bytes_per_pixel = 2; semiplanar = true; break;
      default: return awj_sdk::fail(AWJ_UNSUPPORTED, "D3D11 像素格式不受支持。");
    }
    if (desc.color.icc_size > 4 * 1024 * 1024 || (desc.color.icc_size && !desc.color.icc))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "D3D11 ICC 长度/地址无效。");
    const bool borrow = resource.Usage == D3D11_USAGE_STAGING && (resource.CPUAccessFlags & D3D11_CPU_ACCESS_READ);
    if (!borrow && options->copy_policy == AWJ_COPY_REQUIRE_BORROWED)
      return awj_sdk::fail(AWJ_COPY_REQUIRED, "此 GPU 纹理必须回读，严格输入复用模式拒绝。");
    ComPtr<ID3D11Device> device; texture->GetDevice(device.GetAddressOf());
    if (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
      return awj_sdk::fail(AWJ_UNSUPPORTED, "SDK 不借用 SINGLETHREADED D3D11 设备。");
    ComPtr<ID3D11DeviceContext> immediate; device->GetImmediateContext(immediate.GetAddressOf());
    ComPtr<ID3D11Multithread> protection;
    if (!(options->flags & AWJ_D3D11_CALLER_SERIALIZED) &&
        (FAILED(immediate.As(&protection)) || !protection->GetMultithreadProtected()))
      return awj_sdk::fail(AWJ_INVALID_ARGUMENT, "D3D11 context 需要多线程保护或显式外部串行化契约。");
    auto source_owner = std::make_unique<SourceOwner>();
    source_owner->owner = options->owner;
    if (options->owner.retain) { options->owner.retain(options->owner.user); source_owner->retained = true; }
    SyncLease sync;
    sync.key = options->release_key;
    if (options->sync_kind == AWJ_D3D11_SYNC_KEYED_MUTEX) {
      if (FAILED(texture.As(&sync.mutex))) return gpu_error();
      const auto status = sync.mutex->AcquireSync(options->acquire_key, options->timeout_ms);
      if (status == WAIT_TIMEOUT) return awj_sdk::fail(AWJ_TIMEOUT, "GPU 来源 keyed mutex 超时。");
      if (status != S_OK) return gpu_error();
      sync.acquired = true;
    }
    auto lease = std::make_shared<ReadLease>();
    const uint64_t rows = static_cast<uint64_t>(height) + (semiplanar ? (static_cast<uint64_t>(height) + 1) / 2 : 0);
    const uint64_t estimate_stride = (static_cast<uint64_t>(width) * bytes_per_pixel + 255) & ~UINT64_C(255);
    uint64_t estimate{};
    if (!awj_sdk::multiply(estimate_stride, rows, estimate) || estimate > context->options.max_frame_bytes)
      return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "D3D11 回读尺寸超过帧预算。");
    if (borrow) {
      lease->surface = std::make_shared<Surface>(); lease->surface->device = device;
      lease->surface->immediate = immediate; lease->surface->texture = texture;
      lease->subresource = options->subresource;
    } else {
      const auto status = select_surface(*context, device.Get(), width, height, resource.Format, estimate, *lease);
      if (status != AWJ_OK) return status;
      if (context->stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "GPU copy 提交前已取消。");
      // 仅复制有效内容；NV12/P010 的色度存储以 2x2 luma 为单位。
      const D3D11_BOX box{0, 0, 0, semiplanar ? (desc.width + 1u) & ~1u : desc.width,
          semiplanar ? (desc.height + 1u) & ~1u : desc.height, 1};
      immediate->CopySubresourceRegion(lease->surface->texture.Get(), 0, 0, 0, 0, texture.Get(), options->subresource, &box);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // Map 等待 copy 完成；不能在 GPU 仍读取 WGC 表面时因取消而提前释放来源。
    if (FAILED(immediate->Map(lease->surface->texture.Get(), lease->subresource, D3D11_MAP_READ, 0, &mapped))) return gpu_error();
    lease->mapped = true;
    uint64_t mapped_bytes{};
    if (!mapped.pData || !awj_sdk::multiply(mapped.RowPitch, rows, mapped_bytes) ||
        mapped_bytes > context->options.max_frame_bytes || mapped_bytes > SIZE_MAX)
      return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "D3D11 实际 RowPitch 超过帧预算。");
    if (lease->pool) {
      std::lock_guard lock{lease->pool->mutex};
      uint64_t other_bytes = 0;
      for (const auto& cached : lease->pool->surfaces)
        if (cached != lease->surface) other_bytes += cached->capacity;
      if (mapped_bytes > context->options.memory_budget_bytes || other_bytes > context->options.memory_budget_bytes - mapped_bytes)
        return awj_sdk::fail(AWJ_LIMIT_EXCEEDED, "实际 D3D11 缓存行距超过池预算。");
      lease->surface->capacity = mapped_bytes;
    }
    if (context->stop.stop_requested()) return awj_sdk::fail(AWJ_CANCELLED, "GPU 回读完成后已取消。");
    if (desc.color.icc_size) {
      lease->icc.assign(desc.color.icc, desc.color.icc + desc.color.icc_size);
      desc.color.icc = lease->icc.data();
    }
    desc.planes[0] = {static_cast<const uint8_t*>(mapped.pData), mapped_bytes, 0, static_cast<int64_t>(mapped.RowPitch)};
    if (semiplanar) desc.planes[1] = {static_cast<const uint8_t*>(mapped.pData), mapped_bytes,
        static_cast<uint64_t>(mapped.RowPitch) * height, static_cast<int64_t>(mapped.RowPitch)};
    const auto checked = awj_sdk::validate(context->options, desc); if (checked != AWJ_OK) return checked;
    auto frame = std::make_unique<awj_frame>(); frame->desc = desc;
    if (borrow) {
      lease->owner = std::move(source_owner);
      if (sync.acquired) { lease->keyed = sync.mutex; lease->release_key = sync.key; sync.acquired = false; }
    } else {
      const uint64_t copy_width = semiplanar ? (desc.width + 1u) & ~1u : desc.width;
      const uint64_t copy_height = semiplanar ? (desc.height + 1u) & ~1u : desc.height;
      frame->diagnostics.gpu_copy_bytes = copy_width * bytes_per_pixel * (copy_height + (semiplanar ? copy_height / 2 : 0));
      frame->diagnostics.gpu_readback_bytes = mapped_bytes;
    }
    frame->storage = std::move(lease);
    *output = frame.release();
    return AWJ_OK;
  });
}
}
