#pragma once

// Slint 字符串边界的辅助函数。这里只依赖 slint.h 和标准库，因此回归测试
// （tests/ui_smoke_tests.cpp）可以在不引入 Studio 状态头的情况下验证它们。

#include <slint.h>

#include <string>
#include <string_view>

namespace awj::studio {

// slint::SharedString 与 std::string 互转。
inline slint::SharedString to_shared(std::string_view text) {
  return slint::SharedString{std::string{text}.c_str()};
}
inline std::string shared_to_string(const slint::SharedString& value) {
  return std::string{value.data(), value.size()};
}

// 清空 SharedString。绝不要写 `value = {}` 或 `value = nullptr`：
//
// slint::SharedString 同时提供 operator=(const SharedString&) 和
// operator=(const char*)（后者把参数当 null 结尾的 C 字符串）。空花括号对
// operator=(const char*) 是恒等转换，比拷贝赋值所需的用户定义转换更好，
// 因此 `= {}` 会选中它，把 nullptr 交给 std::string_view(const char*)，
// 最终在 strlen(nullptr) 上以 0xC0000005（读地址 0）崩溃。
//
// 1.0.14 Studio「开始转换」必崩就是这个原因：begin_queue_conversion_run 的
// 重置循环里写了 `item.log_text = {}`（minidump 的 RIP 落在 strlen、调用点
// 落在该函数内部）。清空一律走这里。
inline void clear_shared_string(slint::SharedString& value) {
  value = slint::SharedString{};
}

}  // namespace awj::studio
