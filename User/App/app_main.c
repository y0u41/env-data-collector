/**
 ****************************************************************************************************
 * @file        app_main.c
 * @brief       应用主逻辑: 温湿度(DHT11/SHT30) + 光敏 采集 -> 超限报警 -> LCD 本地显示
 *              可选: ESP8266 -> OneNET 上报
 * @note        1, 采集与显示永远工作, 联网是"尽力而为", 任何一步失败都只影响状态栏, 绝不阻塞界面;
 *              2, 主循环按 20ms 节拍跑: 报警/按键每 20ms, 采集+刷屏每 APP_SAMPLE_PERIOD_MS;
 *              3, APP_ENABLE_NET = 0 时完全不碰 ESP8266 / OneNET;
 *              4, 界面字号按屏幕高度与实际行数自动选择, 上限由 app_config.h 指定;
 *              5, 本文件与 CubeMX 生成的代码完全隔离, 重新生成代码不会覆盖。
 ****************************************************************************************************
 */

#include "./App/app_main.h"
#include "./App/app_config.h"
#include "./App/app_alarm.h"
#include "./App/app_onenet.h"
#include "./App/app_mqtt.h"
#include "./App/app_log.h"
#include "./BSP/LED/led.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/DHT11/dht11.h"
#include "./BSP/SHT30/sht30.h"
#include "./BSP/LSENS/lsens.h"
#include "./BSP/ESP8266/esp8266.h"
#include "./BSP/KEY/key.h"
#include "./BSP/RTC/rtc.h"
#include "./SYSTEM/delay/delay.h"
#include <stdio.h>
#include <string.h>

/* ===================== 界面布局 ===================== */
#define UI_X            6           /* 左边距 */
#define UI_LINE_MAX     80          /* 每行最大字符数 */
#define UI_ROW_MAX      9           /* 行数上限(联网 + 报警全开时 9 行) */

/* ===================== 界面配色 ===================== */
/* 底色 + 普通文本色 + 提示级文本色, 集中在这三行, 换配色只改这里。
 * 注意 1: 底色与普通文本色必须互为反色, 否则黑底黑字 / 白底白字都看不见。
 * 注意 2: 提示级⛔ 不能直接用 YELLOW(0xFFE0) —— 纯黄在白底上对比度只有约 1.1:1,
 *         实测就是"黄字看不清"; 故改用深琥珀 UI_WARN, 白底上对比度约 4.7:1,
 *         色相仍是黄橙系, 与红(危险)/蓝(正常)一眼可分。
 *         报警"红底"那一相仍用纯黄(黄压红对比度很高), 见 ui_flash_task 后的两处覆盖。 */
#define UI_BG           WHITE       /* 整屏底色(白底) */
#define UI_FG           BLACK       /* 普通文本色(与 UI_BG 相反) */
#define UI_WARN         0x9B60      /* 提示级文本色: 深琥珀(R=152,G=108,B=0) */

/* ===================== 温湿度传感器类型 ===================== */
#define TH_NONE         0
#define TH_DHT11        1
#define TH_SHT30        2

/* DHT11 判"0/1"靠 delay_us(40) 后读引脚电平, 而 "0" 的高电平只有 26~28us;
 * 115200 下 USART3 每约 87us 进一次中断, 会把采样点推后 -> 偶发 CRC 失败。
 * 所以读单总线器件期间暂停 USART3 中断(详见 esp8266.h)。没开联网时是空操作。
 * 注意只关 USART3, 不动 SysTick, HAL_GetTick()/delay_us() 不受影响。 */
#if APP_ENABLE_NET
#define TH_BUS_LOCK()       esp8266_rx_pause()
#define TH_BUS_UNLOCK()     esp8266_rx_resume()
#else
#define TH_BUS_LOCK()       do { } while (0)
#define TH_BUS_UNLOCK()     do { } while (0)
#endif

/* ===================== 联网状态机 ===================== */
#define NET_ST_START        0
#define NET_ST_AT           1
#define NET_ST_ECHO         2
#define NET_ST_MODE         3
#define NET_ST_JOIN         4
#define NET_ST_TCP          5
#define NET_ST_TRANS        6
#define NET_ST_MQTT         7
#define NET_ST_ONLINE       8
#define NET_ST_RETRYWAIT    9

static uint8_t  g_th_type = TH_NONE;
static uint8_t  g_th_ok = 0;                                /* 最近一次温湿度读取是否有效 */
static uint8_t  g_th_fail = 0;                              /* 连续读取失败次数 */
static uint16_t g_th_period = APP_SAMPLE_PERIOD_MS;         /* 温湿度采样间隔(ms) */

static uint8_t  g_font_title;
static uint8_t  g_font_data;
static uint8_t  g_char_w;
static uint16_t g_row_y[UI_ROW_MAX];

/* 行内容缓存: "内容 + 颜色"都没变就不重写 LCD。
 * 主循环每 500ms 会刷一遍所有行, 而绝大部分时候内容是不变的, 重写纯属浪费
 * FSMC 带宽、并让网络等待期间的服务回调变慢。 */
static char     g_ui_txt[UI_ROW_MAX][UI_LINE_MAX + 2];
static uint16_t g_ui_col[UI_ROW_MAX];
static uint8_t  g_ui_valid[UI_ROW_MAX];
#if APP_ALARM_FLASH
static uint16_t g_ui_bg = UI_BG;            /* 当前整屏底色(报警闪烁时才变成红) */
#endif
#if APP_ENABLE_LOG
static uint8_t  g_log_page = 0;             /* 0 = 主页; 1..N = 报警历史页第几页 */
#endif

/* 行号在 ui_setup() 里按开关动态分配; 关掉联网就不显示 WiFi/Cloud/Sent 三行,
 * 省下的高度全部让给字号。 */
