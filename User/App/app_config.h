/**
 ****************************************************************************************************
 * @file        app_config.h
 * @brief       工程配置: 传感器选择 + 界面字号 + 功能开关 + WiFi + OneNET 参数
 * @note        ★ 平时只需要改这一个文件 ★
 ****************************************************************************************************
 */

#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ============================ 1. 温湿度传感器选择 ============================ */
/* 0 = 只用 DHT11  (PG11 单总线, 板载 U4 座自带 4.7K 上拉)
 * 1 = 只用 SHT30  (PB6=SCL / PB7=SDA 软件 I2C, 与板载 24C02 共总线, 地址不冲突)
 * 2 = 自动探测: 先找 SHT30, 找不到再找 DHT11 (推荐, 插哪个用哪个)
 * 串口与 LCD 上都会显示实际识别到的型号。 */
#define APP_SENSOR_TH           2

/* ============================ 2. 界面字号 ============================ */
/* 正点原子字库只支持 12 / 16 / 24 / 32 四种 ASCII 字号。
 * 这两个值是"上限", 程序会按屏幕实际高度、以及实际要显示的行数,
 * 自动向下取一档保证放得下:
 *     3.5 寸横屏 480x320, APP_ENABLE_NET=0 -> 32 / 32
 *     3.5 寸横屏 480x320, APP_ENABLE_NET=1 -> 32 / 32
 *     2.8 寸横屏 320x240, APP_ENABLE_NET=0 -> 32 / 32
 *     2.8 寸横屏 320x240, APP_ENABLE_NET=1 -> 24 / 16
 * 关掉联网时 WiFi/Cloud/Sent 三行不显示, 省下的高度全部让给字号。 */
#define APP_UI_FONT_TITLE       32
#define APP_UI_FONT_DATA        32

/* ============================ 3. 功能开关 ============================ */
/* 0 = 只用传感器 + LCD 本地显示, 完全不碰 ESP8266 和 OneNET (默认)
 * 1 = 打开 ESP8266 联网 + OneNET 上报(需要先填好下面第 4、5 节的参数)
 * 无论取值如何, 采集与 LCD 显示都照常工作, 联网失败不会卡住界面。 */
#define APP_ENABLE_NET          1

/* ============================ 4. WiFi 热点 (必须 2.4GHz) ============================ */
/* ★ 凭据安全：优先从 app_config_local.h 读取（已 gitignore），不存在时用下面默认值 ★ */
#if __has_include("app_config_local.h")
#include "app_config_local.h"
#else
#define APP_WIFI_SSID           "OPPOA5PRO"             /* 热点名称 */
#define APP_WIFI_PWD            "tmy11111"              /* 热点密码, 开放热点填 "" */
#endif

/* ============================ 5. OneNET 接入参数 ============================ */
/* 控制台 -> 产品 -> 产品ID; 设备 -> 设备名; 设备详情 -> 设备密钥(用于生成 token)
 *
 * 设备密钥(仅留档, token 到期后用它重算, 见 readme.txt):
 *   SSvNJb0nZ4knt6WNO6d3JZeTizctC6FRk3t7C0kT32c=
 */
#define ONENET_HOST             "mqtts.heclouds.com"    /* 非加密接入点 */
#define ONENET_PORT             1883

/* OneNET 参数优先从 app_config_local.h 读取 */
#ifndef ONENET_PID
#define ONENET_PID              "feCiGN7S9q"            /* 产品 ID */
#endif
#ifndef ONENET_DEVICE
#define ONENET_DEVICE           "dht111"                /* 设备名称 */
#endif
/* token(由上面的设备密钥 + 产品ID + 设备名算出, et=2000000000 即 2033 年到期) */
#ifndef ONENET_TOKEN
#define ONENET_TOKEN            "version=2018-10-31&res=products%2FfeCiGN7S9q%2Fdevices%2Fdht111&et=2000000000&method=sha1&sign=uKV9j8kzJvhULgKpXpXDgjmM5AI%3D"
#endif

/* 上报格式:
 *   0 = 数据流-数据点  主题 $sys/{pid}/{dev}/dp/post/json      (MQTT物联网套件, 不需要预先定义物模型)
 *   1 = 物模型属性    主题 $sys/{pid}/{dev}/thing/property/post (OneNET Studio, 需先建好 3 个属性)
 * 默认 0; 若串口显示 MQTT 已连上但平台看不到数据, 就改成 1 重新编译。 */
#define APP_ONENET_MODE         0

