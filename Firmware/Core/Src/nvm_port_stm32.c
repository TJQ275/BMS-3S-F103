/**
 * @file    nvm_port_stm32.c
 * @brief   STM32F103 的 Flash 移植层：用最后一页（1KB）
 *
 * ⚠️ 该页位于 0x0800FC00。F103C8 共 64KB，代码目前约 19.5KB，够用；
 *    如果以后固件超过 ~63KB，必须把 NVM 页挪走（改 NVM_PAGE_ADDR + 链接脚本）。
 */
#include "main.h"
#include "nvm_port.h"

#include <string.h>

#define NVM_PAGE_ADDR   0x0800FC00u
#define NVM_PAGE_NUM    ((NVM_PAGE_ADDR - 0x08000000u) / FLASH_PAGE_SIZE)

bool NVM_PortRead(uint32_t offset, void *buf, uint32_t len)
{
    if (offset + len > NVM_PAGE_SIZE) return false;
    memcpy(buf, (const void *)(NVM_PAGE_ADDR + offset), len);
    return true;
}

bool NVM_PortWrite(uint32_t offset, const void *buf, uint32_t len)
{
    const uint16_t *src = (const uint16_t *)buf;
    uint32_t addr = NVM_PAGE_ADDR + offset;
    bool ok = true;

    if (offset + len > NVM_PAGE_SIZE) return false;

    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len / 2u; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i * 2u, src[i]) != HAL_OK) {
            ok = false;
            break;
        }
    }
    HAL_FLASH_Lock();
    return ok;
}

bool NVM_PortErase(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0;
    HAL_StatusTypeDef st;

    HAL_FLASH_Unlock();
    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = NVM_PAGE_ADDR;
    erase.NbPages     = 1;
    st = HAL_FLASHEx_Erase(&erase, &page_error);
    HAL_FLASH_Lock();

    return (st == HAL_OK) && (page_error == 0xFFFFFFFFu);
}
