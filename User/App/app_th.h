/**
 ****************************************************************************************************
 * @file        app_th.h
 * @brief       温湿度传感器模块(从 app_main.c 提取)
 * @note        自动探测 DHT11/SHT30, 统一读取接口。
 ****************************************************************************************************
 */

#ifndef __APP_TH_H
#define __APP_TH_H

#include "./SYSTEM/sys/sys.h"
#include <stdint.h>

/* 传感器类型 */
#define TH_NONE         0
#define TH_DHT11        1
#define TH_SHT30        2

/* 状态(非 static, 供 app_main.c 主循环读写) */
extern uint8_t  g_th_type;
extern uint8_t  g_th_ok;
extern uint8_t  g_th_fail;
extern uint16_t g_th_period;

/* 接口 */
uint8_t     th_detect(void);
uint8_t     th_read(int16_t *t_x10, uint16_t *h_x10);
const char *th_name(void);

#endif /* __APP_TH_H */
