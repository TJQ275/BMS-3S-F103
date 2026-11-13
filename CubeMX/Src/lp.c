/**
 * @file    lp.c
 * @brief   STOP 低功耗：RTC 闹钟每秒唤醒 + ALERT(EXTI0) 立即唤醒
 *
 * 为什么直接写寄存器：
 *   STM32F1 的 RTC 是老版本（只有一个 32 位计数器 + 闹钟），没有 F4 那种
 *   WakeUp Timer，HAL_RTCEx 里也没有对应 API。直接操作 RTC/EXTI 寄存器最可靠。
 *
 * 功耗：STOP 约 20µA + LSI/RTC 约 2µA；每秒醒 10ms 做一次采样，平均 <0.1mA。
 */
#include "bms_hal.h"
#include "bms_config.h"
#include "lp.h"
#include "bms.h"
#include "adc_ntc.h"

static bool s_ready;
static volatile uint8_t s_wake_source;   /* 1=RTC 2=ALERT */

/* ---------------- 时钟恢复 ---------------- */
static void lp_clock_72(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};

    osc.OscillatorType  = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState        = RCC_HSE_ON;
    osc.HSEPredivValue  = RCC_HSE_PREDIV_DIV1;
    osc.HSIState        = RCC_HSI_ON;
    osc.PLL.PLLState    = RCC_PLL_ON;
    osc.PLL.PLLSource   = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL      = RCC_PLL_MUL9;
    (void)HAL_RCC_OscConfig(&osc);

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);

    pclk.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    pclk.AdcClockSelection    = RCC_ADCPCLK2_DIV6;
    (void)HAL_RCCEx_PeriphCLKConfig(&pclk);
}

/* 下一个整秒触发 */
static void rtc_set_next_alarm(void)
{
    RTC->CRL |= RTC_CRL_CNF;
    RTC->ALRH = 0;
    RTC->ALRL = (uint16_t)(RTC->CNTL + 1u);
    RTC->CRL &= (uint16_t)~RTC_CRL_CNF;
}

/* ---------------- 初始化 ---------------- */
bool LP_Init(void)
{
    uint32_t t0;

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    PWR->CR |= PWR_CR_DBP;              /* 解锁备份域 */

    RCC->CSR |= RCC_CSR_LSION;          /* F1 的 LSI 使能在 RCC_CSR，不在 BDCR */
    t0 = HAL_GetTick();
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {
        if ((uint32_t)(HAL_GetTick() - t0) > 200u) return false;
    }

    RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | RCC_BDCR_RTCSEL_LSI;
    RCC->BDCR |= RCC_BDCR_RTCEN;

    /* LSI≈40kHz，分频 40000 -> 1Hz 计数 */
    RTC->CRL |= RTC_CRL_CNF;
    RTC->PRLH = 0;
    RTC->PRLL = 39999u;
    RTC->CRL &= (uint16_t)~RTC_CRL_CNF;

    RTC->CRL &= (uint16_t)~RTC_CRL_RSF;
    t0 = HAL_GetTick();
    while (!(RTC->CRL & RTC_CRL_RSF)) {
        if ((uint32_t)(HAL_GetTick() - t0) > 200u) return false;
    }

    RTC->CRH |= RTC_CRH_ALRIE;
    rtc_set_next_alarm();

    EXTI->IMR  |= EXTI_IMR_MR17;        /* RTC 闹钟 -> EXTI17 */
    EXTI->RTSR |= EXTI_RTSR_TR17;

    HAL_NVIC_SetPriority(RTC_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(RTC_IRQn);
    HAL_NVIC_SetPriority(EXTI0_IRQn, 4, 0);   /* ALERT 更急 */
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    s_ready = true;
    return true;
}

/* ---------------- 睡眠 ---------------- */
void LP_SleepUntilEvent(void)
{
    if (!s_ready) return;

    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);

    /* 关库仑计：否则 CC_READY 每 250ms 拉高 ALERT，把 MCU 反复唤醒。
     * 睡眠条件保证此时无电流，关掉不会丢积分。 */
    (void)BQ_SetCcEnabled(false);

    __HAL_GPIO_EXTI_CLEAR_IT(ALERT_Pin);
    EXTI->PR = EXTI_PR_PR17;            /* 清掉可能挂着闹钟事件 */
    s_wake_source = 0;

    HAL_IWDG_Refresh(&hiwdg);           /* STOP 期间 IWDG 仍在走 */

    HAL_SuspendTick();
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

    /* 醒来：内核回到 HSI，恢复主频；I2C/UART 的时序寄存器仍然有效 */
    lp_clock_72();

#ifdef __HAL_UART_CLEAR_OREFLAG
    __HAL_UART_CLEAR_OREFLAG(&huart1);
#endif
    (void)BQ_SetCcEnabled(true);        /* 恢复库仑计 */
    BMS_RestartRx();
    NTC_Restart();

    HAL_ResumeTick();
    HAL_IWDG_Refresh(&hiwdg);
}

/* ---------------- 中断 ---------------- */
void RTC_IRQHandler(void)
{
    if (RTC->CRL & RTC_CRL_ALRF) {
        RTC->CRL &= (uint16_t)~RTC_CRL_ALRF;
        EXTI->PR = EXTI_PR_PR17;
        rtc_set_next_alarm();
        s_wake_source = 1;
    }
}

void EXTI0_IRQHandler(void)
{
    s_wake_source = 2;
    HAL_GPIO_EXTI_IRQHandler(ALERT_Pin);
}
