/**
 * @file util.h
 * @brief Small internal helpers shared across the library's source files.
 *
 * Not part of the public API — not installed, not on the Arduino include
 * path. Just a place to put generic static-style helpers instead of
 * duplicating them per translation unit.
 */

#ifndef RB960X_UTIL_H
#define RB960X_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* The unit tests build the driver with RB960X_TEST_CLOCK defined, so
 * DELAY_MS and MILLIS come from tests/test_time.h (the mock port's clock)
 * and the versions below are skipped. */
#if defined(RB960X_TEST_CLOCK)
#include "test_time.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif


#ifndef DELAY_MS

/* ------------------------------------------------------------------ */
#if defined(ARDUINO)

#include <Arduino.h>

/**
 * @brief Blocking delay in milliseconds (Arduino).
 *
 * @param[in] ms  Delay time in milliseconds.
 */
#define DELAY_MS(ms)                                                \
    do                                                              \
    {                                                               \
        delay((unsigned long)(ms));                                 \
    } while (0)

/* ------------------------------------------------------------------ */
#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/**
 * @brief Blocking delay in milliseconds (Windows).
 *
 * @param[in] ms  Delay time in milliseconds.
 *
 * @note By default the Windows timer ticks about every 15.6 ms, so short
 *       delays round up to that. Call timeBeginPeriod(1) if you need
 *       about 1 ms accuracy.
 */
#define DELAY_MS(ms)                                                \
    do                                                              \
    {                                                               \
        Sleep((DWORD)(ms));                                         \
    } while (0)

/* ------------------------------------------------------------------ */
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)

#include <errno.h>
#include <time.h>

/**
 * @brief Blocking delay in milliseconds (Linux, macOS, POSIX).
 *
 * Uses nanosleep(). If a signal interrupts the sleep, it carries on
 * sleeping until the full time has passed.
 *
 * @param[in] ms  Delay time in milliseconds. Evaluated only once.
 *
 * @note With a strict @c -std=c99 / @c -std=c11, define
 *       @c _POSIX_C_SOURCE as @c 200809L or higher before including
 *       any headers, or nanosleep() won't be declared.
 */
#define DELAY_MS(ms)                                                \
    do                                                              \
    {                                                               \
        unsigned long delay_ms_ = (unsigned long)(ms);              \
        struct timespec ts_;                                        \
                                                                    \
        ts_.tv_sec  = (time_t)(delay_ms_ / 1000UL);                 \
        ts_.tv_nsec = (long)((delay_ms_ % 1000UL) * 1000000UL);     \
                                                                    \
        while ((nanosleep(&ts_, &ts_) != 0) && (errno == EINTR))    \
        {                                                           \
        }                                                           \
    } while (0)

/* ------------------------------------------------------------------ */
#else
#error "DELAY_MS: unsupported platform - define DELAY_MS(ms) before including utils.h"
#endif

#endif /* DELAY_MS */


#ifndef MILLIS

/* ------------------------------------------------------------------ */
#if defined(ARDUINO)

#include <Arduino.h>

/**
 * @brief Milliseconds since start-up (Arduino). Wraps after about 49 days;
 *        subtract two readings as uint32_t to get the time between them.
 */
#define MILLIS() ((uint32_t)millis())

/* ------------------------------------------------------------------ */
#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/**
 * @brief Milliseconds since start-up (Windows). Wraps after about 49 days;
 *        subtract two readings as uint32_t to get the time between them.
 */
#define MILLIS() ((uint32_t)GetTickCount())

/* ------------------------------------------------------------------ */
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)

#include <stdint.h>
#include <time.h>

/** Used by MILLIS(); call MILLIS() instead. */
static inline uint32_t utilMillisPosix(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U));
}

/**
 * @brief Milliseconds from a monotonic clock (Linux, macOS, POSIX). Wraps
 *        after about 49 days; subtract two readings as uint32_t to get the
 *        time between them.
 *
 * @note Needs @c _POSIX_C_SOURCE with a strict @c -std=c99, as DELAY_MS does.
 */
#define MILLIS() utilMillisPosix()

/* ------------------------------------------------------------------ */
#else
#error "MILLIS: unsupported platform - define MILLIS() before including utils.h"
#endif

#endif /* MILLIS */

/** True if @p str begins with @p prefix. */
bool utilStartsWith(const char *str, const char *prefix);

/**
 * @brief Copy a string into a fixed-size buffer, always NULL terminating.
 *
 * Unlike strncpy(), @p dst is always NULL terminated (as long as
 * @p dstSize is at least 1), and the rest of the buffer is not filled
 * with zeros. If @p src is too long for @p dst, it is cut off at
 * @p dstSize - 1 characters.
 *
 * @param[out] dst      Buffer to copy into.
 * @param[in]  src      NULL-terminated string to copy from.
 * @param[in]  dstSize  Size of @p dst in bytes, including space for the
 *                      terminating NULL.
 *
 * @note Does nothing if @p dst or @p src is NULL, or if @p dstSize is 0.
 * @note @p dst and @p src must not overlap.
 * @note The caller can't tell if the string was cut off. If that matters,
 *       compare strlen(@p src) with @p dstSize before calling.
 */
void utilStrCopy(char * const dst, const char * const src, const size_t dstSize);


#ifdef __cplusplus
}
#endif

#endif /* RB960X_UTIL_H */
