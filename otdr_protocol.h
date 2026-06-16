#ifndef OTDR_PROTOCOL_H_
#define OTDR_PROTOCOL_H_

#include "xil_types.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"


#define OTDR_PORT                   5000
#define FRAME_SYNC_STRING           "GLinkOtdr-3800M"


#define CMD_HOST_START_MEASURE      0x10000000
#define CMD_HOST_STOP_MEASURE       0x10000001
#define CMD_HOST_SET_IP             0x10000002
#define CMD_HOST_NETWORK_IDLE       0x10000004
#define CMD_UPDATE_START      0x20000001
#define CMD_UPDATE_DATA       0x20000002
#define CMD_UPDATE_FINISH     0x20000003
#define CMD_DSP_UPLOAD_ALL_DATA     0x90000000
#define CMD_DSP_UPLOAD_REF_DATA     0x90000001
#define CMD_RESPONSE_STATE          0xA0000000


#define STATE_CODE_CMD_OK           0
#define STATE_CODE_CMD_ID_ERROR     4


#pragma pack(push, 1)
typedef struct {
    char     FrameSync[16];  // "GLinkOtdr-3800M\0"
    uint32_t TotalLength;
    uint32_t Rev;
    uint32_t FrameType;      // 0:Host->Target, 1:Target->Host
    uint32_t Src;
    uint32_t Dst;
    uint32_t PacketID;
    uint32_t RSVD1;
    uint32_t CmdCode;
    uint32_t DataLen;
} frame_header_t;

typedef struct start_measure {
    uint32_t  cmd;
    uint32_t  len;


    struct {
        uint32_t  OtdrMode;
        uint32_t  OtdrOptMode;
        uint32_t  RSVD;
        uint32_t  EnableRefresh;
        uint32_t  RefreshPeriod_ms;
    } Ctrl;

    struct {
        uint32_t  Lambda_nm;
        uint32_t  MeasureLength_m;
        uint32_t  PulseWidth_ns;
        uint32_t  MeasureTime_ms;
        float     n;
        float     EndThreshold;
        float     NonRelectThreshold;
    } State;


    uint32_t  Ext_HwAccTimes;
    uint32_t  Ext_SwAccTimes;
    uint32_t  Ext_ComSelIdx;
    uint32_t  Ext_AdcDelay;
    uint32_t  Ext_CaptureSamples;
    uint32_t  Ext_RcosEnable;         // 0: rectangular reference, 1: raised-cosine reference
    uint32_t  Ext_UploadMode;         // 0: debug float32, 1: official uint16 dB_x1000
} start_measure_t;

typedef struct {
    uint32_t SampleRate_Hz;
    uint32_t MeasureLength_m;
    uint32_t PulseWidth_ns;
    uint32_t Lambda_nm;
    uint32_t MeasureTime_ms;
    float    n;
    float    FiberLength;
    float    FiberLoss;
    float    FiberAttenCoef;
    float    NonRelectThreshold;
    float    EndThreshold;
    uint32_t OtdrMode;
    uint32_t MeasureMode;
} otdr_measure_param_t; // 52 bytes

typedef struct {
    uint32_t EventXlabel;
    uint32_t EventType;
    float    EventReflectLoss;
    float    EventInsertLoss;
    float    AttenCoef;
    float    EventTotalLoss;
} otdr_event_point_t; // 24 bytes

typedef struct {
    uint32_t StateCode;
    uint32_t RSVD2;
} otdr_state_resp_t;
#pragma pack(pop)


int  otdr_protocol_is_measuring(void);
void otdr_protocol_set_measuring(int state);
int  otdr_protocol_is_waiting_ack(void);
void otdr_protocol_set_waiting_ack(int state);
uint32_t otdr_protocol_next_packet_id(void);
err_t otdr_protocol_send_state(uint32_t state_code);


int otdr_protocol_dispatch_cmd(void *pcb,
                                const u8 *payload, u32 payload_len,
                                const u8 *full_frame, u32 total_len);

#endif /* OTDR_PROTOCOL_H_ */
