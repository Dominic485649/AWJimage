#pragma once
#include "stop_compat.h"
#if AWJ_USE_PORTABLE_STDLIB
#include "vendor/expected/include/tl/expected.hpp"
#else
#include <expected>
#endif

namespace awj::compat {
#if AWJ_USE_PORTABLE_STDLIB
using tl::expected;
using tl::unexpected;
#else
using std::expected;
using std::unexpected;
#endif
}
