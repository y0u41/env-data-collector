========================================================================
 atk_f103 —— STM32F103 + 温湿度(DHT11 / SHT30) + 光敏 => LCD 本地显示
             可选: ESP8266 -> OneNET 环境数据上云
 开发板: 正点原子 精英版 V2 (STM32F103ZET6)
========================================================================

一、功能
    1. 温湿度采集: DHT11(PG11 单总线) 或 SHT30(PB6/PB7 软件 I2C), 二选一或自动探测;
    2. 板载光敏电阻 LS1 采集光照强度(换算成 cd/m2 估计值);
    3. 两级超限判断(本轮改动, 报警触发点变了, 详见下面 ★):
         危险区: 温度 <10 或 >32℃ / 湿度 <25 或 >85%RH / 光照 光敏饱和
                 -> 连续越界 3 秒后蜂鸣器响 10 秒 + LCD 整屏闪烁, 按 KEY0 可消音;
         舒适区: 温度 18~28℃ / 湿度 40~70%RH / 光照 100~4000 cd/m2(光敏读数 20,
                 正好等于旧版那条会响的线 -> 同样一次强光现在只变黄不响)
                 -> 只把对应行和 Alarm 行变黄 + 显示 warnT/warnH/warnL, 不响;
    4. TFTLCD 本地实时显示(字号按屏幕尺寸自动放大), 不依赖任何外部模块;
    5. 报警历史记录: 每次报警把"时间+原因+当时温湿度/光照"存内部 FLASH, 最多 64 条,
       按 KEY1 在 LCD 上翻页回看(KEY0 返回), 掉电不丢;
    6. [可选] 通过 ESP8266(WiFi) 以 MQTT 协议把温湿度/光照上报到 OneNET。

    ★ 报警行为(User\App\app_config.h 可改):
        - 任一参数连续越界 APP_ALARM_CONFIRM_MS(默认 3 秒) -> 开始响,
          持续 APP_ALARM_TIME_MS(默认 10 秒); 期间 LCD 整屏在
          "红底白字 <-> 白底彩字"之间交替闪烁(APP_ALARM_FLASH, 老人听力衰退, 光靠蜂鸣器不够);
        - 10 秒到自动停, 静默 APP_ALARM_GAP_MS(默认 10 秒); 仍越界则再响 10 秒, 如此反复;
        - 报警期间按 KEY0(板上丝印 KEY0, PE4) -> 立即停, 屏幕回到白底,
          并消音到所有参数恢复正常为止(否则 0.5 秒后又响, KEY0 就没意义了);
        - 全部恢复正常 -> 自动解除消音, 下次越界可再次报警。
        - 恢复要回到"阈值内收一点"才算正常(迟滞 APP_ALARM_HYST_xxx), 避免数值卡在阈值
          附近时"响 10 秒停 10 秒"无限循环。
      LCD 最后一行 Alarm 的颜色:
          蓝 = 舒适区内, 一切正常
          黄 = 只超出舒适区, 不响(显示 warnT / warnH / warnL)
          红 = 超出危险区, 正在响
          黑 = 超出危险区但已消音 / 间歇期
      对应数值行同样 红=危险区, 黄=舒适区外, 蓝=正常。

    ★ 两级阈值为什么这么定(User\App\app_config.h 第 7 节):
        原来的 20~25℃ / 50~60% 是"舒适区", 不是"危险线"。冬天室温 18℃、夏天 29℃ 都会持续
        报警, 老人会直接把电源拔掉, 报警功能等于归零。所以拆成两级: 舒适区只做黄色提示,
        危险区才响。医学上真正该报的是**低温**(独居老人冬天舍不得开暖气 -> 低体温风险)。
        APP_COMFORT_xxx 必须落在危险区以内, 程序不检查这个约束, 填反了会"只提示不报警"。

    ★ 报警历史(APP_ENABLE_LOG=1, 本轮新增):
        存哪里: 内部 FLASH 最后一页 0x0807F800(2KB), 不占引脚、不走总线, 存 64 条环形覆盖;
        存什么: 时间(年月日时分秒) + 原因(TEMP/HUMI/LIGHT 组合) + 当时温度/湿度/光照;
        什么时候写: 只在"报警刚开始"写一次, 不写心跳、不写正常数据;
        怎么看:   按 KEY1 进历史页(同时把全部记录打到串口), KEY1 翻下一页, 到底再按回主页,
                   KEY0 返回主页; 报警一响会强制切回主页(报警界面优先);
        时间戳:   RTC(PC14/PC15 晶振)。装 CR1220 纽扣电池 -> 掉电继续走;
                   不装 -> 每次上电从**固件编译时间**开始, 串口会提示 [clock not battery-backed]。
        注意:     写一次历史要整页擦除+整页写回(约 80ms), 这期间 CPU 被 FLASH 挂住,
                   串口可能丢几个字节(都是 OneNET 的 QoS0 回执, 无影响);
                   若在这 80ms 中间掉电, 整页记录一起丢(上电会当作空库重来, 不会显示乱码)。

    ★ 联网断线检测(APP_ENABLE_NET=1 时, 本轮新增):
        上线后主循环每 20ms 排空串口接收缓冲, 靠两件事发现链路断了:
        - 模块上报 CLOSED / WIFI DISCONNECT  -> 立即重连;
        - 每 30 秒发一次 PINGREQ 并真的等 PINGRESP, 连续 2 次等不到 -> 立即重连。
        重连流程: +++ 退出透传 -> AT+CIPCLOSE -> 等 15 秒 -> AT+RST 复位模块并重扫波特率
                  -> 重新 CWJAP / CIPSTART / MQTT CONNECT, 最坏约 65 秒内必被察觉。
        串口会在断线时依次打印:
          MQTT: ping no answer (1/2)
          MQTT: link lost (2 pings no answer), reconnect
          NET FAIL: Cloud : link lost (resp: ...)
        相关宏: APP_PING_PERIOD_MS / MQTT_PING_TIMEOUT_MS / APP_PING_FAIL_MAX /
                APP_NET_RETRY_MS / APP_NET_FAIL_RESCAN。

    ★ 温湿度传感器由 User\App\app_config.h 里的 APP_SENSOR_TH 选择:
        APP_SENSOR_TH = 0  -> 只用 DHT11
        APP_SENSOR_TH = 1  -> 只用 SHT30
        APP_SENSOR_TH = 2  -> 自动探测: 先找 SHT30, 找不到再找 DHT11 (默认)
      开机时串口会打印 "TH   : SHT30 detected", LCD 的传感器那一行也显示实际型号。

    ★ 联网功能由 User\App\app_config.h 里的 APP_ENABLE_NET 控制:
        APP_ENABLE_NET = 0 -> 只做采集 + LCD 显示, 完全不碰 ESP8266 / OneNET (默认)
        APP_ENABLE_NET = 1 -> 打开 WiFi + OneNET 上报, 需先填好 WiFi 与 OneNET 参数
      两种取值都保证采集与显示照常运行; 联网任何一步失败只更新屏幕状态行,
      绝不会阻塞界面(联网是状态机, 每次主循环只推进一步)。

