#ifndef OTDR_MEASUREMENT_H
#define OTDR_MEASUREMENT_H

#include "netif/xadapter.h"

/*
 * Run one measurement-engine iteration.
 * Network input remains serviced while a DMA transfer is in progress.
 */
void otdr_measurement_poll(struct netif *netif);

/* Request termination after the current capture boundary. */
void otdr_measurement_request_abort(void);

#endif
