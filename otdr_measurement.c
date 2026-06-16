#include "otdr_measurement.h"

#include "dma.h"
#include "otdr_config.h"
#include "otdr_dsp.h"
#include "otdr_hardware.h"
#include "otdr_protocol.h"
#include "otdr_reference.h"
#include "otdr_tcp_server.h"
#include "otdr_upload.h"

#include "lwip/err.h"
#include "xil_cache.h"
#include "xil_printf.h"

#include <stdint.h>
#include <string.h>

#define OTDR_DMA_BASE_ADDR  ((UINTPTR)0x06000000U)
#define OTDR_DMA_TIMEOUT    50000000
#define OTDR_GOLAY_STEPS    4U

typedef enum {
    GOLAY_STEP_AP = 0,
    GOLAY_STEP_AN = 1,
    GOLAY_STEP_BP = 2,
    GOLAY_STEP_BN = 3
} golay_step_t;

typedef struct {
    golay_step_t golay_step;
    uint32_t software_acc_count;
} measurement_state_t;

static measurement_state_t measurement_state = {
    GOLAY_STEP_AP,
    0U
};

static float corr_a_buffer[MAX_CAPTURE_POINTS];
static float corr_b_buffer[MAX_CAPTURE_POINTS];
static float upload_buffer[MAX_CAPTURE_POINTS * 2U];
static float experiment_upload_buffer[MAX_CAPTURE_POINTS * 5U];
static int32_t accumulation_buffer[MAX_CAPTURE_POINTS * OTDR_GOLAY_STEPS];

static volatile int abort_requested = 0;

void otdr_measurement_request_abort(void)
{
    abort_requested = 1;
}

static void reset_measurement_state(void)
{
    measurement_state.golay_step = GOLAY_STEP_AP;
    measurement_state.software_acc_count = 0U;
}

static float get_average_scale(void)
{
    const otdr_config_t *config = otdr_config_get();
    float divisor =
        (float)config->hardware_acc_count *
        (float)config->software_acc_count;
    if (divisor < 1.0f) {
        divisor = 1.0f;
    }
    return 1.0f / divisor;
}

static void remove_tail_dc(float *buffer, uint32_t count)
{
    if (buffer == NULL || count < 16U) {
        return;
    }

    uint32_t window = count / 50U;
    if (window < 256U) {
        window = (count < 256U) ? count : 256U;
    }
    if (window > 4096U) {
        window = 4096U;
    }
    if (window == 0U || window > count) {
        return;
    }

    const uint32_t start = count - window;
    double sum = 0.0;
    for (uint32_t i = start; i < count; ++i) {
        sum += (double)buffer[i];
    }

    const float dc = (float)(sum / (double)window);
    for (uint32_t i = 0; i < count; ++i) {
        buffer[i] -= dc;
    }
}

static int32_t *capture_frame(struct netif *netif,
                              uint32_t hardware_wave_type,
                              const char *mode_name)
{
    const otdr_config_t *config = otdr_config_get();

    dma_start_transfer();
    otdr_hardware_trigger_capture(config->capture_size,
                                  config->adc_delay,
                                  hardware_wave_type,
                                  config->pulse_width_ns,
                                  config->downsample_enable,
                                  config->hardware_acc_count);

    volatile int timeout_count = 0;
    while (!dma_is_rx_done()) {
        xemacif_input(netif);
        ++timeout_count;
        if (timeout_count > OTDR_DMA_TIMEOUT) {
            xil_printf("\r\n[Error] DMA transfer timeout (%s mode)\r\n", mode_name);
            dma_clear_rx_done();
            dma_reset();
            otdr_protocol_set_measuring(0);
            otdr_protocol_set_waiting_ack(0);
            reset_measurement_state();
            return NULL;
        }
    }

    dma_clear_rx_done();
    Xil_DCacheInvalidateRange(OTDR_DMA_BASE_ADDR,
                              config->capture_size);
    return (int32_t *)OTDR_DMA_BASE_ADDR;
}

static void upload_float_data(float *data,
                              uint32_t point_count,
                              const float *event_curve,
                              uint32_t event_point_count,
                              const char *error_text)
{
    const err_t send_error =
        otdr_upload_float32((u8 *)data,
                            point_count * sizeof(float),
                            point_count,
                            event_curve,
                            event_point_count);
    if (send_error == ERR_OK) {
        otdr_protocol_set_waiting_ack(1);
    } else {
        xil_printf("[UDP] %s\r\n", error_text);
    }
}

static void handle_abort(void)
{
    if (!abort_requested) {
        return;
    }

    abort_requested = 0;
    otdr_protocol_set_measuring(0);

    /*
     * A completed frame is uploaded immediately by the measurement path.
     * Re-uploading here is unsafe because upload_buffer may not match the
     * current raw-debug format or the current configuration. The host should
     * retain and save the last complete frame it already received.
     */
    xil_printf(
        "[DSP] Stop acknowledged; no duplicate frame uploaded. "
        "The host keeps the last complete measurement.\r\n");

    reset_measurement_state();
}

