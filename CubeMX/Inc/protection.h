/**
 * @file    protection.h
 * @brief   二级保护：在 AFE 硬件保护之上做温度、通讯、延时/回差与故障锁存
 */
#ifndef PROTECTION_H
#define PROTECTION_H

#include <stdint.h>
#include <stdbool.h>
#include "bq76920.h"

/* 故障位（上报给上位机用） */
#define PROT_F_OV        (1u << 0)   /* 单体过压 */
#define PROT_F_UV        (1u << 1)   /* 单体欠压 */
#define PROT_F_OCD       (1u << 2)   /* 放电过流 */
#define PROT_F_SCD       (1u << 3)   /* 放电短路 */
#define PROT_F_CELL_HOT  (1u << 4)   /* 电芯过温 */
#define PROT_F_CELL_COLD (1u << 5)   /* 电芯低温禁充 */
#define PROT_F_MOS_HOT   (1u << 6)   /* MOS 过温 */
#define PROT_F_COMM      (1u << 7)   /* AFE 通讯失败 */
#define PROT_F_AFE       (1u << 8)   /* AFE XREADY 异常 */
#define PROT_F_BAL_HOT   (1u << 9)   /* 均衡超温 */

typedef struct {
    uint32_t active;     /* 当前仍生效的故障 */
    uint32_t latched;    /* 历史故障（需命令或复位清除） */
    bool chg_ok;         /* 允许充电 */
    bool dsg_ok;         /* 允许放电 */
    bool bal_ok;         /* 允许均衡 */
} prot_state_t;

void        Prot_Init(void);
void        Prot_Update(const bq_sample_t *s, int16_t mos_c10,
                        prot_state_t *out, uint32_t now_ms);
void        Prot_ClearLatch(void);
const char *Prot_Name(uint32_t bit);

#endif /* PROTECTION_H */
