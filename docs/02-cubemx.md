# 02 STM32CubeMX 配置（STM32F103C8T6）

> **好消息：这部分已经替你做完了。**
> `CubeMX/BMS-3S-F103.ioc` 是已生成并经 **CubeMX 6.17 无界面生成 + Keil MDK 5.38 编译通过**（0 error / 0 warning）的工程，
> 直接双击打开、改配置、点 GENERATE CODE 即可；对应的 Keil 工程在 `CubeMX/MDK-ARM/`。
>
> 下面保留完整的手工配置步骤，用于：① 核对 `CubeMX/README.md` 里的说明；② 你想从零自己配一遍练手；
> ③ 换成别的芯片（4S/5S 或 G0/G4）时照着重配。

---

## 0. 已完成的配置一览（对应 .ioc）

| 项目 | 值 | 对应代码位置 |
|---|---|---|
| 芯片 | STM32F103C8Tx（LQFP48） | — |
| 时钟 | HSE 8MHz，PLL×9 = 72MHz，APB1 /2，ADC /6 = 12MHz | `SystemClock_Config` |
| I2C1 | PB6/PB7，400kHz Fast Mode | `i2c.c` |
| USART1 | PA9/PA10，115200，中断优先级 6 | `usart.c` |
| ADC1 | IN0/IN1/IN4，Scan+Continuous+DMA Circular | `adc.c` + `adc_ntc.c` |
| IWDG | 64 分频，1250 重装（≈2s） | `iwdg.c` |
| GPIO | ALERT/BOOT/CHG_DET/LED/BUZZER/KEY 六个标签 | `gpio.c` + `main.h` |
| 工具链 | MDK-ARM V5 | `MDK-ARM/*.uvprojx` |

生成后的实测：

```
Program Size: Code=19298 RO-data=650 RW-data=136 ZI-data=2216
0 Error(s), 0 Warning(s)
```

---


目标：生成一个能直接编译本仓库 `Firmware/Core` 的 Keil / CubeIDE 工程。

## 0. 准备

1. 安装 **STM32CubeMX 6.x** + **STM32CubeF1 固件包**（Help → Manage embedded software packages → STM32F1）；
2. 芯片选 **STM32F103C8Tx**（LQFP48，64KB Flash / 20KB RAM）。

## 1. 新建工程

`File → New Project → Part Number → STM32F103C8Tx → Start Project`。

## 2. 引脚分配（Pinout view 直接点）

| 引脚 | 功能 | CubeMX 里的模式 | 用户标签 |
|---|---|---|---|
| PA0 | ADC1_IN0 | ADC1_IN0 | NTC_MOS |
| PA1 | ADC1_IN1 | ADC1_IN1 | NTC_AMB |
| PA4 | ADC1_IN4 | ADC1_IN4 | VBAT_SENSE（备用） |
| PA9 | USART1_TX | USART1_TX | — |
| PA10 | USART1_RX | USART1_RX | — |
| PB0 | EXTI0 | GPIO_EXTI0 | ALERT |
| PB1 | GPIO | GPIO_Output | BOOT |
| PB5 | GPIO | GPIO_Input | CHG_DET |
| PB6 | I2C1_SCL | I2C1_SCL | — |
| PB7 | I2C1_SDA | I2C1_SDA | — |
| PB12 | GPIO | GPIO_Output | LED |
| PB13 | GPIO | GPIO_Output | BUZZER |
| PB14 | GPIO | GPIO_Input | KEY |
| PC13 | GPIO | GPIO_Output | LED2（可选，板载灯） |

> **标签必须和代码里的名字一致**（`ALERT`/`BOOT`/`CHG_DET`/`LED`/`BUZZER`/`KEY`），
> 否则 `bms_config.h` 里的兜底宏会生效，按键/灯就对不上号。
> PA13/PA14 是 SWD，**不要动**。

## 3. System Core

### RCC
`System Core → RCC`：
- **High Speed Clock (HSE)**：`Crystal/Ceramic Resonator`
- LSE：Disable（用不到 RTC）

