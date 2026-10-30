// C++ 调用同一公共 C 头，不提供或依赖另一套 C++ ABI/封装层。
#include <awj/awj.h>
#include <cstdio>
int main() {
  const uint8_t pixels[4] = {1, 2, 3, 255};
  awj_context* context = nullptr; awj_frame* frame = nullptr;
  awj_frame_desc desc; awj_frame_desc_init(&desc);
  desc.width = desc.height = 1;
  desc.planes[0].data = pixels; desc.planes[0].size = 4; desc.planes[0].stride = 4;
  const uint8_t* row = nullptr; uint64_t bytes = 0;
  int code = 1;
  if (awj_context_create(nullptr, &context) == AWJ_OK &&
      awj_frame_create(context, &desc, nullptr, &frame) == AWJ_OK &&
      awj_frame_row(frame, 0, 0, &row, &bytes) == AWJ_OK && row == pixels && bytes == 4) {
    std::printf("C++11 consumer: the same C ABI, version=%s\n", awj_version()); code = 0;
  }
  awj_frame_release(frame); awj_context_destroy(context); return code;
}
