#ifndef OTDR_DSP_H
#define OTDR_DSP_H

#include <stdint.h>

/* Maximum supported point count. */
#define MAX_CAPTURE_POINTS 524288

/* Correlation interfaces. */
void do_cross_correlation(int16_t* in_avg, int16_t* out_corr, uint32_t elements);
void do_cross_correlation_float(float* in_avg, float* out_corr, uint32_t elements);


void dsp_self_test_corr(void);
#endif
