/**
 ****************************************************************************************************
 * @file        esp8266.c
 * @brief       ESP8266 (AT 固件) WiFi 模块驱动 —— USART3 (PB10=TX, PB11=RX)
 * @note        1, 所有 AT 命令均为阻塞式, 带超时;
 *              2, 接收采用"中断 + 环形缓冲", 不影响主循环;
 *              3, 建链成功后进入透传模式, 之后 STM32 直接写串口即可收发原始数据。
 ****************************************************************************************************
 */

#include "./BSP/ESP8266/esp8266.h"
#include "./SYSTEM/delay/delay.h"
#include "usart.h"          /* CubeMX 生成: huart3 */
#include <stdio.h>
#include <string.h>

/* 接收环形缓冲 */
static uint8_t  g_esp_rx_buf[ESP8266_RX_BUF_SIZE];
static volatile uint16_t g_esp_rx_head = 0;
static volatile uint16_t g_esp_rx_tail = 0;
static uint8_t  g_esp_rx_byte = 0;

/* 应答快照 */
static char     g_esp_resp[ESP8266_RESP_BUF_SIZE];
static uint16_t g_esp_resp_len = 0;

/* AT 命令拼接缓冲 */
static char     g_esp_cmd[192];

/* 排错用: 当前波特率 / 上一条命令期间实际收到的字节数与原始字节 */
static uint32_t g_esp_baud = 115200;
static uint16_t g_esp_rx_total = 0;
static uint8_t  g_esp_rx_raw[8];

/* 等待应答期间的服务回调(由应用层注册, 用于让报警/显示不被网络阻塞) */
static void    (*g_esp_poll_hook)(void) = NULL;
static uint32_t g_esp_poll_last = 0;

/**
 * @brief       ESP8266 初始化: 启动 USART3 的接收中断
 * @param       无
 * @retval      无
 */
void esp8266_init(void)
{
    esp8266_clear();
    HAL_UART_Receive_IT(&huart3, &g_esp_rx_byte, 1);
}

/**
 * @brief       清空接收缓冲与应答快照
 * @param       无
 * @retval      无
 */
void esp8266_clear(void)
{
    __disable_irq();
    g_esp_rx_head = 0;
    g_esp_rx_tail = 0;
    __enable_irq();

    memset(g_esp_resp, 0, sizeof(g_esp_resp));
    g_esp_resp_len = 0;

    g_esp_rx_total = 0;
    memset(g_esp_rx_raw, 0, sizeof(g_esp_rx_raw));
}

/**
 * @brief       读取一字节(非阻塞)
 * @param       无
 * @retval      -1: 无数据; 其它: 读到的字节
 */
int esp8266_getchar(void)
{
    int ch = -1;

    __disable_irq();

    if (g_esp_rx_head != g_esp_rx_tail)
    {
        ch = g_esp_rx_buf[g_esp_rx_tail];
        g_esp_rx_tail = (uint16_t)((g_esp_rx_tail + 1) % ESP8266_RX_BUF_SIZE);
    }

    __enable_irq();

    return ch;
}

/**
 * @brief       查询缓冲内可读字节数
 * @param       无
 * @retval      可读字节数
 */
uint16_t esp8266_available(void)
{
    uint16_t n;

    __disable_irq();
    n = (uint16_t)((g_esp_rx_head + ESP8266_RX_BUF_SIZE - g_esp_rx_tail) % ESP8266_RX_BUF_SIZE);
    __enable_irq();

    return n;
}

/**
 * @brief       获取最近一次 AT 应答快照(便于打印调试)
 * @param       无
 * @retval      应答字符串
 */
const char *esp8266_get_resp(void)
{
    return g_esp_resp;
}

/**
 * @brief       USART3 接收完成回调(每收 1 字节触发一次, 收完立即重新挂载)
 * @param       huart: UART 句柄
 * @retval      无
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        uint16_t next = (uint16_t)((g_esp_rx_head + 1) % ESP8266_RX_BUF_SIZE);

        if (next != g_esp_rx_tail)          /* 缓冲未满才写入, 满了丢弃新数据 */
        {
            g_esp_rx_buf[g_esp_rx_head] = g_esp_rx_byte;
            g_esp_rx_head = next;
        }

        if (g_esp_rx_total < sizeof(g_esp_rx_raw))      /* 留证: 前 8 个原始字节 */
        {
            g_esp_rx_raw[g_esp_rx_total] = g_esp_rx_byte;
        }

        g_esp_rx_total++;

        HAL_UART_Receive_IT(&huart3, &g_esp_rx_byte, 1);
    }
}

