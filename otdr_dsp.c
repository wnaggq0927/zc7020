#include "xil_printf.h"
#include "xtime_l.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "kiss_fft.h"
#include "arm_math.h"
#include "arm_const_structs.h"
#include "otdr_config.h"
#include "otdr_reference.h"
#include "rcos_tables.h"

/*
 * kiss_fft_cpx is retained as the complex storage type. The FFT engine is
 * CMSIS-DSP arm_cfft_f32 because its interleaved float layout is compatible.
 */
#pragma GCC optimize ("O2")

#define DSP_PI 3.14159265358979323846f
#define CORR_OUTPUT_GAIN 1.0f

extern void otdr_watchdog_poll(void);

/*
 * A 262144-point transform is decomposed into a 512 x 512 FFT.
 * Padding protects the CMSIS-DSP NEON implementation from short vector reads.
 */
#define FFT_N1 512
#define FFT_N2 512
#define FFT_N  (FFT_N1 * FFT_N2)
#define FFT_NEON_PAD_CPX 4

static kiss_fft_cpx dsp_work_buf[FFT_N + FFT_NEON_PAD_CPX] __attribute__((aligned(32)));
static kiss_fft_cpx dsp_ref_buf[FFT_N + FFT_NEON_PAD_CPX]  __attribute__((aligned(32)));
static kiss_fft_cpx dsp_col_temp[FFT_N1 + FFT_NEON_PAD_CPX] __attribute__((aligned(32)));
static int16_t dsp_selftest_in[FFT_N] __attribute__((aligned(32)));
static int16_t dsp_selftest_out[FFT_N] __attribute__((aligned(32)));

/*
 * Execute the decomposed FFT or IFFT in four stages:
 * column transforms, twiddle correction, row transforms, and transpose.
 */
void fft_2d_262144(kiss_fft_cpx* data, uint8_t is_ifft) {
    uint8_t ifft_flag = is_ifft ? 1U : 0U;
    /* Transform non-contiguous columns through a temporary aligned buffer. */
    for (int c = 0; c < FFT_N2; c++) {
        for (int r = 0; r < FFT_N1; r++) {
            dsp_col_temp[r] = data[r * FFT_N2 + c];
        }

        arm_cfft_f32(&arm_cfft_sR_f32_len512,
                     (float32_t*)dsp_col_temp,
                     ifft_flag,
                     1U);

        for (int r = 0; r < FFT_N1; r++) {
            data[r * FFT_N2 + c] = dsp_col_temp[r];
        }

        if ((c % 16) == 0) {
            otdr_watchdog_poll();
        }
    }
    /* Apply twiddle factors using a recurrence to avoid per-point trig calls. */
    float sign = is_ifft ? 1.0f : -1.0f;
    for (int r = 0; r < FFT_N1; r++) {
        float step = sign * 2.0f * DSP_PI * (float)r / (float)FFT_N;
        float step_re = cosf(step);
        float step_im = sinf(step);
        float w_re = 1.0f;
        float w_im = 0.0f;

        for (int c = 0; c < FFT_N2; c++) {
            int idx = r * FFT_N2 + c;
            float d_re = data[idx].r;
            float d_im = data[idx].i;

            data[idx].r = d_re * w_re - d_im * w_im;
            data[idx].i = d_re * w_im + d_im * w_re;

            float next_w_re = w_re * step_re - w_im * step_im;
            float next_w_im = w_re * step_im + w_im * step_re;
            w_re = next_w_re;
            w_im = next_w_im;
        }

        if ((r % 16) == 0) {
            otdr_watchdog_poll();
        }
    }
    /* Transform contiguous rows in place. */
    for (int r = 0; r < FFT_N1; r++) {
        arm_cfft_f32(&arm_cfft_sR_f32_len512,
                     (float32_t*)&data[r * FFT_N2],
                     ifft_flag,
                     1U);

        if ((r % 16) == 0) {
            otdr_watchdog_poll();
        }
    }
    /* Restore one-dimensional FFT ordering. */
    for (int r = 0; r < FFT_N1; r++) {
        for (int c = r + 1; c < FFT_N2; c++) {
            int idx1 = r * FFT_N2 + c;
            int idx2 = c * FFT_N2 + r;

            kiss_fft_cpx tmp = data[idx1];
            data[idx1] = data[idx2];
            data[idx2] = tmp;
        }

        if ((r % 64) == 0) {
            otdr_watchdog_poll();
        }
    }
}

