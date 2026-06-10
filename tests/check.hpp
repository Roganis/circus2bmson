#ifndef C2B_TEST_CHECK_HPP
#define C2B_TEST_CHECK_HPP

// Tiny assertion helpers so each test is a standalone executable that returns
// non-zero on failure (no external test framework needed).
#include <cstdio>

namespace check {
inline int& failures() {
  static int f = 0;
  return f;
}
}  // namespace check

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
      ++check::failures();                                                  \
    }                                                                       \
  } while (0)

#define CHECK_MSG(cond, ...)                                                \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("FAIL %s:%d  %s  [", __FILE__, __LINE__, #cond);          \
      std::printf(__VA_ARGS__);                                             \
      std::printf("]\n");                                                   \
      ++check::failures();                                                  \
    }                                                                       \
  } while (0)

#define REPORT_AND_RETURN()                                                 \
  do {                                                                      \
    if (check::failures() == 0)                                             \
      std::printf("OK (all checks passed)\n");                             \
    else                                                                    \
      std::printf("%d CHECK(s) FAILED\n", check::failures());              \
    return check::failures() == 0 ? 0 : 1;                                  \
  } while (0)

#endif  // C2B_TEST_CHECK_HPP