static uint8_t  g_row_title;
static uint8_t  g_row_temp;
static uint8_t  g_row_humi;
static uint8_t  g_row_lux;
static uint8_t  g_row_sensor;
static uint8_t  g_row_alarm;
static uint8_t  g_row_count;
#if APP_ENABLE_NET
static uint8_t  g_row_wifi;
static uint8_t  g_row_cloud;
static uint8_t  g_row_sent;
static uint8_t  g_net_state;
static uint32_t g_net_timer;
static uint8_t  g_transparent;
static uint8_t  g_baud_scanned;     /* 波特率只全扫一次, 之后每轮只用快命令试, 避免拖慢报警/采样 */
static uint16_t g_net_attempts = 0; /* 第几次建链: 第 2 次起先 AT+RST, 把模块从残留状态复位干净 */
static uint16_t g_net_fails    = 0; /* 连续失败次数: 够多就允许重新全扫描波特率(模块可能自己复位过) */
static uint8_t  g_ping_fail    = 0; /* 连续几次心跳没收到 PINGRESP */
#endif

/* ===================== 独立看门狗 ===================== */
/* 24 小时运行的居家监护设备, "死机后不报警"比"报警误响"危险得多。
 * LSI≈40kHz, PR=4(64 分频), RLR=5000 → 超时约 8s。
 * 喂狗点: 主循环每次迭代 + app_poll() 服务回调(网络阻塞等待期间每 20ms 一次)。
 * 也就是说最慢的一步(AT+CWJAP 最长 20s)也在正常喂狗, 只有真正卡死才会复位。 */
#if APP_ENABLE_IWDG
#define IWDG_FEED()     do { IWDG->KR = 0xAAAA; } while (0)

static void iwdg_start(void)
{
    DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;  /* 调试器暂停时冻结看门狗(F1 的 DBGMCU 无需开时钟),
                                             * 否则用调试器单步时会被反复复位 */

    IWDG->KR  = 0x5555;                     /* 解除写保护 */
    IWDG->PR  = 4;                          /* 64 分频 */
    IWDG->RLR = 5000;                       /* 超时约 8s */
    IWDG->KR  = 0xCCCC;                     /* 启动 */
    IWDG->KR  = 0xAAAA;                     /* 立即喂一次 */
}
#else
#define IWDG_FEED()     do { } while (0)
#endif

/* ============================================================================
 *  界面
 * ==========================================================================*/

static const uint8_t g_font_tbl[4] = {32, 24, 16, 12};

static uint8_t font_up(uint8_t f)
{
    if (f <= 12)
    {
        return 16;
    }

    if (f <= 16)
    {
        return 24;
    }

    return 32;
}

/**
 * @brief       由屏幕高度挑一个放得下 body_rows 行正文的字号
 */
static uint8_t font_fit(uint16_t h, uint8_t body_rows)
{
    uint8_t  i;
    uint32_t need;
    uint32_t n = (body_rows > 0) ? (body_rows - 1) : 0;

    for (i = 0; i < 4; i++)
    {
        uint8_t d = g_font_tbl[i];
        uint8_t t = font_up(d);

        need = 5UL + t + 6UL + n * (uint32_t)(d + 6) + d;

        if (need <= h)
        {
            return d;
        }
    }

    return 12;
}

/**
 * @brief       依据屏幕尺寸与显示行数计算字号与各行坐标
 */
static void ui_setup(void)
{
    uint8_t d;
    uint8_t i;
    uint8_t r = 0;

    g_row_title = r++;
#if APP_ENABLE_NET
    g_row_wifi  = r++;
    g_row_cloud = r++;
#endif
    g_row_temp   = r++;
    g_row_humi   = r++;
    g_row_lux    = r++;
#if APP_ENABLE_NET
    g_row_sent   = r++;
#endif
    g_row_sensor = r++;
    g_row_alarm  = r++;
    g_row_count  = r;

    d = font_fit(lcddev.height, (uint8_t)(g_row_count - 1));

    if (d > APP_UI_FONT_DATA)
    {
        d = APP_UI_FONT_DATA;
    }

    g_font_data  = d;
    g_font_title = font_up(d);

    if (g_font_title > APP_UI_FONT_TITLE)
    {
        g_font_title = APP_UI_FONT_TITLE;
    }

    if (g_font_title < g_font_data)
    {
        g_font_title = g_font_data;
    }

    g_char_w = (uint8_t)(g_font_data / 2);

    for (i = 0; i < UI_ROW_MAX; i++)        /* 字号/行坐标变了, 缓存全部作废 */
    {
        g_ui_valid[i]  = 0;
        g_ui_txt[i][0] = '\0';
    }

    g_row_y[g_row_title] = 5;

    for (i = 1; i < g_row_count; i++)
    {
        uint8_t prev_font = (i == g_row_title + 1) ? g_font_title : g_font_data;
        g_row_y[i] = (uint16_t)(g_row_y[i - 1] + prev_font + 6);
    }
}

/**
 * @brief       刷新一行(定宽右补空格, 自动覆盖旧内容, 无闪烁)
 * @note        内容与颜色都没变时直接返回, 不写屏。报警时若底色为红, 文字统一换成
 *              高对比色, 保证红底上依然看得清。
 */
