#include <awj/awj.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  awj_context* context = NULL;
  awj_frame* frame = NULL;
  awj_frame_desc desc;
  const uint8_t pixels[4] = {1, 2, 3, 255};
  const uint8_t* row = NULL;
  uint64_t bytes = 0;
  if (awj_abi_version() != AWJ_ABI_VERSION || strcmp(awj_version(), AWJ_EXPECTED_VERSION)) return 1;
  if (awj_context_create(NULL, &context) != AWJ_OK) return 2;
  awj_frame_desc_init(&desc);
  desc.width = desc.height = 1;
  desc.planes[0].data = pixels;
  desc.planes[0].size = 4;
  desc.planes[0].stride = 4;
  if (awj_frame_create(context, &desc, NULL, &frame) != AWJ_OK) return 3;
  if (awj_frame_row(frame, 0, 0, &row, &bytes) != AWJ_OK || row != pixels || bytes != 4) return 4;
  awj_frame_release(frame);
  awj_context_destroy(context);
  puts("C ABI consumer passed");
  return 0;
}
