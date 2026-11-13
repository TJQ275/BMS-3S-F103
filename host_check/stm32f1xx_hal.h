/* 主机端编译用的最小 HAL 桩，只为语法/类型检查，不参与固件 */
#ifndef STM32F1XX_HAL_H
#define STM32F1XX_HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

typedef struct { int dummy; } GPIO_TypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed; } GPIO_InitTypeDef;

#define GPIOA ((GPIO_TypeDef *)1)
#define GPIOB ((GPIO_TypeDef *)2)
#define GPIOC ((GPIO_TypeDef *)3)

#define GPIO_PIN_0  (1u << 0)
#define GPIO_PIN_1  (1u << 1)
#define GPIO_PIN_2  (1u << 2)
#define GPIO_PIN_3  (1u << 3)
#define GPIO_PIN_4  (1u << 4)
#define GPIO_PIN_5  (1u << 5)
#define GPIO_PIN_6  (1u << 6)
#define GPIO_PIN_7  (1u << 7)
#define GPIO_PIN_8  (1u << 8)
#define GPIO_PIN_9  (1u << 9)
#define GPIO_PIN_10 (1u << 10)
#define GPIO_PIN_11 (1u << 11)
#define GPIO_PIN_12 (1u << 12)
#define GPIO_PIN_13 (1u << 13)
#define GPIO_PIN_14 (1u << 14)
#define GPIO_PIN_15 (1u << 15)

#define GPIO_PIN_RESET 0u
#define GPIO_PIN_SET   1u

#define GPIO_MODE_INPUT      0u
#define GPIO_MODE_OUTPUT_PP  1u
#define GPIO_MODE_AF_PP      2u
#define GPIO_MODE_AF_OD      3u
#define GPIO_MODE_ANALOG     4u
#define GPIO_MODE_IT_RISING  5u

#define GPIO_NOPULL   0u
#define GPIO_PULLUP   1u
#define GPIO_PULLDOWN 2u

#define GPIO_SPEED_FREQ_LOW  0u
#define GPIO_SPEED_FREQ_HIGH 1u

typedef struct { void *Instance; } I2C_HandleTypeDef;
typedef struct { void *Instance; } UART_HandleTypeDef;
typedef struct { void *Instance; void *DMA_Handle; } ADC_HandleTypeDef;
typedef struct { void *Instance; } IWDG_HandleTypeDef;
typedef struct { void *Instance; } DMA_HandleTypeDef;

#define USART1 ((void *)0x40013800)
#define I2C1   ((void *)0x40005400)
#define ADC1   ((void *)0x40012400)
#define IWDG   ((void *)0x40003000)

#define I2C_MEMADD_SIZE_8BIT 1u

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t a, uint16_t r,
                                   uint16_t s, uint8_t *b, uint16_t n, uint32_t t);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t a, uint16_t r,
                                    uint16_t s, uint8_t *b, uint16_t n, uint32_t t);
void     HAL_Delay(uint32_t ms);
uint32_t HAL_GetTick(void);
void     HAL_GPIO_Init(GPIO_TypeDef *p, GPIO_InitTypeDef *i);
void     HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t pin, uint32_t state);
uint32_t HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t pin);
HAL_StatusTypeDef HAL_ADC_Start_DMA(ADC_HandleTypeDef *h, uint32_t *buf, uint32_t len);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *b, uint16_t n, uint32_t t);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *b, uint16_t n);
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *h);
void HAL_NVIC_SetPriority(int irq, uint32_t pre, uint32_t sub);
void HAL_NVIC_EnableIRQ(int irq);
void HAL_GPIO_EXTI_IRQHandler(uint16_t pin);

#define EXTI0_IRQn 6

#endif
