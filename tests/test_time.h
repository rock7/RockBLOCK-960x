/**
 * @file test_time.h
 * @brief DELAY_MS and MILLIS for the unit tests, run on the mock port's
 *        clock (see test_time.cpp).
 *
 * Included by src/util.h when RB960X_TEST_CLOCK is defined, which
 * tests/CMakeLists.txt does for the C sources built for the tests, so
 * util.h's own versions are skipped. A retry wait of minutes takes no real time,
 * and the AT client's timeouts expire as the mock port yields.
 */

#ifndef RB960X_TEST_TIME_H
#define RB960X_TEST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Advance the mock clock by @p ms. */
void rb960xTestDelayMs(unsigned long ms);

/** The mock clock. */
uint32_t rb960xTestMillis(void);

#ifdef __cplusplus
}
#endif

#define DELAY_MS(ms) rb960xTestDelayMs((unsigned long)(ms))
#define MILLIS() rb960xTestMillis()

#endif /* RB960X_TEST_TIME_H */
