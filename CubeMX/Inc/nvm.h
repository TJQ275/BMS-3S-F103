/**
 * @file    nvm.h
 * @brief   参数/状态掉电保存：Flash 末尾一页，追加式日志 + CRC，擦除次数最少
 *
 * 存的内容：SOC、剩余容量、学习到的满容量、循环次数、历史故障、累计运行时间。
 * 写入策略：每 5 分钟或状态变化时追加一条 32 字节记录；一页写满才擦一次
 *           （1KB / 32B = 32 条/擦除），按 5 分钟一条算，擦一次能撑 2.6 小时。
 */
#ifndef NVM_H
#define NVM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float    soc_pct;
    float    remain_mah;
    float    cap_mah;
    uint16_t cycles_x10;     /* 循环次数 x10，如 12 表示 1.2 次 */
    uint16_t faults_hist;    /* 历史故障位 */
    uint32_t uptime_s;
} nvm_state_t;

void     NVM_Init(void);
bool     NVM_Load(nvm_state_t *st);
bool     NVM_Save(const nvm_state_t *st);
bool     NVM_EraseAll(void);
uint32_t NVM_EraseCount(void);   /* 页擦除次数，用来估寿命 */

#endif /* NVM_H */