static void ui_show(uint8_t row, const char *s, uint16_t color)
{
    static char buf[UI_LINE_MAX + 2];
    uint8_t  font = (row == g_row_title) ? g_font_title : g_font_data;
    uint8_t  lc;
    uint16_t len = (uint16_t)strlen(s);
    uint16_t draw = color;

    if (row >= UI_ROW_MAX)
    {
        return;
    }

    lc = (uint8_t)((lcddev.width - 2 * UI_X) / (font / 2));

    if (lc > UI_LINE_MAX)
    {
        lc = UI_LINE_MAX;
    }

    if (len > lc)
    {
        len = lc;
    }

    memcpy(buf, s, len);

    while (len < lc)
    {
        buf[len++] = ' ';
    }

    buf[len] = '\0';

    if ((g_ui_valid[row] != 0) && (g_ui_col[row] == color) && (strcmp(g_ui_txt[row], buf) == 0))
    {
        return;                                     /* 没变化: 不碰 LCD */
    }

    memcpy(g_ui_txt[row], buf, (uint16_t)(lc + 1));
    g_ui_col[row]   = color;
    g_ui_valid[row] = 1;

#if APP_ENABLE_LOG
    if (g_log_page != 0)
    {
        return;                                     /* 正在看历史页: 只更新缓存, 不画主页
                                                     * (返回主页时由 ui_redraw_all() 统一重绘) */
    }
#endif

#if APP_ALARM_FLASH
    if (g_ui_bg == RED)
    {
        draw = (row == g_row_alarm) ? YELLOW : WHITE;
    }
#endif

    lcd_show_string(UI_X, g_row_y[row], lcddev.width, font, font, buf, draw);
}

/**
 * @brief       按当前底色重绘所有写过的行
 * @note        两处用到: 报警闪烁切换底色时, 以及从报警历史页返回主页时。
 */
static void ui_redraw_all(void)
{
    uint8_t r;

    for (r = 0; r < g_row_count; r++)
    {
        uint8_t  font;
        uint16_t color;

        if (g_ui_valid[r] == 0)
        {
            continue;
        }

        font  = (r == g_row_title) ? g_font_title : g_font_data;
        color = g_ui_col[r];

#if APP_ALARM_FLASH
        if (g_ui_bg == RED)
        {
            color = (r == g_row_alarm) ? YELLOW : WHITE;
        }
#endif

        lcd_show_string(UI_X, g_row_y[r], lcddev.width, font, font, g_ui_txt[r], color);
    }
}

#if APP_ALARM_FLASH
/**
 * @brief       报警时的整屏闪烁(每 20ms 调一次, 主循环和网络等待回调都调)
 * @note        老人听力衰退是常态, 光靠蜂鸣器不够。这里只改底色 + 重绘文本,
 *              报警时序、阈值、蜂鸣器行为、消音逻辑一律不碰。
 *              报警开始: 红底白字 <-> 白底彩字 交替; 消音或恢复正常: 自动回到白底。
 */
static void ui_flash_task(void)
{
    static uint32_t t_flash = 0;
    uint32_t now = HAL_GetTick();

    if (alarm_is_beeping())
    {
        if ((now - t_flash) >= APP_ALARM_FLASH_MS)
        {
            t_flash = now;
            g_ui_bg = (g_ui_bg == RED) ? UI_BG : RED;

            lcd_clear(g_ui_bg);
            ui_redraw_all();
        }
    }
    else if (g_ui_bg != UI_BG)                      /* 报警结束: 恢复白底正常配色 */
    {
        g_ui_bg = UI_BG;
        t_flash = now;

        lcd_clear(UI_BG);
        ui_redraw_all();
    }
}
#else
#define ui_flash_task()     do { } while (0)
#endif /* APP_ALARM_FLASH */

/**
 * @brief       数值行的颜色: 危险区红 / 舒适区外琥珀 / 正常蓝
 */
static uint16_t ui_level_color(uint8_t bit, uint8_t faults, uint8_t warns)
{
    if ((faults & bit) != 0)
    {
        return RED;
    }

    if ((warns & bit) != 0)
    {
        return UI_WARN;
    }

    return BLUE;
}

/**
 * @brief       刷新最下面那行报警状态
 * @note        主循环和"网络等待期间的服务回调"都会调用它, 后者每 20ms 就可能进来一次;
 *              去重已在 ui_show() 内部完成, 这里只管算内容和颜色。
 */
static void ui_refresh_alarm_row(void)
{
    char     buf[UI_LINE_MAX + 2];
    uint8_t  faults = alarm_get_faults();
    uint8_t  warns  = alarm_get_warns();
    uint16_t color;

    snprintf(buf, sizeof(buf), "Alarm : %s", alarm_reason());

    if ((faults == 0) && (warns == 0))
    {
        color = BLUE;                                   /* 舒适区内 */
    }
    else if (alarm_is_beeping())
    {
        color = RED;                                    /* 危险区, 正在响 */
    }
    else if (faults != 0)
    {
        color = UI_FG;                                  /* 危险区但已消音 / 间歇期 */
    }
    else
    {
        color = UI_WARN;                                /* 只是超出舒适区, 不响 */
    }

    ui_show(g_row_alarm, buf, color);
}

/* ============================================================================
 *  报警历史页(KEY1 进入 / 翻页, KEY0 返回)
 * ==========================================================================*/
#if APP_ENABLE_LOG

static uint8_t g_log_font = 16;
static uint8_t g_log_per  = 6;      /* 每页条数(由 log_setup() 按屏幕算) */

/**
 * @brief       每页条数
 */
static uint8_t log_page_count(void)
{
    uint8_t total = app_log_count();
    uint8_t per   = (g_log_per != 0) ? g_log_per : 1;

    if (total == 0)
    {
        return 1;
    }

    return (uint8_t)((total + per - 1) / per);
}

/**
 * @brief       按屏幕宽度挑一个放得下一整行记录的较大字号
 * @note        一行记录形如 "09-22 14:31 35.2C TEMP+HUMI", 约 30 个字符,
 *              所以字号要满足 (宽-2*边距)/(字号/2) >= 30。3.5" 屏能用 24 号, 2.8" 屏只能 16 号。
 */
static uint8_t log_font_fit(void)
{
    static const uint8_t tbl[2] = { 24, 16 };
    uint8_t i;

    for (i = 0; i < 2; i++)
    {
        if ((uint16_t)((lcddev.width - 2 * UI_X) / (tbl[i] / 2)) >= 30U)
        {
            return tbl[i];
        }
    }

    return 16;
}

/**
 * @brief       按屏幕高度算字号与每页条数
 */