/* ============================ 6. 数据流名称 ============================ */
#define ONENET_DP_TEMP          "temp"                  /* 温度  x10 ℃ */
#define ONENET_DP_HUMI          "humi"                  /* 湿度  x10 %RH */
#define ONENET_DP_LUX           "lux"                   /* 光照  0~100 */

/* ============================ 7. 报警阈值(两级: 舒适区 / 危险区) ============================ */
/* 这是本工程最重要的产品决策, 两级语义完全不同:
 *
 *   危险区 APP_xxx_X10 / APP_LUX_xxx_CDM2
 *       超出 -> 真正报警: 蜂鸣器响 10s + LCD 整屏闪烁 + 对应行变红。
 *       依据是医学风险点: 低温有低体温风险(独居老人冬天舍不得开暖气, 这才是最该报的),
 *       高温有中暑/脱水风险, 湿度极端值对应呼吸道不适与结露发霉。
 *
 *   舒适区 APP_COMFORT_xxx
 *       超出(但还在危险区内) -> 只把对应行和 Alarm 行变成黄色 + 显示 warnT/warnH/warnL,
 *       **不响蜂鸣器**。老人居室冬天 16~20℃、夏天空调房 26~28℃ 都属正常;
 *       把"舒适区"当报警线会让设备天天响, 老人会直接把电源拔掉, 报警功能归零。
 *
 * ★ 约束: COMFORT 必须落在危险区以内(即 DANGER_MIN <= COMFORT_MIN < COMFORT_MAX <= DANGER_MAX),
 *   程序不检查这个约束, 填反了会出现"只提示不报警"的怪现象。
 * 单位: 温度/湿度 0.1 单位; 光照 cd/m2(填 0 = 该方向不判断)。 */
#define APP_TEMP_MIN_X10            100     /* 危险: 10.0 ℃ 以下 */
#define APP_TEMP_MAX_X10            320     /* 危险: 32.0 ℃ 以上 */
#define APP_HUMI_MIN_X10            250     /* 危险: 25.0 %RH 以下 */
#define APP_HUMI_MAX_X10            850     /* 危险: 85.0 %RH 以上 */
#define APP_LUX_MIN_CDM2            0       /* 危险: 不做下限判断 */

#define APP_COMFORT_TEMP_MIN_X10    180     /* 舒适: 18.0 ℃ */
#define APP_COMFORT_TEMP_MAX_X10    280     /* 舒适: 28.0 ℃ */
#define APP_COMFORT_HUMI_MIN_X10    400     /* 舒适: 40.0 %RH */
#define APP_COMFORT_HUMI_MAX_X10    700     /* 舒适: 70.0 %RH */
#define APP_COMFORT_LUX_MIN_CDM2    100     /* 舒适: 100 cd/m2 */
#define APP_COMFORT_LUX_MAX_CDM2    4000    /* 舒适: 4000 cd/m2 == 光敏读数 20,
                                             * 正好等于旧版"2000 cd/m2 + FULL_SCALE 10000"那条
                                             * 会响的线 —— 也就是说同样的光照条件, 现在只变黄不响。 */

/* 光照量程说明(改之前务必看懂):
 * 板载 LS1 是光敏电阻, lsens_get_val() 给出的是 0~100 的"相对光强", 不是标定过的照度计。
 * FULL_SCALE 表示"光敏读数 100 相当于多少 cd/m2", 用来把 cd/m2 阈值换算到光敏读数。
 * 默认 20000(室内直射阳光量级) -> 光敏读数 1 对应 200 cd/m2。
 * 标定方法: 用手机照度计 App 对着同一光源, 同时看串口打印的 lsens= 读数,
 *           用 照度*(100/lsens) 反推 FULL_SCALE, 改这个宏即可。
 *
 * 光照的危险上限是"光敏饱和(强光直射)"。因为 alarm_eval 用的是 `>`
 * 比较, 取 FULL_SCALE-100 才能在读数打满量程的瞬间触发;
 * 写成表达式是为了标定后改 FULL_SCALE 时它能自动跟着走。 */
#define APP_LUX_FULL_SCALE_CDM2     20000
#define APP_LUX_MAX_CDM2            (APP_LUX_FULL_SCALE_CDM2 - 100)

/* 报警时序 */
#define APP_ALARM_TIME_MS       10000   /* 单次报警持续时间(蜂鸣器连续响) */
#define APP_ALARM_GAP_MS        10000   /* 两次报警之间的静默时间 */

