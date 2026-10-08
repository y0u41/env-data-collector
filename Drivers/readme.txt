    该文件夹下面存放驱动层代码，包括：
    1，BSP：本工程的板级支持包驱动代码（LED、BEEP、KEY、LCD、DHT11、SHT30、LSENS、ESP8266）
    2，SYSTEM：板级基础层（sys 公共头 / delay 微秒级延时 / usart printf 重定向）
    3，CMSIS：ARM 提供的 CMSIS 头文件与启动文件（由 STM32CubeMX 拷贝维护）
    4，STM32F1xx_HAL_Driver：ST 提供的 F1xx HAL 库驱动代码（由 STM32CubeMX 拷贝维护）

    注意：CMSIS 与 STM32F1xx_HAL_Driver 归 STM32CubeMX 管理，重新生成代码时会被覆盖；
          BSP 与 SYSTEM 是本工程自有代码，不受 CubeMX 重新生成影响。

    包含路径约定：工程里统一写 #include "./BSP/xxx/xxx.h"、"./SYSTEM/xxx/xxx.h"，
          靠 Keil 的包含路径 ../../Drivers/ 解析到本文件夹，与正点原子官方例程一致。