static void log_setup(void)
{
    uint8_t  tf;
    uint16_t avail;

    g_log_font = log_font_fit();
    tf         = font_up(g_log_font);

    /* 顶部标题一行 + 底部提示一行, 剩下给记录 */
    avail = (lcddev.height > (uint16_t)(5 + tf + 6 + g_log_font + 6)) ?
            (uint16_t)(lcddev.height - (5 + tf + 6 + g_log_font + 6)) : (uint16_t)(g_log_font + 6);

    g_log_per = (uint8_t)(avail / (g_log_font + 6));

    if (g_log_per < 3)
    {
        g_log_per = 3;
    }

    if (g_log_per > 14)
    {
        g_log_per = 14;
    }
}

/**
 * @brief       把第 idx 条记录(idx=0 最新)渲染成一行文本
 */
static void log_line(uint8_t idx, char *dst, uint16_t dstsz)
{
    const app_log_item_t *it = app_log_get(idx);
    char     f[24];
    int16_t  t;
    uint16_t a;

    if (it == NULL)
    {
        snprintf(dst, dstsz, "--  <bad record>");
        return;
    }

    app_log_fault_str(it->faults, f, sizeof(f));

    t = it->temp_x10;
    a = (uint16_t)((t < 0) ? -t : t);

    snprintf(dst, dstsz, "%02u-%02u %02u:%02u %s%u.%uC %s",
             (unsigned)it->month, (unsigned)it->date, (unsigned)it->hour, (unsigned)it->min,
             (t < 0) ? "-" : "", (unsigned)(a / 10U), (unsigned)(a % 10U), f);
}

/**
 * @brief       画整个历史页
 */
static void log_page_show(void)
{
    uint8_t total = app_log_count();
    uint8_t pages = log_page_count();
    uint8_t p;
    uint8_t i;
    uint8_t tf = font_up(g_log_font);
    char    line[UI_LINE_MAX + 2];

    if (g_log_page == 0)
    {
        g_log_page = 1;
    }

    if (g_log_page > pages)
    {
        g_log_page = pages;
    }

    p = (uint8_t)(g_log_page - 1);

    lcd_clear(UI_BG);

    snprintf(line, sizeof(line), "Alarm Log %u/%u   total %u",
             (unsigned)g_log_page, (unsigned)pages, (unsigned)total);
    lcd_show_string(UI_X, 5, lcddev.width, tf, tf, line, BLUE);

    if (total == 0)
    {
        lcd_show_string(UI_X, (uint16_t)(5 + tf + 6), lcddev.width, g_log_font, g_log_font,
                        "no alarm recorded yet", UI_FG);
    }
    else
    {
        for (i = 0; i < g_log_per; i++)
        {
            uint8_t  idx = (uint8_t)(p * g_log_per + i);
            uint16_t y   = (uint16_t)(5 + tf + 6 + (uint16_t)i * (g_log_font + 6));

            if (idx >= total)
            {
                break;
            }

            log_line(idx, line, sizeof(line));

            /* 最新的一条用红色, 其余用普通文本色 */
            lcd_show_string(UI_X, y, lcddev.width, g_log_font, g_log_font, line,
                            (idx == 0) ? RED : UI_FG);
        }
    }

    /* 底部操作提示 */
    lcd_show_string(UI_X, (uint16_t)(lcddev.height - g_log_font - 2), lcddev.width,
                    g_log_font, g_log_font, "KEY1=next   KEY0=back", GRAY);
}

/**
 * @brief       进入历史页(顺便把全部记录打到串口)
 */
static void log_page_open(void)
{
    if (alarm_is_beeping())
    {
        return;                             /* 报警优先: 报警中不允许翻页 */
    }

    log_setup();
    g_log_page = 1;

    printf("\r\nKEY1 pressed -> show alarm log\r\n");
    app_log_dump();                         /* 串口也输出一份完整记录 */

    log_page_show();
}

/**
 * @brief       返回主页(用行缓存把主页内容重画回去, 不需要重新采样)
 */
static void log_page_close(void)
{
    if (g_log_page == 0)
    {
        return;
    }

    g_log_page = 0;

    lcd_clear(UI_BG);
    ui_redraw_all();
}

#endif /* APP_ENABLE_LOG */

/**
 * @brief       每 20ms 的公共任务: 按键 + 报警 + 报警闪烁 + 报警行
 * @note        主循环和"网络等待期间的服务回调"都调它, 所以联网阻塞时 KEY0/KEY1
 *              依然有效(否则 AT+CWJAP 那 20 秒里按什么都没反应)。
 */
static void app_periodic(void)
{
    uint8_t k = key_scan();

    if (k != 0)
    {
        printf("KEY  : %s%s\r\n", (k & KEY0_PRES) ? "KEY0 " : "", (k & KEY1_PRES) ? "KEY1" : "");
    }

#if APP_ENABLE_LOG
    /* ---- 1. KEY0 第一优先级: 报警消音(任何页面下都生效) ---- */
    if ((k & KEY0_PRES) != 0)
    {
        if (alarm_key_mute() != 0)
        {
            k &= (uint8_t)~KEY0_PRES;       /* 消音成功, 本次按键不再做别的 */
        }
    }

    /* ---- 2. KEY1: 主页进入历史页 / 历史页翻页 / 翻到底回主页 ---- */
    if ((k & KEY1_PRES) != 0)
    {
        if (g_log_page == 0)
        {
            log_page_open();
        }
        else if (g_log_page >= log_page_count())
        {
            log_page_close();
        }
        else
        {
            g_log_page++;
            log_page_show();
        }

        k &= (uint8_t)~KEY1_PRES;
    }

    /* ---- 3. KEY0(没在报警时): 历史页里当"返回"用 ---- */
    if (((k & KEY0_PRES) != 0) && (g_log_page != 0))
    {
        log_page_close();
    }
#else
    if ((k & KEY0_PRES) != 0)
    {
        (void)alarm_key_mute();
    }
#endif

    alarm_task();

#if APP_ENABLE_LOG
    /* 报警一响就强制回到主页 —— 监护设备的报警界面永远优先于用户正在看的页面 */
    if ((g_log_page != 0) && alarm_is_beeping())
    {
        log_page_close();
    }
#endif

    ui_flash_task();                        /* 报警时整屏闪烁 */
    ui_refresh_alarm_row();
}

