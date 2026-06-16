#ifndef OTDR_CONFIG_H
#define OTDR_CONFIG_H

#include <stdint.h>

typedef struct start_measure start_measure_t;

typedef struct {
    uint32_t channel;
    uint32_t extended_format;
} otdr_start_config_result_t;

typedef struct {
    uint32_t software_acc_count;
    uint32_t capture_size;
    uint32_t adc_delay;
    uint32_t wave_type;
    uint32_t otdr_mode;
    uint32_t hardware_acc_count;
    uint32_t pulse_width_ns;
    uint32_t wavelength_nm;
    float refractive_index;
    uint32_t measure_range_m;
    uint32_t downsample_enable;
    float end_threshold_db;
    float nonreflect_threshold_db;
    uint32_t upload_raw_golay;
    uint32_t rcos_filter_enable;
    uint32_t upload_mode;
} otdr_config_t;

/* Return the active configuration as a read-only shared object. */
const otdr_config_t *otdr_config_get(void);

/*
 * Parse one start-measure command and update the active configuration.
 * Wire-format compatibility and legacy defaults are handled here.
 */
void otdr_config_apply_start(const start_measure_t *params,
                             otdr_start_config_result_t *result);

#endif
