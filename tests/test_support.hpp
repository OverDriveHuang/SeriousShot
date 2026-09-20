#pragma once

#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace hdrshot::test {

[[noreturn]] inline void fail(
    const std::string_view expression,
    const std::source_location location = std::source_location::current()) {
  std::ostringstream message;
  message << location.file_name() << ':' << location.line() << " check failed: " << expression;
  throw std::runtime_error(message.str());
}

inline void check(
    const bool condition,
    const std::string_view expression,
    const std::source_location location = std::source_location::current()) {
  if (!condition) {
    fail(expression, location);
  }
}

inline void check_near(
    const double actual,
    const double expected,
    const double tolerance,
    const std::source_location location = std::source_location::current()) {
  if (std::abs(actual - expected) > tolerance) {
    std::ostringstream message;
    message << "actual=" << actual << " expected=" << expected << " tolerance=" << tolerance;
    fail(message.str(), location);
  }
}

using TestCase = std::pair<std::string_view, std::function<void()>>;

inline int run(const std::vector<TestCase>& cases) {
  std::size_t passed = 0;
  for (const auto& [name, body] : cases) {
    try {
      body();
      ++passed;
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << passed << '/' << cases.size() << " tests passed\n";
  return passed == cases.size() ? 0 : 1;
}

}  // namespace hdrshot::test

#define HDRSHOT_CHECK(expression) ::hdrshot::test::check((expression), #expression)
#define HDRSHOT_CHECK_NEAR(actual, expected, tolerance) \
  ::hdrshot::test::check_near((actual), (expected), (tolerance))
