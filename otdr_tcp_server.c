#include "otdr_tcp_server.h"   // 本模块对应的头文件
#include "otdr_protocol.h"     // OTDR 自定义协议处理函数
#include "xil_printf.h"        // Xilinx 精简 printf
#include "lwip/udp.h"          // LwIP UDP 协议栈 API
#include "lwip/tcp.h"          // [新增] LwIP TCP 协议栈 API，用于心跳服务器
#include "string.h"            // 内存和字符串操作
#include "sleep.h"             // 微秒延时
#include "xtime_l.h"           // [新增] Zynq 高精度硬件定时器，用于超时计算
#include "xil_io.h"            // [新增] 底层寄存器操作，用于系统复位

// ======================== [新增] 看门狗与心跳状态变量 ========================
#define SLCR_UNLOCK_ADDR    0xF8000008   // SLCR 解锁寄存器地址
#define SLCR_PSS_RST_CTRL   0xF8000200   // PS 系统复位控制寄存器

static XTime last_heartbeat_time = 0;    // 记录最后一次有效通信的硬件时间
static int otdr_has_connected = 0;       // 标记系统是否曾经被上位机连接过

// 全局 UDP 控制块，用于监听和发送
struct udp_pcb *otdr_udp_pcb = NULL;

// 用于记住上位机（Python）的地址和端口
ip_addr_t client_ip;            // 客户端 IP 地址
u16_t client_port = 0;          // 客户端端口号
int udp_client_connected = 0;   // 是否已有客户端发送过命令（可向其回复数据）

// 引用外部定义的网络接口，用于轮询接收底层数据包
extern struct netif server_netif;
extern void xemacif_input(struct netif *netif);

// ======================== [新增] 心跳刷新与看门狗轮询 ========================

/**
 * @brief 刷新心跳时间，只要收到任何上位机数据（UDP指令、5555端口、6666端口）就调用
 */
void otdr_heartbeat_update(void)
{
    XTime_GetTime(&last_heartbeat_time);
    otdr_has_connected = 1;
}

/**
 * @brief 看门狗轮询函数（需要在 main.c 的 while(1) 中循环调用）
 */
void otdr_watchdog_poll(void)
{
    XTime now;
    XTime_GetTime(&now);

    // 如果从来没被连接过，等待 1 小时 (3600秒) 后自动复位防死机
    if (!otdr_has_connected) {
        if ((now / COUNTS_PER_SECOND) > 3600) {
            xil_printf("\r\n[Watchdog] 1小时无连接，系统自动安全复位...\r\n");
            Xil_Out32(SLCR_UNLOCK_ADDR, 0xDF0D);
            Xil_Out32(SLCR_PSS_RST_CTRL, 0x01);
        }
        return;
    }

    // 如果已经被连接过，检查 1 分钟 (60秒) 心跳超时
    uint32_t elapsed_seconds = (now - last_heartbeat_time) / COUNTS_PER_SECOND;
    if (elapsed_seconds > 60) {
        xil_printf("\r\n[Watchdog] 心跳超时(>60s)，TCP连接异常，系统自动复位...\r\n");
        Xil_Out32(SLCR_UNLOCK_ADDR, 0xDF0D);
        Xil_Out32(SLCR_PSS_RST_CTRL, 0x01);
    }
}

// ======================== [新增] TCP Echo 服务器 (端口 5555) ========================

static err_t echo_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (p == NULL) { // 客户端正常关闭连接
        tcp_close(tpcb);
        return ERR_OK;
    }

    otdr_heartbeat_update(); // 刷新心跳

    tcp_recved(tpcb, p->tot_len); // 通知 LwIP 已经处理了这些数据

    // 原封不动发回去，限制最大 64 字节
    u16 send_len = (p->tot_len > 64) ? 64 : p->tot_len;
    tcp_write(tpcb, p->payload, send_len, TCP_WRITE_FLAG_COPY);

    pbuf_free(p);
    return ERR_OK;
}

static err_t echo_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    tcp_recv(newpcb, echo_recv_cb);
    return ERR_OK;
}

static void echo_server_init(void)
{
    struct tcp_pcb *pcb = tcp_new();
    if (pcb) {
        tcp_bind(pcb, IP_ADDR_ANY, 5555);
        pcb = tcp_listen(pcb);
        tcp_accept(pcb, echo_accept_cb);
    }
}

// ======================== [新增] TCP Daytime 服务器 (端口 6666) ========================

static err_t daytime_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    otdr_heartbeat_update(); // 刷新心跳

    // 获取开机到现在的毫秒数
    XTime now;
    XTime_GetTime(&now);
    uint32_t ms = (uint32_t)(1000ULL * now / COUNTS_PER_SECOND);

    // 转为字符串发送，不超过 16 字节
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", ms);

    tcp_write(newpcb, buf, strlen(buf), TCP_WRITE_FLAG_COPY);
    tcp_output(newpcb); // 强制立刻发送

    tcp_close(newpcb);  // 发送完毕立刻断开 TCP 连接

    return ERR_OK;
}

