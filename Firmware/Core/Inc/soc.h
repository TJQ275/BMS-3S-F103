/**
 * @file    soc.h
 * @brief   SOC 估算：安时积分 + OCV 静置修正
 */
#ifndef SOC_H
#define SOC_H

#include <stdint.h>
#include <stdbool.h>

void  SOC_Init(uint32_t now_ms, uint16_t cell_min_mv, bool voltage_valid);
void  SOC_Update(int32_t current_ma, uint16_t cell_min_mv, uint32_t now_ms);
void  SOC_MarkFull(void);          /* 充满时调用，重新对齐 100% */
void  SOC_SetCapacity(float mah);  /* 实测容量后更新 */

float SOC_GetPercent(void);
float SOC_GetRemainingMah(void);
float SOC_GetCapacityMah(void);
float SOC_OcvToPercent(uint16_t mv);
bool  SOC_IsCalibrated(void);

#endif /* SOC_H */
