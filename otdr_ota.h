#ifndef OTDR_OTA_H
#define OTDR_OTA_H

#include "xil_types.h"

/*
 * Handle one OTA command.
 * Returns 1 when the command belongs to the OTA subsystem.
 */
int otdr_ota_handle_command(u32 command, const u8 *payload,
                            u32 payload_len);

#endif
