#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <windows.h>
#include <thread>
#include <atomic>
#include <wincodec.h>
#include <wrl/client.h>

import awj.jxr_codec;
import awj.image;

namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void terminate_test_process(int exit_code) noexcept {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(exit_code);
}

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  terminate_test_process(1);
}

struct ComInit {
  HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)};

  ~ComInit() {
    if (SUCCEEDED(hr)) {
      CoUninitialize();
    }
  }
};

bool write_test_jxr(const std::filesystem::path& path, int bit_depth = 8) {
  const bool high_depth = bit_depth > 8;
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    return false;
  }

  ComPtr<IWICStream> stream;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr)) {
    return false;
  }
  hr = stream->InitializeFromFilename(path.native().c_str(), GENERIC_WRITE);
  if (FAILED(hr)) {
    return false;
  }

  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder);
  if (FAILED(hr)) {
    return false;
  }
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) {
    return false;
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  hr = encoder->CreateNewFrame(&frame, &options);
  if (FAILED(hr)) {
    return false;
  }
  hr = frame->Initialize(options.Get());
  if (FAILED(hr)) {
    return false;
  }
  hr = frame->SetSize(2, high_depth ? 1 : 2);
  if (FAILED(hr)) {
    return false;
  }
  WICPixelFormatGUID format = bit_depth == 32 ? GUID_WICPixelFormat128bppRGBAFloat
                             : high_depth ? GUID_WICPixelFormat64bppRGBA
                                          : GUID_WICPixelFormat24bppBGR;
  const WICPixelFormatGUID requested_format = format;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr) || !IsEqualGUID(format, requested_format)) {
    return false;
  }
  if (bit_depth == 32) {
    float pixels[] = {-.25F, 2.0F, 4.0F, 1.0F, .125F, 1.5F, .5F, 1.0F};
    hr = frame->WritePixels(1, 32, static_cast<UINT>(sizeof(pixels)),
                            reinterpret_cast<BYTE*>(pixels));
    if (FAILED(hr) || FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
  }
  if (high_depth) {
    std::uint16_t pixels[] = {
        0, 32768, 65535, 65535,
        65535, 0, 32768, 65535,
    };
    hr = frame->WritePixels(1, 16, static_cast<UINT>(sizeof(pixels)),
                            reinterpret_cast<BYTE*>(pixels));
    if (FAILED(hr)) {
      return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
      return false;
    }
    hr = encoder->Commit();
    return SUCCEEDED(hr);
  }
  std::uint8_t pixels[] = {
      0, 0, 255, 0, 255, 0,
      255, 0, 0, 255, 255, 255,
  };
  hr = frame->WritePixels(2, 6, static_cast<UINT>(sizeof(pixels)), pixels);
  if (FAILED(hr)) {
    return false;
  }
  hr = frame->Commit();
  if (FAILED(hr)) {
    return false;
  }
  hr = encoder->Commit();
  return SUCCEEDED(hr);
}

int decode_and_check(const std::filesystem::path& path,
                     std::uint32_t expected_height = 2,
                     int min_bit_depth = 8) {
  awj::JxrImageDecoder decoder;
  auto dimensions = decoder.probe_dimensions(path);
  if (!dimensions || dimensions->width != 2 ||
      dimensions->height != expected_height) {
    return fail(dimensions ? "JXR dimensions invalid." : dimensions.error());
  }
  auto decoded = decoder.decode(path);
  if (!decoded || decoded->decoder_id != "windows-jxr" || decoded->used_fallback ||
      decoded->image.width != 2 || decoded->image.height != expected_height ||
      decoded->image.planes.empty()) {
    return fail(decoded ? "JXR decode result invalid." : decoded.error());
  }
  if (decoded->image.bit_depth < min_bit_depth ||
      !decoded->image.source_info ||
      decoded->image.source_info->bit_depth < min_bit_depth ||
      (min_bit_depth > 8 && decoded->image.source_info->has_hdr_metadata)) {
    return fail("JXR high-bit-depth input was incorrectly classified as HDR.");
  }
  return 0;
}

}  // namespace

