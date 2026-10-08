/**
 ****************************************************************************************************
 * @file        app_log.c
 * @brief       报警历史记录(存内部 FLASH 最后一页)
 * @note        存储布局(页起始 0x0807F800, 共 2048 字节, 只用前 1040 字节):
 *                [0..3]   魔数 'L''O''G''1'
 *                [4..5]   累计写入条数(用来算环形索引)
 *                [6..7]   头部 CRC16(只覆盖 [0..5])
 *                [8..15]  保留
 *                [16..]   记录区, 每条 16 字节, 最多 64 条 = 1024 字节
 *              记录自身也带 CRC16, 所以半页坏掉时能逐条识别, 不会显示出乱码。
 ****************************************************************************************************
 */

#include "./App/app_log.h"
#include "./App/app_alarm.h"
#include "./BSP/STMFLASH/stmflash.h"
#include "./BSP/RTC/rtc.h"
#include <string.h>
#include <stdio.h>

#define APP_LOG_FLASH_ADDR      0x0807F800UL    /* 512KB FLASH 的最后一页(2KB) */
#define APP_LOG_MAGIC_0         0x4F4CUL        /* 'L','O' */
#define APP_LOG_MAGIC_1         0x3147UL        /* 'G','1' */
#define APP_LOG_REC_SIZE        8U              /* 每条记录 = 8 个半字 = 16 字节 */
#define APP_LOG_HDR_HW          4U              /* 头部 = 4 个半字 = 8 字节 */
#define APP_LOG_AREA_HW         (APP_LOG_HDR_HW + APP_LOG_MAX * APP_LOG_REC_SIZE)
#define APP_LOG_IMG_SIZE        (APP_LOG_MAX * 16U)

/* FLASH 镜像: 上电读进来, 之后改动都在这里做, 写回时整块写 */
static uint16_t g_img[APP_LOG_AREA_HW];

static uint8_t  g_count = 0;        /* 有效记录条数(上限 APP_LOG_MAX) */
static uint8_t  g_head  = 0;        /* 下一条要写的位置(环形) */
static uint16_t g_total = 0;        /* 累计写入条数(一直涨, 用来算环形索引) */
static uint8_t  g_ready = 0;        /* 是否可用(载入后才允许写) */

/* 解码出来的记录(给 app_log_get 返回用, 避免调用方直接摸 FLASH 布局) */
static app_log_item_t g_item;

/**
 * @brief       CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
 * @param       p   : 数据
 * @param       len : 字节数
 * @retval      CRC 值
 */
