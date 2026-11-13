/**
 * @file    bms_hal.h
 * @brief   统一引入 HAL 句柄（CubeMX 工程里这些定义在 i2c.h/usart.h/... 中）
 *
 * 这样 bms 模块既能挂到 CubeMX 工程里，也能在 PC 端用 stub main.h 编译。
 */
#ifndef BMS_HAL_H
#define BMS_HAL_H

#include "main.h"

extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart1;
extern ADC_HandleTypeDef  hadc1;
extern IWDG_HandleTypeDef hiwdg;

#endif /* BMS_HAL_H */
