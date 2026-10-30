#define NOMINMAX
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "awj/awj_d3d11.h"
#include <array>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行 D3D11 测试失败: %s; %s\n", __LINE__, #value, awj_last_error()); return 1; } } while (false)
struct Owner { int retained{}, released{}; };
void AWJ_CALL retain(void* user) { ++static_cast<Owner*>(user)->retained; }
void AWJ_CALL release(void* user) { ++static_cast<Owner*>(user)->released; }
int shared_texture(ID3D11Device* device) {
  const std::array<uint8_t, 16> pixels{1,2,3,255, 4,5,6,255, 7,8,9,255, 10,11,12,255};
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 2; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  D3D11_SUBRESOURCE_DATA initial{pixels.data(), 8, 0};
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&desc, &initial, texture.GetAddressOf()))) {
    std::puts("当前 WARP 不支持共享 NT 纹理；此子项未验证。"); return 77;
  }
  ComPtr<IDXGIResource1> resource; ComPtr<IDXGIKeyedMutex> keyed;
  CHECK(SUCCEEDED(texture.As(&resource)) && SUCCEEDED(texture.As(&keyed)));
  HANDLE handle{};
  CHECK(SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)));
  awj_context* context{}; CHECK(awj_context_create(nullptr, &context) == AWJ_OK);
  awj_d3d11_options options; awj_d3d11_options_init(&options);
  options.device = device; options.shared_nt_handle = handle;
  options.flags = AWJ_D3D11_CALLER_SERIALIZED; options.sync_kind = AWJ_D3D11_SYNC_KEYED_MUTEX;
  options.acquire_key = 0; options.release_key = 1; options.timeout_ms = 0;
  awj_frame* frame{}; CHECK(awj_d3d11_import(context, &options, &frame) == AWJ_OK);
  const uint8_t* row{}; uint64_t bytes{};
  CHECK(awj_frame_row(frame, 0, 1, &row, &bytes) == AWJ_OK && std::memcmp(row, pixels.data() + 8, 8) == 0);
  CHECK(keyed->AcquireSync(1, 0) == S_OK && SUCCEEDED(keyed->ReleaseSync(1)));
  awj_frame* failed{};
  CHECK(awj_d3d11_import(context, &options, &failed) == AWJ_TIMEOUT && !failed);
  CHECK(keyed->AcquireSync(1, 0) == S_OK && SUCCEEDED(keyed->ReleaseSync(0)));
  CHECK(CloseHandle(handle)); texture.Reset(); resource.Reset(); keyed.Reset(); awj_context_destroy(context);
  CHECK(awj_frame_row(frame, 0, 0, &row, &bytes) == AWJ_OK && std::memcmp(row, pixels.data(), 8) == 0);
  awj_frame_release(frame);
  std::puts("WARP 共享 NT 纹理导入、keyed mutex 获取/归还/超时及回读保活通过；不代表跨设备或真实 WGC 已验证。");
  return 0;
}
int main(int argc, char** argv) {
  ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> immediate;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, immediate.GetAddressOf()))) {
    std::puts("D3D11 WARP 不可用，此项未验证。"); return 77;
  }
  if (argc == 2 && std::strcmp(argv[1], "--shared") == 0) return shared_texture(device.Get());
  const std::array<uint16_t, 16> pixels{0x3800,0xb400,0x4000,0x3c00, 0x3c00,0,0,0x3c00,
                                      0,0x3c00,0,0x3c00, 0,0,0x3c00,0x3c00};
  D3D11_TEXTURE2D_DESC resource{};
  resource.Width = 2; resource.Height = 2; resource.MipLevels = 1; resource.ArraySize = 1;
  resource.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; resource.SampleDesc.Count = 1;
  resource.Usage = D3D11_USAGE_DEFAULT;
  D3D11_SUBRESOURCE_DATA data{pixels.data(), 16, 0};
  ComPtr<ID3D11Texture2D> texture;
  CHECK(SUCCEEDED(device->CreateTexture2D(&resource, &data, texture.GetAddressOf())));
  awj_context_options limits; awj_context_options_init(&limits);
  limits.max_frame_bytes = 1024 * 1024; limits.memory_budget_bytes = 8 * 1024 * 1024;
  awj_context* context{}; CHECK(awj_context_create(&limits, &context) == AWJ_OK);
  awj_d3d11_options options; awj_d3d11_options_init(&options);
  options.texture = texture.Get(); options.flags = AWJ_D3D11_SOURCE_READY | AWJ_D3D11_CALLER_SERIALIZED | AWJ_D3D11_SCRGB;
  Owner owner; options.owner = {&owner, retain, release};
  awj_frame* first{};
  options.copy_policy = AWJ_COPY_REQUIRE_BORROWED;
  CHECK(awj_d3d11_import(context, &options, &first) == AWJ_COPY_REQUIRED && !first && owner.retained == 0);
  options.copy_policy = AWJ_COPY_ALLOW_REQUIRED;
  CHECK(awj_d3d11_import(context, &options, &first) == AWJ_OK);
  CHECK(owner.retained == 1 && owner.released == 1);
  awj_diagnostics diagnostics{sizeof(diagnostics), AWJ_ABI_VERSION, 0, 0, 0, 0, 0, 0};
  CHECK(awj_frame_diagnostics(first, &diagnostics) == AWJ_OK && diagnostics.gpu_copy_bytes > 0 && diagnostics.gpu_readback_bytes > 0);
  awj_frame_desc desc; awj_frame_desc_init(&desc);
  CHECK(awj_frame_describe(first, &desc) == AWJ_OK && desc.format == AWJ_RGBA16F && desc.color.linear_reference_white_nits == 80);
  const uint8_t* row{}; uint64_t size{};
  CHECK(awj_frame_row(first, 0, 0, &row, &size) == AWJ_OK && size == 16 && std::memcmp(row, pixels.data(), 16) == 0);
  awj_frame* second{}; awj_frame* third{}; awj_frame* fourth{};
  CHECK(awj_d3d11_import(context, &options, &second) == AWJ_OK);
  CHECK(awj_d3d11_import(context, &options, &third) == AWJ_OK);
  CHECK(awj_d3d11_import(context, &options, &fourth) == AWJ_BUSY && !fourth);
  awj_frame_release(second);
  CHECK(awj_d3d11_import(context, &options, &fourth) == AWJ_OK);
  awj_frame_release(first); awj_frame_release(third); awj_frame_release(fourth);
  CHECK(owner.retained == owner.released);

  resource.Usage = D3D11_USAGE_STAGING; resource.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  CHECK(SUCCEEDED(device->CreateTexture2D(&resource, &data, staging.GetAddressOf())));
  options.texture = staging.Get(); options.copy_policy = AWJ_COPY_REQUIRE_BORROWED;
  const auto before = owner.released;
  CHECK(awj_d3d11_import(context, &options, &first) == AWJ_OK);
  CHECK(owner.released == before);
  CHECK(awj_frame_diagnostics(first, &diagnostics) == AWJ_OK && diagnostics.gpu_copy_bytes == 0 && diagnostics.gpu_readback_bytes == 0);
  staging.Reset();
  awj_context_destroy(context);
  CHECK(awj_frame_row(first, 0, 1, &row, &size) == AWJ_OK && std::memcmp(row, pixels.data() + 8, 16) == 0);
  awj_frame_release(first);
  CHECK(owner.retained == owner.released);
  std::puts("D3D11 WARP FP16 实际回读、严格模式、池背压与直接 staging 借用通过；不代表真实 WGC/硬件 HDR 采集已验证。");
  return 0;
}
