/**
 * @file    lp.h
 * @brief   低功耗：STOP 模式 + RTC 1s 周期唤醒 + ALERT(EXTI0) 立即唤醒
 *
 * 功耗：STOP 下约 20µA + RTC 2µA；每秒醒一次做一次完整采样（约 10ms @36mA），
 *       平均 <0.1mA，3500mAh 电池从"4 天"变成"理论 3 年"（实际受自放电限制）。
 */
#ifndef LP_H
#define LP_H

#include <stdint.h>
#include <stdbool.h>

/* 配置 RTC(LSI) + 唤醒定时器 + EXTI0，必须在 BMS_Init 里调用一次 */
bool LP_Init(void);

/* 进入 STOP，直到 RTC 1s 到点或 ALERT 拉高。返回前已恢复 72MHz 与外设 */
void LP_SleepUntilEvent(void);

#endif /* LP_H */
