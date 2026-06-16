#ifndef OTDR_UPLOAD_H
#define OTDR_UPLOAD_H

#include "lwip/err.h"
#include "xil_types.h"

err_t otdr_upload_reference(const u8 *raw_data, u32 data_len);
err_t otdr_upload_int32(const u8 *raw_data, u32 data_len);
err_t otdr_upload_float32(const u8 *raw_data, u32 data_len,
                          u32 point_count,
                          const float *event_curve,
                          u32 event_point_count);
err_t otdr_upload_official_float(const float *curve, u32 point_count);

#endif
