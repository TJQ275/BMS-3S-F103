#ifndef MAIN_H
#define MAIN_H

#include "stm32f1xx_hal.h"

extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart1;
extern ADC_HandleTypeDef  hadc1;
extern IWDG_HandleTypeDef hiwdg;

void Error_Handler(void);

#endif
