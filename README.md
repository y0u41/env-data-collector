# atk_f103 · 居家环境监测与报警终端

![License](https://img.shields.io/badge/license-MIT-green) ![Language](https://img.shields.io/badge/language-C-555555) ![Platform](https://img.shields.io/badge/platform-STM32F103ZET6-blue) ![Build](https://img.shields.io/badge/build-0%20Error-brightgreen)

基于 **正点原子精英版 STM32F103ZET6** 的居家环境监测终端：**温湿度 / 光照采集 → TFT-LCD 本地显示 → 蜂鸣器与整屏闪烁报警 → ESP8266 上云（OneNET）**，并支持**报警历史掉电留存**。

面向真实使用场景（独居老人居家监护）做了取舍：**两级报警抑制误报**、**非阻塞联网状态机**、**断线自动重连**、**报警历史写片内 FLASH**。

## 功能特性

- 🌡 **温湿度采集（双传感器自动探测）**：DHT11（PG11，单总线）与 SHT30（PB6/PB7，软件 I2C）二选一，默认开机自动探测、**SHT30 优先**，识别结果打印串口并显示在 LCD 末行
- 💡 **光照采集**：板载光敏 LS1（PF8 = ADC3_IN6），按满量程换算为 cd/m²
- 🖥 **本地显示（始终工作）**：TFT-LCD（FSMC）实时显示 Temp / Humi / Light / 传感器状态 / 联网状态；字号按**屏幕高度与实际行数自适应**（字库 12/16/24/32 四档取最大可放下者）
- 🔔 **两级报警 + 误报抑制**
  - **舒适区**（18–28 ℃ / 40–70 %RH / 100–4000 cd/m²）：对应数值行与 `Alarm` 行变**黄**并显示 `warnT/warnH/warnL`，**不响**
  - **危险区**（<10 或 >32 ℃ / <25 或 >85 %RH / 光敏量程打满）：**蜂鸣器响 10 s + LCD 整屏闪烁**（红底白字 ↔ 白底彩字）
  - **确认窗**：任一参数连续越界 3 s 才确认，滤掉瞬时尖峰
  - **迟滞**：0.5 ℃ / 2 %RH / 100 cd/m²，避免数值卡在阈值附近无限"响 10 s 停 10 s"
  - **一键消音**：报警期间按 KEY0 立即停，并消音到所有参数回到舒适区为止
- ☁️ **上云（可选）**：ESP8266（AT 指令）经 **自写 MQTT 3.1.1 客户端** 把 temp / humi / lux 上报 **OneNET**（MQTT 物联网套件）；`APP_ENABLE_NET=0` 时完全不碰联网，纯本地采集显示
- 🔌 **非阻塞联网状态机**：AT 阻塞等待（`AT+CWJAP` 最长 20 s、`AT+CIPSTART` 15 s、等 CONNACK 5 s）期间由驱动每 20 ms 回调 `app_poll()`，**采集 / 显示 / 报警 / 按键照常响应**，不会像阻塞式写法那样卡住界面
- 📶 **断线检测与自动重连**：主循环每 20 ms 排空 USART3 接收缓冲，识别模块 URC（`CLOSED` / `WIFI DISCONNECT`）与**连续 2 次收不到 `PINGRESP`** 两类事件，触发 `+++` 退出透传 → `AT+CIPCLOSE` → `AT+RST` + 重扫波特率 → 重新 `CWJAP` / `CIPSTART` / MQTT `CONNECT`，**最坏约 65 s 内察觉并开始重连**
- 💾 **报警历史（掉电留存）**：时间戳（RTC）+ 报警原因（TEMP / HUMI / LIGHT 组合）+ 当时温湿度光照，写入**片内 FLASH 最后一页 0x0807F800**，**64 条环形**，每条 16 B + CRC16，头部另有魔数与 CRC；只在"报警开始"写一次；按 KEY1 翻页回看，KEY0 返回
- ⚙️ **集中配置**：所有可调项集中在 `User/App/app_config.h`（传感器选择、字号上限、报警/舒适区阈值、确认窗与迟滞、联网开关、心跳周期、日志开关等）

## 硬件与接线

| 部件 | 说明 |
|---|---|
| 主控 | 正点原子**精英版 V2**，STM32F103ZET6（72 MHz / 512 KB Flash / 64 KB SRAM） |
| 屏幕 | TFT-LCD（FSMC 并口），实测 3.5″ 480×320 显示字号 32/32；兼容 2.8″ 320×240 与 7″ 800×480 |
| 温湿度 | DHT11（板载 U4 座，自带 4.7 K 上拉）或 SHT30（软件 I2C，与板载 24C02 共总线，地址不冲突） |
| 光照 | 板载光敏 LS1 |
| 联网 | ESP8266（AT 固件）经 USART3 连接，MQTT 上报 OneNET |
| 调试 | USART1（板载 CH340C），115200 8N1 |

**引脚占用（已逐脚核对官方 IO 分配表）**

| 功能 | 引脚 | 备注 |
|---|---|---|
| USART1 调试 | PA9 / PA10 | 板载 CH340C，P3 跳线帽需插上 |
| USART3 ↔ ESP8266 | PB10 (TX) / PB11 (RX) | 杜邦线交叉连接 |
| 光敏 LS1 | PF8 = ADC3_IN6 | 板载，免接线 |
| DHT11 | PG11 | 板载座自带 4.7 K 上拉 |
| SHT30 | PB6 = SCL / PB7 = SDA | 软件 I2C，与 24C02 共总线 |
| 蜂鸣器 | PB8 | 板载有源蜂鸣器 |
| KEY0 / KEY1 | PE4 / PE3 | 报警消音 / 历史翻页 |
| LED0 / LED1 | PB5 / PE5 | 运行与联网指示 |
| RTC 晶振 | PC14 / PC15 | 装 CR1220 后掉电继续走时 |

## 数据流

```
 温湿度传感器       ┐
  DHT11 (PG11)     │
   或              ├─► STM32F103ZET6 ──┬─► TFTLCD (FSMC)   本地实时显示  ← 核心，始终工作
  SHT30 (PB6/PB7)  │   72MHz / HAL     │
 光敏 LS1 (PF8)    ┘                   └─► USART3 (PB10/PB11) ─► ESP8266 ─► WiFi 2.4G
   ADC3_IN6                                                （APP_ENABLE_NET=1 时启用）│
                                                      MQTT over TCP :1883          │
                                                 mqtts.heclouds.com ◄───────────────┘
                                                      OneNET「MQTT 物联网套件」
                                              数据流: temp / humi / lux

 报警发生时 ──► RTC(PC14/PC15) 取时间戳 ──► 片内 FLASH 末页(0x0807F800)
                                                 保存 64 条历史，KEY1 翻页回看
```

## 关键设计决策

| 决策 | 原因 |
|---|---|
| **报警分舒适区 / 危险区两级** | 老人居室冬 16–20 ℃、夏 26–28 ℃ 都属正常。把舒适区当报警线，设备会天天响，**老人会直接把电源拔掉，报警功能归零**。所以舒适区只做黄色提示，危险区才响 |
| **确认窗 + 迟滞** | 这类居家监护设备最大的敌人是误报：手电扫过、开关门、传感器抖动。**一次误报的伤害比十次漏报还大**（用户会拔电源），所以先用 3 s 确认窗滤瞬时尖峰，再用迟滞防止阈值附近反复报警 |
| **危险区用整屏闪烁而不只是蜂鸣** | 60 岁以上人群约三分之一有听力损失，纯声音报警可能无效；红底闪烁 + 蜂鸣双通道提示 |
| **报警历史用片内 FLASH 而非板载 24C02** | 24C02 与 SHT30 共用软件 I2C，再写一套软 I2C 驱动既浪费代码又有读坏 SHT30 的风险；片内 FLASH 不占引脚、不走总线，且 F103ZET6 页大小 2 KB，末页存 64 条仅用约 1 KB。代价是擦写寿命（内部约 1 万次），按每天报警 3 次算可用约 9 年 |
| **联网做成"尽力而为"的状态机** | 采集与显示是核心，必须永远工作；联网任一环节失败只更新屏幕上对应状态行，15 s 后重试，**绝不阻塞主循环** |
| **主循环必须排空串口接收缓冲** | 旧版上线后无代码读串口，768 字节环形缓冲只涨不落，满了之后模块的 `CLOSED` / `PINGRESP` 会被 ISR 静默丢弃，而 `mqtt_ping()` 只判断"串口写成功没有"（几乎永远成功）——结果是**云端已断、本地一路显示 `Cloud : online`、`Sent` 还在涨**。对监护设备，这种**静默失效比误报危险得多** |

## 目录结构

```
atk_f103/
├── atk_f103.ioc          STM32CubeMX 工程配置（可随时重新生成代码）
├── User/                 CubeMX 生成（Inc / Src），只在 USER CODE 段加了两行：include 与 app_main()
├── Drivers/              驱动层（结构对齐正点原子例程）
│   ├── BSP/              LED / BEEP / KEY / LCD / DHT11 / SHT30 / LSENS / ESP8266 / RTC / STMFLASH
│   ├── SYSTEM/           sys / delay（SysTick 时钟摘取法，µs 级延时）/ usart（printf 重定向）
│   ├── CMSIS/            ARM CMSIS
│   └── STM32F1xx_HAL_Driver/   ST HAL 库
├── User/App/             应用层（自写代码集中于此，CubeMX 重新生成不会覆盖）
│   ├── app_config.h      ★ 全部可配置项（凭据优先从 app_config_local.h 读取）
│   ├── app_main.c        主循环：采集 → 显示 → 上报 → 心跳 → 断线重连
│   ├── app_ui.c          LCD 显示界面
│   ├── app_th.c          温湿度传感器（SHT30/DHT11 自动探测）
│   ├── app_net.c         ESP8266 AT 状态机
│   ├── app_alarm.c       两级超限判断 + 确认窗 + 迟滞 → 蜂鸣器与整屏闪烁 + 消音
│   ├── app_mqtt.c        极简 MQTT 3.1.1（CONNECT / PUBLISH / PINGREQ / DISCONNECT）
│   ├── app_onenet.c      OneNET 主题与 JSON 载荷组装
│   └── app_log.c         报警历史（片内 FLASH 末页，64 条环形 + CRC16）
├── Projects/MDK-ARM/     Keil 工程 atk_f103.uvprojx
├── Output/               编译产物与交付固件（atk_f103.hex）
├── Tools/                ESP8266 AT 固件烧录说明 + AT 串口引脚补丁（备查）
├── Docs/                 项目文档（见下）
├── readme.txt            接线表 / OneNET 配置 / AT 排错（GBK 编码，被 Keil 工程引用，勿移动）
└── keilkill.bat          清理编译中间文件
```

**代码规模**：应用层自写 **11 个文件 / 2,409 行**（`User/App`）。

## 编译与烧录

1. 工具链：**STM32CubeMX 6.18 + STM32Cube FW_F1 V1.8.7 + HAL 库 + Keil MDK（Arm Compiler 6）**
2. 打开 `Projects\MDK-ARM\atk_f103.uvprojx`，直接 `F7` 编译（**0 Error / 0 Warning**）、`LOAD` 下载
3. 联网前需先给 ESP8266 烧 AT 固件并配好 OneNET 参数，步骤见 `Tools/` 与 `readme.txt`

**编译体积（Code + RO-data）**

| 配置 | 体积 | 说明 |
|---|---|---|
| `APP_ENABLE_NET = 0` | ≈ **55.5 KB** | 只做采集 + LCD 显示，完全不碰 ESP8266 |
| `APP_ENABLE_NET = 1` | ≈ **62.0 KB** | 完整联网 + MQTT 上报 |

> 两种配置下**采集与显示都始终工作**，联网失败不影响本地报警。

## 验收状态

**已验收完毕（2026-09-22）**，验收记录见 `Docs/项目说明.md` §9；交付固件为 `Output/atk_f103.hex`（MD5 `b5bad6a0389f9d1382df55cde64ca0ed`），`Output/atk_f103.map` 与 `atk_f103.build_log.htm` 为体积与构建复核证据。

## 实拍照片

<!-- 把 2–3 张实拍照片放到 images/ 目录（文件名 setup-1.jpg / setup-2.jpg / setup-3.jpg），
     删除下面这行的注释即可显示。 -->

<!--
<img src="images/setup-1.jpg" width="420" alt="运行实拍 1">
<img src="images/setup-2.jpg" width="420" alt="运行实拍 2">
-->

> 建议拍：① LCD 正常显示温湿度光照；② 越界时整屏红色闪烁报警；③ 接线全貌（ESP8266 + 传感器）。

## 文档

| 文档 | 作用 |
|---|---|
| [Docs/项目说明.md](Docs/项目说明.md) | **功能现状以此为准**：配置总览 / 数据流 / 目录 / 引脚 / 关键设计决策 / 编译下载 / 验收要点 |
| [Docs/基线变更记录.md](Docs/基线变更记录.md) | 立项目径的变更与关闭（BC-001 ~ BC-009）+ 现行基线快照 |
| [Docs/适老化优化建议_市场调研.md](Docs/适老化优化建议_市场调研.md) | 市场调研与前向路线图（P0/P1/P2） |
| [Docs/README.md](Docs/README.md) | 文档索引与工程目录对应关系 |

## 已知限制

- 光敏 LS1 是光敏电阻，读数为 0~100 的**相对光强**，不是标定照度计（如需真实照度应改用 BH1750 等数字照度传感器）
- 写报警历史需"整页擦除 + 整页写回"约 80 ms，期间 CPU 被 FLASH 控制器挂起，串口可能丢字节（丢的是 QoS0 回执，无影响）；这 80 ms 内掉电会丢整页记录（头部 CRC 校验不过，上电按空库处理，不会显示乱码）

## 致谢与版权

- 底层驱动（`sys / delay / usart / lcd / key / beep / led / lsens / stmflash` 等）与工程模板基于**正点原子精英版例程**，版权归 **广州市星翼电子科技有限公司** 所有
- 应用层（`User/App/`：采集调度、两级报警、非阻塞联网状态机、自写 MQTT、报警历史）为本项目自写
- 本项目以 **MIT 许可证** 发布，详见 [LICENSE](LICENSE)