void do_cross_correlation_float(float* in_avg, float* out_corr, uint32_t elements) {
    const char *current_code_str = otdr_reference_code();
    const int ref_wave_len = otdr_reference_length();
    const int current_spb = otdr_reference_sps();
    const uint32_t rcos_filter_enable =
        otdr_config_get()->rcos_filter_enable;

    if (elements > FFT_N) {
        xil_printf("[DSP WARN] elements(%d) > FFT_N(%d), bypass correlation\r\n", elements, FFT_N);
        for (uint32_t i = 0; i < elements; i++) {
            out_corr[i] = in_avg[i];
        }
        return;
    }

    if (ref_wave_len <= 0 || ((uint64_t)elements + (uint64_t)ref_wave_len - 1ULL) > FFT_N) {
        xil_printf("[DSP WARN] conv length overflow: elements=%d, ref_wave_len=%d, FFT_N=%d, bypass correlation\r\n",
                   elements, ref_wave_len, FFT_N);
        for (uint32_t i = 0; i < elements; i++) {
            out_corr[i] = in_avg[i];
        }
        return;
    }

    xil_printf("[DSP] matlab-style conv same start, elements=%d, ref_wave_len=%d, sps=%d\r\n",
               elements, ref_wave_len, current_spb);

    XTime t_start, t_fft1, t_fft2, t_ifft;
    XTime_GetTime(&t_start);

    for (uint32_t i = 0; i < FFT_N; i++) {
        dsp_work_buf[i].r = (i < elements) ? in_avg[i] : 0.0f;
        dsp_work_buf[i].i = 0.0f;
        dsp_ref_buf[i].r = 0.0f;
        dsp_ref_buf[i].i = 0.0f;

        if ((i % 30000U) == 0U) {
            otdr_watchdog_poll();
        }
    }
    /*
     * Build the MATLAB-equivalent matched-filter reference:
     * upsample(code, sps), shape it, then reverse it for convolution.
     */
    const rcos_table_t *rcos = 0;
    int h_len = current_spb;
    const float *h_coef = 0;

    if (rcos_filter_enable) {
        rcos = find_rcos_table(current_spb);
        if (rcos == 0) {
            xil_printf("[DSP WARN] no MATLAB rcos table for sps=%d, bypass correlation\r\n", current_spb);
            for (uint32_t i = 0; i < elements; i++) {
                out_corr[i] = in_avg[i];
            }
            return;
        }
        h_len = rcos->len;
        h_coef = rcos->coef;
    }

    int h_center = h_len / 2;
    xil_printf("[DSP] reference shaping: %s, sps=%d, taps=%d\r\n",
               rcos_filter_enable ? "raised-cosine" : "rectangular",
               current_spb, h_len);

    for (int sym = 0; sym < 256; sym++) {
        float code_val = (current_code_str[sym] == '1') ? 1.0f : -1.0f;
        int up_pos = sym * current_spb;

        for (int t = 0; t < h_len; t++) {
            int out_idx = up_pos + t - h_center;
            if (out_idx >= 0 && out_idx < ref_wave_len) {
                dsp_ref_buf[out_idx].r +=
                    code_val * (rcos_filter_enable ? h_coef[t] : 1.0f);
            }
        }

        if ((sym % 32) == 0) {
            otdr_watchdog_poll();
        }
    }
    for (int i = 0; i < ref_wave_len / 2; i++) {
        float tmp = dsp_ref_buf[i].r;
        dsp_ref_buf[i].r = dsp_ref_buf[ref_wave_len - 1 - i].r;
        dsp_ref_buf[ref_wave_len - 1 - i].r = tmp;
    }

    fft_2d_262144(dsp_work_buf, 0);
    XTime_GetTime(&t_fft1);

    fft_2d_262144(dsp_ref_buf, 0);
    XTime_GetTime(&t_fft2);
    /* Multiply both spectra to implement linear convolution. */
    for (uint32_t i = 0; i < FFT_N; i++) {
        float a_re = dsp_work_buf[i].r;
        float a_im = dsp_work_buf[i].i;
        float b_re = dsp_ref_buf[i].r;
        float b_im = dsp_ref_buf[i].i;

        dsp_work_buf[i].r = a_re * b_re - a_im * b_im;
        dsp_work_buf[i].i = a_re * b_im + a_im * b_re;

        if ((i % 30000U) == 0U) {
            otdr_watchdog_poll();
        }
    }

    fft_2d_262144(dsp_work_buf, 1);
    XTime_GetTime(&t_ifft);

    uint32_t same_offset = (uint32_t)(ref_wave_len / 2);
    for (uint32_t i = 0; i < elements; i++) {
        uint32_t idx = i + same_offset;
        out_corr[i] = (idx < FFT_N) ? (dsp_work_buf[idx].r * CORR_OUTPUT_GAIN) : 0.0f;
    }

    uint32_t t1 = (uint32_t)(1000ULL * (t_fft1 - t_start) / COUNTS_PER_SECOND);
    uint32_t t2 = (uint32_t)(1000ULL * (t_fft2 - t_fft1) / COUNTS_PER_SECOND);
    uint32_t t3 = (uint32_t)(1000ULL * (t_ifft - t_fft2) / COUNTS_PER_SECOND);
    xil_printf("[DSP] FFT conv same OK: A=%dms, B=%dms, Mul+IFFT=%dms\r\n",
               t1, t2, t3);
}
void do_cross_correlation(int16_t* in_avg, int16_t* out_corr, uint32_t elements)
{
    float *tmp_corr = (float *)dsp_ref_buf;

    for (uint32_t i = 0; i < elements; i++) {
        tmp_corr[i] = (float)in_avg[i];
    }

    do_cross_correlation_float(tmp_corr, tmp_corr, elements);

    for (uint32_t i = 0; i < elements; i++) {
        float res = tmp_corr[i];

        if (res > 32767.0f) {
            res = 32767.0f;
        } else if (res < -32768.0f) {
            res = -32768.0f;
        }

        out_corr[i] = (int16_t)res;
    }
}
void dsp_self_test_corr(void)
{
    const uint32_t elements = 196605;
    const uint32_t delay1 = 10000;
    const uint32_t delay2 = 50000;
    const int16_t amp1 = 100;
    const int16_t amp2 = 50;

    xil_printf("[SELFTEST] DSP correlation self-test start\r\n");

    otdr_reference_select(0, 20);
    memset(dsp_selftest_in, 0, elements * sizeof(int16_t));
    memset(dsp_selftest_out, 0, elements * sizeof(int16_t));

    const char *ap_code = otdr_reference_ap_code();
    const int current_spb = otdr_reference_sps();
    for (int i = 0; i < 256; i++) {
        int16_t code_val = (ap_code[i] == '1') ? 1 : -1;
        for (int j = 0; j < current_spb; j++) {
            uint32_t idx1 = delay1 + (uint32_t)i * (uint32_t)current_spb + (uint32_t)j;
            uint32_t idx2 = delay2 + (uint32_t)i * (uint32_t)current_spb + (uint32_t)j;

            if (idx1 < elements) {
                dsp_selftest_in[idx1] = code_val * amp1;
            }
            if (idx2 < elements) {
                dsp_selftest_in[idx2] = code_val * amp2;
            }
        }
    }

    do_cross_correlation(dsp_selftest_in, dsp_selftest_out, elements);

    int16_t max_v = dsp_selftest_out[0];
    int16_t min_v = dsp_selftest_out[0];
    uint32_t max_idx = 0;
    uint32_t min_idx = 0;
    int32_t max_abs = (dsp_selftest_out[0] < 0) ? -dsp_selftest_out[0] : dsp_selftest_out[0];
    uint32_t max_abs_idx = 0;

    for (uint32_t i = 0; i < elements; i++) {
        int16_t v = dsp_selftest_out[i];
        int32_t av = (v < 0) ? -v : v;

        if (v > max_v) {
            max_v = v;
            max_idx = i;
        }
        if (v < min_v) {
            min_v = v;
            min_idx = i;
        }
        if (av > max_abs) {
            max_abs = av;
            max_abs_idx = i;
        }
    }

    int32_t win1_abs = 0;
    uint32_t win1_idx = delay1;
    int32_t win2_abs = 0;
    uint32_t win2_idx = delay2;

    for (uint32_t i = delay1 - 20; i <= delay1 + 20; i++) {
        int16_t v = dsp_selftest_out[i];
        int32_t av = (v < 0) ? -v : v;
        if (av > win1_abs) {
            win1_abs = av;
            win1_idx = i;
        }
    }

    for (uint32_t i = delay2 - 20; i <= delay2 + 20; i++) {
        int16_t v = dsp_selftest_out[i];
        int32_t av = (v < 0) ? -v : v;
        if (av > win2_abs) {
            win2_abs = av;
            win2_idx = i;
        }
    }

    xil_printf("[SELFTEST] expect peaks near %d and %d\r\n", delay1, delay2);
    xil_printf("[SELFTEST] global max=%d@%d min=%d@%d max_abs=%d@%d\r\n",
               max_v, max_idx, min_v, min_idx, max_abs, max_abs_idx);
    xil_printf("[SELFTEST] window1 max_abs=%d@%d, window2 max_abs=%d@%d\r\n",
               win1_abs, win1_idx, win2_abs, win2_idx);

    if ((win1_idx == delay1) && (win2_idx == delay2)) {
        xil_printf("[SELFTEST] PASS: correlation lag index is aligned\r\n");
    } else {
        xil_printf("[SELFTEST] CHECK: correlation lag index has offset\r\n");
    }
}
