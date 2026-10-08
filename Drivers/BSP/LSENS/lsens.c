/**
 ****************************************************************************************************
 * @file        lsens.c
 * @brief       光敏传感器驱动 (板载 LS1, PF8 = ADC3_IN6)
 * @note        PF8 的模拟输入模式与 ADC3 通道 6 的配置由 CubeMX 生成代码完成
 *              (见 Core/Src/adc.c 的 MX_ADC3_Init / HAL_ADC_MspInit),
 *              本文件只负责启动转换、取平均并换算成 0~100 的相对光强。
 ****************************************************************************************************
 */

#include "./BSP/LSENS/lsens.h"
#include "adc.h"        /* CubeMX 生成: hadc3 */

/**
 * @brief       光敏传感器初始化
 * @param       无
 * @retval      无
 * @note        GPIO/ADC 均由 CubeMX 初始化, 这里无需重复配置。
 */
void lsens_init(void)
{
    /* 空实现: 保留接口以便与正点原子官方例程保持一致 */
}

/**
 * @brief       读取光敏传感器值
 * @param       无
 * @retval      0~100: 0=最暗, 100=最亮
 */
uint8_t lsens_get_val(void)
{
    uint32_t sum = 0;
    uint32_t val;
    uint8_t  i;

    for (i = 0; i < LSENS_ADC_SAMPLES; i++)
    {
        HAL_ADC_Start(&hadc3);
        HAL_ADC_PollForConversion(&hadc3, 10);
        sum += HAL_ADC_GetValue(&hadc3);
    }

    val = sum / LSENS_ADC_SAMPLES;      /* 12 位原始值 0~4095 */
    val = val / 40;                     /* 折算到 0~100 附近 */

    if (val > 100)
    {
        val = 100;
    }

#if LSENS_INVERT
    return (uint8_t)val;
#else
    return (uint8_t)(100 - val);
#endif
}
