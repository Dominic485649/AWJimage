#pragma once
#include "stop_compat.h"
#include <algorithm>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>
#if !AWJ_USE_PORTABLE_STDLIB
#include <ranges>
namespace awj::compat { namespace ranges = std::ranges; }
#else
// 只覆盖 SDK 使用的 common-range 算法；不实现 views 或通用 ranges 库。
namespace awj::compat::ranges {
struct identity {
  template<class T> constexpr T&& operator()(T&& value) const noexcept {
    return std::forward<T>(value);
  }
};
template<class Input, class Output> struct in_out_result { Input in; Output out; };

template<class Range, class Pred, class Projection = identity>
constexpr bool all_of(Range&& range, Pred pred, Projection projection = {}) {
  return std::all_of(std::begin(range), std::end(range), [&](auto&& value) {
    return std::invoke(pred, std::invoke(projection, value));
  });
}
template<class Range, class Pred, class Projection = identity>
constexpr bool any_of(Range&& range, Pred pred, Projection projection = {}) {
  return std::any_of(std::begin(range), std::end(range), [&](auto&& value) {
    return std::invoke(pred, std::invoke(projection, value));
  });
}
template<class Range, class Pred, class Projection = identity>
constexpr bool none_of(Range&& range, Pred pred, Projection projection = {}) {
  return !any_of(std::forward<Range>(range), std::move(pred), std::move(projection));
}
template<class Range, class Value, class Projection = identity>
constexpr auto find(Range& range, const Value& value, Projection projection = {}) {
  return std::find_if(std::begin(range), std::end(range), [&](auto&& element) {
    return std::invoke(projection, element) == value;
  });
}
template<class Range, class Pred, class Projection = identity>
constexpr auto find_if(Range& range, Pred pred, Projection projection = {}) {
  return std::find_if(std::begin(range), std::end(range), [&](auto&& value) {
    return std::invoke(pred, std::invoke(projection, value));
  });
}
template<class Input, class Output>
constexpr auto copy(Input first, Input last, Output output) {
  return in_out_result<Input, Output>{last, std::copy(first, last, output)};
}
template<class Range, class Output>
constexpr auto copy(Range&& range, Output output) {
  return awj::compat::ranges::copy(std::begin(range), std::end(range), std::move(output));
}
template<class Input, class Count, class Output>
constexpr auto copy_n(Input first, Count count, Output output) {
  static_assert(std::is_base_of_v<std::forward_iterator_tag,
      typename std::iterator_traits<Input>::iterator_category>);
  if (count <= 0) return in_out_result<Input, Output>{first, output};
  return in_out_result<Input, Output>{std::next(first, count), std::copy_n(first, count, output)};
}
template<class Range, class Output, class Function, class Projection = identity>
constexpr auto transform(Range&& range, Output output, Function function, Projection projection = {}) {
  auto last = std::end(range);
  auto result = std::transform(std::begin(range), last, output, [&](auto&& value) {
    return std::invoke(function, std::invoke(projection, value));
  });
  return in_out_result<decltype(last), Output>{last, result};
}
template<class Left, class Right, class Pred = std::equal_to<>,
         class LeftProjection = identity, class RightProjection = identity>
constexpr bool equal(Left&& left, Right&& right, Pred pred = {},
                     LeftProjection left_projection = {}, RightProjection right_projection = {}) {
  return std::equal(std::begin(left), std::end(left), std::begin(right), std::end(right),
      [&](auto&& a, auto&& b) {
        return std::invoke(pred, std::invoke(left_projection, a), std::invoke(right_projection, b));
      });
}
template<class Range, class Value>
constexpr auto fill(Range& range, const Value& value) {
  std::fill(std::begin(range), std::end(range), value);
  return std::end(range);
}
template<class Range, class Pred, class Value, class Projection = identity>
constexpr auto replace_if(Range& range, Pred pred, const Value& replacement, Projection projection = {}) {
  std::replace_if(std::begin(range), std::end(range), [&](auto&& value) {
    return std::invoke(pred, std::invoke(projection, value));
  }, replacement);
  return std::end(range);
}
template<class Range, class Compare = std::less<>, class Projection = identity>
constexpr auto sort(Range& range, Compare compare = {}, Projection projection = {}) {
  std::sort(std::begin(range), std::end(range), [&](auto&& left, auto&& right) {
    return std::invoke(compare, std::invoke(projection, left), std::invoke(projection, right));
  });
  return std::end(range);
}
template<class Range, class Compare = std::less<>, class Projection = identity>
constexpr auto max_element(Range& range, Compare compare = {}, Projection projection = {}) {
  return std::max_element(std::begin(range), std::end(range), [&](auto&& left, auto&& right) {
    return std::invoke(compare, std::invoke(projection, left), std::invoke(projection, right));
  });
}
}
#endif
