/**
 * @file    adc_ntc.c
 */
#include "bms_hal.h"
#include "adc_ntc.h"

#include <math.h>
#include <stdbool.h>

/* CubeMX 里 DMA 目标缓冲：3 个通道顺序搬运 */
uint16_t g_adc_dma_buf[3];

static bool s_started;

void NTC_Start(void)
{
    if (s_started) return;
    (void)HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_adc_dma_buf, 3);
    s_started = true;
}

float NTC_AdcToCelsius(uint16_t adc)
{
    float r, t;

    if (adc == 0 || adc >= 4095) return -127.0f;   /* 开路/短路 */

    /* NTC 在下，10k 上拉到 3.3V */
    r = NTC_PULLUP_OHM * (float)adc / (4095.0f - (float)adc);
    if (r < 50.0f || r > 2000000.0f) return -127.0f;

    t = 1.0f / (1.0f / NTC_T0_K + logf(r / NTC_PULLUP_OHM) / NTC_BETA) - 273.15f;

    if (t < -60.0f)  t = -60.0f;
    if (t > 150.0f)  t = 150.0f;
    return t;
}

int16_t NTC_GetTempC10(uint8_t idx)
{
    float c;

    if (!s_started) NTC_Start();
    if (idx > 2) return 0;

    c = NTC_AdcToCelsius(g_adc_dma_buf[idx]);
    return (int16_t)lroundf(c * 10.0f);
}
