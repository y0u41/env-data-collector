    该文件夹下面存放用户层代码，包括：
    1，App：本工程自写的业务逻辑
       app_config.h   全部可配置项（传感器 / 界面字号 / 误报抑制 / 报警阈值 / WiFi / OneNET）
       app_main.c     主循环：采集 -> 显示 -> 上报 -> 心跳 -> 断线重连
       app_alarm.c    超限报警：持续确认窗 + 迟滞去抖、10 秒定时、KEY0 消音
       app_mqtt.c     极简 MQTT 3.1.1 报文组装与解析
       app_onenet.c   OneNET 上报主题与 JSON 载荷组装

    说明：本工程由 STM32CubeMX 生成，CubeMX 把外设初始化代码写在工程根目录的 Core\ 下
          （main.c / adc.c / usart.c / gpio.c / stm32f1xx_it.c 等），本文件夹不受其影响。
          包含路径里挂了 ../../User，所以代码里统一写 #include "./App/xxx.h" 即可。
