#include "otdr_protocol.h"
#include "otdr_config.h"
#include "otdr_tcp_server.h"
#include "otdr_hardware.h"
#include "otdr_measurement.h"
#include "otdr_ota.h"
#include "xil_printf.h"
#include "string.h"

/* Internal response constants. */
#define STATE_CODE_PACKET_LENTH_ERROR 5
#define STATE_CODE_IP_ERROR           20

/* Protocol runtime state. */
volatile int otdr_is_measuring = 0;
volatile int otdr_wait_for_ack = 0;

static uint32_t s_packet_id = 0U;

/* Measurement state accessors used by the measurement engine. */
int  otdr_protocol_is_measuring(void)          { return otdr_is_measuring; }
void otdr_protocol_set_measuring(int state)    { otdr_is_measuring = state; }
int  otdr_protocol_is_waiting_ack(void)        { return otdr_wait_for_ack; }
void otdr_protocol_set_waiting_ack(int state)  { otdr_wait_for_ack = state; }


extern err_t otdr_network_send(const u8 *data, u32 len);


uint32_t otdr_protocol_next_packet_id(void)
{
    return s_packet_id++;
}

err_t otdr_protocol_send_state(uint32_t state_code) {

    u8 tx_buf[sizeof(frame_header_t) + sizeof(otdr_state_resp_t)];
    frame_header_t *tx_hdr = (frame_header_t *)tx_buf;
    otdr_state_resp_t *tx_state = (otdr_state_resp_t *)(tx_buf + sizeof(frame_header_t));

    memset(tx_buf, 0, sizeof(tx_buf));


    strcpy(tx_hdr->FrameSync, FRAME_SYNC_STRING);
    tx_hdr->TotalLength = sizeof(tx_buf);
    tx_hdr->Rev         = 1;
    tx_hdr->FrameType   = 1;
    tx_hdr->PacketID    = otdr_protocol_next_packet_id();
    tx_hdr->CmdCode     = CMD_RESPONSE_STATE;
    tx_hdr->DataLen     = sizeof(otdr_state_resp_t);
    tx_hdr->RSVD1       = 0xffffeeee;


    tx_state->StateCode = state_code;
    tx_state->RSVD2     = 0xffffeeee;

    return otdr_network_send(tx_buf, sizeof(tx_buf));
}


int otdr_protocol_dispatch_cmd(void *pcb, const u8 *payload, u32 payload_len,
                                const u8 *full_frame, u32 total_len) {

    if (full_frame == NULL || total_len < sizeof(frame_header_t)) return 0;

    const frame_header_t *rx_hdr = (const frame_header_t *)full_frame;

    if (strncmp(rx_hdr->FrameSync, FRAME_SYNC_STRING, 15) != 0) return 0;

    if (otdr_ota_handle_command(rx_hdr->CmdCode,
                                payload, payload_len)) {
        return 1;
    }

    switch (rx_hdr->CmdCode) {


    case CMD_HOST_START_MEASURE: {
                if (payload_len >= 48) {
                    start_measure_t *start_params = (start_measure_t *)payload;
                    otdr_start_config_result_t config_result;

                    otdr_config_apply_start(start_params, &config_result);
                    const otdr_config_t *config = otdr_config_get();
                    otdr_hardware_configure_frontend(config_result.channel,
                                                     config->hardware_acc_count);

                    xil_printf("[OTDR] Start (%s): mode=%d, bytes=%d, hw_acc=%d, sw_acc=%d\r\n",
                               config_result.extended_format ?
                                   "extended" : "compatible",
                               config->wave_type,
                               config->capture_size,
                               config->hardware_acc_count,
                               config->software_acc_count);
                }


                otdr_is_measuring = 1;
                otdr_wait_for_ack = 0;
                otdr_protocol_send_state(STATE_CODE_CMD_OK);
                return 1;
            }


        case CMD_HOST_STOP_MEASURE: {
            uint32_t stop_type = 1;

            if (payload_len >= 8) {
                uint32_t *p_payload = (uint32_t *)payload;
                stop_type = p_payload[0];
            }

            xil_printf("[OTDR] Stop command received, type=%d (1=cancel, 2=finish)\r\n",
                       stop_type);
            otdr_is_measuring = 0;
            otdr_protocol_set_waiting_ack(0);

            if (stop_type == 1) {

                otdr_protocol_send_state(STATE_CODE_CMD_OK);
            }
            else if (stop_type == 2) {

                otdr_measurement_request_abort();
                otdr_protocol_send_state(STATE_CODE_CMD_OK);
            }
            return 1;
        }


        case CMD_HOST_SET_IP: {
            if (payload_len >= 48) {
                char new_ip[16], new_mask[16], new_gw[16];

                memcpy(new_ip, payload, 16);
                memcpy(new_mask, payload + 16, 16);
                memcpy(new_gw, payload + 32, 16);

                new_ip[15] = '\0'; new_mask[15] = '\0'; new_gw[15] = '\0';

                ip_addr_t ip, mask, gw;

                if (ipaddr_aton(new_ip, &ip) && ipaddr_aton(new_mask, &mask) && ipaddr_aton(new_gw, &gw)) {
                    xil_printf("[Network] Updating address: IP=%s, mask=%s, gateway=%s\r\n",
                               new_ip, new_mask, new_gw);
                    otdr_protocol_send_state(STATE_CODE_CMD_OK);


                    extern struct netif server_netif;
                    netif_set_addr(&server_netif, &ip, &mask, &gw);
                } else {
                    xil_printf("[Network] Invalid IP configuration\r\n");
                    otdr_protocol_send_state(STATE_CODE_IP_ERROR);
                }
            } else {
                otdr_protocol_send_state(STATE_CODE_PACKET_LENTH_ERROR);
            }
            return 1;
        }


        case CMD_HOST_NETWORK_IDLE:

            if (otdr_is_measuring &&
                otdr_config_get()->otdr_mode == 2U) {
                otdr_wait_for_ack = 0;
            }
            return 1;


        default:
            xil_printf("[OTDR] Unknown command: 0x%X\r\n", rx_hdr->CmdCode);
            otdr_protocol_send_state(STATE_CODE_CMD_ID_ERROR);
            return 1;
    }
}
