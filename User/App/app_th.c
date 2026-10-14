/**
 ****************************************************************************************************
 * @file        app_th.c
 * @brief       温湿度传感器模块(从 app_main.c 提取)
 ****************************************************************************************************
 */

#include "./App/app_th.h"
#include "./App/app_config.h"
#include "./BSP/DHT11/dht11.h"
#include "./BSP/SHT30/sht30.h"
#include "./BSP/ESP8266/esp8266.h"
#include <stdio.h>

/* ===================== 状态定义 ===================== */
uint8_t  g_th_type = TH_NONE;
uint8_t  g_th_ok = 0;
uint8_t  g_th_fail = 0;
uint16_t g_th_period = APP_SAMPLE_PERIOD_MS;

/* ===================== 总线锁(DHT11 时序敏感) ===================== */
#if APP_ENABLE_NET
#define TH_BUS_LOCK()       esp8266_rx_pause()
#define TH_BUS_UNLOCK()     esp8266_rx_resume()
#else
#define TH_BUS_LOCK()       do { } while (0)
#define TH_BUS_UNLOCK()     do { } while (0)
#endif

/* ===================== 探测 ===================== */

uint8_t th_detect(void)
{
#if (APP_SENSOR_TH == 1)
    return (sht30_init() == SHT30_OK) ? TH_SHT30 : TH_NONE;
#elif (APP_SENSOR_TH == 0)
    uint8_t r;

    TH_BUS_LOCK();
    r = (dht11_init() == 0) ? TH_DHT11 : TH_NONE;
    TH_BUS_UNLOCK();

    return r;
#else
    uint8_t r;

    if (sht30_init() == SHT30_OK)
    {
        return TH_SHT30;
    }

    TH_BUS_LOCK();
    r = (dht11_init() == 0) ? TH_DHT11 : TH_NONE;
    TH_BUS_UNLOCK();

    return r;
#endif
}

/* ===================== 读取 ===================== */

uint8_t th_read(int16_t *t_x10, uint16_t *h_x10)
{
    uint8_t t8, h8;
    uint8_t r;

    if (g_th_type == TH_SHT30)
    {
        return sht30_read(t_x10, h_x10);
    }

    if (g_th_type == TH_DHT11)
    {
        t8 = 0xEE;
        h8 = 0xEE;

        TH_BUS_LOCK();
        r = dht11_read_data(&t8, &h8);
        TH_BUS_UNLOCK();

        if (r)
        {
            printf("DHT11: no ACK (check DQ/PG11 wiring & 4.7K pull-up)\r\n");
            return 1;
        }

        if ((t8 == 0xEE) || (h8 == 0xEE))
        {
            printf("DHT11: CRC fail (data line too slow/noisy)\r\n");
            return 1;
        }

        *t_x10 = (int16_t)((int16_t)t8 * 10);
        *h_x10 = (uint16_t)((uint16_t)h8 * 10U);

        return 0;
    }

    return 2;
}

/* ===================== 名称 ===================== */

const char *th_name(void)
{
    if (g_th_type == TH_SHT30)
    {
        return "SHT30";
    }

    if (g_th_type == TH_DHT11)
    {
        return "DHT11";
    }

    return "TH";
}
