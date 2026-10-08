/**
 ****************************************************************************************************
 * @file        lsens.h
 * @brief       光敏传感器驱动头文件 (板载 LS1, PF8 = ADC3_IN6)
 ****************************************************************************************************
 */

#ifndef __LSENS_H
#define __LSENS_H

#include "./SYSTEM/sys/sys.h"

/* 若实测发现"光照越强数值越小", 把这里改成 1 即可翻转 */
#define LSENS_INVERT        0

#define LSENS_ADC_CH        ADC_CHANNEL_6       /* ADC3 通道 6 -> PF8 */
#define LSENS_ADC_SAMPLES   10                  /* 每次读取的采样次数 */

void    lsens_init(void);
uint8_t lsens_get_val(void);                    /* 返回 0~100, 0=最暗, 100=最亮 */

#endif
