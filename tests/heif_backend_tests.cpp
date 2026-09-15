#include <cstring>
#include <iostream>

#include <libheif/heif.h>

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  if (heif_get_encoder_descriptors(heif_compression_undefined, nullptr, nullptr, 0) != 0) {
    return fail("libheif decoder-only build unexpectedly exposes an encoder.");
  }

  const heif_decoder_descriptor* decoders[2]{};
  const int all_count =
      heif_get_decoder_descriptors(heif_compression_undefined, decoders, 2);
  if (all_count != 1 || decoders[0] == nullptr) {
    return fail("libheif decoder-only build does not expose exactly one decoder.");
  }
  const char* id = heif_decoder_descriptor_get_id_name(decoders[0]);
  if (id == nullptr || std::strcmp(id, "libde265") != 0) {
    return fail("libheif HEVC decoder is not libde265.");
  }

  const heif_decoder_descriptor* hevc[2]{};
  if (heif_get_decoder_descriptors(heif_compression_HEVC, hevc, 2) != 1 ||
      hevc[0] == nullptr ||
      std::strcmp(heif_decoder_descriptor_get_id_name(hevc[0]), "libde265") != 0) {
    return fail("libheif does not expose libde265 as the sole HEVC decoder.");
  }
  return 0;
}