### SYS
`System Core → SYS`：
- **Debug**：`Serial Wire`
- **Timebase Source**：`SysTick`

### GPIO（逐个引脚）
| 引脚 | GPIO mode | Pull | 说明 |
|---|---|---|---|
| PB0 / ALERT | `External Interrupt Mode with Rising edge trigger` | **Pull-down** | BQ 的 ALERT 高有效 |
| PB1 / BOOT | `Output Push Pull` | No pull-up | 上电后代码会切成高阻 |
| PB5 / CHG_DET | `Input mode` | **Pull-down** | 有充电器时拉高 |
| PB12 / LED | `Output Push Pull` | No pull-up | 低电平灭 |
| PB13 / BUZZER | `Output Push Pull` | No pull-up | 经 NPN 驱动蜂鸣器 |
| PB14 / KEY | `Input mode` | **Pull-up** | 按下为低 |
| PC13 / LED2 | `Output Push Pull` | No pull-up | 可选 |

### IWDG
`System Core → IWDG` → **Activated**：
- Prescaler：`IWDG_PRESCALER_64`（LSI 40kHz/64 ≈ 625Hz）
- Down-counter reload：`1250` → 约 2s 超时

> 调试时 IWDG 会让程序在断点处被复位：Keil 里勾 `Debug → Settings → DBGMCU → Freeze Watchdog`，
> 或调试阶段先注释掉 `HAL_IWDG_Init`。

### NVIC
`System Core → NVIC`：
| 中断 | 使能 | 抢占优先级 |
|---|---|---|
| EXTI line0 interrupt | ✅ | 5 |
| USART1 global interrupt | ✅ | 6 |
| DMA1 channel1 global interrupt | ✅ | 5 |

优先级分组保持默认 **4 bits for pre-emption priority**（HAL_Init 里设置）。

## 4. Clock Configuration

`Clock Configuration` 页，从上到下依次选（或直接填数字）：

| 项目 | 值 |
|---|---|
| Input frequency (HSE) | 8 MHz |
| PLL Source | HSE |
| PLLMul | **×9** |
| SYSCLK | 72 MHz |
| AHB Prescaler | /1 → 72 MHz |
| APB1 Prescaler | **/2 → 36 MHz**（I2C1 的上限） |
| APB2 Prescaler | /1 → 72 MHz |
| ADC Prescaler | **/6 → 12 MHz**（F1 的 ADC 上限 14MHz） |
| USB Prescaler | /1.5（不用 USB 也无所谓） |

> 如果板子上没有 8MHz 晶振，可以先用 `HSI` + PLL，但 I2C 400k 的时序误差会变大，
> 建议还是焊晶振。

## 5. 外设参数

### I2C1
- **I2C**：`I2C`
- Speed Mode：`Fast Mode`
- **Clock Speed：400000 Hz**
- Duty Cycle：`Tlow/Thigh = 2`
- 其余默认（7 位地址、无通用呼叫、时钟延展使能）

### USART1
- Mode：`Asynchronous`
- Baud Rate：**115200**
- Word Length：8 Bits（无校验）
- Stop Bits：1
- 硬件流控：None
- **NVIC 里使能 USART1 global interrupt**（接收用中断）

### ADC1
`Analog → ADC1`，勾选 `IN0`、`IN1`、`IN4`。

**Parameter Settings**：

| 参数 | 值 |
|---|---|
| Scan Conversion Mode | **Enabled** |
| Continuous Conversion Mode | **Enabled** |
| Discontinuous Conversion Mode | Disabled |
| External Trigger Conversion Source | `Software start` |
| Data Alignment | Right |
| **Number of Conversion** | **3** |
| Rank 1 | Channel 0，Sampling Time **239.5 Cycles** |
| Rank 2 | Channel 1，239.5 Cycles |
| Rank 3 | Channel 4，239.5 Cycles |

**DMA Settings** → `Add` → `ADC1`：
| 参数 | 值 |
|---|---|
| Direction | Peripheral To Memory |
| Priority | Low |
| Mode | **Circular** |
| Increment Address | Memory ✅ / Peripheral ❌ |
| Data Width | 都是 **Half Word** |