二、硬件接线
    功能          STM32 引脚     板上资源                接线
    ---------------------------------------------------------------------
    SHT30 SCL     PB6            I2C_SCL(4.7K 上拉)     杜邦线接模块 SCL
    SHT30 SDA     PB7            I2C_SDA(4.7K 上拉)     杜邦线接模块 SDA
    SHT30 VCC     3.3V 排针                             严禁接 5V
    SHT30 GND     GND
    DHT11 DATA    PG11           U4 单总线座(4.7K 上拉) 杜邦线接 PG11 排针
                                 (与 U4 座 DQ 脚同一网络, 接哪边都行)
    DHT11 VCC     3.3V 排针
    DHT11 GND     GND
    ESP8266 TX    PB11           USART3_RX               ESP8266 TXD -> PB11
    ESP8266 RX    PB10           USART3_TX               ESP8266 RXD -> PB10
    ESP8266 VCC   5V 或 3.3V                            见下面第 3) 条说明
    ESP8266 GND   GND                                   必须与开发板共地
    调试串口      PA9 / PA10     CH340C                 P3 跳线帽必须插上
    光敏          PF8            LS1                    免接线
    TFTLCD        FSMC 排座                             免接线
    蜂鸣器        PB8            板载 BEEP              免接线(高电平鸣叫)
    KEY0          PE4            板载 KEY0              免接线(按下为低电平)

    说明:
    1) SHT30 与 DHT11 可以同时接上, 程序按 APP_SENSOR_TH 选择; 自动模式下 SHT30 优先。
       PB6/PB7 与板载 24C02 共用 I2C 总线, 地址不冲突(24C02 是 0xA0, SHT30 是 0x44),
       板载已有 4.7K 上拉, SHT30 模块自带的 4.7K/10K 上拉也不用拆。
    2) 本工程用"软件 I2C"而不是 STM32F1 的硬件 I2C: F1 的硬件 I2C 有已知的总线锁死
       问题, 正点原子官方 24C02 例程在同样这两个脚上用的也是软件 I2C。
    3) ESP8266 只支持 2.4GHz; 模块板自带 CH340+LDO(5V 输入) 时 VCC 接开发板 5V 排针,
       裸 ESP-01S / ESP-12F 则只能接 3.3V, 严禁接 5V; 建议在模块 VCC-GND 就近并
       470uF 电解 + 100nF 陶瓷电容, 防止发射瞬间拉崩电源;
       带 CH340 的模块板, 其 USB 口不要同时插电脑(会与 STM32 抢串口总线)。

