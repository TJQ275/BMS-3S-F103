/* 主机端：HAL 桩 + 一个假的 BQ76920 寄存器模型，用来跑通驱动和 BMS 主循环 */
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>

I2C_HandleTypeDef  hi2c1;
UART_HandleTypeDef huart1;
ADC_HandleTypeDef  hadc1;
IWDG_HandleTypeDef hiwdg;

uint8_t  g_regs[256];          /* 假 AFE 寄存器 */
uint8_t  g_uart_tx[512];
uint16_t g_uart_tx_len;
static uint32_t s_tick = 1000;

void     stub_set_tick(uint32_t t) { s_tick = t; }
uint32_t stub_get_tick(void) { return s_tick; }
void     stub_clear_uart(void) { g_uart_tx_len = 0; }

/* ---------- 假 AFE ---------- */
void stub_afe_reset(void)
{
    memset(g_regs, 0, sizeof(g_regs));
    g_regs[0x50] = 0x0C;   /* ADCGAIN1 -> 贡献 24 */
    g_regs[0x59] = 0xE0;   /* ADCGAIN2 -> 贡献 7  => gain = 396 µV/LSB */
    g_regs[0x51] = 0x00;   /* ADCOFFSET = 0 */
    g_regs[0x00] = 0x00;
}

static uint16_t gain_now(void)
{
    return (uint16_t)(365 + (((g_regs[0x50] & 0x0C) << 1) | ((g_regs[0x59] & 0xE0) >> 5)));
}

void stub_afe_set_cell(int idx, uint16_t mv)
{
    uint16_t raw = (uint16_t)((uint32_t)mv * 1000u / gain_now());
    g_regs[0x0C + 2 * idx] = (uint8_t)(raw >> 8);
    g_regs[0x0D + 2 * idx] = (uint8_t)(raw & 0xFF);
}

void stub_afe_set_pack(uint16_t mv, int cells)
{
    uint16_t raw = (uint16_t)((uint32_t)mv * 1000u / (4u * gain_now()));
    g_regs[0x2A] = (uint8_t)(raw >> 8);
    g_regs[0x2B] = (uint8_t)(raw & 0xFF);
    (void)cells;
}

void stub_afe_set_current_ma(int32_t ma, float shunt_mohm)
{
    int16_t raw = (int16_t)((float)ma * shunt_mohm / 8.44f);
    g_regs[0x32] = (uint8_t)((uint16_t)raw >> 8);
    g_regs[0x33] = (uint8_t)((uint16_t)raw & 0xFF);
    g_regs[0x00] |= 0x80;          /* CC_READY */
}

void stub_afe_set_temp_c(int16_t c)
{
    /* R_ntc = 10k * exp(B*(1/T - 1/T0)) */
    double t0 = 298.15, tt = (double)c + 273.15;
    double r = 10000.0 * exp((double)3950 * (1.0 / tt - 1.0 / t0));
    double vts = 3300.0 * r / (10000.0 + r);
    uint16_t adc = (uint16_t)(vts / 0.382);
    g_regs[0x2C] = (uint8_t)((adc >> 8) & 0x3F);
    g_regs[0x2D] = (uint8_t)(adc & 0xFF);
}

/* ---------- HAL 实现 ---------- */
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t a, uint16_t r,
                                   uint16_t s, uint8_t *b, uint16_t n, uint32_t t)
{
    (void)h; (void)a; (void)s; (void)t;
    for (uint16_t i = 0; i < n; i++) b[i] = g_regs[(r + i) & 0xFF];
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t a, uint16_t r,
                                    uint16_t s, uint8_t *b, uint16_t n, uint32_t t)
{
    (void)h; (void)a; (void)s; (void)t;
    if (n != 1) return HAL_ERROR;
    if (r == 0x00) g_regs[0] &= (uint8_t)~b[0];   /* 写 1 清 */
    else           g_regs[r] = b[0];
    return HAL_OK;
}

void     HAL_Delay(uint32_t ms) { s_tick += ms; }
uint32_t HAL_GetTick(void) { return s_tick; }
void     HAL_GPIO_Init(GPIO_TypeDef *p, GPIO_InitTypeDef *i) { (void)p; (void)i; }
void     HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t pin, uint32_t st)
{ (void)p; (void)pin; (void)st; }
uint32_t HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t pin) { (void)p; (void)pin; return 1u; }
HAL_StatusTypeDef HAL_ADC_Start_DMA(ADC_HandleTypeDef *h, uint32_t *b, uint32_t n)
{ (void)h; (void)b; (void)n; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Stop_DMA(ADC_HandleTypeDef *h) { (void)h; return HAL_OK; }

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *b, uint16_t n, uint32_t t)
{
    (void)h; (void)t;
    if ((uint32_t)g_uart_tx_len + n <= sizeof(g_uart_tx)) {
        memcpy(&g_uart_tx[g_uart_tx_len], b, n);
        g_uart_tx_len = (uint16_t)(g_uart_tx_len + n);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *b, uint16_t n)
{ (void)h; (void)b; (void)n; return HAL_OK; }

HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *h) { (void)h; return HAL_OK; }
void HAL_NVIC_SetPriority(int irq, uint32_t pre, uint32_t sub) { (void)irq; (void)pre; (void)sub; }

/* 低功耗在 PC 上不启用 */
bool LP_Init(void) { return false; }
void LP_SleepUntilEvent(void) { }
void HAL_NVIC_EnableIRQ(int irq) { (void)irq; }
void HAL_GPIO_EXTI_IRQHandler(uint16_t pin) { (void)pin; }

void Error_Handler(void) { printf("Error_Handler\n"); }
