#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace minitest {

struct Result {
  int assertions = 0;
  int failures = 0;
};

inline Result& result() {
  static Result instance;
  return instance;
}

inline void expect_true(bool value, const char* expression, const char* file, int line) {
  ++result().assertions;
  if (!value) {
    ++result().failures;
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expression);
  }
}

template <typename A, typename B>
inline void expect_eq(const A& actual, const B& expected, const char* actual_expr, const char* expected_expr,
                      const char* file, int line) {
  ++result().assertions;
  if (!(actual == expected)) {
    ++result().failures;
    std::fprintf(stderr, "%s:%d: assertion failed: %s == %s\n", file, line, actual_expr, expected_expr);
  }
}

inline void expect_streq(const char* actual, const char* expected, const char* actual_expr, const char* expected_expr,
                         const char* file, int line) {
  ++result().assertions;
  const char* const a = actual != nullptr ? actual : "(null)";
  const char* const e = expected != nullptr ? expected : "(null)";
  if (std::strcmp(a, e) != 0) {
    ++result().failures;
    std::fprintf(stderr, "%s:%d: assertion failed: %s == %s (actual='%s' expected='%s')\n",
                 file, line, actual_expr, expected_expr, a, e);
  }
}

inline int finish() {
  if (result().failures == 0) {
    std::printf("host tests: PASS (%d assertions)\n", result().assertions);
    return EXIT_SUCCESS;
  }
  std::printf("host tests: FAIL (%d/%d failed)\n", result().failures, result().assertions);
  return EXIT_FAILURE;
}

}  // namespace minitest

#define MINITEST_EXPECT_TRUE(expr) ::minitest::expect_true((expr), #expr, __FILE__, __LINE__)
#define MINITEST_EXPECT_FALSE(expr) ::minitest::expect_true(!(expr), "!(" #expr ")", __FILE__, __LINE__)
#define MINITEST_EXPECT_EQ(actual, expected) \
  ::minitest::expect_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)
#define MINITEST_EXPECT_STREQ(actual, expected) \
  ::minitest::expect_streq((actual), (expected), #actual, #expected, __FILE__, __LINE__)
