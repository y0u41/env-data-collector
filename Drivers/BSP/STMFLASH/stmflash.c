/**
 ****************************************************************************************************
 * @file        stmflash.c
 * @brief       STM32 内部 FLASH 读写(用来模拟 EEPROM, 掉电不丢)
 * @note        移植自正点原子官方 HAL 例程「实验31 FLASH模拟EEPROM」, 逻辑与官方一致。
 ****************************************************************************************************
 */

#include "./BSP/STMFLASH/stmflash.h"

/* 整页回读缓冲: stmflash_write() 需要"读出来 -> 擦除 -> 改 -> 写回去", 所以必须有这块缓存。
 * 本板页大小 2KB, 占 1K 个半字 = 1KB*2 = 2KB RAM。 */
static uint16_t g_flashbuf[STM32_SECTOR_SIZE / 2];

/**
 * @brief       读一个半字(16 位)
 * @param       faddr: 读取地址(必须是 2 的倍数)
 * @retval      读到的值
 */
uint16_t stmflash_read_halfword(uint32_t faddr)
{
    return *(volatile uint16_t *)faddr;
}

/**
 * @brief       从指定地址开始读出指定长度的半字数据
 * @param       raddr : 起始地址
 * @param       pbuf  : 数据指针
 * @param       length: 要读取的半字(16 位)数
 * @retval      无
 */
void stmflash_read(uint32_t raddr, uint16_t *pbuf, uint16_t length)
{
    uint16_t i;

    for (i = 0; i < length; i++)
    {
        pbuf[i] = stmflash_read_halfword(raddr);    /* 读 2 个字节 */
        raddr += 2;
    }
}

/**
 * @brief       不检查地连续写入(调用前必须保证目标区域已经是 0xFFFF)
 */
static void stmflash_write_nocheck(uint32_t waddr, uint16_t *pbuf, uint16_t length)
{
    uint16_t i;

    for (i = 0; i < length; i++)
    {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, waddr, pbuf[i]);
        waddr += 2;
    }
}

/**
 * @brief       在 FLASH 指定位置写入指定长度的数据(自动擦除)
 * @param       waddr : 起始地址(必须是 2 的倍数, 且不能跨到越界区域)
 * @param       pbuf  : 数据指针
 * @param       length: 要写入的半字(16 位)数
 * @retval      无
 * @note        先检查要写的区域是否已经是空的(全 0xFFFF): 是则直接写, 否则把整页读进
 *              g_flashbuf、擦掉整页、改好数据再整页写回。数据长度超过一页时自动分页处理。
 */
void stmflash_write(uint32_t waddr, uint16_t *pbuf, uint16_t length)
{
    uint32_t secpos;            /* 页地址 */
    uint16_t secoff;            /* 页内偏移(以半字为单位) */
    uint16_t secremain;         /* 页内剩余空间(以半字为单位) */
    uint16_t i;
    uint32_t offaddr;           /* 去掉 0x08000000 后的偏移地址 */
    FLASH_EraseInitTypeDef flash_eraseop;
    uint32_t erase_addr;        /* 擦除出错时返回的错误页地址(用不到, 但 HAL 要求传) */

    if ((waddr < STM32_FLASH_BASE) || (waddr >= (STM32_FLASH_BASE + STM32_FLASH_SIZE)))
    {
        return;                 /* 非法地址 */
    }

    HAL_FLASH_Unlock();

    offaddr   = waddr - STM32_FLASH_BASE;
    secpos    = offaddr / STM32_SECTOR_SIZE;
    secoff    = (uint16_t)((offaddr % STM32_SECTOR_SIZE) / 2);
    secremain = (uint16_t)(STM32_SECTOR_SIZE / 2 - secoff);

    if (length <= secremain)
    {
        secremain = length;     /* 本次写得完 */
    }

    while (1)
    {
        /* 把整页读出来, 看要写的那一段是不是空的 */
        stmflash_read(secpos * STM32_SECTOR_SIZE + STM32_FLASH_BASE, g_flashbuf, STM32_SECTOR_SIZE / 2);

        for (i = 0; i < secremain; i++)
        {
            if (g_flashbuf[secoff + i] != 0xFFFF)
            {
                break;          /* 有数据, 需要擦除 */
            }
        }

        if (i < secremain)      /* 需要擦除 */
        {
            flash_eraseop.TypeErase   = FLASH_TYPEERASE_PAGES;
            flash_eraseop.Banks       = FLASH_BANK_1;
            flash_eraseop.NbPages     = 1;
            flash_eraseop.PageAddress = secpos * STM32_SECTOR_SIZE + STM32_FLASH_BASE;

            HAL_FLASHEx_Erase(&flash_eraseop, &erase_addr);

            for (i = 0; i < secremain; i++)     /* 把新数据合并进页缓冲 */
            {
                g_flashbuf[i + secoff] = pbuf[i];
            }

            stmflash_write_nocheck(secpos * STM32_SECTOR_SIZE + STM32_FLASH_BASE,
                                   g_flashbuf, STM32_SECTOR_SIZE / 2);
        }
        else                    /* 目标区域本来就是空的, 直接写 */
        {
            stmflash_write_nocheck(waddr, pbuf, secremain);
        }

        if (length == secremain)
        {
            break;              /* 写完了 */
        }

        /* 没写完: 挪到下一页继续 */
        secpos++;
        secoff     = 0;
        pbuf      += secremain;
        waddr     += (uint32_t)secremain * 2;
        length    -= secremain;
        secremain  = (length > (STM32_SECTOR_SIZE / 2)) ? (STM32_SECTOR_SIZE / 2) : length;
    }

    HAL_FLASH_Lock();
}
