#pragma once

// Windows Studio 的入口装配：Slint 后端探测与 run_studio_ui。
// 从 main.cpp 拆出；Linux 侧对应 studio_run_linux。

namespace awj::studio {

// 探测 OpenGL 驱动是否提供 FemtoVG 需要的函数；缺失时强制软件渲染。
void ensure_slint_backend();

int run_studio_ui(const wchar_t* health_event,
                  const wchar_t* installed_version);
int run_shell_convert_window(int argc, wchar_t* argv[]);

}  // namespace awj::studio
