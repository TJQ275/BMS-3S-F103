/**
 * @file    adc_ntc.h
 * @brief   MOS/环境温度：ADC1 + DMA 循环采样 + 10k NTC(B3950)
 */
#ifndef ADC_NTC_H
#define ADC_NTC_H

#include <stdint.h>

#define NTC_PULLUP_OHM   10000.0f
#define NTC_BETA         3950.0f
#define NTC_T0_K         298.15f

void    NTC_Start(void);
int16_t NTC_GetTempC10(uint8_t idx);   /* idx: 0=MOS, 1=环境 */
float   NTC_AdcToCelsius(uint16_t adc);

#endif /* ADC_NTC_H */