#if APP_ENABLE_NET
/**
 * @brief       网络等待期间的服务回调(注册给 ESP8266 驱动)
 * @note        进入联网后, AT+CWJAP 最长等 20s、AT+CIPSTART 最长等 15s, 这期间主循环是停住的。
 *              没有这个回调时: 蜂鸣器节拍会冻结、KEY0 消音会漏掉、屏幕状态行不更新。
 *              有了它, 报警/消音/翻页/看门狗在网络忙时照常工作。
 */
static void app_poll(void)
{
    IWDG_FEED();
    app_periodic();
}
#endif

/**
 * @brief       把 0.1 单位的值格式化成 "标签-12.3 单位"
 */
static void ui_fmt_x10(char *buf, uint16_t bufsz, const char *label, int16_t v, const char *unit)
{
    if (v < 0)
    {
        snprintf(buf, bufsz, "%s-%u.%u %s", label,
                 (unsigned)((-v) / 10), (unsigned)((-v) % 10), unit);
    }
    else
    {
        snprintf(buf, bufsz, "%s%u.%u %s", label,
                 (unsigned)(v / 10), (unsigned)(v % 10), unit);
    }
}

/* ============================================================================
 *  温湿度传感器
 * ==========================================================================*/

static uint8_t th_detect(void)
{
#if (APP_SENSOR_TH == 1)
    return (sht30_init() == SHT30_OK) ? TH_SHT30 : TH_NONE;
#elif (APP_SENSOR_TH == 0)
    uint8_t r;

    TH_BUS_LOCK();                              /* DHT11 时序敏感, 期间不收串口 */
    r = (dht11_init() == 0) ? TH_DHT11 : TH_NONE;
    TH_BUS_UNLOCK();

    return r;
#else
    uint8_t r;

    if (sht30_init() == SHT30_OK)
    {
        return TH_SHT30;
    }

    TH_BUS_LOCK();                              /* 软件 I2C 由主机控时钟, 不用锁 */
    r = (dht11_init() == 0) ? TH_DHT11 : TH_NONE;
    TH_BUS_UNLOCK();

    return r;
#endif
}

static uint8_t th_read(int16_t *t_x10, uint16_t *h_x10)
{
    uint8_t t8, h8;
    uint8_t r;

    if (g_th_type == TH_SHT30)
    {
        return sht30_read(t_x10, h_x10);
    }

    if (g_th_type == TH_DHT11)
    {
        /* 官方驱动的 CRC 校验失败时不赋值, 却又返回"成功", 这里用哨兵值识别出来,
         * 并把 ACK 失败与 CRC 失败区分开打印, 便于接线排错。 */
        t8 = 0xEE;
        h8 = 0xEE;

        /* ★ 整个读取过程约 25ms, 必须独占时序: 否则每 87us 一次的 USART3 中断
         *   会把 delay_us(40) 后的采样点推后, 把 "0"(高电平 26~28us) 读成 "1"。 */
        TH_BUS_LOCK();
        r = dht11_read_data(&t8, &h8);
        TH_BUS_UNLOCK();

        if (r)
        {
            printf("DHT11: no ACK (check DQ/PG11 wiring & 4.7K pull-up)\r\n");
            return 1;
        }

        if ((t8 == 0xEE) || (h8 == 0xEE))
        {
            printf("DHT11: CRC fail (data line too slow/noisy)\r\n");
            return 1;
        }

        *t_x10 = (int16_t)((int16_t)t8 * 10);
        *h_x10 = (uint16_t)((uint16_t)h8 * 10U);

        return 0;
    }

    return 2;
}

static const char *th_name(void)
{
    if (g_th_type == TH_SHT30)
    {
        return "SHT30";
    }

    if (g_th_type == TH_DHT11)
    {
        return "DHT11";
    }

    return "TH";
}

/* ============================================================================
 *  联网(可选)
 * ==========================================================================*/
#if APP_ENABLE_NET

static void net_fail(const char *msg)
{
    printf("NET FAIL: %s (resp: %s)\r\n", msg, esp8266_get_resp());

    ui_show(g_row_wifi, msg, RED);
    LED0(1);

    if (g_net_state >= NET_ST_TCP)
    {
        if (g_transparent)
        {
            esp8266_exit_transparent();
            g_transparent = 0;
        }

        esp8266_cmd("AT+CIPCLOSE", "OK", 2000);
    }

    /* 连续失败够多次 → 允许重新全扫描波特率。
     * 模块若因为供电抖动/自己看门狗复位, 波特率可能回到默认值, 而 g_baud_scanned
     * 一旦置 1 就再也不会重扫, 那种情况下会永久连不上。 */
    if (++g_net_fails >= APP_NET_FAIL_RESCAN)
    {
        g_net_fails    = 0;
        g_baud_scanned = 0;
        printf("NET: rescan baudrate on next try\r\n");
    }

    mqtt_link_clear_closed();
    g_ping_fail = 0;

    g_net_timer = HAL_GetTick();
    g_net_state = NET_ST_RETRYWAIT;
}

