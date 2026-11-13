/**
 * @file    bms.h
 * @brief   BMS 应用层：调度、状态机、上报
 */
#ifndef BMS_H
#define BMS_H

#include <stdint.h>
#include <stdbool.h>
#include "bq76920.h"
#include "protection.h"
#include "protocol.h"

void BMS_Init(void);
void BMS_Task(void);                     /* 主循环里一直调用 */
void BMS_OnAlertIrq(void);               /* EXTI 回调里调用 */
const proto_telemetry_t *BMS_GetTelemetry(void);
const prot_state_t      *BMS_GetProtection(void);
const bq_sample_t       *BMS_GetSample(void);
bool                     BMS_SaveState(void);   /* 立即写一条 NVM（进低功耗/掉电前调用） */
void                     BMS_RestartRx(void);  /* STOP 唤醒后重新挂串口接收 */

#endif /* BMS_H */
