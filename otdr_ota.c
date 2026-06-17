#include "otdr_ota.h"

#include "otdr_ota_package.h"
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
#define OTA_IMAGE_BASE_ADDR  0x13000000U
#define OTA_OLD_IMAGE_ADDR   0x14000000U
#define OTA_BOOT_FLASH_ADDR  0x00000000U
#define OTA_BACKUP_FLASH_ADDR 0x00800000U
#define OTA_SLOT_SIZE        (8U * 1024U * 1024U)
#define OTA_MAX_FILE_SIZE    (OTA_SLOT_SIZE + 4096U)
#define SLCR_UNLOCK_ADDR     0xF8000008U
#define SLCR_PSS_RST_CTRL    0xF8000200U

static uint32_t total_size;
static uint32_t received_size;

static void ota_send_fail(void)
{
    otdr_protocol_send_state(STATE_CODE_OTDR_UPDATE_FAIL);
}

static int ota_write_image_with_backup(u8 *image_buf, uint32_t image_size)
{
    xil_printf("[OTA] Backing up current boot image...\r\n");
    read_flash_at(OTA_BOOT_FLASH_ADDR,
                  (u8 *)OTA_OLD_IMAGE_ADDR,
                  (u8 *)OTA_WRITE_BASE_ADDR,
                  image_size);

    if (!update_flash_at(OTA_BACKUP_FLASH_ADDR,
                         (u8 *)OTA_OLD_IMAGE_ADDR,
                         (u8 *)OTA_READ_BASE_ADDR,
                         (u8 *)OTA_WRITE_BASE_ADDR,
                         image_size)) {
        xil_printf("[OTA] Backup slot verify failed, update aborted\r\n");
        return 0;
    }

    xil_printf("[OTA] Writing new image to boot slot...\r\n");
    if (update_flash_at(OTA_BOOT_FLASH_ADDR,
                        image_buf,
                        (u8 *)OTA_READ_BASE_ADDR,
                        (u8 *)OTA_WRITE_BASE_ADDR,
                        image_size)) {
        return 1;
    }

    xil_printf("[OTA] New image verify failed, restoring backup...\r\n");
    if (!update_flash_at(OTA_BOOT_FLASH_ADDR,
                         (u8 *)OTA_OLD_IMAGE_ADDR,
                         (u8 *)OTA_READ_BASE_ADDR,
                         (u8 *)OTA_WRITE_BASE_ADDR,
                         image_size)) {
        xil_printf("[OTA] Backup restore failed, manual recovery required\r\n");
        return 0;
    }

    xil_printf("[OTA] Backup restored, update aborted\r\n");
    return 0;
}

int otdr_ota_handle_command(u32 command, const u8 *payload,
                            u32 payload_len)
{
    switch (command) {
    case CMD_UPDATE_START:
        if (payload_len >= sizeof(uint32_t)) {
            memcpy(&total_size, payload, sizeof(total_size));
            if (total_size == 0U || total_size > OTA_MAX_FILE_SIZE) {
                xil_printf("[OTA] Invalid file size=%d bytes\r\n",
                           total_size);
                total_size = 0U;
                received_size = 0U;
                ota_send_fail();
                return 1;
            }

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
            const uint32_t end_offset = offset + chunk_len;

            if (total_size == 0U ||
                chunk_len == 0U ||
                offset > total_size ||
                end_offset < offset ||
                end_offset > total_size) {
                xil_printf("[OTA] Invalid data chunk, offset=%d len=%d total=%d\r\n",
                           offset, chunk_len, total_size);
                ota_send_fail();
                return 1;
            }

            memcpy((u8 *)OTA_FILE_BASE_ADDR + offset,
                   payload + sizeof(offset), chunk_len);
            if (end_offset > received_size) {
                received_size = end_offset;
            }
            otdr_protocol_send_state(STATE_CODE_CMD_OK);
        }
        return 1;

    case CMD_UPDATE_FINISH:
        xil_printf("[OTA] Transfer finished, expected=%d, received=%d\r\n",
                   total_size, received_size);
        if (received_size == total_size && total_size > 0U) {
            u8 *image_buf = NULL;
            uint32_t image_size = 0U;
            otdr_ota_pkg_info_t pkg_info;

            if (!otdr_ota_prepare_image((u8 *)OTA_FILE_BASE_ADDR,
                                        total_size,
                                        (u8 *)OTA_IMAGE_BASE_ADDR,
                                        OTA_MAX_FILE_SIZE,
                                        &image_buf,
                                        &image_size,
                                        &pkg_info)) {
                xil_printf("[OTA] Package check failed, update aborted\r\n");
                otdr_protocol_send_state(STATE_CODE_FILE_CONTENT_ERROR);
                return 1;
            }

            if (pkg_info.is_package) {
                xil_printf("[OTA] Package OK: format=%d target=0x%08x firmware=%d\r\n",
                           pkg_info.package_version,
                           pkg_info.target_id,
                           pkg_info.firmware_version);
            } else {
                xil_printf("[OTA] Legacy raw BOOT image accepted\r\n");
            }

            if (image_size == 0U || image_size > OTA_SLOT_SIZE) {
                xil_printf("[OTA] Image too large for A/B slots, size=%d max=%d\r\n",
                           image_size, OTA_SLOT_SIZE);
                otdr_protocol_send_state(STATE_CODE_FILE_CONTENT_ERROR);
                return 1;
            }

            otdr_protocol_send_state(STATE_CODE_CMD_OK);
            if (ota_write_image_with_backup(image_buf, image_size)) {
                xil_printf("[OTA] Flash update complete, rebooting...\r\n");
                usleep(500000U);
                Xil_Out32(SLCR_UNLOCK_ADDR, 0xDF0DU);
                Xil_Out32(SLCR_PSS_RST_CTRL, 0x01U);
            } else {
                ota_send_fail();
            }
        } else {
            xil_printf("[OTA] Image size mismatch, update aborted\r\n");
            ota_send_fail();
        }
        return 1;

    default:
        return 0;
    }
}
