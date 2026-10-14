/**
 * @file        app_net.c
 * @brief       联网状态机（从 app_main.c 提取）
 * @note        ESP8266 建链 + MQTT 连接 + 断线重连。
 *              状态变量用非 static，供 app_main.c 主循环读写。
 */

#include "./App/app_net.h"
#include "./App/app_config.h"
#include "./App/app_mqtt.h"
#include "./App/app_onenet.h"
#include "./BSP/LED/led.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/ESP8266/esp8266.h"
#include <stdio.h>
#include <string.h>

/* ===================== 颜色宏（与 app_main.c 一致） ===================== */
#ifndef UI_FG
#define UI_FG           BLACK
#endif

/* ===================== 共享状态定义 ===================== */
uint8_t  g_net_state;
uint32_t g_net_timer;
uint8_t  g_transparent;
uint8_t  g_baud_scanned;
uint16_t g_net_attempts;
uint16_t g_net_fails;
uint8_t  g_ping_fail;

/* ===================== 前向声明（app_main.c 中的 UI 函数） ===================== */
/* ui_show 在 app_main.c 中定义，此处通过 extern 声明调用 */
extern void ui_show(uint8_t row, const char *s, uint16_t color);
extern uint8_t g_row_wifi;
extern uint8_t g_row_cloud;
extern uint8_t g_row_sent;

/* ===================== 网络失败处理 ===================== */

void Net_Fail(const char *msg)
{
    printf("NET FAIL: %s (resp: %s)\r\n", msg, esp8266_get_resp());

    ui_show(g_row_wifi, msg, RED);
    LED0(1);

    if (g_net_state >= NET_ST_TCP)
    {
        if (g_transparent)
        {
            esp8266_exit_transparent();
            g_transparent = 0;
        }

        esp8266_cmd("AT+CIPCLOSE", "OK", 2000);
    }

    /* 连续失败够多次 → 允许重新全扫描波特率 */
    if (++g_net_fails >= APP_NET_FAIL_RESCAN)
    {
        g_net_fails    = 0;
        g_baud_scanned = 0;
        printf("NET: rescan baudrate on next try\r\n");
    }

    mqtt_link_clear_closed();
    g_ping_fail = 0;

    g_net_timer = HAL_GetTick();
    g_net_state = NET_ST_RETRYWAIT;
}

/* ===================== 网络状态机 ===================== */

void Net_Step(void)
{
    char line[64];

    if (g_net_state == NET_ST_ONLINE)
    {
        return;
    }

    switch (g_net_state)
    {
        case NET_ST_START:
            if (g_net_attempts > 0)
            {
                ui_show(g_row_wifi, "WiFi  : reset ...", UI_FG);
                printf("NET: reset ESP8266 before retry\r\n");

                esp8266_reset();
                (void)esp8266_set_baud(115200);
                esp8266_clear();
                g_baud_scanned = 0;
                mqtt_link_clear_closed();
            }

            g_net_attempts++;
            esp8266_init();
            ui_show(g_row_wifi, "WiFi  : AT ...", UI_FG);
            g_net_state = NET_ST_AT;
            break;

        case NET_ST_AT:
        {
            uint8_t ok;

            if (g_baud_scanned)
            {
                ok = (esp8266_at() == ESP8266_STAT_OK) ? 1 : 0;
            }
            else
            {
                uint16_t rx_bytes = 0;
                uint32_t baud = esp8266_scan_baud(&rx_bytes);

                g_baud_scanned = 1;

                if (baud == 0)
                {
                    if (rx_bytes > 0)
                    {
                        Net_Fail("WiFi  : RX ok,no AT");
                    }
                    else
                    {
                        Net_Fail("WiFi  : no resp");
                    }
                    break;
                }
                ok = 1;
            }

            if (ok)
            {
                g_net_state = NET_ST_ECHO;
            }
            else
            {
                Net_Fail("WiFi  : no module");
            }
            break;
        }

        case NET_ST_ECHO:
            esp8266_echo_off();
            g_net_state = NET_ST_MODE;
            break;

        case NET_ST_MODE:
            esp8266_set_mode(1);
            snprintf(line, sizeof(line), "WiFi  : join %s", APP_WIFI_SSID);
            ui_show(g_row_wifi, line, UI_FG);
            g_net_state = NET_ST_JOIN;
            break;

        case NET_ST_JOIN:
            printf("WiFi joining [%s] ...\r\n", APP_WIFI_SSID);

            if (esp8266_join_ap(APP_WIFI_SSID, APP_WIFI_PWD) == ESP8266_STAT_OK)
            {
                ui_show(g_row_wifi, "WiFi  : OK", BLUE);
                printf("WiFi OK\r\n");
                g_net_state = NET_ST_TCP;
            }
            else
            {
                Net_Fail("WiFi  : join FAIL");
            }
            break;

        case NET_ST_TCP:
            ui_show(g_row_cloud, "Cloud : TCP ...", UI_FG);

            if (esp8266_open_tcp(ONENET_HOST, ONENET_PORT) == ESP8266_STAT_OK)
            {
                g_net_state = NET_ST_TRANS;
            }
            else
            {
                Net_Fail("Cloud : TCP FAIL");
            }
            break;

        case NET_ST_TRANS:
            if (esp8266_enter_transparent() == ESP8266_STAT_OK)
            {
                g_transparent = 1;
                g_net_state = NET_ST_MQTT;
            }
            else
            {
                Net_Fail("Cloud : transparent FAIL");
            }
            break;

        case NET_ST_MQTT:
            ui_show(g_row_cloud, "Cloud : MQTT ...", UI_FG);

            if (onenet_connect() == MQTT_STAT_OK)
            {
                ui_show(g_row_cloud, "Cloud : online", BLUE);
                printf("MQTT connected to OneNET\r\n");
                LED0(0);
                g_net_fails = 0;
                g_ping_fail = 0;
                g_net_state = NET_ST_ONLINE;
            }
            else
            {
                Net_Fail("Cloud : MQTT FAIL");
            }
            break;

        case NET_ST_RETRYWAIT:
        default:
            if ((HAL_GetTick() - g_net_timer) >= APP_NET_RETRY_MS)
            {
                ui_show(g_row_wifi, "WiFi  : retry ...", UI_FG);
                ui_show(g_row_cloud, "Cloud : ---", UI_FG);
                g_net_state = NET_ST_START;
            }
            break;
    }
}