/**
 * @brief       USART3 错误回调(溢出/帧错误后 HAL 会关闭接收, 这里重新挂载)
 * @param       huart: UART 句柄
 * @retval      无
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_PEFLAG(huart);
        HAL_UART_Receive_IT(&huart3, &g_esp_rx_byte, 1);
    }
}

/**
 * @brief       注册"等待应答期间的服务回调"
 * @param       hook: 回调函数(传 NULL 取消注册)
 * @retval      无
 */
void esp8266_set_poll_hook(void (*hook)(void))
{
    g_esp_poll_hook = hook;
}

/**
 * @brief       暂停 USART3 接收中断(供 DHT11/DS18B20 等单总线器件使用)
 * @param       无
 * @retval      无
 * @note        只关 USART3 这一个中断源, SysTick 照常走, 所以 HAL_GetTick()/delay_us() 不受影响。
 */
void esp8266_rx_pause(void)
{
    HAL_NVIC_DisableIRQ(USART3_IRQn);
}

/**
 * @brief       恢复 USART3 接收中断
 * @param       无
 * @retval      无
 * @note        暂停期间可能已经溢出(ORE), 这里一并清标志并重新挂载, 避免 HAL 之后不再收数。
 */
void esp8266_rx_resume(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
}

/**
 * @brief       等待应答循环里的服务泵(每 ESP8266_POLL_PERIOD_MS 回调一次上层)
 * @param       无
 * @retval      无
 * @note        只在阻塞式等待(最长 20s 的 AT+CWJAP 等)内部被调用,
 *              目的就是把"报警/消音/显示/喂狗"插进这段等待里。
 */
void esp8266_service(void)
{
    uint32_t now;

    if (g_esp_poll_hook == NULL)
    {
        return;
    }

    now = HAL_GetTick();

    if ((uint32_t)(now - g_esp_poll_last) >= ESP8266_POLL_PERIOD_MS)
    {
        g_esp_poll_last = now;
        g_esp_poll_hook();
    }
}

/**
 * @brief       等待应答字符串出现
 * @param       ack       : 期望出现的关键字, 例如 "OK"
 * @param       timeout_ms: 超时(ms)
 * @retval      ESP8266_STAT_OK / _ERROR / _TIMEOUT
 */
uint8_t esp8266_wait_ack(const char *ack, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint16_t keep = (ESP8266_RESP_BUF_SIZE > 2) ? (ESP8266_RESP_BUF_SIZE - 2) : 0;

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        int ch;

        esp8266_service();                  /* ★ 等待期间继续跑报警/消音/显示, 不被网络卡住 */

        ch = esp8266_getchar();

        if (ch >= 0)
        {
            if (g_esp_resp_len < keep)
            {
                g_esp_resp[g_esp_resp_len++] = (char)ch;
            }
            else                                /* 快照满了就整体左移, 保留最新的内容 */
            {
                memmove(g_esp_resp, g_esp_resp + 1, keep - 1);
                g_esp_resp[keep - 1] = (char)ch;
            }

            g_esp_resp[g_esp_resp_len] = '\0';

            if (strstr(g_esp_resp, ack) != NULL)
            {
                return ESP8266_STAT_OK;
            }

            if (strstr(g_esp_resp, "ERROR") != NULL)
            {
                return ESP8266_STAT_ERROR;
            }

            if (strstr(g_esp_resp, "FAIL") != NULL)
            {
                return ESP8266_STAT_ERROR;
            }
        }
    }

    return ESP8266_STAT_TIMEOUT;
}

/**
 * @brief       发送一条 AT 命令并等待应答
 * @param       cmd       : 命令内容(不含 "\r\n")
 * @param       ack       : 期望应答关键字
 * @param       timeout_ms: 超时(ms)
 * @retval      ESP8266_STAT_OK / _ERROR / _TIMEOUT
 */
