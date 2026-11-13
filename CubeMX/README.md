# BMS-3S-F103 —— STM32CubeMX 工程

已经用 **STM32CubeMX 6.17.0 + STM32Cube FW_F1 V1.8.7** 生成好，并用 **Keil MDK 5.38** 编译通过。

```
CubeMX/
├── BMS-3S-F103.ioc          ← 用 CubeMX 打开这个
├── Core? 否：本工程用扁平结构
├── Inc/                     ← 生成的 .h + 本项目的 bms_*.h
├── Src/                     ← 生成的 .c + 本项目的 bms_*.c
├── Drivers/                 ← HAL + CMSIS（CubeMX 自动拷贝）
└── MDK-ARM/
    ├── BMS-3S-F103.uvprojx  ← Keil 工程
    ├── BMS-3S-F103.hex      ← 已编译好的固件（可直接烧）
    └── build.log 不包含
```

## 1. 打开方式

- **改配置**：双击 `BMS-3S-F103.ioc` → CubeMX 打开 → 改完点 **GENERATE CODE**；
- **编译/下载**：双击 `MDK-ARM/BMS-3S-F103.uvprojx` → Keil 打开 → F7 编译 → F8 下载。

## 2. 已配好的内容

| 外设 | 配置 |
|---|---|
| RCC | HSE 8MHz 晶振，PLL×9 → 72MHz，APB1=36MHz，ADC=PCLK2/6=12MHz |
| I2C1 | PB6/PB7，Fast Mode **400kHz**，Duty 2 |
| USART1 | PA9/PA10，**115200 8N1**，开全局中断（优先级 6） |
| ADC1 | IN0/IN1/IN4（PA0/PA1/PA4），Scan + Continuous + **DMA1_CH1 Circular/HalfWord**，239.5 周期 |
| IWDG | 预分频 64，重装 1250（≈2s） |
| GPIO | PB0=ALERT(EXTI 上升沿/下拉)、PB1=BOOT(输出)、PB5=CHG_DET(下拉输入)、PB12=LED、PB13=BUZZER、PB14=KEY(上拉) |
| 工具链 | MDK-ARM V5，C99，堆 0x200 / 栈 0x400 |

编译结果：

```
Program Size: Code=19298 RO-data=650 RW-data=136 ZI-data=2216
0 Error(s), 0 Warning(s)
```

## 3. 本项目加进工程的文件

`Src/`：`bms.c  bq76920.c  soc.c  protection.c  balancing.c  protocol.c  adc_ntc.c`
`Inc/`：`bms.h  bq76920.h  soc.h  protection.h  balancing.h  protocol.h  adc_ntc.h  bms_config.h  bms_hal.h`

`main.c` 里只加了 3 行（在 USER CODE 段，重新生成代码不会被删）：

```c
#include "bms.h"      /* USER CODE BEGIN Includes */
BMS_Init();           /* USER CODE BEGIN 2 */
BMS_Task();           /* USER CODE BEGIN 3 */
```

## 4. 两个已知点

1. **EXTI0 默认没开中断**：PB0 已配成 EXTI（上升沿/下拉），但 NVIC 里的 EXTI0 没勾，
   软件靠 1s 轮询读 AFE 状态。AFE 的硬件保护是独立的，不影响安全。
   要毫秒级故障上报：CubeMX → System Core → NVIC → 勾 `EXTI line0 interrupt` → 重新生成，
   `stm32f1xx_it.c` 会生成 handler，自动回调 `bms.c` 里的 `HAL_GPIO_EXTI_Callback`。
   （自己手写 handler 会和启动文件的弱符号冲突，Keil AC5 报 L6200E，别踩这个坑。）
2. **重新生成代码后**：CubeMX 会重写 `.uvprojx`，本项目那 7 个 `.c` 可能从 Keil 工程里消失
   （文件还在磁盘上）。到 Keil 里 `Add Existing Files to Group 'Application/User/Core'` 重新加一次即可。

## 5. 烧录

Keil 里选 ST-Link（Options → Debug → ST-Link Debugger），Flash Download 勾 **Reset and Run**，
F8 下载。上电后 LED 1Hz 闪＝运行正常，100ms 快闪＝有故障。
