#pragma once

// Linux Studio 的入口。实现在 studio_run_linux.cpp，仅非 Windows 构建时编译。

#ifndef _WIN32

namespace awj::studio {

int run_studio_ui();

}  // namespace awj::studio

#endif  // !_WIN32
