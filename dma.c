#include "dma.h"
#include "otdr_config.h"

static u8 *RxBuffer_A = (u8 *)0x06000000;
static u8 *RxBuffer_B = (u8 *)0x07000000;

/* Ping-pong receive state shared with the DMA interrupt handler. */
static volatile int current_buffer = 0;

static volatile u8 rx_done_flag = 0;

static XAxiDma AxiDma;

#define DMA_DEV_ID   XPAR_AXIDMA_0_DEVICE_ID
#define RX_INTR_ID   XPAR_FABRIC_AXIDMA_0_VEC_ID

static void RxIntrHandler(void *Callback)
{
    XAxiDma *AxiDmaInst = (XAxiDma *)Callback;

    u32 IrqStatus = XAxiDma_IntrGetIrq(AxiDmaInst, XAXIDMA_DEVICE_TO_DMA);

    XAxiDma_IntrAckIrq(AxiDmaInst, IrqStatus, XAXIDMA_DEVICE_TO_DMA);

    if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK))
        return;

    if (IrqStatus & XAXIDMA_IRQ_IOC_MASK) {
        rx_done_flag = 1;
    }
}

int dma_init(XScuGic *IntcPtr)
{
    int Status;

    XAxiDma_Config *Config = XAxiDma_LookupConfig(DMA_DEV_ID);
    if (!Config)
        return XST_FAILURE;

    Status = XAxiDma_CfgInitialize(&AxiDma, Config);
    if (Status != XST_SUCCESS)
        return XST_FAILURE;

    Status = XScuGic_Connect(IntcPtr, RX_INTR_ID,
                             (Xil_InterruptHandler)RxIntrHandler, &AxiDma);
    if (Status != XST_SUCCESS)
        return Status;

    XScuGic_Enable(IntcPtr, RX_INTR_ID);

    XAxiDma_IntrDisable(&AxiDma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrEnable(&AxiDma, XAXIDMA_IRQ_IOC_MASK, XAXIDMA_DEVICE_TO_DMA);

    current_buffer = 0;
    return XST_SUCCESS;
}

int dma_is_rx_done(void)
{
    return rx_done_flag;
}

void dma_clear_rx_done(void)
{
    rx_done_flag = 0;
}

u8 *dma_switch_and_get_data(void)
{
    u8 *send_ptr;

    if (current_buffer == 0) {

        XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)RxBuffer_B,
                               otdr_config_get()->capture_size,
                               XAXIDMA_DEVICE_TO_DMA);
        send_ptr = RxBuffer_A;
        current_buffer = 1;
    } else {

        XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)RxBuffer_A,
                               otdr_config_get()->capture_size,
                               XAXIDMA_DEVICE_TO_DMA);
        send_ptr = RxBuffer_B;
        current_buffer = 0;
    }

    return send_ptr;
}

void dma_start_transfer(void)
{
    rx_done_flag = 0;
    current_buffer = 0;

    int status = XAxiDma_SimpleTransfer(&AxiDma, (UINTPTR)RxBuffer_A,
                           otdr_config_get()->capture_size,
                           XAXIDMA_DEVICE_TO_DMA);
    if (status != XST_SUCCESS) {

        xil_printf("[DMA] Receive transfer start failed, status=%d\r\n",
                   status);
    }
}

void dma_reset(void)
{
    XAxiDma_Reset(&AxiDma);

    while (!XAxiDma_ResetIsDone(&AxiDma));
}