static void net_step(void)
{
    char line[64];

    if (g_net_state == NET_ST_ONLINE)
    {
        return;
    }

    switch (g_net_state)
    {
        case NET_ST_START:
            if (g_net_attempts > 0)
            {
                /* 重连: 模块可能停在透传 / 多连接 / 半开连接等残留状态,
                 * 直接重发 AT 命令经常一直失败, 先软复位到干净状态。 */
                ui_show(g_row_wifi, "WiFi  : reset ...", UI_FG);
                printf("NET: reset ESP8266 before retry\r\n");

                esp8266_reset();
                (void)esp8266_set_baud(115200);     /* RST 后波特率回到模块默认 */
                esp8266_clear();
                g_baud_scanned = 0;                 /* 波特率可能变了, 允许重扫 */
                mqtt_link_clear_closed();
            }

            g_net_attempts++;

            esp8266_init();
            ui_show(g_row_wifi, "WiFi  : AT ...", UI_FG);
            g_net_state = NET_ST_AT;
            break;

        case NET_ST_AT:
        {
            uint8_t ok;

            if (g_baud_scanned)
            {
                ok = (esp8266_at() == ESP8266_STAT_OK) ? 1 : 0;     /* 已探测过: 直接用当前波特率快试 */
            }
            else
            {
                uint16_t rx_bytes = 0;
                uint32_t baud = esp8266_scan_baud(&rx_bytes);

                g_baud_scanned = 1;

                if (baud == 0)
                {
                    /* 按"有没有收到过字节"给出可执行的结论, 而不是笼统的 no module */
                    if (rx_bytes > 0)
                    {
                        net_fail("WiFi  : RX ok,no AT");            /* 接线通, 模块不是 AT 固件/被占用 */
                    }
                    else
                    {
                        net_fail("WiFi  : no resp");                /* 一个字节都没有: 供电/接线/波特率 */
                    }

                    break;
                }

                ok = 1;
            }

            if (ok)
            {
                g_net_state = NET_ST_ECHO;
            }
            else
            {
                net_fail("WiFi  : no module");
            }

            break;
        }

        case NET_ST_ECHO:
            esp8266_echo_off();
            g_net_state = NET_ST_MODE;
            break;

        case NET_ST_MODE:
            esp8266_set_mode(1);
            snprintf(line, sizeof(line), "WiFi  : join %s", APP_WIFI_SSID);
            ui_show(g_row_wifi, line, UI_FG);
            g_net_state = NET_ST_JOIN;
            break;

        case NET_ST_JOIN:
            printf("WiFi joining [%s] ...\r\n", APP_WIFI_SSID);

            if (esp8266_join_ap(APP_WIFI_SSID, APP_WIFI_PWD) == ESP8266_STAT_OK)
            {
                ui_show(g_row_wifi, "WiFi  : OK", BLUE);
                printf("WiFi OK\r\n");
                g_net_state = NET_ST_TCP;
            }
            else
            {
                net_fail("WiFi  : join FAIL");
            }

            break;

        case NET_ST_TCP:
            ui_show(g_row_cloud, "Cloud : TCP ...", UI_FG);

            if (esp8266_open_tcp(ONENET_HOST, ONENET_PORT) == ESP8266_STAT_OK)
            {
                g_net_state = NET_ST_TRANS;
            }
            else
            {
                net_fail("Cloud : TCP FAIL");
            }

            break;

        case NET_ST_TRANS:
            if (esp8266_enter_transparent() == ESP8266_STAT_OK)
            {
                g_transparent = 1;
                g_net_state = NET_ST_MQTT;
            }
            else
            {
                net_fail("Cloud : transparent FAIL");
            }

            break;

        case NET_ST_MQTT:
            ui_show(g_row_cloud, "Cloud : MQTT ...", UI_FG);

            if (onenet_connect() == MQTT_STAT_OK)
            {
                ui_show(g_row_cloud, "Cloud : online", BLUE);
                printf("MQTT connected to OneNET\r\n");
                LED0(0);
                g_net_fails = 0;
                g_ping_fail = 0;
                g_net_state = NET_ST_ONLINE;
            }
            else
            {
                net_fail("Cloud : MQTT FAIL");
            }

            break;

        case NET_ST_RETRYWAIT:
        default:
            if ((HAL_GetTick() - g_net_timer) >= APP_NET_RETRY_MS)
            {
                ui_show(g_row_wifi, "WiFi  : retry ...", UI_FG);
                ui_show(g_row_cloud, "Cloud : ---", UI_FG);
                g_net_state = NET_ST_START;
            }

            break;
    }
}

#endif /* APP_ENABLE_NET */

/* ============================================================================
 *  主入口
 * ==========================================================================*/

/**
 * @brief       应用主入口(main.c 中调用)
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    int16_t  t_x10 = 0;
    uint16_t h_x10 = 0;
    uint8_t  lux = 0;
    uint32_t lux_cdm2 = 0;
    uint8_t  faults;
    uint8_t  warns;
    uint8_t  th_ok;
    uint32_t t_sample;
    uint32_t t_th;
    uint32_t now;
    char     line[64];
#if APP_ENABLE_NET
    uint16_t msg_id = 0;
    uint32_t sent = 0;
    uint32_t t_report;
    uint32_t t_ping;
#endif

    delay_init(72);
    led_init();
    alarm_init();                                       /* 蜂鸣器 + KEY0 */

    printf("\r\n");
    printf("==================================================\r\n");
    printf("  atk_f103 : TH + LSENS -> ALARM -> LCD");
#if APP_ENABLE_NET
    printf(" -> ESP8266 -> OneNET");