uint8_t esp8266_cmd(const char *cmd, const char *ack, uint32_t timeout_ms)
{
    printf("ESP >> %s\r\n", cmd);

    esp8266_clear();                            /* 丢弃历史残留数据 */

    if (HAL_UART_Transmit(&huart3, (uint8_t *)cmd, (uint16_t)strlen(cmd), 1000) != HAL_OK)
    {
        return ESP8266_STAT_ERROR;
    }

    if (HAL_UART_Transmit(&huart3, (uint8_t *)"\r\n", 2, 1000) != HAL_OK)
    {
        return ESP8266_STAT_ERROR;
    }

    return esp8266_wait_ack(ack, timeout_ms);
}

/**
 * @brief       AT 通信测试
 * @param       无
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_at(void)
{
    return esp8266_cmd("AT", "OK", 2000);
}

/**
 * @brief       修改 USART3 波特率(仅重算 BRR, 不碰已挂载的接收中断)
 * @param       baud: 目标波特率
 * @retval      ESP8266_STAT_OK / _ERROR
 */
uint8_t esp8266_set_baud(uint32_t baud)
{
    if (baud == 0)
    {
        return ESP8266_STAT_ERROR;
    }

    huart3.Init.BaudRate = baud;

    if (HAL_UART_Init(&huart3) != HAL_OK)
    {
        return ESP8266_STAT_ERROR;
    }

    g_esp_baud = baud;

    /* 关键: HAL_UART_Init() 结尾会把 huart->RxState 复位成 READY,
     *       而 UART_Receive_IT() 只在 RxState==BUSY_RX 时才搬字节,
     *       不重新挂载的话收到的字节会被静默丢弃(RXNEIE 还开着), 表现为"一个字节都收不到"。 */
    (void)HAL_UART_Receive_IT(&huart3, &g_esp_rx_byte, 1);

    return ESP8266_STAT_OK;
}

/**
 * @brief       查询当前波特率
 * @param       无
 * @retval      波特率
 */
uint32_t esp8266_get_baud(void)
{
    return g_esp_baud;
}

/**
 * @brief       查询上一条命令期间收到的字节总数
 * @param       无
 * @retval      字节数(0 = 一个字节都没收到, 说明 RX 通路断)
 */
uint16_t esp8266_rx_total(void)
{
    return g_esp_rx_total;
}

/**
 * @brief       取收到的前 8 个原始字节(串口打印出来即可判断波特率是否匹配)
 * @param       无
 * @retval      字节数组指针
 */
const uint8_t *esp8266_rx_raw(void)
{
    return g_esp_rx_raw;
}

/**
 * @brief       逐个常见波特率试探 "AT", 自动适配模块真实波特率
 * @param       p_rx_bytes: 输出, 失败时"收到过字节"的最大字节数(>0 说明接线是通的)
 * @retval      可用波特率; 0 = 全部失败(模块没供电 / 接线错 / 不是 AT 固件)
 * @note        常见值: 115200(新 AT 固件默认)、9600(老固件)、74880(仅启动日志)、
 *              57600 / 38400 / 19200 / 4800。
 */
uint32_t esp8266_scan_baud(uint16_t *p_rx_bytes)
{
    static const uint32_t bauds[] = { 115200, 9600, 74880, 57600, 38400, 19200, 4800 };
    uint16_t best = 0;
    uint8_t  i, k;

    for (i = 0; i < (uint8_t)(sizeof(bauds) / sizeof(bauds[0])); i++)
    {
        if (esp8266_set_baud(bauds[i]) != ESP8266_STAT_OK)
        {
            continue;
        }

        delay_ms(30);                                   /* 等模块 UART 稳定 */

        if (esp8266_cmd("AT", "OK", (bauds[i] == 115200) ? 1500 : 700) == ESP8266_STAT_OK)
        {
            printf("ESP8266: AT ok @ %u bps\r\n", (unsigned)bauds[i]);

            if (p_rx_bytes != NULL)
            {
                *p_rx_bytes = g_esp_rx_total;
            }

            return bauds[i];
        }

        if (g_esp_rx_total > 0)                         /* 该波特率下有字节, 打印出来判据 */
        {
            printf("ESP8266: @ %u bps got %u B:", (unsigned)bauds[i], (unsigned)g_esp_rx_total);

            for (k = 0; (k < 8) && (k < g_esp_rx_total); k++)
            {
                printf(" %02X", g_esp_rx_raw[k]);
            }

            printf("\r\n");
        }

        if (g_esp_rx_total > best)
        {
            best = g_esp_rx_total;
        }
    }

    if (p_rx_bytes != NULL)
    {
        *p_rx_bytes = best;
    }

    (void)esp8266_set_baud(115200);                     /* 恢复默认, 便于下一轮重试 */

    return 0;
}