三、软件环境与界面字号
    STM32CubeMX 6.18 + STM32Cube FW_F1 V1.8.7 + Keil MDK(Arm Compiler 6)
    工程文件: Projects\MDK-ARM\atk_f103.uvprojx
    编译器  : Arm Compiler 6 (AC6)

    界面字号(User\App\app_config.h):
        APP_UI_FONT_TITLE = 32     标题字号上限
        APP_UI_FONT_DATA  = 32     正文字号上限

    正点原子字库只支持 12 / 16 / 24 / 32 四种 ASCII 字号。程序按"屏幕实际高度"
    和"实际要显示的行数"在四种字号里自动挑一个放得下的最大值, 再受上面两个上限约束。
    关掉联网(APP_ENABLE_NET=0)时 WiFi/Cloud/Sent 三行不显示, 省下的高度全部让给字号:
        3.5 寸横屏 480x320, NET=0  ->  标题 32 / 正文 32
        3.5 寸横屏 480x320, NET=1  ->  标题 32 / 正文 32
        2.8 寸横屏 320x240, NET=0  ->  标题 32 / 正文 32
        2.8 寸横屏 320x240, NET=1  ->  标题 24 / 正文 16
    开机串口会打印 "LCD  : id=xxxx  480x320  font=32/32", 可据此确认实际字号。

四、使用步骤

    A) 只用本地显示 (默认, APP_ENABLE_NET = 0, 不需要 ESP8266):
       1. 按第二节接好温湿度传感器(光敏板载, 免接线);
       2. Keil 打开 Projects\MDK-ARM\atk_f103.uvprojx, 全编译, 下载;
       3. 打开串口助手(USART1, 115200) 观察日志;
       4. 上电后 LCD 应显示(以 3.5 寸横屏为例):
            atk_f103 Environment Monitor
            Temp  : 23.5 C
            Humi  : 55.3 %
            Light : 3000 cd/m2
            SHT30 : OK
            Alarm : LIGHT
          Temp / Humi / Light 每 0.5 秒刷新一次, LED1 同步闪烁。
          (APP_ENABLE_NET=1 时才会额外出现 WiFi / Cloud / Sent 三行)

          上面这个例子: 光照 3000 > 2000 且持续 3 秒确认后, Light 行和 Alarm 行变红,
          蜂鸣器响 10 秒, 同时 LCD 整屏红底/白底交替闪烁;
          此时按一下 KEY0 立即停(屏幕回白底), Alarm 行变黑并显示 "LIGHT mute"。

    ★ 报警阈值(User\App\app_config.h 第 7 节):
        APP_TEMP_MIN_X10 / APP_TEMP_MAX_X10   温度上下限, 单位 0.1℃ (200 / 250)
        APP_HUMI_MIN_X10 / APP_HUMI_MAX_X10   湿度上下限, 单位 0.1%RH (500 / 600)
        APP_LUX_MIN_CDM2 / APP_LUX_MAX_CDM2   光照上下限, 单位 cd/m2 (0 / 2000),
                                              填 0 表示该方向不做判断
       * 传感器没接好时 Temp/Humi 显示 --.-, 最后一行显示 "SHT30 : read fail"
         或 "TH    : not found"。

    B) 需要上传 OneNET 时:
       1. 打开 User\App\app_config.h, 把 APP_ENABLE_NET 改成 1;
       2. 在同一文件里填入 4 项参数(见第五节);
       3. 重新编译下载; LCD 会依次显示 WiFi / Cloud 的连接进度。

