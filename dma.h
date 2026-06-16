#ifndef DMA_H_
#define DMA_H_

#include "xaxidma.h"
#include "xscugic.h"
#include "xil_types.h"

#define UDP_PAYLOAD_SIZE    1024

/*
 * Initialize AXI DMA and connect its receive interrupt.
 */
int dma_init(XScuGic *IntcPtr);

/* Return nonzero after the current receive transfer completes. */
int dma_is_rx_done(void);

/* Clear the receive-complete flag. */
void dma_clear_rx_done(void);

/*
 * Switch ping-pong buffers and return the completed receive buffer.
 */
u8 *dma_switch_and_get_data(void);
void dma_reset(void);
void dma_start_transfer(void);
#endif /* DMA_H_ */
