/**
 ****************************************************************************************************
 * @file        app_ui.c
 * @brief       LCD 界面显示模块(从 app_main.c 提取)
 ****************************************************************************************************
 */

#include "./App/app_ui.h"
#include "./App/app_config.h"
#include "./App/app_alarm.h"
#include "./App/app_log.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/ESP8266/esp8266.h"
#include <stdio.h>
#include <string.h>

/* ===================== 内部状态 ===================== */
static uint8_t  g_char_w;
static uint16_t g_row_y[UI_ROW_MAX];

/* 行内容缓存: 内容+颜色没变就不重写 LCD */
static char     g_ui_txt[UI_ROW_MAX][UI_LINE_MAX + 2];
static uint16_t g_ui_col[UI_ROW_MAX];
static uint8_t  g_ui_valid[UI_ROW_MAX];

#if APP_ALARM_FLASH
static uint16_t g_ui_bg = UI_BG;            /* 当前整屏底色(报警闪烁时才变成红) */
#endif

/* ===================== 行号定义(非 static) ===================== */
uint8_t  g_row_title;
uint8_t  g_row_temp;
uint8_t  g_row_humi;
uint8_t  g_row_lux;
uint8_t  g_row_sensor;
uint8_t  g_row_alarm;
uint8_t  g_row_count;
#if APP_ENABLE_NET
uint8_t  g_row_wifi;
uint8_t  g_row_cloud;
uint8_t  g_row_sent;
#endif

/* ===================== 字号(非 static) ===================== */
uint8_t  g_font_title;
uint8_t  g_font_data;

/* ===================== 报警历史页状态 ===================== */
#if APP_ENABLE_LOG
uint8_t  g_log_page = 0;
static uint8_t  g_log_font = 16;
static uint8_t  g_log_per  = 6;
#endif

/* ===================== 字号辅助 ===================== */
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

/* ===================== UI 初始化 ===================== */

void ui_setup(void)
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

    for (i = 0; i < UI_ROW_MAX; i++)
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

/* ===================== 行显示 ===================== */

void ui_show(uint8_t row, const char *s, uint16_t color)
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
        return;
    }

    memcpy(g_ui_txt[row], buf, (uint16_t)(lc + 1));
    g_ui_col[row]   = color;
    g_ui_valid[row] = 1;

#if APP_ENABLE_LOG
    if (g_log_page != 0)
    {
        return;
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

/* ===================== 全部重绘 ===================== */

void ui_redraw_all(void)
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

/* ===================== 报警闪烁 ===================== */

#if APP_ALARM_FLASH
void ui_flash_task(void)
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
    else if (g_ui_bg != UI_BG)
    {
        g_ui_bg = UI_BG;
        t_flash = now;

        lcd_clear(UI_BG);
        ui_redraw_all();
    }
}
#else
void ui_flash_task(void) { }
#endif /* APP_ALARM_FLASH */

/* ===================== 报警行颜色 ===================== */

uint16_t ui_level_color(uint8_t bit, uint8_t faults, uint8_t warns)
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

/* ===================== 报警行刷新 ===================== */

void ui_refresh_alarm_row(void)
{
    char     buf[UI_LINE_MAX + 2];
    uint8_t  faults = alarm_get_faults();
    uint8_t  warns  = alarm_get_warns();
    uint16_t color;

    snprintf(buf, sizeof(buf), "Alarm : %s", alarm_reason());

    if ((faults == 0) && (warns == 0))
    {
        color = BLUE;
    }
    else if (alarm_is_beeping())
    {
        color = RED;
    }
    else if (faults != 0)
    {
        color = UI_FG;
    }
    else
    {
        color = UI_WARN;
    }

    ui_show(g_row_alarm, buf, color);
}

/* ===================== 格式化 0.1 单位值 ===================== */

void ui_fmt_x10(char *buf, uint16_t bufsz, const char *label, int16_t v, const char *unit)
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

/* ===================== 报警历史页 ===================== */
#if APP_ENABLE_LOG

static uint8_t log_page_count_internal(void)
{
    uint8_t total = app_log_count();
    uint8_t per   = (g_log_per != 0) ? g_log_per : 1;

    if (total == 0)
    {
        return 1;
    }

    return (uint8_t)((total + per - 1) / per);
}

uint8_t log_page_count(void)
{
    return log_page_count_internal();
}

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

static void log_setup(void)
{
    uint8_t  tf;
    uint16_t avail;

    g_log_font = log_font_fit();
    tf         = font_up(g_log_font);

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

void log_page_show(void)
{
    uint8_t total = app_log_count();
    uint8_t pages = log_page_count_internal();
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

            lcd_show_string(UI_X, y, lcddev.width, g_log_font, g_log_font, line,
                            (idx == 0) ? RED : UI_FG);
        }
    }

    lcd_show_string(UI_X, (uint16_t)(lcddev.height - g_log_font - 2), lcddev.width,
                    g_log_font, g_log_font, "KEY1=next   KEY0=back", GRAY);
}

void log_page_open(void)
{
    if (alarm_is_beeping())
    {
        return;
    }

    log_setup();
    g_log_page = 1;

    printf("\r\nKEY1 pressed -> show alarm log\r\n");
    app_log_dump();

    log_page_show();
}

void log_page_close(void)
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