/**
 * @brief       关闭命令回显(简化应答解析, 重启后失效)
 * @param       无
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_echo_off(void)
{
    return esp8266_cmd("ATE0", "OK", 2000);
}

/**
 * @brief       软复位模块
 * @param       无
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_reset(void)
{
    esp8266_cmd("AT+RST", "OK", 3000);
    delay_ms(2000);                             /* 等待模块重启完成 */
    esp8266_clear();
    return esp8266_at();
}

/**
 * @brief       设置 WiFi 工作模式
 * @param       mode: 1=STA, 2=AP, 3=STA+AP
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_set_mode(uint8_t mode)
{
    snprintf(g_esp_cmd, sizeof(g_esp_cmd), "AT+CWMODE=%d", mode);
    return esp8266_cmd(g_esp_cmd, "OK", 3000);
}

/**
 * @brief       连接 WiFi 热点(仅支持 2.4GHz)
 * @param       ssid: 热点名
 * @param       pwd : 密码, 开放热点传 ""
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_join_ap(const char *ssid, const char *pwd)
{
    snprintf(g_esp_cmd, sizeof(g_esp_cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pwd);
    return esp8266_cmd(g_esp_cmd, "OK", 20000);     /* 联网较慢, 给 20s */
}

/**
 * @brief       建立单路 TCP 连接(需先 AT+CIPMUX=0)
 * @param       host: 服务器域名或 IP
 * @param       port: 端口
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_open_tcp(const char *host, uint16_t port)
{
    if (esp8266_cmd("AT+CIPMUX=0", "OK", 3000) != ESP8266_STAT_OK)
    {
        return ESP8266_STAT_ERROR;
    }

    if (esp8266_cmd("AT+CIPMODE=0", "OK", 3000) != ESP8266_STAT_OK)   /* 先确保非透传 */
    {
        return ESP8266_STAT_ERROR;
    }

    snprintf(g_esp_cmd, sizeof(g_esp_cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", host, (unsigned)port);
    return esp8266_cmd(g_esp_cmd, "CONNECT", 15000);
}

/**
 * @brief       进入透传模式
 * @param       无
 * @retval      ESP8266_STAT_xxx
 * @note        成功后模块回显 "OK" 再输出 ">" 提示符, 之后所有写入都会被原样发到 TCP。
 */
uint8_t esp8266_enter_transparent(void)
{
    if (esp8266_cmd("AT+CIPMODE=1", "OK", 3000) != ESP8266_STAT_OK)
    {
        return ESP8266_STAT_ERROR;
    }

    esp8266_clear();
    printf("ESP >> AT+CIPSEND\r\n");

    if (HAL_UART_Transmit(&huart3, (uint8_t *)"AT+CIPSEND\r\n", 12, 1000) != HAL_OK)
    {
        return ESP8266_STAT_ERROR;
    }

    return esp8266_wait_ack(">", 5000);
}

/**
 * @brief       退出透传模式(发送 "+++", 前后需各留 20ms 静默)
 * @param       无
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_exit_transparent(void)
{
    delay_ms(20);
    HAL_UART_Transmit(&huart3, (uint8_t *)"+++", 3, 1000);
    delay_ms(20);

    return esp8266_wait_ack("OK", 3000);
}

/**
 * @brief       透传发送原始数据
 * @param       buf: 数据指针
 * @param       len: 数据长度
 * @retval      ESP8266_STAT_xxx
 */
uint8_t esp8266_send(const uint8_t *buf, uint16_t len)
{
    if (HAL_UART_Transmit(&huart3, (uint8_t *)buf, len, 3000) != HAL_OK)
    {
        return ESP8266_STAT_ERROR;
    }

    return ESP8266_STAT_OK;
}
