#include "otdr_hardware.h"

#include "sleep.h"
#include "xil_io.h"
#include "xparameters.h"

#define OTDR_GPIO_TRIGGER_BASE  XPAR_GPIO_0_BASEADDR
#define OTDR_GPIO_PULSE_BASE    XPAR_GPIO_1_BASEADDR
#define OTDR_GPIO_FRONTEND_BASE XPAR_GPIO_2_BASEADDR

#define OTDR_GPIO_CHANNEL_1_OFFSET 0x00U
#define OTDR_GPIO_CHANNEL_2_OFFSET 0x08U

#define OTDR_SAMPLE_BYTES           4U
#define OTDR_PACKING_SAMPLE_COUNT   3U
#define OTDR_ADC_SAMPLE_PERIOD_NS   4U
#define OTDR_TRIGGER_HOLD_CYCLES    50
#define OTDR_CAPTURE_MARGIN_US      50U

static uint32_t align_sample_count(uint32_t sample_count)
{
    sample_count =
        (sample_count / OTDR_PACKING_SAMPLE_COUNT) *
        OTDR_PACKING_SAMPLE_COUNT;
    return (sample_count == 0U) ? OTDR_PACKING_SAMPLE_COUNT : sample_count;
}

uint32_t otdr_hardware_capture_size_for_range(uint32_t range_m)
{
    uint32_t sample_count;

    if (range_m <= 1000U) {
        sample_count = 15999U;
    } else if (range_m <= 30000U) {
        sample_count = 32769U;
    } else if (range_m <= 60000U) {
        sample_count = 65535U;
    } else if (range_m <= 100000U) {
        sample_count = 131073U;
    } else {
        sample_count = 262143U;
    }

    return sample_count * OTDR_SAMPLE_BYTES;
}

uint32_t otdr_hardware_capture_size_for_samples(uint32_t sample_count)
{
    return align_sample_count(sample_count) * OTDR_SAMPLE_BYTES;
}

void otdr_hardware_configure_frontend(uint32_t channel,
                                      uint32_t hardware_acc_count)
{
    Xil_Out32(OTDR_GPIO_FRONTEND_BASE + OTDR_GPIO_CHANNEL_1_OFFSET,
              channel);
    Xil_Out32(OTDR_GPIO_FRONTEND_BASE + OTDR_GPIO_CHANNEL_2_OFFSET,
              hardware_acc_count);
}

void otdr_hardware_trigger_capture(uint32_t capture_bytes,
                                   uint32_t adc_delay,
                                   uint32_t wave_type,
                                   uint32_t pulse_width_ns,
                                   uint32_t downsample_enable,
                                   uint32_t hardware_acc_count)
{
    const uint32_t pulse_cycles = pulse_width_ns / OTDR_ADC_SAMPLE_PERIOD_NS;
    const uint32_t pulse_count_max =
        (pulse_cycles > 0U) ? (pulse_cycles - 1U) : 0U;
    const uint32_t total_samples = capture_bytes / OTDR_SAMPLE_BYTES;
    const uint32_t packed_beat_count =
        total_samples / OTDR_PACKING_SAMPLE_COUNT;
    const uint32_t downsample_factor = downsample_enable ? 2U : 1U;
    const uint32_t safe_wait_us =
        ((adc_delay + total_samples * downsample_factor) *
         OTDR_ADC_SAMPLE_PERIOD_NS) /
        1000U + OTDR_CAPTURE_MARGIN_US;
    const uint32_t trigger_control =
        ((wave_type & 0x07U) << 25U) |
        ((adc_delay & 0x00FFFFFFU) << 1U);

    Xil_Out32(OTDR_GPIO_PULSE_BASE + OTDR_GPIO_CHANNEL_1_OFFSET,
              pulse_count_max);
    Xil_Out32(OTDR_GPIO_PULSE_BASE + OTDR_GPIO_CHANNEL_2_OFFSET,
              downsample_enable);
    Xil_Out32(OTDR_GPIO_TRIGGER_BASE + OTDR_GPIO_CHANNEL_2_OFFSET,
              packed_beat_count);

    for (uint32_t i = 0U; i < hardware_acc_count; ++i) {
        Xil_Out32(OTDR_GPIO_TRIGGER_BASE + OTDR_GPIO_CHANNEL_1_OFFSET,
                  trigger_control | 0x00000001U);
        for (volatile int hold = 0; hold < OTDR_TRIGGER_HOLD_CYCLES; ++hold) {
        }
        Xil_Out32(OTDR_GPIO_TRIGGER_BASE + OTDR_GPIO_CHANNEL_1_OFFSET,
                  trigger_control & ~0x00000001U);

        if (i + 1U < hardware_acc_count) {
            usleep(safe_wait_us);
        }
    }
}
