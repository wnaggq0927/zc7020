#include "otdr_ota.h"

#include "otdr_protocol.h"
#include "qspi_g128_flash.h"

#include "sleep.h"
#include "xil_io.h"
#include "xil_printf.h"

#include <stdint.h>
#include <string.h>

#define OTA_FILE_BASE_ADDR   0x10000000U
#define OTA_READ_BASE_ADDR   0x11000000U
#define OTA_WRITE_BASE_ADDR  0x12000000U
#define SLCR_UNLOCK_ADDR     0xF8000008U
#define SLCR_PSS_RST_CTRL    0xF8000200U

static uint32_t total_size;
static uint32_t received_size;

int otdr_ota_handle_command(u32 command, const u8 *payload,
                            u32 payload_len)
{
    switch (command) {
    case CMD_UPDATE_START:
        if (payload_len >= sizeof(uint32_t)) {
            memcpy(&total_size, payload, sizeof(total_size));
            received_size = 0U;
            memset((void *)OTA_FILE_BASE_ADDR, 0, total_size);
            xil_printf("[OTA] Update requested, image size=%d bytes\r\n",
                       total_size);
            otdr_protocol_send_state(STATE_CODE_CMD_OK);
        }
        return 1;

    case CMD_UPDATE_DATA:
        if (payload_len >= sizeof(uint32_t)) {
            uint32_t offset;
            memcpy(&offset, payload, sizeof(offset));
            const uint32_t chunk_len =
                payload_len - sizeof(offset);
            memcpy((u8 *)OTA_FILE_BASE_ADDR + offset,
                   payload + sizeof(offset), chunk_len);
            received_size += chunk_len;
            otdr_protocol_send_state(STATE_CODE_CMD_OK);
        }
        return 1;

    case CMD_UPDATE_FINISH:
        xil_printf("[OTA] Transfer finished, expected=%d, received=%d\r\n",
                   total_size, received_size);
        otdr_protocol_send_state(STATE_CODE_CMD_OK);

        if (received_size == total_size && total_size > 0U) {
            xil_printf("[OTA] Writing image to QSPI flash...\r\n");
            update_flash((u8 *)OTA_FILE_BASE_ADDR,
                         (u8 *)OTA_READ_BASE_ADDR,
                         (u8 *)OTA_WRITE_BASE_ADDR,
                         total_size);
            xil_printf("[OTA] Flash update complete, rebooting...\r\n");
            usleep(500000U);
            Xil_Out32(SLCR_UNLOCK_ADDR, 0xDF0DU);
            Xil_Out32(SLCR_PSS_RST_CTRL, 0x01U);
        } else {
            xil_printf("[OTA] Image size mismatch, update aborted\r\n");
        }
        return 1;

    default:
        return 0;
    }
}
