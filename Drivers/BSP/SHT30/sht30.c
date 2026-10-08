/**
 ****************************************************************************************************
 * @file        sht30.c
 * @brief       SHT30 / SHT31 温湿度传感器驱动（软件 I2C, PB6/PB7）
 ****************************************************************************************************
 */

#include "./BSP/SHT30/sht30.h"
#include "./SYSTEM/delay/delay.h"
#include <stddef.h>

/* ===================== 引脚操作 ===================== */
#define SCL_H()     HAL_GPIO_WritePin(SHT30_SCL_GPIO_PORT, SHT30_SCL_GPIO_PIN, GPIO_PIN_SET)
#define SCL_L()     HAL_GPIO_WritePin(SHT30_SCL_GPIO_PORT, SHT30_SCL_GPIO_PIN, GPIO_PIN_RESET)
#define SDA_H()     HAL_GPIO_WritePin(SHT30_SDA_GPIO_PORT, SHT30_SDA_GPIO_PIN, GPIO_PIN_SET)
#define SDA_L()     HAL_GPIO_WritePin(SHT30_SDA_GPIO_PORT, SHT30_SDA_GPIO_PIN, GPIO_PIN_RESET)
#define SDA_READ()  HAL_GPIO_ReadPin(SHT30_SDA_GPIO_PORT, SHT30_SDA_GPIO_PIN)

/* 半周期延时: 约 3us -> SCL 约 100kHz, 兼顾杜邦线较长时的信号质量 */
#define I2C_DELAY() delay_us(3)

/* ===================== SHT30 命令 ===================== */
#define SHT30_CMD_SOFT_RESET    0x30A2      /* 软复位 */
#define SHT30_CMD_MEAS_HIGH     0x2400      /* 单次测量, 高重复性, 不使能时钟拉伸 */
#define SHT30_CMD_READ_STATUS   0xF32D      /* 读状态寄存器 */

/**
 * @brief       I2C 起始信号
 */
static void i2c_start(void)
{
    SDA_H();
    SCL_H();
    I2C_DELAY();
    SDA_L();
    I2C_DELAY();
    SCL_L();
    I2C_DELAY();
}

/**
 * @brief       I2C 停止信号
 */
static void i2c_stop(void)
{
    SDA_L();
    SCL_H();
    I2C_DELAY();
    SDA_H();
    I2C_DELAY();
}

/**
 * @brief       等待从机应答
 * @retval      0: 收到应答; 1: 无应答(超时)
 */
static uint8_t i2c_wait_ack(void)
{
    uint8_t t = 0;

    SDA_H();                    /* 主机释放 SDA, 交由从机拉低 */
    I2C_DELAY();
    SCL_H();
    I2C_DELAY();

    while (SDA_READ() && (t < 250))
    {
        t++;
        delay_us(1);
    }

    SCL_L();
    I2C_DELAY();

    return (t >= 250) ? 1 : 0;
}

/**
 * @brief       I2C 发送一个字节
 */
static void i2c_send_byte(uint8_t dat)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        if (dat & 0x80)
        {
            SDA_H();
        }
        else
        {
            SDA_L();
        }

        dat <<= 1;
        I2C_DELAY();
        SCL_H();
        I2C_DELAY();
        SCL_L();
        I2C_DELAY();
    }
}

/**
 * @brief       I2C 读一个字节
 * @param       ack: 1=读完后发应答, 0=发非应答(最后一个字节)
 */
static uint8_t i2c_read_byte(uint8_t ack)
{
    uint8_t i;
    uint8_t dat = 0;

    SDA_H();                    /* 释放 SDA 交由从机驱动 */

    for (i = 0; i < 8; i++)
    {
        dat <<= 1;
        SCL_H();
        I2C_DELAY();

        if (SDA_READ())
        {
            dat |= 0x01;
        }

        SCL_L();
        I2C_DELAY();
    }

    if (ack)
    {
        SDA_L();                /* 应答 */
    }
    else
    {
        SDA_H();                /* 非应答 */
    }

    I2C_DELAY();
    SCL_H();
    I2C_DELAY();
    SCL_L();
    I2C_DELAY();

    SDA_H();

    return dat;
}

/**
 * @brief       向 SHT30 写一条 16 位命令
 * @retval      0: 成功; 1: 无应答
 */
