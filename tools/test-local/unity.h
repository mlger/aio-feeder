/*
 * Minimal Unity-compatible shim for local (non-PlatformIO) host test runs.
 *
 * PlatformIO's CDN is unavailable in some restricted dev environments; CI
 * always runs the real Unity via `pio test -d test` (see test/platformio.ini).
 * This shim mirrors the subset of the Unity API the suites use so the exact
 * same test sources compile and pass locally with g++ (tools/test-local/run.sh).
 */
#ifndef AIO_TEST_LOCAL_UNITY_SHIM_H
#define AIO_TEST_LOCAL_UNITY_SHIM_H

#include <stdio.h>
#include <string.h>

extern int g_fail, g_run;

void setUp(void);
void tearDown(void);

#define UNITY_BEGIN() (g_run = 0, g_fail = 0, 0)
#define UNITY_END() \
  (printf("%d tests, %d failures\n", g_run, g_fail), g_fail ? 1 : 0)
#define RUN_TEST(fn) \
  do { g_run++; printf("- %s\n", #fn); setUp(); fn(); tearDown(); } while (0)

#define UT_FAIL(msg)                                   \
  do {                                                 \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
    g_fail++;                                          \
    return;                                            \
  } while (0)

#define UT_FAIL_EQ(file, line, expr, exp, act)                        \
  do {                                                                \
    printf("  FAIL %s:%d: %s exp %lld got %lld\n", file, line, expr,  \
           (long long)(exp), (long long)(act));                       \
    g_fail++;                                                         \
    return;                                                           \
  } while (0)

#define TEST_ASSERT(cond) \
  do { if (!(cond)) UT_FAIL(#cond); } while (0)
#define TEST_ASSERT_TRUE(c) TEST_ASSERT(c)
#define TEST_ASSERT_FALSE(c) \
  do { if (c) UT_FAIL("!" #c); } while (0)
#define TEST_ASSERT_NULL(p) TEST_ASSERT((p) == nullptr)
#define TEST_ASSERT_NOT_NULL(p) TEST_ASSERT((p) != nullptr)

#define TEST_ASSERT_EQUAL_INT(e, a)                                          \
  do {                                                                       \
    if ((long long)(e) != (long long)(a))                                    \
      UT_FAIL_EQ(__FILE__, __LINE__, #e, (long long)(e), (long long)(a));    \
  } while (0)
#define TEST_ASSERT_EQUAL(e, a) TEST_ASSERT_EQUAL_INT(e, a)
#define TEST_ASSERT_NOT_EQUAL(e, a) \
  do { if ((long long)(e) == (long long)(a)) UT_FAIL(#e " != " #a); } while (0)
#define TEST_ASSERT_EQUAL_UINT TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_UINT8 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_UINT16 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_UINT32 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_UINT64 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_INT8 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_INT16 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_INT32 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_INT64 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_HEX8 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_size_t TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_size_t_SIGNED TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_HEX16 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_HEX32 TEST_ASSERT_EQUAL_INT
#define TEST_ASSERT_EQUAL_PTR(e, a) TEST_ASSERT((void *)(e) == (void *)(a))
#define TEST_ASSERT_EQUAL_STRING(e, a) TEST_ASSERT(strcmp((e), (a)) == 0)

#define TEST_ASSERT_EQUAL_HEX8_ARRAY(e, a, n)                       \
  do {                                                              \
    const uint8_t *_e = (const uint8_t *)(e), *_a = (const uint8_t *)(a); \
    size_t _n = (n);                                                \
    for (size_t _i = 0; _i < _n; _i++) {                            \
      if (_e[_i] != _a[_i]) {                                       \
        printf("  FAIL %s:%d: [" #e "][%zu] exp %02X got %02X\n",   \
               __FILE__, __LINE__, _i, _e[_i], _a[_i]);             \
        g_fail++;                                                   \
        return;                                                     \
      }                                                             \
    }                                                               \
  } while (0)

#define TEST_ASSERT_LESS_THAN_UINT32(e, a) TEST_ASSERT((a) < (e))
#define TEST_ASSERT_GREATER_THAN_UINT32(e, a) TEST_ASSERT((a) > (e))
#define TEST_ASSERT_LESS_OR_EQUAL_UINT32(e, a) TEST_ASSERT((a) <= (e))
#define TEST_ASSERT_EQUAL_UINT8_ARRAY(e, a, n) TEST_ASSERT_EQUAL_HEX8_ARRAY(e, a, n)
#define TEST_ASSERT_EQUAL_INT8_ARRAY(e, a, n) TEST_ASSERT_EQUAL_HEX8_ARRAY(e, a, n)
/* Unity's internal name, used by one pairing test — alias it for the shim */
#define UNITY_TEST_ASSERT_EQUAL_UINT(e, a, l) TEST_ASSERT_EQUAL_INT(e, a)
#define UNITY_TEST_ASSERT_EQUAL_UINT8(e, a, l, ...) TEST_ASSERT_EQUAL_INT(e, a)

#endif /* AIO_TEST_LOCAL_UNITY_SHIM_H */
