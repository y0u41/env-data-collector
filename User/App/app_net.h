#ifndef APP_NET_H
#define APP_NET_H

#include <stdint.h>

/* 联网状态机模块（从 app_main.c 提取）。
 * 状态变量用 g_ 前缀 + 非 static，供 app_main.c 的主循环读写。 */

/* 联网状态机状态 */
#define NET_ST_START        0
#define NET_ST_AT           1
#define NET_ST_ECHO         2
#define NET_ST_MODE         3
#define NET_ST_JOIN         4
#define NET_ST_TCP          5
#define NET_ST_TRANS        6
#define NET_ST_MQTT         7
#define NET_ST_ONLINE       8
#define NET_ST_RETRYWAIT    9

/* 共享状态（定义在 app_net.c） */
extern uint8_t  g_net_state;
extern uint32_t g_net_timer;
extern uint8_t  g_transparent;
extern uint8_t  g_baud_scanned;
extern uint16_t g_net_attempts;
extern uint16_t g_net_fails;
extern uint8_t  g_ping_fail;

/* 网络状态机接口 */
void Net_Fail(const char *msg);
void Net_Step(void);

#endif /* APP_NET_H */