static uint8_t sht30_write_cmd(uint16_t cmd)
{
    uint8_t err = 0;

    i2c_start();

    i2c_send_byte((uint8_t)(SHT30_I2C_ADDR << 1));      /* 写地址 */
    err |= i2c_wait_ack();

    i2c_send_byte((uint8_t)(cmd >> 8));                 /* 命令高字节 */
    err |= i2c_wait_ack();

    i2c_send_byte((uint8_t)(cmd & 0xFF));               /* 命令低字节 */
    err |= i2c_wait_ack();

    i2c_stop();

    return err;
}

/**
 * @brief       从 SHT30 读 6 字节原始数据
 * @param       buf: 温度MSB/温度LSB/温度CRC/湿度MSB/湿度LSB/湿度CRC
 * @retval      0: 成功; 1: 无应答
 */
static uint8_t sht30_read_raw(uint8_t *buf)
{
    uint8_t i;

    i2c_start();

    i2c_send_byte((uint8_t)((SHT30_I2C_ADDR << 1) | 0x01));     /* 读地址 */
    if (i2c_wait_ack())
    {
        i2c_stop();
        return 1;
    }

    for (i = 0; i < 5; i++)
    {
        buf[i] = i2c_read_byte(1);
    }

    buf[5] = i2c_read_byte(0);

    i2c_stop();

    return 0;
}

/**
 * @brief       SHT30 数据 CRC-8 校验
 * @note        多项式 0x31, 初值 0xFF, 无反转
 */
static uint8_t sht30_crc8(const uint8_t *dat, uint8_t len)
{
    uint8_t crc = 0xFF;
    uint8_t i, b;

    for (i = 0; i < len; i++)
    {
        crc ^= dat[i];

        for (b = 0; b < 8; b++)
        {
            if (crc & 0x80)
            {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }

    return crc;
}

/**
 * @brief       软复位
 * @retval      SHT30_OK / SHT30_ERR_NACK
 */
uint8_t sht30_soft_reset(void)
{
    if (sht30_write_cmd(SHT30_CMD_SOFT_RESET))
    {
        return SHT30_ERR_NACK;
    }

    delay_ms(2);                /* 复位需要约 1.5ms */

    return SHT30_OK;
}

/**
 * @brief       初始化 SHT30(配置 PB6/PB7 为开漏输出并探测器件)
 * @param       无
 * @retval      SHT30_OK 表示器件在位
 */
uint8_t sht30_init(void)
{
    GPIO_InitTypeDef gpio_init_struct = {0};

    SHT30_GPIO_CLK_ENABLE();

    gpio_init_struct.Pin = SHT30_SCL_GPIO_PIN | SHT30_SDA_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_OD;        /* 开漏: 写 1 即释放总线, 可回读 */
    gpio_init_struct.Pull = GPIO_PULLUP;                /* 板载已有 4.7K, 这里再开内部上拉更稳 */
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SHT30_SCL_GPIO_PORT, &gpio_init_struct);

    SCL_H();
    SDA_H();
    delay_ms(1);

    return sht30_soft_reset();  /* 能收到应答说明器件在位 */
}

/**
 * @brief       读取温湿度
 * @param       temp_x10: 温度, 单位 0.1℃ (例: 265 表示 26.5℃)
 * @param       humi_x10: 湿度, 单位 0.1%RH
 * @retval      SHT30_OK / SHT30_ERR_NACK / SHT30_ERR_CRC
 */
uint8_t sht30_read(int16_t *temp_x10, uint16_t *humi_x10)
{
    uint8_t  buf[6];
    uint16_t raw_t, raw_h;

    if ((temp_x10 == NULL) || (humi_x10 == NULL))
    {
        return SHT30_ERR_PARAM;
    }

    /* 单次测量, 高重复性: 最大耗时 15ms */
    if (sht30_write_cmd(SHT30_CMD_MEAS_HIGH))
    {
        return SHT30_ERR_NACK;
    }

    delay_ms(20);

    if (sht30_read_raw(buf))
    {
        return SHT30_ERR_NACK;
    }

    if ((sht30_crc8(buf, 2) != buf[2]) || (sht30_crc8(buf + 3, 2) != buf[5]))
    {
        return SHT30_ERR_CRC;
    }

    raw_t = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    raw_h = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);

    /* 官方换算: T = -45 + 175 * raw/65535 ; RH = 100 * raw/65535
     * 换成 0.1 单位并用整数运算, 避免引入浮点库:
     *   T_x10  = -450 + 1750 * raw_t / 65535
     *   RH_x10 =        1000 * raw_h / 65535
     */
    *temp_x10 = (int16_t)(-450 + (int32_t)(1750L * raw_t) / 65535L);
    *humi_x10 = (uint16_t)((int32_t)(1000L * raw_h) / 65535L);

    return SHT30_OK;
}
