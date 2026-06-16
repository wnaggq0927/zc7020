#include "otdr_upload.h"

#include "otdr_config.h"
#include "otdr_event.h"
#include "otdr_protocol.h"
#include "otdr_tcp_server.h"

#include "xil_printf.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define OTDR_DATA_FORMAT_FLOAT32 0x80000000U
#define OTDR_DATA_FORMAT_INT32   0x40000000U
#define OTDR_FRAME_FOOTER        0xffffeeeeU
#define OFFICIAL_CURVE_GAIN      1.0e-3f
#define MAX_TX_BUFFER_SIZE       (10U * 1024U * 1024U + 500U * 1024U)

static u8 tx_buffer[MAX_TX_BUFFER_SIZE];
static otdr_event_point_t upload_events[OTDR_MAX_EVENTS];

extern void otdr_watchdog_poll(void);

static void fill_frame_header(frame_header_t *header, uint32_t command,
                              uint32_t payload_len)
{
    memset(header, 0, sizeof(*header));
    strcpy(header->FrameSync, FRAME_SYNC_STRING);
    header->TotalLength = sizeof(*header) + payload_len;
    header->Rev = 1U;
    header->FrameType = 1U;
    header->PacketID = otdr_protocol_next_packet_id();
    header->RSVD1 = OTDR_FRAME_FOOTER;
    header->CmdCode = command;
    header->DataLen = payload_len;
}

static void fill_measurement_parameters(otdr_measure_param_t *param)
{
    const otdr_config_t *config = otdr_config_get();

    memset(param, 0, sizeof(*param));
    param->SampleRate_Hz =
        config->downsample_enable ? 125000000U : 250000000U;
    param->MeasureLength_m = config->measure_range_m;
    param->PulseWidth_ns = config->pulse_width_ns;
    param->Lambda_nm = config->wavelength_nm;
    param->MeasureTime_ms = 15000U;
    param->n = config->refractive_index;
    param->FiberLength = 1000.0f;
    param->OtdrMode = config->otdr_mode;
    param->MeasureMode = 1U;
}

static err_t upload_measurement_payload(const u8 *raw_data,
                                        uint32_t data_len,
                                        uint32_t data_num,
                                        const float *event_curve,
                                        uint32_t event_point_count)
{
    otdr_measure_param_t param;
    const uint32_t event_num =
        otdr_event_analyze(event_curve,
                           event_point_count,
                           upload_events,
                           OTDR_MAX_EVENTS);
    const uint32_t event_len =
        event_num * sizeof(otdr_event_point_t);
    const uint32_t payload_len =
        sizeof(param) + sizeof(data_num) + data_len +
        sizeof(event_num) + event_len + sizeof(uint32_t);
    const uint32_t total_len = sizeof(frame_header_t) + payload_len;

    if (total_len > MAX_TX_BUFFER_SIZE) {
        return ERR_MEM;
    }

    fill_measurement_parameters(&param);
    fill_frame_header((frame_header_t *)tx_buffer,
                      CMD_DSP_UPLOAD_ALL_DATA, payload_len);

    u8 *ptr = tx_buffer + sizeof(frame_header_t);
    memcpy(ptr, &param, sizeof(param));
    ptr += sizeof(param);
    memcpy(ptr, &data_num, sizeof(data_num));
    ptr += sizeof(data_num);
    memcpy(ptr, raw_data, data_len);
    ptr += data_len;
    memcpy(ptr, &event_num, sizeof(event_num));
    ptr += sizeof(event_num);
    if (event_len > 0U) {
        memcpy(ptr, upload_events, event_len);
        ptr += event_len;
    }

    const uint32_t footer = OTDR_FRAME_FOOTER;
    memcpy(ptr, &footer, sizeof(footer));

    return otdr_network_send(tx_buffer, total_len);
}

