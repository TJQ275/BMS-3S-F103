/* 主机端：用 RAM 模拟 1KB Flash 页，验证 NVM 逻辑 */
#include "nvm_port.h"
#include <string.h>

static uint8_t s_page[NVM_PAGE_SIZE];
int g_nvm_erase_count;

void nvm_sim_reset(void)
{
    memset(s_page, 0xFF, sizeof(s_page));
    g_nvm_erase_count = 0;
}

void nvm_sim_corrupt(uint32_t offset)
{
    if (offset < NVM_PAGE_SIZE) s_page[offset] ^= 0xFF;
}

bool NVM_PortRead(uint32_t offset, void *buf, uint32_t len)
{
    if (offset + len > NVM_PAGE_SIZE) return false;
    memcpy(buf, s_page + offset, len);
    return true;
}

bool NVM_PortWrite(uint32_t offset, const void *buf, uint32_t len)
{
    if (offset + len > NVM_PAGE_SIZE) return false;
    /* 模拟 Flash：只能 1->0，写 1 不会变成 1 */
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) s_page[offset + i] &= p[i];
    return true;
}

bool NVM_PortErase(void)
{
    memset(s_page, 0xFF, sizeof(s_page));
    g_nvm_erase_count++;
    return true;
}
