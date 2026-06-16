#include "sys_intr.h"
#include "dma.h"
#include "otdr_tcp_server.h"
#include "otdr_measurement.h"
#include "qspi_g128_flash.h"
#include "otdr_dsp.h"

#include "lwipopts.h"
#include "netif/xadapter.h"
#include "xil_cache.h"
#include "xqspips.h"

#define QSPI_DEVICE_ID XPAR_XQSPIPS_0_DEVICE_ID

extern void lwip_init(void);
extern void otdr_watchdog_poll(void);

XQspiPs QspiInstance;
static XScuGic Intc;
struct netif server_netif;

int main(void)
{
    int status;
    struct netif *netif;
    ip_addr_t ipaddr;
    ip_addr_t netmask;
    ip_addr_t gateway;
    unsigned char mac_address[] = {
        0x00, 0x0a, 0x35, 0x00, 0x01, 0x02
    };

    Xil_ICacheEnable();
    Xil_DCacheFlush();
    Xil_DCacheEnable();

    status = Init_qspi(&QspiInstance, QSPI_DEVICE_ID);
    if (status != XST_SUCCESS) {
        xil_printf("QSPI Init Failed\r\n");
    }

    Init_Intr_System(&Intc);
    Setup_Intr_Exception(&Intc);

    status = dma_init(&Intc);
    if (status != XST_SUCCESS) {
        xil_printf("DMA Init Failed\r\n");
    }

    netif = &server_netif;
    IP4_ADDR(&ipaddr, 192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 192, 168, 1, 1);

    lwip_init();
    xemac_add(netif,
              &ipaddr,
              &netmask,
              &gateway,
              mac_address,
              XPAR_XEMACPS_0_BASEADDR);
    netif_set_default(netif);
    netif_set_up(netif);

    otdr_network_init();
    xil_printf("\r\n=== OTDR System Boot Complete ===\r\n");
    dsp_self_test_corr();

    while (1) {
        xemacif_input(netif);
        otdr_watchdog_poll();
        otdr_measurement_poll(netif);
    }

    return 0;
}