err_t otdr_upload_reference(const u8 *raw_data, u32 data_len)
{
    const uint32_t data_num = data_len / 2U;
    const uint32_t payload_len =
        sizeof(uint32_t) * 3U + data_len;
    const uint32_t total_len = sizeof(frame_header_t) + payload_len;

    if (total_len > MAX_TX_BUFFER_SIZE) {
        return ERR_MEM;
    }

    fill_frame_header((frame_header_t *)tx_buffer,
                      CMD_DSP_UPLOAD_REF_DATA, payload_len);

    u8 *payload = tx_buffer + sizeof(frame_header_t);
    const uint32_t command = CMD_DSP_UPLOAD_REF_DATA;
    const uint32_t footer = OTDR_FRAME_FOOTER;
    memcpy(payload, &command, sizeof(command));
    memcpy(payload + 4U, &data_num, sizeof(data_num));
    memcpy(payload + 8U, raw_data, data_len);
    memcpy(payload + 8U + data_len, &footer, sizeof(footer));

    return otdr_network_send(tx_buffer, total_len);
}

err_t otdr_upload_int32(const u8 *raw_data, u32 data_len)
{
    const uint32_t point_count = data_len / sizeof(int32_t);
    const uint32_t data_num =
        OTDR_DATA_FORMAT_INT32 | (point_count & 0x3fffffffU);
    return upload_measurement_payload(raw_data, data_len, data_num,
                                      NULL, 0U);
}

err_t otdr_upload_float32(const u8 *raw_data, u32 data_len,
                          u32 point_count,
                          const float *event_curve,
                          u32 event_point_count)
{
    const uint32_t data_num =
        OTDR_DATA_FORMAT_FLOAT32 | (point_count & 0x7fffffffU);
    return upload_measurement_payload(raw_data, data_len, data_num,
                                      event_curve, event_point_count);
}

err_t otdr_upload_official_float(const float *curve, u32 point_count)
{
    otdr_measure_param_t param;
    const uint32_t data_len = point_count * sizeof(uint16_t);
    const uint32_t event_num = 0U;
    const uint32_t payload_len =
        sizeof(param) + sizeof(point_count) + data_len +
        sizeof(event_num) + sizeof(uint32_t);
    const uint32_t total_len = sizeof(frame_header_t) + payload_len;

    if (total_len > MAX_TX_BUFFER_SIZE) {
        return ERR_MEM;
    }

    fill_measurement_parameters(&param);
    fill_frame_header((frame_header_t *)tx_buffer,
                      CMD_DSP_UPLOAD_ALL_DATA, payload_len);

    u8 *ptr = tx_buffer + sizeof(frame_header_t);
    memcpy(ptr, &param, sizeof(param));
    ptr += sizeof(param);
    memcpy(ptr, &point_count, sizeof(point_count));
    ptr += sizeof(point_count);

    uint16_t *out_curve = (uint16_t *)ptr;
    float min_db = 50.0f;
    float max_db = -5.0f;

    for (uint32_t i = 0U; i < point_count; ++i) {
        float amplitude = curve[i];
        if (amplitude < 0.0f) {
            amplitude = -amplitude;
        }
        if (amplitude < 1.0e-6f) {
            amplitude = 1.0e-6f;
        }

        amplitude *= OFFICIAL_CURVE_GAIN;
        if (amplitude < 1.0e-6f) {
            amplitude = 1.0e-6f;
        }

        float db = 20.0f * log10f(amplitude);
        if (db < -5.0f) {
            db = -5.0f;
        }
        if (db > 50.0f) {
            db = 50.0f;
        }
        if (db < min_db) {
            min_db = db;
        }
        if (db > max_db) {
            max_db = db;
        }

        uint32_t encoded =
            (uint32_t)((db + 5.0f) * 1000.0f + 0.5f);
        if (encoded > 65535U) {
            encoded = 65535U;
        }
        out_curve[i] = (uint16_t)encoded;

        if ((i % 30000U) == 0U) {
            otdr_watchdog_poll();
        }
    }
    ptr += data_len;
    memcpy(ptr, &event_num, sizeof(event_num));
    ptr += sizeof(event_num);

    const uint32_t footer = OTDR_FRAME_FOOTER;
    memcpy(ptr, &footer, sizeof(footer));

    xil_printf("[UPLOAD] official curve: points=%d, db_x1000=[%d,%d]\r\n",
               point_count,
               (int)(min_db * 1000.0f),
               (int)(max_db * 1000.0f));

    return otdr_network_send(tx_buffer, total_len);
}
