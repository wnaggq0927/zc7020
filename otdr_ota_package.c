#include "otdr_ota_package.h"

#include <string.h>

static uint32_t ota_crc32(const u8 *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (uint32_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static u8 ota_key_byte(uint32_t index, uint32_t target_id)
{
    uint32_t x = OTDR_OTA_KEY ^ target_id ^
                 (index * 1103515245U + 12345U);
    x ^= (x >> 16);
    x ^= (x >> 8);
    return (u8)x;
}

static uint32_t ota_header_crc(const otdr_ota_pkg_header_t *header)
{
    otdr_ota_pkg_header_t tmp;

    memcpy(&tmp, header, sizeof(tmp));
    tmp.header_crc32 = 0U;
    return ota_crc32((const u8 *)&tmp, (uint32_t)sizeof(tmp));
}

int otdr_ota_prepare_image(u8 *file_buf,
                           uint32_t file_size,
                           u8 *image_buf,
                           uint32_t image_buf_size,
                           u8 **image_out,
                           uint32_t *image_size_out,
                           otdr_ota_pkg_info_t *info_out)
{
    otdr_ota_pkg_header_t header;
    const u8 *encrypted;

    if (file_buf == NULL || image_buf == NULL ||
        image_out == NULL || image_size_out == NULL ||
        info_out == NULL) {
        return 0;
    }

    memset(info_out, 0, sizeof(*info_out));

    if (file_size < (uint32_t)sizeof(header)) {
        *image_out = file_buf;
        *image_size_out = file_size;
        return 1;
    }

    memcpy(&header, file_buf, sizeof(header));
    if (header.magic != OTDR_OTA_PKG_MAGIC) {
        *image_out = file_buf;
        *image_size_out = file_size;
        return 1;
    }

    if ((header.version != OTDR_OTA_PKG_VERSION &&
         header.version != OTDR_OTA_PKG_VERSION_V1) ||
        header.target_id != OTDR_OTA_TARGET_ID ||
        header.plain_size == 0U ||
        header.encrypted_size != header.plain_size ||
        header.plain_size > image_buf_size ||
        file_size < (uint32_t)sizeof(header) ||
        header.encrypted_size > (file_size - (uint32_t)sizeof(header))) {
        return 0;
    }

    if (ota_header_crc(&header) != header.header_crc32) {
        return 0;
    }

    encrypted = file_buf + sizeof(header);
    for (uint32_t i = 0U; i < header.plain_size; ++i) {
        image_buf[i] = encrypted[i] ^ ota_key_byte(i, header.target_id);
    }

    if (ota_crc32(image_buf, header.plain_size) != header.plain_crc32) {
        memset(image_buf, 0, header.plain_size);
        return 0;
    }

    info_out->is_package = 1U;
    info_out->package_version = header.version;
    info_out->target_id = header.target_id;
    info_out->firmware_version =
        (header.version >= OTDR_OTA_PKG_VERSION) ? header.firmware_version : 0U;

    *image_out = image_buf;
    *image_size_out = header.plain_size;
    return 1;
}
