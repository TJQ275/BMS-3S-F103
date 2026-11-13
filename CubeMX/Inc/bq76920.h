/**
 * @file    bq76920.h
 * @brief   TI BQ76920 模拟前端（AFE）驱动，3~5 串
 *          只依赖 HAL 的 I2C + GPIO + HAL_Delay，方便移植
 */
#ifndef BQ76920_H
#define BQ76920_H

#include <stdint.h>
#include <stdbool.h>

/* 一次完整采样的结果 */
typedef struct {
    uint16_t cell_mv[5];    /* 单体电压，未接的节为 0 */
    uint8_t  cell_count;    /* 实际检测到的节数 */
    uint16_t pack_mv;       /* 总压 */
    int32_t  current_ma;    /* + 充电 / - 放电 */
    int16_t  temp_c10;      /* TS1 温度 x10 ℃ */
    bool     temp_valid;    /* TS1 是否读到有效 NTC */
    uint8_t  sys_stat;      /* SYS_STAT 原值 */
} bq_sample_t;

/* 初始化：打 BOOT、开 ADC/CC、读增益与偏置、下发保护阈值
 * 返回 false 表示 I2C 不通或器件未启动 */
bool    BQ_Init(void);

/* 采样（电压 + 总压 + 温度），单次约 1.2ms */
bool    BQ_ReadAll(bq_sample_t *s);
bool    BQ_ReadTemp(int16_t *c10);

/* 读库仑计（读后 CC_READY 会自动清）；fresh = 本次是否有新数据 */
bool    BQ_ReadCurrent(int32_t *ma, bool *fresh);

/* SYS_STAT 读写 */
uint8_t BQ_Status(void);
bool    BQ_ClearStatus(uint8_t mask);

/* 充/放电 MOS 控制（真正生效还受 AFE 硬件保护支配） */
bool    BQ_SetFets(bool chg_on, bool dsg_on);
bool    BQ_GetFets(bool *chg_on, bool *dsg_on);

/* 被动均衡：mask bit0=Cell1 ... bit4=Cell5，内部 FET 导通外接电阻放电 */
bool    BQ_SetBalancing(uint8_t mask);

/* 睡眠前关掉库仑计，避免 CC_READY 每 250ms 把 MCU 唤醒（此时无电流，不丢积分） */
bool    BQ_SetCcEnabled(bool en);

/* 进入 SHIP 模式（整机断电，电流 <1µA 级） */
void    BQ_EnterShipMode(void);

/* 连续 I2C 失败次数，供上层判通讯故障 */
uint8_t BQ_CommFails(void);
uint16_t BQ_LastError(void);

#endif /* BQ76920_H */
