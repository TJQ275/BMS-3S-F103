/**
 * @file    nvm_port.h
 * @brief   NVM 的 Flash 读写移植层（STM32 用 Flash，PC 测试用 RAM 模拟）
 */
#ifndef NVM_PORT_H
#define NVM_PORT_H

#include <stdint.h>
#include <stdbool.h>

#define NVM_PAGE_SIZE   1024u

bool NVM_PortRead(uint32_t offset, void *buf, uint32_t len);
bool NVM_PortWrite(uint32_t offset, const void *buf, uint32_t len);
bool NVM_PortErase(void);

#endif /* NVM_PORT_H */