五、OneNET 配置与 token 生成
    1. 登录 open.iot.10086.cn, 进入「MQTT 物联网套件」(新版);
    2. 创建产品 -> 记录 产品ID(pid);
    3. 产品下创建设备 -> 记录 设备名; 在产品详情里查 设备密钥(key);
    4. 数据流名称: temp / humi / lux (平台会在首次上报时自动创建);
    5. token 生成: 在 PC 上用 Python 跑一次, et 设到 2033 年即可长期有效
         import base64, hmac, hashlib
         from urllib.parse import quote
         pid, dev, key = "产品ID", "设备名", "设备密钥"
         et  = 2000000000                    # 2033-05-18
         res = "products/%s/devices/%s" % (pid, dev)
         sfs = "%d\nsha1\n%s\n2018-10-31" % (et, res)
         sign = base64.b64encode(hmac.new(base64.b64decode(key),
                                          sfs.encode(), hashlib.sha1).digest()).decode()
         token = "version=2018-10-31&res=%s&et=%d&method=sha1&sign=%s" % (
                     quote(res, safe=""), et, quote(sign, safe=""))
         print(token)
       把打印出的整串填到 ONENET_TOKEN。
    6. 上报主题: $sys/{产品ID}/{设备名}/dp/post/json
       上报载荷: {"id":N,"dp":{"temp":[{"v":26.5}],"humi":[{"v":55.3}],"lux":[{"v":73}]}}
       (载荷不带时间戳, 平台按到达时间入库, 因此不需要 RTC/SNTP)

六、排错

    SHT30 读不到:
      - LCD 显示 "SHT30 : read fail" -> 接线松; 或模块 ADDR 接高(地址变 0x45),
        改 Drivers\BSP\SHT30\sht30.h 里的 SHT30_I2C_ADDR 为 0x45
      - LCD 显示 "TH    : not found" -> SCL/SDA 接反或没供电(务必 3.3V)
      - 杜邦线尽量短(20cm 以内), 太长会导致 I2C 波形变差

    DHT11 读不到:
      - LCD 显示 "DHT11 : read fail" -> 检查 PG11 接线; 杜邦线过长容易失败;
        裸 DHT11(4 脚)需要 4.7K~10K 上拉, 接板上 U4 座则已有上拉

    报警相关:
      - 一直误报光照 -> 板载 LS1 是光敏电阻, 输出的是 0~100 相对值, 不是标定过的
        照度计。APP_LUX_FULL_SCALE_CDM2(默认 10000) 表示"光敏读数 100 相当于多少
        cd/m2", 2000 cd/m2 对应光敏读数 20。标定方法: 用手机照度计 App 对着同一光源,
        同时看串口打印的 lsens= 读数, 用 照度*(100/lsens) 反推, 改这个宏即可。
        不想判光照就把 APP_LUX_MAX_CDM2 填 0。
      - 报警后按 KEY0 没用 -> KEY0 只在蜂鸣器正在响时生效; 若一直响不停, 说明
        消音后参数恢复正常又立即越界了(每 0.5 秒判断一次), 检查是不是阈值太紧。
      - 越界了却不马上响 -> 这是有意设计的 3 秒确认窗(APP_ALARM_CONFIRM_MS),
        用来滤掉瞬时尖峰。想立刻响就把它填 0。
      - 报警时屏幕一直红白闪 -> 正常, 是 APP_ALARM_FLASH; 不想要就填 0。
      - 温度到 30℃ 只变黄不响 -> 正常, 30℃ 还在危险区内(10~32℃), 属舒适区外提示。
                                  想改成 25℃ 就响, 把 APP_TEMP_MAX_X10 改 250。

    报警历史相关:
      - 串口打 "RTC: LSE not ready, fallback to LSI" -> PC14/PC15 的 32.768kHz 晶振没起振
        (或板子没焊晶振)。会退回内部 LSI, 时间能走但每天误差几十秒。
      - 串口打 "[clock not battery-backed]" -> VBAT 座里没装 CR1220 纽扣电池,
        时间是从固件编译时刻开始的, 时间戳只能当相对参考(记录本身照样存得住)。
      - 串口打 "LOG: no valid log in flash, start empty" -> 第一次上电, 或上次写一半掉电了;
        属正常, 触发一次报警后就会写进去。
      - 记录条数不对/想清空 -> 用 app_log_clear()(见 项目说明.md 第 0.5 节)。
      - 按 KEY1 没反应 -> KEY1 是 PE3; 报警正在响时不允许翻页(会强制回主页)。

    ESP8266 相关:
      - 发 AT 无 OK            -> PB10/PB11 接反, 或模块波特率不是 115200, 或供电不足
      - AT+CWJAP 返回 FAIL     -> 热点不是 2.4G / 密码错 / 信号太弱
      - AT+CIPSTART 无 CONNECT -> 模块没联网, 或 DNS 解析失败(先把 CWJAP 解决)
      - Cloud : MQTT FAIL      -> 产品ID / 设备名 / token 三者不匹配, 或 token 已过期
      - Cloud : link lost      -> 断线检测生效(收到 CLOSED 或 PINGRESP 连续 2 次无应答),
                                  属正常自愈流程: 15 秒后会自动 AT+RST 复位模块并重连。
                                  若反复出现, 查供电(模块发射瞬间拉崩 3.3V)、热点信号、
                                  以及 OneNET 设备是否被顶掉(同一设备名被别处登录)。
      单独调试模块: 用 USB-TTL 接模块, 串口助手 115200, 发 AT+GMR 看固件版本。

