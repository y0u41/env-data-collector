/**
 ****************************************************************************************************
 * @file        app_main.h
 * @brief       应用主入口
 ****************************************************************************************************
 */

#ifndef __APP_MAIN_H
#define __APP_MAIN_H

#include "./SYSTEM/sys/sys.h"
#include <stdint.h>

void app_main(void);

/* UI 行显示（app_main.c 定义，app_net.c 调用） */
void ui_show(uint8_t row, const char *s, uint16_t color);

#endif