/* 误报抑制 —— 上面两级阈值只定义"越界多少", 这里定义"什么时候才算越界/才算恢复"。
 *   误报是这类居家监护设备最大的敌人: 老人被反复吵到会直接把电源拔掉,
 *   一次误报造成的伤害比十次漏报还大。
 *
 *   CONFIRM_MS : 越界必须"连续"持续这么久才确认, 用来滤掉瞬时尖峰
 *                (光照被手电/阳光扫过、开关门、传感器抖动)。填 0 = 立即生效。
 *                危险区和舒适区共用这一个确认窗。
 *   HYST_xxx   : 迟滞量。已经在某一级上时, 恢复边界相对触发边界内收,
 *                否则数值卡在阈值上下跳动时, 会"响 10s 停 10s"无限循环,
 *                或者黄字一闪一闪。迟滞量必须小于阈值区间的一半。 */
#define APP_ALARM_CONFIRM_MS    3000    /* 连续越界确认窗(ms), 0 = 立即生效 */
#define APP_ALARM_HYST_TEMP_X10 5       /* 温度迟滞 0.5℃ */
#define APP_ALARM_HYST_HUMI_X10 20      /* 湿度迟滞 2.0%RH */
#define APP_ALARM_HYST_LUX_CDM2 100     /* 光照迟滞 100 cd/m2 */

/* ============================ 8. 运行参数 ============================ */
#define APP_REPORT_PERIOD_MS    5000                    /* 数据上报周期(ms) */
#define APP_PING_PERIOD_MS      30000                   /* MQTT 心跳周期(ms), 须小于 MQTT_KEEPALIVE_S */
#define MQTT_KEEPALIVE_S        120                     /* MQTT keepalive(秒), 平台允许 10~1800 */
#define MQTT_PING_TIMEOUT_MS    3000                    /* 发 PINGREQ 后等 PINGRESP 的超时(ms) */
#define APP_PING_FAIL_MAX       2                       /* 连续几次心跳无应答就判定链路断了并重连 */
#define APP_TH_RETRY            3                       /* 温湿度连续失败多少次后重新探测传感器 */
#define APP_DHT11_PERIOD_MS     2000                    /* DHT11 采样间隔: 手册要求 ≥1s, 取 2s 更稳 */
#define APP_NET_RETRY_MS        15000                   /* 联网失败后的重试间隔(ms) */
#define APP_NET_FAIL_RESCAN     4                       /* 连续失败几次后允许重新全扫描波特率
                                                         * (模块自己复位过的话波特率可能变了,
                                                         *  只扫一次会永久连不上) */
#define APP_SAMPLE_PERIOD_MS    500                     /* 光照采样 + 刷屏周期(ms) */
#define APP_ENABLE_IWDG         1                       /* 独立看门狗(约 8s): 死机自动复位; 填 0 关闭 */

/* ============================ 9. 报警时的整屏闪烁 ============================ */
/* 60 岁以上人群约三分之一有不同程度听力损失, 光靠蜂鸣器对一部分老人等于没有报警。
 * 打开后: 蜂鸣器响的同时, LCD 整屏在"红底白字 / 白底彩字"之间交替,
 *         既听得到也看得见; 消音或恢复正常后自动回到白底。
 * 报警开始/结束的时序、阈值、蜂鸣器行为一律不变, 只是多一层视觉提示。 */
#define APP_ALARM_FLASH         1                       /* 1 = 报警时整屏闪烁; 0 = 关闭 */
#define APP_ALARM_FLASH_MS      400                     /* 闪烁半周期(ms), 400 即每秒约 1.25 次 */

/* ============================ 10. 报警历史记录 ============================ */
/* 1 = 每次报警把"时间 + 原因 + 当时的温湿度/光照"存进内部 FLASH 最后一页, 可用 KEY1 翻页查看;
 * 0 = 完全不碰 RTC 和 FLASH(省掉 RTC 初始化与那一次整页擦写)。
 *
 * 硬件前提: PC14/PC15 的 32.768kHz 晶振(板上已有) + VBAT 纽扣电池座(CR1220)。
 *   装了电池 -> 掉电后时间继续走, 记录里的时间戳是真实时间;
 *   没装电池 -> 每次上电时间从**固件编译时间**开始(串口会打 [clock not battery-backed]),
 *               时间戳只能当相对参考, 但记录本身照样存得住。
 *
 * 存储位置: 0x0807F800 (512KB FLASH 的最后一页, 2KB), 存 64 条, 满了覆盖最早的。
 *   记录自带 CRC16, 半页写坏时能逐条识别; 头部也有 CRC, 整页写一半掉电则当作空库重来。
 * 相关文件: Drivers/BSP/RTC, Drivers/BSP/STMFLASH, User/App/app_log.c */
#define APP_ENABLE_LOG          1

#endif