七、目录结构(对齐正点原子官方例程)
    atk_f103.ioc        STM32CubeMX 工程配置(可随时重新生成代码)
    Core\               CubeMX 生成的初始化代码(main.c / gpio.c / adc.c / usart.c ...)
    Drivers\            驱动层
      BSP\              LED / LCD / DHT11 / SHT30 / LSENS(光敏) / ESP8266
      SYSTEM\           delay(us 级延时) / sys / usart(printf 重定向)
      CMSIS\            ARM CMSIS 头文件与启动文件(由 CubeMX 维护)
      STM32F1xx_HAL_Driver\  ST HAL 库(由 CubeMX 维护)
    User\App\           应用层
                        app_config.h(全部配置) / app_main.c(主逻辑与界面)
                        app_mqtt.c(MQTT 报文) / app_onenet.c(OneNET 上报)
    Projects\MDK-ARM\   Keil 工程(atk_f103.uvprojx)
    Output\             编译输出(atk_f103.hex)
    Tools\              ESP8266 AT 固件烧录说明与引脚补丁(备查)
    说明: Drivers\BSP\RTC(实时时钟, 官方实验15 移植) 与 Drivers\BSP\STMFLASH(内部 FLASH 读写,
          官方实验31 移植) 是报警历史的硬件支撑; User\App\app_log.c 是历史记录的存取与校验。

八、扩展指引
    1. 再加一路传感器: Drivers\BSP\ 下新建驱动目录, 在 app_main.c 里加采集与显示,
       并在 app_onenet.c 的 payload 里增加一个数据流即可;
    2. 平台反向控制: 需订阅 $sys/{pid}/{dev}/cmd/request/#, 并解析平台下发的
       MQTT PUBLISH 报文, 可参照 User\App\app_mqtt.c 自行扩展;
    3. 换 4G 模块(ATK-ML307 等): 只替换 Drivers\BSP\ESP8266 里的联网 AT 指令,
       MQTT 层与 App 层完全不用改。

九、验收记录
    验收日期: 2026-09-22        验收结论: 已验收完毕(通过)
    验收固件: Output\atk_f103.hex   199897 字节
              MD5 = b5bad6a0389f9d1382df55cde64ca0ed
    编译结果: 42 个源文件, 0 Error / 0 Warning
              Code=56308  RO-data=14708  RW-data=28  ZI-data=9460
    本轮改动: 联网静默失效修复(P0) / 报警确认窗+迟滞+误报抑制(P1) /
              常用项整理(P2) / 两级阈值分级(P3-A) / 报警历史(P3-B);
    验收条目: 见"项目说明.md" §8(基本 11 条 + 扩展 6 条) 与 §9.1 对照表, 全部通过。

    遗留事项(不影响验收结论):
      1. 光照换算系数 APP_LUX_FULL_SCALE_CDM2 仍为估算值 20000, 可用手机照度计实测后修正,
         或把 APP_COMFORT_LUX_MAX_CDM2 改 0 关闭光照提示;
      2. RTC 走时靠 VBAT 座内的 CR1220 维持; 未装电池时每次上电时间回落到固件编译时刻,
         历史记录时间戳只能作相对参考;
      3. 报警历史选内部 FLASH 末页(0x0807F800)存储, 未用 24C02: 24C02 与 SHT30 共用
         PB6/PB7 软件 I2C, 另起一套软 I2C 有干扰 SHT30 的风险; 内部 FLASH 不占引脚。
