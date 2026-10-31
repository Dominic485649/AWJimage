#pragma once
#include "stop_compat.h"
#if AWJ_USE_PORTABLE_STDLIB
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/xchar.h>
#else
#include <format>
#include <print>
#endif

namespace awj::compat {
#if AWJ_USE_PORTABLE_STDLIB
using fmt::format;
using fmt::print;
using fmt::println;
#else
using std::format;
using std::print;
using std::println;
#endif
}
