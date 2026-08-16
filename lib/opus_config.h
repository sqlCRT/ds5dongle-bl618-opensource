#ifndef OPUS_CONFIG_H
#define OPUS_CONFIG_H

#define FIXED_POINT        1
#define DISABLE_FLOAT_API  1
#define OPUS_BUILD         1
#define VAR_ARRAYS         1

#define PACKAGE_VERSION    "1.5.2-bl616"

#define HAVE_LRINTF        1

/* E907 DSP: use kmmwb2/smmwb/clz32 for hot fixed-point paths */
#if defined(__riscv)
#define E907_OPUS_DSP      1
#define OPUS_TCM_CODE      __attribute__((section(".tcm_code")))
#define OPUS_TCM_CONST     __attribute__((section(".tcm_const")))
#else
#define OPUS_TCM_CODE
#define OPUS_TCM_CONST
#endif

#include <stdlib.h>
#include <string.h>

#endif
