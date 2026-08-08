#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdio.h>

/*
 * Build-time log level control.
 *
 * Define LOG_LEVEL before including this header (or via -DLOG_LEVEL=N):
 *   0 = silent   (errors only)
 *   1 = warnings + errors
 *   2 = info + warnings + errors          (default for release)
 *   3 = debug + info + warnings + errors  (development)
 *
 * ISR-safe variant LOG_ISR is compiled out unless LOG_LEVEL >= 3.
 */

#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#if LOG_LEVEL >= 3
#define LOG_DBG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOG_DBG(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= 2
#define LOG_INF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOG_INF(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= 1
#define LOG_WRN(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOG_WRN(fmt, ...) ((void)0)
#endif

#define LOG_ERR(fmt, ...) printf(fmt, ##__VA_ARGS__)

/* ISR context: only enabled at level 3 to avoid blocking interrupt handlers */
#if LOG_LEVEL >= 3
#define LOG_ISR(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOG_ISR(fmt, ...) ((void)0)
#endif

#endif /* DEBUG_LOG_H */
