#ifndef OTDR_TCP_SERVER_H
#define OTDR_TCP_SERVER_H

#include "otdr_protocol.h"

// 标志位：是否收到了上位机的命令并锁定了上位机的 IP/端口
extern int udp_client_connected;

// 网络初始化（UDP 监听 5000 端口）
int otdr_network_init(void);

// 网络发送接口（UDP 切片发送）
err_t otdr_network_send(const u8 *data, u32 len);

#endif
