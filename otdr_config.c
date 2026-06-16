#include "otdr_config.h"

#include "otdr_hardware.h"
#include "otdr_protocol.h"

static otdr_config_t active_config = {
    .software_acc_count = 1U,
    .capture_size = 32768U,
    .adc_delay = 0U,
    .wave_type = 0U,
    .otdr_mode = 1U,
    .hardware_acc_count = 1U,
    .pulse_width_ns = 80U,
    .wavelength_nm = 1550U,
    .refractive_index = 1.4685f,
    .measure_range_m = 5000U,
    .downsample_enable = 0U,
    .end_threshold_db = 5.0f,
    .nonreflect_threshold_db = 0.0f,
    .upload_raw_golay = 0U,
    .rcos_filter_enable = 1U
};

const otdr_config_t *otdr_config_get(void)
{
    return &active_config;
}

void otdr_config_apply_start(const start_measure_t *params,
                             otdr_start_config_result_t *result)
{
    uint32_t channel = 0U;
    uint32_t hardware_acc_count = 1024U;
    const uint32_t extended_format = (params->len >= 68U) ? 1U : 0U;

    active_config.otdr_mode = params->Ctrl.OtdrMode;
    active_config.wave_type = params->Ctrl.OtdrOptMode;
    active_config.downsample_enable = params->Ctrl.RSVD;
    active_config.upload_raw_golay =
        (params->Ctrl.EnableRefresh == 2U) ? 1U : 0U;
    active_config.rcos_filter_enable =
        (params->len >= 72U) ? params->Ext_RcosEnable : 1U;

    active_config.measure_range_m = params->State.MeasureLength_m;
    active_config.pulse_width_ns = params->State.PulseWidth_ns;
    active_config.wavelength_nm = params->State.Lambda_nm;
    active_config.refractive_index = params->State.n;
    active_config.end_threshold_db = params->State.EndThreshold;
    active_config.nonreflect_threshold_db =
        params->State.NonRelectThreshold;

    if (active_config.end_threshold_db <= 0.0f) {
        active_config.end_threshold_db = 5.0f;
    }

    if (extended_format != 0U) {
        hardware_acc_count = params->Ext_HwAccTimes;
        active_config.software_acc_count = params->Ext_SwAccTimes;
        channel = params->Ext_ComSelIdx;
        active_config.adc_delay = params->Ext_AdcDelay;

        if (params->Ext_CaptureSamples > 0U) {
            active_config.capture_size =
                otdr_hardware_capture_size_for_samples(
                    params->Ext_CaptureSamples);
        } else {
            active_config.capture_size =
                otdr_hardware_capture_size_for_range(
                    active_config.measure_range_m);
        }
    } else {
        active_config.software_acc_count =
            params->Ctrl.RefreshPeriod_ms;
        if (active_config.software_acc_count < 1U ||
            active_config.software_acc_count > 5000U) {
            active_config.software_acc_count = 1U;
        }
        active_config.adc_delay = 0U;
        active_config.capture_size =
            otdr_hardware_capture_size_for_range(
                active_config.measure_range_m);
    }

    active_config.hardware_acc_count =
        (hardware_acc_count > 0U) ? hardware_acc_count : 1U;
    if (active_config.software_acc_count == 0U) {
        active_config.software_acc_count = 1U;
    }

    if (result != 0) {
        result->channel = channel;
        result->extended_format = extended_format;
    }
}