int main() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  ComInit init;
  if (FAILED(init.hr) && init.hr != RPC_E_CHANGED_MODE) {
    return fail("COM initialization failed for JXR test.");
  }

  const auto temp = std::filesystem::temp_directory_path();
  const auto jxr = temp / "awj-jxr-codec-test.jxr";
  const auto wdp = temp / "awj-jxr-codec-test.wdp";
  const auto hdp = temp / "awj-jxr-codec-test.hdp";
  const auto hdr_jxr = temp / "awj-jxr-codec-test-hdr.jxr";
  const auto float_jxr = temp / "awj-jxr-codec-test-float.jxr";
  std::error_code ec;
  std::filesystem::remove(jxr, ec);
  std::filesystem::remove(wdp, ec);
  std::filesystem::remove(hdp, ec);
  std::filesystem::remove(hdr_jxr, ec);
  std::filesystem::remove(float_jxr, ec);

  if (!write_test_jxr(jxr)) {
    return fail("Could not create temporary JPEG XR test input.");
  }
  std::filesystem::copy_file(jxr, wdp, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::filesystem::remove(jxr, ec);
    return fail("Could not copy temporary WDP test input.");
  }
  std::filesystem::copy_file(jxr, hdp, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::filesystem::remove(jxr, ec);
    std::filesystem::remove(wdp, ec);
    return fail("Could not copy temporary HDP test input.");
  }

  if (const auto code = decode_and_check(jxr); code != 0) {
    return code;
  }
  if (const auto code = decode_and_check(wdp); code != 0) {
    return code;
  }
  if (const auto code = decode_and_check(hdp); code != 0) {
    return code;
  }
  if (!write_test_jxr(hdr_jxr, 16)) {
    return fail("Could not create temporary HDR JPEG XR test input.");
  }
  if (const auto code = decode_and_check(hdr_jxr, 1, 16); code != 0) {
    return code;
  }

  if (!write_test_jxr(float_jxr, 32)) return fail("Could not create floating-point JPEG XR input.");
  auto floating = awj::JxrImageDecoder{}.decode(float_jxr);
  if (!floating || floating->used_fallback || floating->image.bit_depth != 16 ||
      floating->image.sample_representation != awj::SampleRepresentation::ieee_half_float ||
      !floating->image.source_info || !floating->image.source_info->has_hdr_metadata ||
      floating->image.source_info->color_primaries != 1 ||
      floating->image.source_info->transfer_characteristics != 8 ||
      floating->image.planes.empty() || floating->image.planes[0].bytes.size() < 16) {
    return fail(floating ? "Floating-point JXR lost its linear scRGB representation." : floating.error());
  }
  std::uint16_t samples[8]{};
  std::memcpy(samples, floating->image.planes[0].bytes.data(), sizeof(samples));
  if (!(samples[0] & 0x8000) || samples[1] <= 0x3c00 || samples[1] >= 0x7c00 ||
      samples[2] <= 0x3c00 || samples[2] >= 0x7c00) {
    return fail("Floating-point JXR highlights or negative values were clipped through UNORM.");
  }

  // Each thread owns its factory; a borrowed STA must not outlive the caller's COM scope.
  std::atomic<bool> decode_failed{false};
  const auto check_thread = [&](DWORD apartment) {
    for (int pass = 0; pass != 3; ++pass) {
      const auto hr = CoInitializeEx(nullptr, apartment);
      if (FAILED(hr)) { decode_failed = true; return; }
      awj::JxrImageDecoder decoder;
      if (!decoder.decode(jxr)) decode_failed = true;
      CoUninitialize();
    }
  };
  {
    std::jthread sta{check_thread, DWORD{COINIT_APARTMENTTHREADED}};
    std::jthread mta{check_thread, DWORD{COINIT_MULTITHREADED}};
  }
  if (decode_failed) return fail("WIC factory reuse broke the COM apartment lifetime.");

  std::filesystem::remove(jxr, ec);
  std::filesystem::remove(wdp, ec);
  std::filesystem::remove(hdp, ec);
  std::filesystem::remove(hdr_jxr, ec);
  std::filesystem::remove(float_jxr, ec);
  return 0;
}
