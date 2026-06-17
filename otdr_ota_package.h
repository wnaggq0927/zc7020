#ifndef OTDR_OTA_PACKAGE_H
#define OTDR_OTA_PACKAGE_H

#include "xil_types.h"

#include <stdint.h>

#define OTDR_OTA_PKG_MAGIC      0x5041544FU
#define OTDR_OTA_PKG_VERSION_V1 1U
#define OTDR_OTA_PKG_VERSION    2U
#define OTDR_OTA_TARGET_ID      0x4F544452U
#define OTDR_OTA_KEY            0xA5C35A96U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t target_id;
    uint32_t plain_size;
    uint32_t encrypted_size;
    uint32_t plain_crc32;
    uint32_t header_crc32;
    uint32_t firmware_version;
} otdr_ota_pkg_header_t;

typedef struct {
    uint32_t is_package;
    uint32_t package_version;
    uint32_t target_id;
    uint32_t firmware_version;
} otdr_ota_pkg_info_t;

int otdr_ota_prepare_image(u8 *file_buf,
                           uint32_t file_size,
                           u8 *image_buf,
                           uint32_t image_buf_size,
                           u8 **image_out,
                           uint32_t *image_size_out,
                           otdr_ota_pkg_info_t *info_out);

#endif