> 顺序很重要：`g_adc_dma_buf[0..2]` 依次对应 Rank1/2/3，也就是 PA0 / PA1 / PA4。

## 6. Project Manager

**Project**：
- Toolchain / IDE：`MDK-ARM`（V5）或 `STM32CubeIDE`
- Project Name / Location：自己定

**Code Generator**：
- ✅ Copy only the necessary library files
- ✅ Generate peripheral initialization as a pair of .c/.h files
- ✅ Keep User Code when re-generating

**Advanced Settings** 默认即可。

点 **GENERATE CODE**。

## 7. 生成之后（关键）

### 7.1 加入本仓库的源文件
把本目录的 `Firmware/Core/Inc`、`Firmware/Core/Src` 里的文件拷进工程（或直接指过去）：

工程里应包含：
```
bq76920.c  soc.c  protection.c  balancing.c  protocol.c  adc_ntc.c  bms.c
```

**不要**把本仓库的 `main.c` 直接覆盖 CubeMX 生成的 `main.c`，只在对应 USER CODE 段加两行：

```c
/* USER CODE BEGIN Includes */
#include "bms.h"
/* USER CODE END Includes */

/* USER CODE BEGIN 2 */
BMS_Init();
/* USER CODE END 2 */

/* USER CODE BEGIN WHILE */
while (1)
{
  /* USER CODE BEGIN 3 */
  BMS_Task();
  /* USER CODE END 3 */
}
```

### 7.2 Include 路径
`Options for Target → C/C++ → Include Paths` 添加：
```
..\Firmware\Core\Inc
```

### 7.3 编译选项
- **C99**：AC6 默认；AC5 要在 `Misc Controls` 加 `--c99`（代码里用了循环内声明变量）；
- ✅ **Use MicroLIB**（`snprintf` 体积小）；
- 优化 `-O1` 或 `-Os`；
- 若报 `snprintf` 未定义，确认勾了 MicroLIB 或包含 stdio。

### 7.4 中断文件
CubeMX 会在 `stm32f1xx_it.c` 里生成 `USART1_IRQHandler`，它调用 `HAL_UART_IRQHandler`，
最终回调到 `bms.c` 的 `HAL_UART_RxCpltCallback`。

**EXTI0 默认没有勾**（生成的 `.ioc` 里这条被 CubeMX 丢了）：PB0 仍配成 EXTI 上升沿，但 NVIC 未使能，
所以不会有中断。需要时在 CubeMX 的 NVIC 面板手动勾 `EXTI line0 interrupt` 再生成，
`stm32f1xx_it.c` 会补上 `EXTI0_IRQHandler` → `HAL_GPIO_EXTI_Callback`。
不要自己在 `bms.c` 里写这个 handler：Keil AC5 会和启动文件的弱导出冲突（L6200E）。

**如果编译报重复定义**：说明你在别处也写了这两个回调，删掉其中一份（保留 `bms.c` 的）。

## 8. 烧录

- ST-Link：`Debug → ST-Link Debugger`，Flash Download 勾 `Reset and Run`；
- 串口：PA9(TX) 接 USB-TTL 的 RX，PA10(RX) 接 TX，GND 共地，115200 8N1；
- 上电后 LED 1Hz 闪 = 正常；100ms 快闪 = 有故障。

## 9. CubeMX 配置自检清单

- [ ] HSE 频率和板上晶振一致（8MHz）
- [ ] APB1 = 36MHz
- [ ] ADC 时钟 ≤14MHz
- [ ] I2C1 = 400k Fast Mode
- [ ] USART1 中断已使能
- [ ] ADC 的 Number of Conversion = 3，DMA Circular + Half Word
- [ ] PB0 是 EXTI **上升沿** + **下拉**
- [ ] IWDG 已激活（调试时记得 freeze）
- [ ] 标签名与代码一致
- [ ] Include 路径加了 `Firmware/Core/Inc`
- [ ] 编译选项 C99 + MicroLIB