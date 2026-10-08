/**
 ****************************************************************************************************
 * @file        app_log.h
 * @brief       报警历史记录: 每次报警把"时间 + 原因 + 当时的温湿度/光照"存进内部 FLASH
 * @note        为什么存内部 FLASH 而不是板载 24C02:
 *                1, 24C02 挂在 PB6/PB7, 和 SHT30 软件 I2C 是同一条总线 —— 再写一套软 I2C
 *                   驱动既浪费代码, 又有把正在稳定工作的 SHT30 读坏的风险;
 *                2, 内部 FLASH 不占任何引脚、不走任何总线, 官方 BSP(Drivers/BSP/STMFLASH)
 *                   已经写好;
 *                3, STM32F103ZET6 的页大小正好 2KB, 用最后一页(0x0807F800)存 64 条记录
 *                   只用掉 1040 字节, 剩下一半还空着。
 *              代价: 内部 FLASH 擦写寿命约 1 万次(24C02 是 100 万次)。按每天报警 3 次算,
 *              1 万次能用 9 年; 而且只在"报警开始"这一瞬间写一次, 不写心跳、不写正常数据。
 *
 * @note        ★ 存储地址 0x0807F800 是 512KB FLASH 的**最后一页**。只要固件不超过 510KB
 *              就不会冲突(当前约 65KB)。若以后代码大幅增长, 记得改 APP_LOG_FLASH_ADDR。
 *
 * @note        掉电风险: FLASH 写入必须"整页擦除 -> 整页写回", 如果在这中间掉电, 整页记录
 *              会一起丢(不是丢一条)。因为每条记录自带 CRC, 上电后会自己识别出无效数据并
 *              当作空库重新开始, 不会出现乱码记录。
 ****************************************************************************************************
 */

#ifndef __APP_LOG_H
#define __APP_LOG_H

#include "./SYSTEM/sys/sys.h"

#define APP_LOG_MAX         64          /* 最多保存多少条报警记录(超出后覆盖最早的) */

/* 一条报警记录 */
typedef struct
{
    uint16_t year;                      /* 年 */
    uint8_t  month;                     /* 月 */
    uint8_t  date;                      /* 日 */
    uint8_t  hour;                      /* 时 */
    uint8_t  min;                       /* 分 */
    uint8_t  sec;                       /* 秒 */
    uint8_t  faults;                    /* 危险区标志 ALARM_FAULT_xxx */
    int16_t  temp_x10;                  /* 报警瞬间温度, 0.1℃ */
    uint16_t humi_x10;                  /* 报警瞬间湿度, 0.1%RH */
    uint16_t lux_cdm2;                  /* 报警瞬间光照, cd/m2 */
} app_log_item_t;

void     app_log_init(void);            /* 上电时调用: 从 FLASH 载入已有记录 */
void     app_log_add(uint8_t faults, int16_t temp_x10,
                     uint16_t humi_x10, uint32_t lux_cdm2);  /* 新增一条(由报警回调触发) */
void     app_log_clear(void);           /* 清空全部记录 */
uint8_t  app_log_count(void);           /* 当前有效记录条数 */
uint8_t  app_log_capacity(void);        /* 容量(APP_LOG_MAX) */

/* idx = 0 表示最新的一条, 依次往前翻; 越界返回 NULL */
const app_log_item_t *app_log_get(uint8_t idx);

/* 把全部记录打到串口(115200), 按时间从新到旧 */
void     app_log_dump(void);

/* 把一条记录的原因位转成字符串, 例如 "TEMP+HUMI" */
void     app_log_fault_str(uint8_t faults, char *dst, uint16_t dstsz);

#endif
