/**
 ****************************************************************************************************
 * @file        app_main.c
 * @brief       应用主逻辑: 温湿度(DHT11/SHT30) + 光敏 采集 -> 超限报警 -> LCD 本地显示
 *              可选: ESP8266 -> OneNET 上报
 * @note        1, 采集与显示永远工作, 联网是"尽力而为", 任何一步失败都只影响状态栏, 绝不阻塞界面;
 *              2, 主循环按 20ms 节拍跑: 报警/按键每 20ms, 采集+刷屏每 APP_SAMPLE_PERIOD_MS;
 *              3, APP_ENABLE_NET = 0 时完全不碰 ESP8266 / OneNET;
 *              4, 界面字号按屏幕高度与实际行数自动选择, 上限由 app_config.h 指定;
 *              5, 本文件与 CubeMX 生成的代码完全隔离, 重新生成代码不会覆盖。
 ****************************************************************************************************
 */

#include "./App/app_main.h"
#include "./App/app_ui.h"
#include "./App/app_th.h"
#include "./App/app_config.h"
#include "./App/app_alarm.h"
#include "./App/app_onenet.h"
#include "./App/app_mqtt.h"
#include "./App/app_log.h"
#include "./App/app_net.h"
#include "./BSP/LED/led.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/LSENS/lsens.h"
#include "./BSP/ESP8266/esp8266.h"
#include "./BSP/KEY/key.h"
#include "./BSP/RTC/rtc.h"
#include "./SYSTEM/delay/delay.h"
#include <stdio.h>
#include <string.h>

/* ===================== 独立看门狗 ===================== */
#if APP_ENABLE_IWDG
#define IWDG_FEED()     do { IWDG->KR = 0xAAAA; } while (0)

static void iwdg_start(void)
{
    DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;

    IWDG->KR  = 0x5555;
    IWDG->PR  = 4;
    IWDG->RLR = 5000;
    IWDG->KR  = 0xCCCC;
    IWDG->KR  = 0xAAAA;
}
#else
#define IWDG_FEED()     do { } while (0)
#endif

/**
 * @brief       每 20ms 的公共任务: 按键 + 报警 + 报警闪烁 + 报警行
 */
static void app_periodic(void)
{
    uint8_t k = key_scan();

    if (k != 0)
    {
        printf("KEY  : %s%s\r\n", (k & KEY0_PRES) ? "KEY0 " : "", (k & KEY1_PRES) ? "KEY1" : "");
    }

#if APP_ENABLE_LOG
    if ((k & KEY0_PRES) != 0)
    {
        if (alarm_key_mute() != 0)
        {
            k &= (uint8_t)~KEY0_PRES;
        }
    }

    if ((k & KEY1_PRES) != 0)
    {
        if (g_log_page == 0)
        {
            log_page_open();
        }
        else if (g_log_page >= log_page_count())
        {
            log_page_close();
        }
        else
        {
            g_log_page++;
            log_page_show();
        }

        k &= (uint8_t)~KEY1_PRES;
    }

    if (((k & KEY0_PRES) != 0) && (g_log_page != 0))
    {
        log_page_close();
    }
#else
    if ((k & KEY0_PRES) != 0)
    {
        (void)alarm_key_mute();
    }
#endif

    alarm_task();

#if APP_ENABLE_LOG
    if ((g_log_page != 0) && alarm_is_beeping())
    {
        log_page_close();
    }
#endif

    ui_flash_task();
    ui_refresh_alarm_row();
}

#if APP_ENABLE_NET
static void app_poll(void)
{
    IWDG_FEED();
    app_periodic();
}
#endif

/**
 * @brief       应用主入口(main.c 中调用)
 */
