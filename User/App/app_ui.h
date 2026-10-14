/**
 ****************************************************************************************************
 * @file        app_ui.h
 * @brief       LCD 界面显示模块(从 app_main.c 提取)
 * @note        行缓存 + 字号自适应 + 报警闪烁 + 报警历史页。
 *              行号用非 static 全局, 供 app_main.c / app_net.c 写内容。
 ****************************************************************************************************
 */

#ifndef __APP_UI_H
#define __APP_UI_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_config.h"
#include <stdint.h>

/* ===================== 界面布局 ===================== */
#define UI_X            6           /* 左边距 */
#define UI_LINE_MAX     80          /* 每行最大字符数 */
#define UI_ROW_MAX      9           /* 行数上限 */

/* ===================== 界面配色 ===================== */
#define UI_BG           WHITE       /* 整屏底色 */
#define UI_FG           BLACK       /* 普通文本色 */
#define UI_WARN         0x9B60      /* 提示级文本色: 深琥珀 */

/* ===================== 行号(非 static, 供 app_main.c / app_net.c 使用) ===================== */
extern uint8_t  g_row_title;
extern uint8_t  g_row_temp;
extern uint8_t  g_row_humi;
extern uint8_t  g_row_lux;
extern uint8_t  g_row_sensor;
extern uint8_t  g_row_alarm;
extern uint8_t  g_row_count;
#if APP_ENABLE_NET
extern uint8_t  g_row_wifi;
extern uint8_t  g_row_cloud;
extern uint8_t  g_row_sent;
#endif

/* ===================== 字号(供 app_main.c 启动打印使用) ===================== */
extern uint8_t  g_font_title;
extern uint8_t  g_font_data;

/* ===================== 报警历史页状态 ===================== */
#if APP_ENABLE_LOG
extern uint8_t  g_log_page;
#endif

/* ===================== 接口 ===================== */
void     ui_setup(void);
void     ui_show(uint8_t row, const char *s, uint16_t color);
void     ui_redraw_all(void);
void     ui_flash_task(void);
uint16_t ui_level_color(uint8_t bit, uint8_t faults, uint8_t warns);
void     ui_refresh_alarm_row(void);
void     ui_fmt_x10(char *buf, uint16_t bufsz, const char *label, int16_t v, const char *unit);

/* 报警历史页 */
#if APP_ENABLE_LOG
void     log_page_open(void);
void     log_page_close(void);
void     log_page_show(void);
uint8_t  log_page_count(void);
#endif

#endif /* __APP_UI_H */
