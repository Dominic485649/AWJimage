#include <awj/awj.h>
#include <stdio.h>

int main(void) {
  const uint8_t pixels[16] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  awj_context* context = NULL;
  awj_frame* frame = NULL;
  awj_result* result = NULL;
  awj_frame_desc desc;
  awj_encode_options options;
  awj_status status = awj_context_create(NULL, &context);
  if (status != AWJ_OK) return 1;
  awj_frame_desc_init(&desc);
  desc.width = 2; desc.height = 2;
  desc.planes[0].data = pixels; desc.planes[0].size = sizeof(pixels); desc.planes[0].stride = 8;
  desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 0; desc.color.range = 1;
  status = awj_frame_create(context, &desc, NULL, &frame);
  if (status == AWJ_OK) {
    awj_encode_options_init(&options); options.codec = AWJ_CODEC_PNG; options.lossless = 1;
    status = awj_encode(context, frame, &options, &result);
  }
  if (status == AWJ_OK && awj_result_size(result) >= 8 && awj_result_data(result)[0] == 137)
    printf("C99 consumer: PNG in memory, ABI=%u, version=%s\n", awj_abi_version(), awj_version());
  else { fprintf(stderr, "%s\n", awj_last_error()); status = AWJ_INTERNAL_ERROR; }
  awj_result_release(result); awj_frame_release(frame); awj_context_destroy(context);
  return status == AWJ_OK ? 0 : 1;
}