static void daytime_server_init(void)
{
    struct tcp_pcb *pcb = tcp_new();
    if (pcb) {
        tcp_bind(pcb, IP_ADDR_ANY, 6666);
        pcb = tcp_listen(pcb);
        tcp_accept(pcb, daytime_accept_cb);
    }
}

// ======================== UDP 接收与解析回调 ========================
/**
 * @brief UDP 数据接收回调函数
 */
static void otdr_udp_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                   const ip_addr_t *addr, u16_t port)
{
    if (p == NULL) return;   // 空包忽略

    // 【核心逻辑】：收到任何命令后，立刻记录发送者的 IP 和端口，以便后续回复
    client_ip = *addr;
    client_port = port;
    udp_client_connected = 1;

    // [新增] 只要收到主通讯链路的命令，也算作心跳
    otdr_heartbeat_update();

    // UDP 通常不存在 TCP 那样的粘包/半包问题，一个数据报就是一帧
    if (p->tot_len >= sizeof(frame_header_t)) {
        u8 *rx_data = (u8 *)p->payload;
        frame_header_t *hdr = (frame_header_t *)rx_data;   // 将 payload 强制转为帧头结构

        // 校验帧同步头是否为 "GLinkOtdr-3800M"
        if (strncmp((char *)hdr->FrameSync, FRAME_SYNC_STRING, 15) == 0) {
            u32 total_len = hdr->TotalLength;   // 帧头中声明的总长度
            if (p->tot_len >= total_len) {      // 确保接收完整帧
                u32 payload_len = total_len - sizeof(frame_header_t); // 有效载荷长度
                u32 payload_ofs = sizeof(frame_header_t);             // 载荷偏移

                // 将数据移交给原有的协议分发器处理（如启动测量、停止等命令）
                otdr_protocol_dispatch_cmd(NULL,
                                           &rx_data[payload_ofs], payload_len,
                                           rx_data, total_len);
            }
        }
    }

    // 释放接收到的 pbuf，防止内存泄漏
    pbuf_free(p);
}

// ======================== UDP及心跳初始化 ========================
/**
 * @brief 初始化 OTDR 网络服务器群组
 * @return 0 成功，-1 失败
 */
int otdr_network_init(void)
{
    // [新增] 初始化心跳计时器
    XTime_GetTime(&last_heartbeat_time);

    // [新增] 启动 TCP 心跳辅助服务器
    echo_server_init();
    daytime_server_init();

    // 创建新的 UDP 控制块
    otdr_udp_pcb = udp_new();
    if (!otdr_udp_pcb) return -1;

    // 绑定到本地所有 IP 地址的 OTDR_PORT 端口（通常 5000）
    if (udp_bind(otdr_udp_pcb, IP_ADDR_ANY, OTDR_PORT) != ERR_OK)
        return -1;

    // 注册接收回调函数
    udp_recv(otdr_udp_pcb, otdr_udp_recv_callback, NULL);

    xil_printf("Server Started (UDP:5000 | Echo:5555 | Daytime:6666)\r\n");
    return 0;
}

// ======================== UDP 极速切片发送 ========================
/**
 * @brief 通过 UDP 向已记录的上位机发送数据
 */
err_t otdr_network_send(const u8 *data, u32 len)
{
    // 如果还没有客户端连接过（没人发过命令），则拒绝发送
    if (!udp_client_connected || otdr_udp_pcb == NULL) return ERR_CONN;

    u32 sent = 0;   // 已发送的字节数
    while (sent < len) {
        // 【核心切片】：以太网 MTU 为 1500，UDP 安全切片大小设为 1400 字节
        u16 chunk = (len - sent > 1400) ? 1400 : (len - sent);

        // 尝试分配 pbuf，若失败则主动轮询网卡底层，将积压的发送包刷出，腾出内存
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, chunk, PBUF_RAM);
        while (p == NULL) {
            // 调用网卡输入处理函数，实际上会触发发送完成回调、释放已发送的 pbuf
            xemacif_input(&server_netif);
            // 再次尝试分配
            p = pbuf_alloc(PBUF_TRANSPORT, chunk, PBUF_RAM);
        }

        // 将当前分片的数据拷贝到 pbuf 的 payload 中
        pbuf_take(p, data + sent, chunk);

        // 向之前记录的客户端 IP 和端口发送该分片
        udp_sendto(otdr_udp_pcb, p, &client_ip, client_port);

        // 释放 pbuf
        pbuf_free(p);

        sent += chunk;       // 更新已发送计数
        usleep(500);         // 微秒级延时，防止连续发送过快导致拥塞
    }

    return ERR_OK;
}