#endif
    printf("\r\n");
    printf("  MCU  : STM32F103ZET6 @72MHz (STM32CubeMX + HAL)\r\n");
    printf("  NET  : %s\r\n", APP_ENABLE_NET ? "enabled" : "disabled");
    printf("  COMFORT: temp %d.%d-%d.%d C, humi %u.%u-%u.%u %%, lux %u-%u cd/m2 (yellow only)\r\n",
           APP_COMFORT_TEMP_MIN_X10 / 10, APP_COMFORT_TEMP_MIN_X10 % 10,
           APP_COMFORT_TEMP_MAX_X10 / 10, APP_COMFORT_TEMP_MAX_X10 % 10,
           (unsigned)(APP_COMFORT_HUMI_MIN_X10 / 10), (unsigned)(APP_COMFORT_HUMI_MIN_X10 % 10),
           (unsigned)(APP_COMFORT_HUMI_MAX_X10 / 10), (unsigned)(APP_COMFORT_HUMI_MAX_X10 % 10),
           (unsigned)APP_COMFORT_LUX_MIN_CDM2, (unsigned)APP_COMFORT_LUX_MAX_CDM2);
    printf("  ALARM  : temp %d.%d-%d.%d C, humi %u.%u-%u.%u %%, lux max %u cd/m2 (beep+flash)\r\n",
           APP_TEMP_MIN_X10 / 10, APP_TEMP_MIN_X10 % 10,
           APP_TEMP_MAX_X10 / 10, APP_TEMP_MAX_X10 % 10,
           (unsigned)(APP_HUMI_MIN_X10 / 10), (unsigned)(APP_HUMI_MIN_X10 % 10),
           (unsigned)(APP_HUMI_MAX_X10 / 10), (unsigned)(APP_HUMI_MAX_X10 % 10),
           (unsigned)APP_LUX_MAX_CDM2);
    printf("  > KEY0 = mute alarm");
#if APP_ENABLE_LOG
    printf("   KEY1 = alarm history (KEY0 = back)");
#endif
    printf("\r\n");
    printf("==================================================\r\n");

    /* ---------------- LCD 初始化 ---------------- */
    lcd_init();
    lcd_display_dir(1);
    lcd_clear(UI_BG);
    ui_setup();

    printf("LCD  : id=%x  %ux%u  font=%u/%u\r\n",
           lcddev.id, lcddev.width, lcddev.height, g_font_title, g_font_data);

    ui_show(g_row_title, (lcddev.width >= 480) ? "atk_f103 Environment Monitor" : "atk_f103", BLUE);
    ui_show(g_row_temp,  "Temp  : --.- C", RED);
    ui_show(g_row_humi,  "Humi  : --.- %", RED);
    ui_show(g_row_lux,   "Light : ---- cd/m2", RED);
    ui_show(g_row_alarm, "Alarm : normal", BLUE);

#if APP_ENABLE_NET
    ui_show(g_row_wifi,  "WiFi  : init ...", UI_FG);
    ui_show(g_row_cloud, "Cloud : ---", UI_FG);
    ui_show(g_row_sent,  "Sent  : 0", UI_FG);
    g_net_state   = NET_ST_START;
    g_transparent = 0;
#endif

    /* ---------------- 温湿度传感器自动探测 ---------------- */
    g_th_type = th_detect();

    if (g_th_type == TH_NONE)
    {
        ui_show(g_row_sensor, "TH    : not found", RED);
        printf("TH   : no sensor found (SHT30@PB6/PB7 or DHT11@PG11)\r\n");
    }
    else
    {
        snprintf(line, sizeof(line), "%s : OK", th_name());
        ui_show(g_row_sensor, line, BLUE);
        printf("TH   : %s detected\r\n", th_name());
    }

    if (g_th_type != TH_DHT11)
    {
        HAL_GPIO_WritePin(DHT11_DQ_GPIO_Port, DHT11_DQ_Pin, GPIO_PIN_SET);
    }

    /* DHT11 手册要求采样间隔 ≥1s, 单独给它一个慢节拍; SHT30 可以跟着刷屏周期走 */
    g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
    g_th_ok = 0;
    g_th_fail = 0;

#if APP_ENABLE_LOG
    /* ---------------- 报警历史: RTC 时间戳 + 内部 FLASH 存储 ---------------- */
    {
        uint8_t rtc_ret = rtc_init();

        app_log_init();

        /* 报警一开始就通过这个回调把快照存进去(app_alarm 与 app_log 之间没有直接依赖) */
        alarm_set_log_hook(app_log_add);

        printf("RTC  : %s  now %04u-%02u-%02u %02u:%02u:%02u  (clock %s)\r\n",
               (rtc_ret == 0) ? "ok" : "INIT FAILED",
               calendar.year, calendar.month, calendar.date,
               calendar.hour, calendar.min, calendar.sec,
               (rtc_is_set() != 0) ? "set, battery-backed" : "from build time, not battery-backed");

        printf("LOG  : type KEY1 to browse alarm history (KEY0 = back)\r\n");
    }
#endif

    lsens_init();

    t_sample = HAL_GetTick();
    t_th = t_sample;
#if APP_ENABLE_NET
    t_report = t_sample;
    t_ping   = t_sample;

    esp8266_set_poll_hook(app_poll);        /* 联网等待期间继续跑: 报警 / 消音 / 状态行 / 喂狗 */
#endif

#if APP_ENABLE_IWDG
    iwdg_start();                           /* 放在主循环前启动, 之后只有真卡死才会复位 */
