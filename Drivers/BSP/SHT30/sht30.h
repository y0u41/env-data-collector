/**
 ****************************************************************************************************
 * @file        sht30.h
 * @brief       SHT30 / SHT31 温湿度传感器驱动（软件 I2C）
 * @note        1, 使用 PB6(SCL) / PB7(SDA) —— 与板载 24C02 共用同一条 I2C 总线,
 *                  SHT30 地址 0x44 与 24C02 的 0xA0 不冲突, 板载已有 4.7K 上拉, 无需外接电阻;
 *              2, 采用软件 I2C 而不是 STM32F1 的硬件 I2C: F1 的硬件 I2C 有已知的总线锁死
 *                  问题, 正点原子官方 24C02 例程在同样这两个脚上用的也是软件 I2C;
 *              3, 温度/湿度均以 0.1 为单位返回整数, 避免在无 FPU 的 F103 上引入浮点库。
 ****************************************************************************************************
 */

#ifndef __SHT30_H
#define __SHT30_H

#include "./SYSTEM/sys/sys.h"

/* ===================== 软件 I2C 引脚定义 ===================== */
#define SHT30_SCL_GPIO_PORT     GPIOB
#define SHT30_SDA_GPIO_PORT     GPIOB
#define SHT30_SCL_GPIO_PIN      GPIO_PIN_6
#define SHT30_SDA_GPIO_PIN      GPIO_PIN_7
#define SHT30_GPIO_CLK_ENABLE() do{ __HAL_RCC_GPIOB_CLK_ENABLE(); }while(0)

/* ===================== 器件地址 ===================== */
/* ADDR 引脚接 GND(默认) 时为 0x44; 接 VDD 时为 0x45 */
#define SHT30_I2C_ADDR          0x44

/* ===================== 返回码 ===================== */
#define SHT30_OK                0
#define SHT30_ERR_NACK          1       /* 器件无应答(没接 / 接线错 / 地址不对) */
#define SHT30_ERR_CRC           2       /* 数据 CRC 校验失败 */
#define SHT30_ERR_PARAM         3

uint8_t sht30_init(void);                                   /* 初始化, 返回 SHT30_OK 表示器件在位 */
uint8_t sht30_read(int16_t *temp_x10, uint16_t *humi_x10);  /* 读温湿度, 单位 0.1℃ / 0.1%RH */
uint8_t sht30_soft_reset(void);                             /* 软复位 */

#endif
