#pragma once

#include <cstdio>
#include <cstdlib>

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

inline int finish() {
  if (result().failures == 0) {
    std::printf("host sanity: PASS (%d assertion)\n", result().assertions);
    return EXIT_SUCCESS;
  }
  std::printf("host sanity: FAIL (%d/%d failed)\n", result().failures, result().assertions);
  return EXIT_FAILURE;
}

}  // namespace minitest

#define MINITEST_EXPECT_TRUE(expr) ::minitest::expect_true((expr), #expr, __FILE__, __LINE__)