#endif

    /* ---------------- 主循环: 20ms 节拍 ---------------- */
    while (1)
    {
        now = HAL_GetTick();

        /* ========== 20ms 任务: 按键 + 报警(含 KEY0 消音) + 报警闪烁 + 联网状态机 ========== */
        app_periodic();
        IWDG_FEED();

#if APP_ENABLE_NET
        net_step();

        if (g_net_state == NET_ST_ONLINE)
        {
            /* ★ 上线后必须持续排空接收缓冲(P0)。
             *   不排空的后果: 768 字节环形缓冲只涨不落, 满了以后模块发来的
             *   CLOSED / PINGRESP 全被静默丢弃 —— 云端断了、模块掉线了,
             *   本地却一直显示 "Cloud : online" 并且 Sent 还在涨。
             *   这种"静默失效"对监护老人的设备是最危险的故障, 所以每轮都排空。 */
            mqtt_poll_rx();

            if (mqtt_link_closed())
            {
                mqtt_link_clear_closed();
                printf("ESP8266: TCP closed (peer or WiFi down)\r\n");
                g_transparent = 0;
                net_fail("Cloud : link lost");
                delay_ms(20);
                continue;
            }
        }
#endif

        /* ========== 采样周期任务: 采集 + 显示 + 上报 ========== */
        if ((now - t_sample) >= APP_SAMPLE_PERIOD_MS)
        {
            t_sample = now;

            /* 1. 采集温湿度: 按 g_th_period 的节拍走(DHT11 需 ≥1s, 不做连发重试) */
            if (g_th_type == TH_NONE)
            {
                /* 没探到传感器: 每隔 2s 重新探测一次, 不永久锁死 */
                if ((now - t_th) >= APP_DHT11_PERIOD_MS)
                {
                    t_th = now;
                    g_th_type = th_detect();
                    g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
                }
            }
            else if ((now - t_th) >= g_th_period)
            {
                t_th = now;

                if (th_read(&t_x10, &h_x10) == 0)
                {
                    g_th_ok = 1;
                    g_th_fail = 0;
                }
                else
                {
                    g_th_ok = 0;

                    /* 连续失败 → 重新探测(实验29 用 while(dht11_init()) 就是这个意思) */
                    if (++g_th_fail >= APP_TH_RETRY)
                    {
                        g_th_fail = 0;
                        g_th_type = th_detect();
                        g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
                    }
                }
            }

            th_ok = g_th_ok;

            /* 2. 采集光照, 并把 0~100 的相对值换算成 cd/m2 估计值 */
            lux = lsens_get_val();
            lux_cdm2 = (uint32_t)lux * (uint32_t)APP_LUX_FULL_SCALE_CDM2 / 100U;

            /* 3. 喂给报警模块(持续确认窗 + 迟滞, 触发阈值见 app_config.h) */
            alarm_feed(t_x10, h_x10, lux_cdm2, th_ok);

            faults = alarm_get_faults();
            warns  = alarm_get_warns();

            /* 4. 本地显示: 危险区红 / 舒适区外黄 / 正常蓝 */
            if (th_ok != 0)
            {
                ui_fmt_x10(line, sizeof(line), "Temp  : ", t_x10, "C");
                ui_show(g_row_temp, line, ui_level_color(ALARM_FAULT_TEMP, faults, warns));

                ui_fmt_x10(line, sizeof(line), "Humi  : ", (int16_t)h_x10, "%");
                ui_show(g_row_humi, line, ui_level_color(ALARM_FAULT_HUMI, faults, warns));

                printf("DATA %s temp=%d.%d humi=%u.%u lsens=%u cdm2=%u alarm=%s%s\r\n",
                       th_name(),
                       (int)(t_x10 / 10), (int)(t_x10 % 10),
                       (unsigned)(h_x10 / 10), (unsigned)(h_x10 % 10),
                       (unsigned)lux, (unsigned)lux_cdm2, alarm_reason(),
                       alarm_is_beeping() ? " BEEP" : "");
            }
            else
            {
                ui_show(g_row_temp, "Temp  : --.- C", GRAY);
                ui_show(g_row_humi, "Humi  : --.- %", GRAY);

                printf("DATA %s read fail (连续第%u次) lsens=%u cdm2=%u alarm=%s%s\r\n",
                       th_name(), (unsigned)g_th_fail, (unsigned)lux, (unsigned)lux_cdm2,
                       alarm_reason(), alarm_is_beeping() ? " BEEP" : "");
            }

            snprintf(line, sizeof(line), "Light : %u cd/m2", (unsigned)lux_cdm2);
            ui_show(g_row_lux, line, ui_level_color(ALARM_FAULT_LUX, faults, warns));

            ui_refresh_alarm_row();                     /* 蓝=舒适区内 / 黄=仅提示 / 红=正在响 / 黑=已消音 */

            if (g_th_type != TH_NONE)
            {
                snprintf(line, sizeof(line), "%s : %s", th_name(), th_ok ? "OK" : "read fail");
                ui_show(g_row_sensor, line, th_ok ? BLUE : RED);
            }

#if APP_ENABLE_NET
            /* 5. 周期上报 OneNET */
            if (g_net_state == NET_ST_ONLINE)
            {
                if ((now - t_report) >= APP_REPORT_PERIOD_MS)
                {
                    t_report = now;
                    msg_id++;

                    if (onenet_publish(msg_id, t_x10, h_x10, lux) != MQTT_STAT_OK)
                    {
                        printf("publish FAIL, will reconnect\r\n");
                        g_transparent = 0;
                        net_fail("Cloud : send FAIL");
                    }
                    else
                    {
                        sent++;
                        snprintf(line, sizeof(line), "Sent  : %u", (unsigned)sent);
                        ui_show(g_row_sent, line, UI_FG);
                    }
                }

                /* 6. MQTT 心跳保活 + 链路活性判定(全工程唯一能发现"静默断线"的地方) */
                if ((now - t_ping) >= APP_PING_PERIOD_MS)
                {
                    t_ping = now;

                    if (mqtt_ping() == MQTT_STAT_OK)
                    {
                        g_ping_fail = 0;
                    }
                    else if (++g_ping_fail >= APP_PING_FAIL_MAX)
                    {
                        g_ping_fail = 0;
                        printf("MQTT: link lost (%u pings no answer), reconnect\r\n",
                               (unsigned)APP_PING_FAIL_MAX);
                        g_transparent = 0;              /* TCP 多半已断, 不必再发 +++ */
                        net_fail("Cloud : link lost");
                    }
                    else
                    {
                        printf("MQTT: ping no answer (%u/%u)\r\n",
                               (unsigned)g_ping_fail, (unsigned)APP_PING_FAIL_MAX);
                    }
                }
            }
#endif /* APP_ENABLE_NET */

            LED1_TOGGLE();
        }

        delay_ms(20);
    }
}