void app_main(void)
{
    int16_t  t_x10 = 0;
    uint16_t h_x10 = 0;
    uint8_t  lux = 0;
    uint32_t lux_cdm2 = 0;
    uint8_t  faults;
    uint8_t  warns;
    uint8_t  th_ok;
    uint32_t t_sample;
    uint32_t t_th;
    uint32_t now;
    char     line[64];
#if APP_ENABLE_NET
    uint16_t msg_id = 0;
    uint32_t sent = 0;
    uint32_t t_report;
    uint32_t t_ping;
#endif

    delay_init(72);
    led_init();
    alarm_init();

    printf("\r\n");
    printf("==================================================\r\n");
    printf("  atk_f103 : TH + LSENS -> ALARM -> LCD");
#if APP_ENABLE_NET
    printf(" -> ESP8266 -> OneNET");
#endif
    printf("\r\n");
    printf("  MCU  : STM32F103ZET6 @72MHz (STM32CubeMX + HAL)\r\n");
    printf("  NET  : %s\r\n", APP_ENABLE_NET ? "enabled" : "disabled");
    printf("  COMFORT: temp %d.%d-%d.%d C, humi %u.%u-%u.%u %%, lux %u-%u cd/m2 (yellow only)\r\n",
           APP_COMFORT_TEMP_MIN_X10 / 10, APP_COMFORT_TEMP_MIN_X10 % 10,
           APP_COMFORT_TEMP_MAX_X10 / 10, APP_COMFORT_TEMP_MAX_X10 % 10,
           (unsigned)(APP_COMFORT_HUMI_MIN_X10 / 10), (unsigned)(APP_COMFORT_HUMI_MIN_X10 % 10),
           (unsigned)(APP_COMFORT_HUMI_MAX_X10 / 10), (unsigned)(APP_COMFORT_HUMI_MAX_X10 % 10),
           (unsigned)APP_COMFORT_LUX_MIN_CDM2, (unsigned)APP_COMFORT_LUX_MAX_CDM2);
    printf("  ALARM  : temp %d.%d-%d.%d C, humi %u.%u-%u.%u %%, lux max %u cd/m2 (beep+flash)\r\n",
           APP_TEMP_MIN_X10 / 10, APP_TEMP_MIN_X10 % 10,
           APP_TEMP_MAX_X10 / 10, APP_TEMP_MAX_X10 % 10,
           (unsigned)(APP_HUMI_MIN_X10 / 10), (unsigned)(APP_HUMI_MIN_X10 % 10),
           (unsigned)(APP_HUMI_MAX_X10 / 10), (unsigned)(APP_HUMI_MAX_X10 % 10),
           (unsigned)APP_LUX_MAX_CDM2);
    printf("  > KEY0 = mute alarm");
#if APP_ENABLE_LOG
    printf("   KEY1 = alarm history (KEY0 = back)");
#endif
    printf("\r\n");
    printf("==================================================\r\n");

    /* ---------------- LCD 初始化 ---------------- */
    lcd_init();
    lcd_display_dir(1);
    lcd_clear(UI_BG);
    ui_setup();

    printf("LCD  : id=%x  %ux%u  font=%u/%u\r\n",
           lcddev.id, lcddev.width, lcddev.height, g_font_title, g_font_data);

    ui_show(g_row_title, (lcddev.width >= 480) ? "atk_f103 Environment Monitor" : "atk_f103", BLUE);
    ui_show(g_row_temp,  "Temp  : --.- C", RED);
    ui_show(g_row_humi,  "Humi  : --.- %", RED);
    ui_show(g_row_lux,   "Light : ---- cd/m2", RED);
    ui_show(g_row_alarm, "Alarm : normal", BLUE);

#if APP_ENABLE_NET
    ui_show(g_row_wifi,  "WiFi  : init ...", UI_FG);
    ui_show(g_row_cloud, "Cloud : ---", UI_FG);
    ui_show(g_row_sent,  "Sent  : 0", UI_FG);
    g_net_state   = NET_ST_START;
    g_transparent = 0;
#endif

    /* ---------------- 温湿度传感器自动探测 ---------------- */
    g_th_type = th_detect();

    if (g_th_type == TH_NONE)
    {
        ui_show(g_row_sensor, "TH    : not found", RED);
        printf("TH   : no sensor found (SHT30@PB6/PB7 or DHT11@PG11)\r\n");
    }
    else
    {
        snprintf(line, sizeof(line), "%s : OK", th_name());
        ui_show(g_row_sensor, line, BLUE);
        printf("TH   : %s detected\r\n", th_name());
    }

    if (g_th_type != TH_DHT11)
    {
        HAL_GPIO_WritePin(DHT11_DQ_GPIO_Port, DHT11_DQ_Pin, GPIO_PIN_SET);
    }

    g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
    g_th_ok = 0;
    g_th_fail = 0;

#if APP_ENABLE_LOG
    /* ---------------- 报警历史: RTC 时间戳 + 内部 FLASH 存储 ---------------- */
    {
        uint8_t rtc_ret = rtc_init();

        app_log_init();
        alarm_set_log_hook(app_log_add);

        printf("RTC  : %s  now %04u-%02u-%02u %02u:%02u:%02u  (clock %s)\r\n",
               (rtc_ret == 0) ? "ok" : "INIT FAILED",
               calendar.year, calendar.month, calendar.date,
               calendar.hour, calendar.min, calendar.sec,
               (rtc_is_set() != 0) ? "set, battery-backed" : "from build time, not battery-backed");

        printf("LOG  : type KEY1 to browse alarm history (KEY0 = back)\r\n");
    }
#endif

    lsens_init();

    t_sample = HAL_GetTick();
    t_th = t_sample;
#if APP_ENABLE_NET
    t_report = t_sample;
    t_ping   = t_sample;

    esp8266_set_poll_hook(app_poll);
#endif

#if APP_ENABLE_IWDG
    iwdg_start();
#endif

    /* ---------------- 主循环: 20ms 节拍 ---------------- */
    while (1)
    {
        now = HAL_GetTick();

        /* ========== 20ms 任务 ========== */
        app_periodic();
        IWDG_FEED();

#if APP_ENABLE_NET
        Net_Step();

        if (g_net_state == NET_ST_ONLINE)
        {
            mqtt_poll_rx();

            if (mqtt_link_closed())
            {
                mqtt_link_clear_closed();
                printf("ESP8266: TCP closed (peer or WiFi down)\r\n");
                g_transparent = 0;
                Net_Fail("Cloud : link lost");
                delay_ms(20);
                continue;
            }
        }
#endif

        /* ========== 采样周期任务 ========== */
        if ((now - t_sample) >= APP_SAMPLE_PERIOD_MS)
        {
            t_sample = now;

            /* 1. 采集温湿度 */
            if (g_th_type == TH_NONE)
            {
                if ((now - t_th) >= APP_DHT11_PERIOD_MS)
                {
                    t_th = now;
                    g_th_type = th_detect();
                    g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
                }
            }
            else if ((now - t_th) >= g_th_period)
            {
                t_th = now;

                if (th_read(&t_x10, &h_x10) == 0)
                {
                    g_th_ok = 1;
                    g_th_fail = 0;
                }
                else
                {
                    g_th_ok = 0;

                    if (++g_th_fail >= APP_TH_RETRY)
                    {
                        g_th_fail = 0;
                        g_th_type = th_detect();
                        g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
                    }
                }
            }

            th_ok = g_th_ok;

            /* 2. 采集光照 */
            lux = lsens_get_val();
            lux_cdm2 = (uint32_t)lux * (uint32_t)APP_LUX_FULL_SCALE_CDM2 / 100U;

            /* 3. 报警 */
            alarm_feed(t_x10, h_x10, lux_cdm2, th_ok);

            faults = alarm_get_faults();
            warns  = alarm_get_warns();

            /* 4. 本地显示 */
            if (th_ok != 0)
            {
                ui_fmt_x10(line, sizeof(line), "Temp  : ", t_x10, "C");
                ui_show(g_row_temp, line, ui_level_color(ALARM_FAULT_TEMP, faults, warns));

                ui_fmt_x10(line, sizeof(line), "Humi  : ", (int16_t)h_x10, "%");
                ui_show(g_row_humi, line, ui_level_color(ALARM_FAULT_HUMI, faults, warns));

                printf("DATA %s temp=%d.%d humi=%u.%u lsens=%u cdm2=%u alarm=%s%s\r\n",
                       th_name(),
                       (int)(t_x10 / 10), (int)(t_x10 % 10),
                       (unsigned)(h_x10 / 10), (unsigned)(h_x10 % 10),
                       (unsigned)lux, (unsigned)lux_cdm2, alarm_reason(),
                       alarm_is_beeping() ? " BEEP" : "");
            }
            else
            {
                ui_show(g_row_temp, "Temp  : --.- C", GRAY);
                ui_show(g_row_humi, "Humi  : --.- %", GRAY);

                printf("DATA %s read fail (连续第%u次) lsens=%u cdm2=%u alarm=%s%s\r\n",
                       th_name(), (unsigned)g_th_fail, (unsigned)lux, (unsigned)lux_cdm2,
                       alarm_reason(), alarm_is_beeping() ? " BEEP" : "");
            }

            snprintf(line, sizeof(line), "Light : %u cd/m2", (unsigned)lux_cdm2);
            ui_show(g_row_lux, line, ui_level_color(ALARM_FAULT_LUX, faults, warns));

            ui_refresh_alarm_row();

            if (g_th_type != TH_NONE)
            {
                snprintf(line, sizeof(line), "%s : %s", th_name(), th_ok ? "OK" : "read fail");
                ui_show(g_row_sensor, line, th_ok ? BLUE : RED);
            }

#if APP_ENABLE_NET
            /* 5. 周期上报 OneNET */
            if (g_net_state == NET_ST_ONLINE)
            {
                if ((now - t_report) >= APP_REPORT_PERIOD_MS)
                {
                    t_report = now;
                    msg_id++;

                    if (onenet_publish(msg_id, t_x10, h_x10, lux) != MQTT_STAT_OK)
                    {
                        printf("publish FAIL, will reconnect\r\n");
                        g_transparent = 0;
                        Net_Fail("Cloud : send FAIL");
                    }
                    else
                    {
                        sent++;
                        snprintf(line, sizeof(line), "Sent  : %u", (unsigned)sent);
                        ui_show(g_row_sent, line, UI_FG);
                    }
                }

                /* 6. MQTT 心跳 */
                if ((now - t_ping) >= APP_PING_PERIOD_MS)
                {
                    t_ping = now;

                    if (mqtt_ping() == MQTT_STAT_OK)
                    {
                        g_ping_fail = 0;
                    }
                    else if (++g_ping_fail >= APP_PING_FAIL_MAX)
                    {
                        g_ping_fail = 0;
                        printf("MQTT: link lost (%u pings no answer), reconnect\r\n",
                               (unsigned)APP_PING_FAIL_MAX);
                        g_transparent = 0;
                        Net_Fail("Cloud : link lost");
                    }
                    else
                    {
                        printf("MQTT: ping no answer (%u/%u)\r\n",
                               (unsigned)g_ping_fail, (unsigned)APP_PING_FAIL_MAX);
                    }
                }
            }
#endif /* APP_ENABLE_NET */

            LED1_TOGGLE();
        }

        delay_ms(20);
    }
}
