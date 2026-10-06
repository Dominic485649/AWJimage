#include <cstddef>
#include <cstdio>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
// Use the independent libjpeg-turbo headers, not JPEGli's compatibility ABI.
#include AWJ_TEST_TURBO_HEADER

bool jpeg_hdr_sdr_compatible(const std::byte* data, std::size_t length, std::size_t size) {
  jpeg_decompress_struct decoder{}; jpeg_error_mgr errors{};
  decoder.err = jpeg_std_error(&errors); jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, reinterpret_cast<const unsigned char*>(data), length);
  const bool valid = jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK &&
      decoder.data_precision == 8 && decoder.image_width == size && decoder.image_height == size;
  if (valid) {
    decoder.out_color_space = JCS_RGB; jpeg_start_decompress(&decoder);
    std::vector<unsigned char> row(decoder.output_width * decoder.output_components);
    while (decoder.output_scanline < decoder.output_height) { JSAMPROW pixels = row.data(); jpeg_read_scanlines(&decoder, &pixels, 1); }
    jpeg_finish_decompress(&decoder);
  }
  jpeg_destroy_decompress(&decoder); return valid;
}