static uint16_t app_log_crc16(const uint8_t *p, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    uint16_t i;
    uint8_t  b;

    for (i = 0; i < len; i++)
    {
        crc ^= (uint16_t)p[i] << 8;

        for (b = 0; b < 8; b++)
        {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }

    return crc;
}

/**
 * @brief       把一条记录编码进镜像
 * @param       idx : 记录槽位(0 ~ APP_LOG_MAX-1)
 * @param       it  : 记录内容
 * @retval      无
 */
static void app_log_encode(uint8_t idx, const app_log_item_t *it)
{
    uint16_t *r = &g_img[APP_LOG_HDR_HW + (uint16_t)idx * APP_LOG_REC_SIZE];
    uint8_t   raw[14];
    uint16_t  crc;

    raw[0]  = (uint8_t)(it->year & 0xFF);
    raw[1]  = (uint8_t)(it->year >> 8);
    raw[2]  = it->month;
    raw[3]  = it->date;
    raw[4]  = it->hour;
    raw[5]  = it->min;
    raw[6]  = it->sec;
    raw[7]  = it->faults;
    raw[8]  = (uint8_t)((uint16_t)it->temp_x10 & 0xFF);
    raw[9]  = (uint8_t)((uint16_t)it->temp_x10 >> 8);
    raw[10] = (uint8_t)(it->humi_x10 & 0xFF);
    raw[11] = (uint8_t)(it->humi_x10 >> 8);
    raw[12] = (uint8_t)(it->lux_cdm2 & 0xFF);
    raw[13] = (uint8_t)(it->lux_cdm2 >> 8);

    crc = app_log_crc16(raw, sizeof(raw));

    r[0] = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    r[1] = (uint16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    r[2] = (uint16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    r[3] = (uint16_t)((uint16_t)raw[6] | ((uint16_t)raw[7] << 8));
    r[4] = (uint16_t)((uint16_t)raw[8] | ((uint16_t)raw[9] << 8));
    r[5] = (uint16_t)((uint16_t)raw[10] | ((uint16_t)raw[11] << 8));
    r[6] = (uint16_t)((uint16_t)raw[12] | ((uint16_t)raw[13] << 8));
    r[7] = crc;
}

/**
 * @brief       把镜像里的一条记录解码出来
 * @param       idx : 记录槽位
 * @param       it  : 输出
 * @retval      1 = CRC 正确; 0 = 该槽位无效
 */
static uint8_t app_log_decode(uint8_t idx, app_log_item_t *it)
{
    const uint16_t *r = &g_img[APP_LOG_HDR_HW + (uint16_t)idx * APP_LOG_REC_SIZE];
    uint8_t  raw[14];
    uint16_t crc;

    raw[0]  = (uint8_t)(r[0] & 0xFF);
    raw[1]  = (uint8_t)(r[0] >> 8);
    raw[2]  = (uint8_t)(r[1] & 0xFF);
    raw[3]  = (uint8_t)(r[1] >> 8);
    raw[4]  = (uint8_t)(r[2] & 0xFF);
    raw[5]  = (uint8_t)(r[2] >> 8);
    raw[6]  = (uint8_t)(r[3] & 0xFF);
    raw[7]  = (uint8_t)(r[3] >> 8);
    raw[8]  = (uint8_t)(r[4] & 0xFF);
    raw[9]  = (uint8_t)(r[4] >> 8);
    raw[10] = (uint8_t)(r[5] & 0xFF);
    raw[11] = (uint8_t)(r[5] >> 8);
    raw[12] = (uint8_t)(r[6] & 0xFF);
    raw[13] = (uint8_t)(r[6] >> 8);

    crc = app_log_crc16(raw, sizeof(raw));

    if (crc != r[7])
    {
        return 0;
    }

    it->year     = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    it->month    = raw[2];
    it->date     = raw[3];
    it->hour     = raw[4];
    it->min      = raw[5];
    it->sec      = raw[6];
    it->faults   = raw[7];
    it->temp_x10 = (int16_t)((uint16_t)raw[8] | ((uint16_t)raw[9] << 8));
    it->humi_x10 = (uint16_t)((uint16_t)raw[10] | ((uint16_t)raw[11] << 8));
    it->lux_cdm2 = (uint16_t)((uint16_t)raw[12] | ((uint16_t)raw[13] << 8));

    return 1;
}

/**
 * @brief       更新头部(魔数 + 累计条数)与头部 CRC
 */
static void app_log_set_header(uint16_t total)
{
    uint8_t  raw[6];
    uint16_t crc;

    raw[0] = (uint8_t)(APP_LOG_MAGIC_0 & 0xFF);
    raw[1] = (uint8_t)(APP_LOG_MAGIC_0 >> 8);
    raw[2] = (uint8_t)(APP_LOG_MAGIC_1 & 0xFF);
    raw[3] = (uint8_t)(APP_LOG_MAGIC_1 >> 8);
    raw[4] = (uint8_t)(total & 0xFF);
    raw[5] = (uint8_t)(total >> 8);

    crc = app_log_crc16(raw, sizeof(raw));

    g_img[0] = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    g_img[1] = (uint16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    g_img[2] = (uint16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    g_img[3] = crc;
}

/**
 * @brief       清空记录(全 0xFFFF + 合法头部)
 */
static void app_log_reset(void)
{
    memset(g_img, 0xFF, sizeof(g_img));

    g_count = 0;
    g_head  = 0;
    g_total = 0;

    app_log_set_header(0);
}

/**
 * @brief       上电载入
 * @param       无
 * @retval      无
 */
void app_log_init(void)
{
    uint8_t  raw[6];
    uint16_t crc;
    uint16_t total;
    uint8_t  i;

    g_ready = 0;

    stmflash_read(APP_LOG_FLASH_ADDR, g_img, APP_LOG_AREA_HW);

    raw[0] = (uint8_t)(g_img[0] & 0xFF);
    raw[1] = (uint8_t)(g_img[0] >> 8);
    raw[2] = (uint8_t)(g_img[1] & 0xFF);
    raw[3] = (uint8_t)(g_img[1] >> 8);
    raw[4] = (uint8_t)(g_img[2] & 0xFF);
    raw[5] = (uint8_t)(g_img[2] >> 8);

    crc = app_log_crc16(raw, sizeof(raw));

    if ((raw[0] != (uint8_t)(APP_LOG_MAGIC_0 & 0xFF)) ||
        (raw[1] != (uint8_t)(APP_LOG_MAGIC_0 >> 8)) ||
        (raw[2] != (uint8_t)(APP_LOG_MAGIC_1 & 0xFF)) ||
        (raw[3] != (uint8_t)(APP_LOG_MAGIC_1 >> 8)) ||
        (crc != g_img[3]))
    {
        /* 从没记录过 / 头部损坏 / 上次写一半掉电 -> 当作空库重新开始 */
        printf("LOG  : no valid log in flash, start empty\r\n");
        app_log_reset();
        g_ready = 1;
        return;
    }

    total = (uint16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));

    g_total = total;
    g_count = (total > APP_LOG_MAX) ? APP_LOG_MAX : (uint8_t)total;
    g_head  = (uint8_t)(total % APP_LOG_MAX);

    /* 还没写满时, "有效条数"其实等于 total; 写满后 total 比 MAX 大 */
    if (total > APP_LOG_MAX)
    {
        g_count = APP_LOG_MAX;
    }

    /* 逐条校验, 遇到坏记录就把有效条数截断到这里 */
    for (i = 0; i < g_count; i++)
    {
        app_log_item_t it;

        if (app_log_decode(i, &it) == 0)
        {
            printf("LOG  : slot %u CRC bad, truncate to %u\r\n", (unsigned)i, (unsigned)i);
            g_count = i;
            break;
        }
    }

    g_ready = 1;

    printf("LOG  : %u record(s) loaded from 0x%08X\r\n",
           (unsigned)g_count, (unsigned)APP_LOG_FLASH_ADDR);
}

/**
 * @brief       新增一条报警记录
 * @param       faults  : 危险区标志 ALARM_FAULT_xxx
 * @param       temp_x10: 当时温度
 * @param       humi_x10: 当时湿度
 * @param       lux_cdm2: 当时光照
 * @retval      无
 * @note        会被 alarm_feed() 通过回调在"报警刚开始"的那一瞬间调用一次。
 *              写 FLASH 要整页擦除 + 整页写回(约 80ms), 这期间 CPU 被 FLASH 控制器挂住,
 *              串口收到的字节可能丢 —— 丢的都是 OneNET 的 QoS0 回执, 没有影响。
 */
void app_log_add(uint8_t faults, int16_t temp_x10, uint16_t humi_x10, uint32_t lux_cdm2)
{
    app_log_item_t it;

    if (g_ready == 0)
    {
        return;                             /* 存储没准备好就不写, 宁可丢记录也不写坏 FLASH */
    }

    rtc_get_time();                         /* 取当前时间做时间戳 */

    it.year     = calendar.year;
    it.month    = calendar.month;
    it.date     = calendar.date;
    it.hour     = calendar.hour;
    it.min      = calendar.min;
    it.sec      = calendar.sec;
    it.faults   = faults;
    it.temp_x10 = temp_x10;
    it.humi_x10 = humi_x10;
    it.lux_cdm2 = (lux_cdm2 > 0xFFFFU) ? 0xFFFFU : (uint16_t)lux_cdm2;

    app_log_encode(g_head, &it);

    g_total++;

    if (g_total <= APP_LOG_MAX)
    {
        g_count = (uint8_t)g_total;         /* 还没写满一圈 */
    }
    /* 已经写满一圈: g_count 保持 APP_LOG_MAX, 老记录被新记录逐个覆盖 */

    g_head = (uint8_t)(g_total % APP_LOG_MAX);

    app_log_set_header(g_total);

    stmflash_write(APP_LOG_FLASH_ADDR, g_img, APP_LOG_AREA_HW);

    printf("LOG  : saved #%u  %04u-%02u-%02u %02u:%02u:%02u  f=0x%02X  T=%d.%d H=%u.%u L=%u%s\r\n",
           (unsigned)g_total,
           it.year, it.month, it.date, it.hour, it.min, it.sec,
           (unsigned)it.faults,
           (int)(it.temp_x10 / 10), (int)(it.temp_x10 < 0 ? -(it.temp_x10 % 10) : (it.temp_x10 % 10)),
           (unsigned)(it.humi_x10 / 10), (unsigned)(it.humi_x10 % 10),
           (unsigned)it.lux_cdm2,
           (rtc_is_set() != 0) ? "" : "  [clock not battery-backed]");
}

/**
 * @brief       清空全部记录
 * @param       无
 * @retval      无
 */
void app_log_clear(void)
{
    if (g_ready == 0)
    {
        return;
    }

    app_log_reset();
    stmflash_write(APP_LOG_FLASH_ADDR, g_img, APP_LOG_AREA_HW);

    printf("LOG  : cleared\r\n");
}

/**
 * @brief       当前有效记录条数
 */
uint8_t app_log_count(void)
{
    return g_count;
}

/**
 * @brief       容量
 */
uint8_t app_log_capacity(void)
{
    return APP_LOG_MAX;
}

/**
 * @brief       按"从新到旧"的顺序取一条记录
 * @param       idx: 0 = 最新
 * @retval      记录指针; NULL = 越界或该条损坏
 */
const app_log_item_t *app_log_get(uint8_t idx)
{
    uint8_t slot;

    if (idx >= g_count)
    {
        return NULL;
    }

    /* 最新一条在 head 的前一格 */
    slot = (uint8_t)((g_head + APP_LOG_MAX - 1 - idx) % APP_LOG_MAX);

    if (app_log_decode(slot, &g_item) == 0)
    {
        return NULL;
    }

    return &g_item;
}

/**
 * @brief       把原因位转成字符串
 * @param       faults: ALARM_FAULT_xxx
 * @param       dst   : 输出缓冲
 * @param       dstsz : 缓冲大小
 * @retval      无
 */
void app_log_fault_str(uint8_t faults, char *dst, uint16_t dstsz)
{
    (void)dstsz;

    dst[0] = '\0';

    if ((faults & ALARM_FAULT_TEMP) != 0)
    {
        strcat(dst, "TEMP");
    }

    if ((faults & ALARM_FAULT_HUMI) != 0)
    {
        if (dst[0] != '\0')
        {
            strcat(dst, "+");
        }

        strcat(dst, "HUMI");
    }

    if ((faults & ALARM_FAULT_LUX) != 0)
    {
        if (dst[0] != '\0')
        {
            strcat(dst, "+");
        }

        strcat(dst, "LIGHT");
    }

    if (dst[0] == '\0')
    {
        strcpy(dst, "?");
    }
}

/**
 * @brief       把全部记录打到串口(从新到旧)
 * @param       无
 * @retval      无
 */
void app_log_dump(void)
{
    uint8_t i;
    char    f[24];

    printf("---- ALARM LOG (%u/%u, newest first) ----\r\n",
           (unsigned)g_count, (unsigned)APP_LOG_MAX);

    if (g_count == 0)
    {
        printf("(empty)\r\n");
        printf("----------------------------------------\r\n");
        return;
    }

    for (i = 0; i < g_count; i++)
    {
        const app_log_item_t *it = app_log_get(i);

        if (it == NULL)
        {
            printf("#%02u  <bad record>\r\n", (unsigned)i);
            continue;
        }

        app_log_fault_str(it->faults, f, sizeof(f));

        printf("#%02u  %04u-%02u-%02u %02u:%02u:%02u  %-11s  T=%d.%d C  H=%u.%u %%  L=%u cd/m2\r\n",
               (unsigned)i,
               it->year, it->month, it->date, it->hour, it->min, it->sec,
               f,
               (int)(it->temp_x10 / 10),
               (int)(it->temp_x10 < 0 ? -(it->temp_x10 % 10) : (it->temp_x10 % 10)),
               (unsigned)(it->humi_x10 / 10), (unsigned)(it->humi_x10 % 10),
               (unsigned)it->lux_cdm2);
    }

    printf("----------------------------------------\r\n");
}