static void process_single_pulse(struct netif *netif, uint32_t elements)
{
    const otdr_config_t *config = otdr_config_get();

    if (measurement_state.software_acc_count == 0U) {
        memset(accumulation_buffer, 0, elements * sizeof(int32_t));
    }

    int32_t *raw_data = capture_frame(netif, 4U, "Single Pulse");
    if (raw_data == NULL) {
        return;
    }

    for (uint32_t i = 0; i < elements; ++i) {
        accumulation_buffer[i] += raw_data[i];
    }

    ++measurement_state.software_acc_count;
    if (measurement_state.software_acc_count <
        config->software_acc_count) {
        return;
    }

    xil_printf("[DSP] Single pulse accumulation completed!\r\n");
    const float scale = get_average_scale();
    for (uint32_t i = 0; i < elements; ++i) {
        upload_buffer[i] = (float)accumulation_buffer[i] * scale;
    }

    upload_float_data(upload_buffer,
                      elements,
                      upload_buffer,
                      elements,
                      "Failed to send single pulse data");
    measurement_state.software_acc_count = 0U;
}

static void finish_golay_measurement(uint32_t elements)
{
    const otdr_config_t *config = otdr_config_get();

    int32_t *ap_frame = &accumulation_buffer[0U * elements];
    int32_t *an_frame = &accumulation_buffer[1U * elements];
    int32_t *bp_frame = &accumulation_buffer[2U * elements];
    int32_t *bn_frame = &accumulation_buffer[3U * elements];
    const float scale = get_average_scale();

    for (uint32_t i = 0; i < elements; ++i) {
        upload_buffer[i] = (float)ap_frame[i] * scale;
        corr_a_buffer[i] = (float)(ap_frame[i] - an_frame[i]) * scale;
        corr_b_buffer[i] = (float)(bp_frame[i] - bn_frame[i]) * scale;

        if (config->upload_raw_golay) {
            experiment_upload_buffer[0U * elements + i] =
                (float)ap_frame[i] * scale;
            experiment_upload_buffer[1U * elements + i] =
                (float)an_frame[i] * scale;
            experiment_upload_buffer[2U * elements + i] =
                (float)bp_frame[i] * scale;
            experiment_upload_buffer[3U * elements + i] =
                (float)bn_frame[i] * scale;
        }
    }

    remove_tail_dc(corr_a_buffer, elements);
    remove_tail_dc(corr_b_buffer, elements);

    otdr_reference_select(0, config->pulse_width_ns);
    do_cross_correlation_float(corr_a_buffer, corr_a_buffer, elements);

    otdr_reference_select(2, config->pulse_width_ns);
    do_cross_correlation_float(corr_b_buffer, corr_b_buffer, elements);

    for (uint32_t i = 0; i < elements; ++i) {
        upload_buffer[elements + i] =
            0.5f * (corr_a_buffer[i] + corr_b_buffer[i]);
        if (config->upload_raw_golay) {
            experiment_upload_buffer[4U * elements + i] =
                upload_buffer[elements + i];
        }
    }

    float *upload_data = upload_buffer;
    uint32_t upload_points = elements * 2U;
    if (config->upload_raw_golay) {
        xil_printf("[DSP] Uploading Golay AP/AN/BP/BN/final correlation float32 data\r\n");
        upload_data = experiment_upload_buffer;
        upload_points = elements * 5U;
    }

    upload_float_data(upload_data,
                      upload_points,
                      &upload_buffer[elements],
                      elements,
                      "Failed to send Golay AP + final corr");
}

static void complete_golay_step(void)
{
    static const char *const messages[OTDR_GOLAY_STEPS] = {
        "[DSP] 1/4 AP raw frame completed\r\n",
        "[DSP] 2/4 AN raw frame completed\r\n",
        "[DSP] 3/4 BP raw frame completed\r\n",
        "[DSP] 4/4 BN raw frame completed, correlating and uploading AP + final corr\r\n"
    };

    xil_printf("%s", messages[measurement_state.golay_step]);
    measurement_state.software_acc_count = 0U;

    if (measurement_state.golay_step < GOLAY_STEP_BN) {
        measurement_state.golay_step =
            (golay_step_t)(measurement_state.golay_step + 1);
    }
}

static void process_golay(struct netif *netif, uint32_t elements)
{
    const otdr_config_t *config = otdr_config_get();

    int32_t *frame_accumulation =
        &accumulation_buffer[(uint32_t)measurement_state.golay_step * elements];

    if (measurement_state.software_acc_count == 0U) {
        memset(frame_accumulation, 0, elements * sizeof(int32_t));
    }

    int32_t *raw_data =
        capture_frame(netif,
                      (uint32_t)measurement_state.golay_step,
                      "Golay");
    if (raw_data == NULL) {
        return;
    }

    for (uint32_t i = 0; i < elements; ++i) {
        frame_accumulation[i] += raw_data[i];
    }

    ++measurement_state.software_acc_count;
    if (measurement_state.software_acc_count <
        config->software_acc_count) {
        return;
    }

    if (measurement_state.golay_step == GOLAY_STEP_BN) {
        complete_golay_step();
        finish_golay_measurement(elements);
        reset_measurement_state();
        return;
    }

    complete_golay_step();
}

void otdr_measurement_poll(struct netif *netif)
{
    handle_abort();

    if (!otdr_protocol_is_measuring() ||
        otdr_protocol_is_waiting_ack() ||
        !udp_client_connected) {
        return;
    }

    const otdr_config_t *config = otdr_config_get();
    const uint32_t elements =
        config->capture_size / sizeof(int32_t);
    if (elements == 0U || elements > MAX_CAPTURE_POINTS) {
        xil_printf("[Error] Invalid capture point count: %lu\r\n",
                   (unsigned long)elements);
        otdr_protocol_set_measuring(0);
        reset_measurement_state();
        return;
    }

    if (config->wave_type == 1U) {
        process_single_pulse(netif, elements);
    } else {
        process_golay(netif, elements);
    }
}
