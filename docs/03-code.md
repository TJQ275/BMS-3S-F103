# 03 代码结构与接入

## 1. 分层

```
应用层   bms.c        调度、状态机、FET 策略、上报节奏
           │
策略层   protection.c 保护判定（延时/回差/锁存）
         balancing.c  被动均衡调度
         soc.c        SOC 估算
           │
驱动层   bq76920.c    AFE 寄存器、换算、保护阈值下发
         adc_ntc.c    MCU 侧 NTC
           │
通讯层   protocol.c   帧编解码、CRC、命令回调
           │
HAL      CubeMX 生成的 main.c / it.c / msp.c
```

依赖方向只向下，`soc.c`/`protection.c` 不碰 HAL，所以能在 PC 上直接跑测试
（`host_check/` 就是这么做的）。

## 2. 文件职责

| 文件 | 职责 | 你大概会改什么 |
|---|---|---|
| `bms_config.h` | **所有参数**：串数、容量、阈值、时间、引脚 | 90% 的调整都在这里 |
| `bq76920.c/h` | BOOT 脉冲、I2C 读写、电压/电流/温度换算、保护寄存器、FET、均衡、SHIP | 换 AFE 时才改 |
| `soc.c/h` | OCV 查表 + 安时积分 + 静置修正 | 换电芯化学体系时改 LUT |
| `protection.c/h` | 9 类故障的确认延时、恢复、锁存 | 阈值/时间 |
| `balancing.c/h` | 均衡条件与占空比 | 均衡电流/阈值 |
| `protocol.c/h` | 帧格式、CRC16、命令分发 | 加新命令 |
| `adc_ntc.c/h` | ADC+DMA、NTC 换算 | NTC 参数 |
| `bms.c/h` | 10ms/100ms/1s 调度，把上面串起来 | 业务逻辑 |
| `main.c` | 参考实现（CubeMX 生成的为准） | 只搬 USER CODE |

## 3. 运行节奏（最重要的一张图）

```
每 10ms   Proto_Poll()  → 解析上位机命令
          按键扫描 / ALERT 响应
每 100ms  BQ_ReadCurrent() → 有 CC_READY 才读 → SOC_Update()
每 1s     BQ_ReadAll()  → 单体/总压/温度
          Prot_Update() → 故障判定
          afe_fault_service() → 清 AFE 故障位
          fet_service() → CHG/DSG
          Bal_Update()  → 均衡掩码
          上报 1 帧
```

**为什么库仑计必须 100ms 读一次**：BQ76920 的 CC 寄存器每 250ms 更新一次并置 CC_READY，
读走即清。1 秒才读一次会丢掉 3/4 的电荷量，SOC 会明显偏慢。

## 4. 关键接口

```c
void BMS_Init(void);                 // main 里调一次
void BMS_Task(void);                 // while(1) 里一直调
const proto_telemetry_t *BMS_GetTelemetry(void);
const prot_state_t      *BMS_GetProtection(void);
```

底层（调试时常用）：

```c
BQ_Init();  BQ_ReadAll(&s);  BQ_ReadCurrent(&ma,&fresh);
BQ_SetFets(chg, dsg);  BQ_SetBalancing(mask);  BQ_Status();
SOC_GetPercent();  SOC_MarkFull();  SOC_SetCapacity(mah);
Prot_ClearLatch();
```

## 5. 三个最容易踩的坑

### 5.1 BOOT 引脚必须释放
`bq_boot_pulse()` 拉高 5ms 后，把 PB1 切成**输入（高阻）**。
如果一直拉高，AFE 内部温度测量会被干扰，温度读数会漂。

### 5.2 AFE 的 SYS_STAT 是"写 1 清"
`BQ_ClearStatus(0x04)` 才是清 OV；写 0 没有任何作用。
而且**必须先确认电压已恢复**再清，否则清了立刻又跳，看起来像"保护失效"。

### 5.3 均衡别在带载时做
`CFG_BAL_IDLE_MA` 默认 50mA，带载时测到的压差含内阻压降（10A × 5mΩ = 50mV！），
会把好电芯放掉。代码里已经强制静置 5 分钟才允许均衡。

## 6. 低功耗（如果要装进电池产品）

现状：F103@72MHz 约 30mA，一块 3500mAh 电池只能撑 4~5 天，不适合长期待机。三种做法：

1. **换 MCU**：STM32L431 STOP2 约 1µA，改一下 CubeMX 时钟即可；
2. **降频**：72MHz → 8MHz，约 5mA，I2C/时序都能跑；
3. **定时唤醒**：主循环改成 `HAL_SuspendTick() + __WFI()`，用 RTC 每 1s 唤醒一次做采样，
   其余时间 STOP 模式（AFE 的硬件保护在 MCU 睡着时仍然有效）。

改造点只在 `BMS_Task()` 外面包一层：
```c
while (1) {
  BMS_Task();
  if (idle) { HAL_SuspendTick(); HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI); SystemClock_Config(); HAL_ResumeTick(); }
}
```
注意 STOP 唤醒后要重新配置时钟，I2C 也要重新初始化。

## 7. 扩展方向

| 想加的功能 | 在哪加 |
|---|---|
| 蓝牙/CAN 上报 | `protocol.c` 抽一层 `transport_send()`，把 UART 换成 CAN |
| 数据记录 | 加 `nvm.c`，每 5 分钟写一页 Flash（记录 SOC/循环次数/故障） |
| 主动均衡 | 换 AFE（BQ76952）或外加电感式均衡板，`balancing.c` 换成开关矩阵 |
| 4S/5S | 改 `BMS_CELL_COUNT`、VC 接线、`CFG_*` 阈值、均衡电阻数量，代码不用动 |
| 上位机 GUI | `tools/bms_monitor.py` 已带 CSV，接 matplotlib/Qt 即可 |

## 8. 调试手段

1. **串口文本模式**：发 `5A A5 06 01 01 CRC` 切到文本，直接看
   `$BMS,c1,c2,c3,pack,I,T,soc,rem,flt,fet,bal`；
2. **断点看结构体**：`BMS_GetSample()` / `BMS_GetProtection()` 加进 Watch；
3. **I2C 抓包**：逻辑分析仪挂 PB6/PB7，确认地址是 `0x10`（7bit 0x08 << 1）；
4. **假 AFE 回归**：改完逻辑先在 PC 上跑 `host_check`，比上板快 10 倍。

## 9. 自测（PC 端）

```powershell
gcc -std=c99 -I host_check -I Firmware/Core/Inc host_check/test_main.c host_check/stub_hal.c Firmware/Core/Src/*.c -lm -o host_check/build/test.exe
host_check/build/test.exe
```
（`main.c` 需要排除，它依赖 CubeMX 的 HAL 初始化。）

覆盖：驱动换算、保护延时/回差、协议 CRC、SOC 积分、主循环冒烟、AFE 故障恢复、ALERT 抢占，共 33 项断言。

## 10. 已修复的真实 bug（复盘）

这些都是"编译通过 + 逻辑测试全绿"之后，按上电真实时序重新走查才发现的，值得记一笔：

### 10.1 AFE 锁存位死锁 → 充电永久打不开（严重）
- **现象**：触发过一次过压后，电压回落到 4.1V，CHG 仍然一直关着，只有断电重启才能恢复。
- **原因**：`Prot_Update()` 的过压条件写成 `vmax >= 4300 || (sys_stat & 0x04)`，
  而 `afe_fault_service()` 又要求"软件故障已恢复"才允许清 `0x04`。
  位没清 → 条件恒真 → 永远不清，互锁。
- **修复**：保护条件只用**测量值**；AFE 锁存位由 `afe_fault_service()` 按"测量恢复 + 延时"清除。
- **回归**：`test_afe_recovery()`；把两处旧逻辑还原后该用例会红 3 条（已用变异测试验证）。

### 10.2 ALERT 中断抢走库仑计 → SOC 冻结（严重，只在真机出现）
- **现象**：接上 ALERT 线后，SOC 一直停在初值不动。
- **原因**：10ms 任务里响应 ALERT 时读了 CC 寄存器，把 250ms 一次的样本读走并清了 `CC_READY`；
  100ms 任务从此拿不到 `fresh`，`SOC_Update()` 永不执行。
- **修复**：ALERT 分支只做"是否有故障位"的判断，CC 采样统一交给 100ms 任务。
- **回归**：`test_alert_keeps_soc()`（变异回旧代码后 SOC 保持 70.90% 不变，用例变红）。

### 10.3 上电首轮 ADC 未出数 → 电压全 0
- **原因**：`SYS_CTRL1.ADC_EN` 置位后约 250ms 才有第一组转换结果，`BQ_Init()` 立刻返回，
  `BMS_Init()` 马上读，拿到全 0；SOC 因此退化成默认 50%。
- **修复**：`BQ_Init()` 里等 250ms；`BMS_Init()` 再重试最多 5 次（顺便喂狗）。

### 10.4 I2C 读失败把全 0 喂给保护
- **原因**：`BQ_ReadAll()` 开头 memset，读失败后结构体全 0，保护层会判成欠压并锁存。
- **修复**：先读进临时结构体，成功才提交；失败保留上一份有效数据，靠 `BQ_CommFails()` 报通讯故障。

### 10.5 SCD 档位换算偏差
- **原因**：AFE 的短路阈值是离散档位（2mΩ 时 44mV≈22A、67mV≈33.5A）。
  配置 33A 会被向下取到 22A，和文档写的 33A 不符。
- **修复**：改成 34000mA，落在 67mV 档，实际 ≈33.5A。

### 10.6 串口溢出后接收停摆
- **原因**：HAL 在 ORE 后会停掉 `Receive_IT`，命令通道从此失效。
- **修复**：加 `HAL_UART_ErrorCallback()` 重新挂接收。

### 10.7 EXTI 中断的取舍
一开始想在 `bms.c` 里补 `EXTI0_IRQHandler`（弱符号），但 Keil AC5 与启动文件的弱导出冲突（L6200E）。
最终决定：**默认不开 EXTI 中断**，靠 1s 轮询；AFE 的硬件保护本来就不依赖 MCU。
需要毫秒级上报时，在 CubeMX 的 NVIC 里勾上 `EXTI line0` 重新生成即可，`HAL_GPIO_EXTI_Callback()` 已经写好了。

## 11. 掉电保存（nvm.c / nvm_port_stm32.c）

**存什么**：SOC、剩余容量、满容量、循环次数、历史故障、累计运行时间（32 字节一条）。

**怎么存**：Flash 最后一页（0x0800FC00）当环形日志，每条 32 字节带 CRC16，
顺序追加；写满 32 条才擦一次页。读取时扫全页、按序号找最新、跳过 CRC 坏掉的记录。

**为什么这么设计**：
- 直接"覆盖写"要先擦页，每次都擦太伤 Flash；追加写让擦除次数降到 1/32；
- 掉电时最多丢最后一条，前面的记录仍然有效（比结构体整体覆盖安全）；
- 读回后做合理性检查（SOC 0~100、容量 100~50000mAh），坏数据当没存过。

**调用点**：`BMS_Init()` 里 `NVM_Load()` 恢复；1s 任务里故障变化立即存、平时 5 分钟存一次。

⚠️ 该页位于 64KB Flash 的最后一页，当前代码 22.5KB，空间充足；
如果以后固件超过约 63KB，必须把 NVM 页挪走并改链接地址。

## 12. 低功耗（lp.c）

**策略**：满足"无充放电、无故障、没接充电器、上位机 10 秒没发数据"后进 STOP；
RTC 闹钟每秒唤醒做一次完整周期，ALERT 拉高则立即唤醒。

**F1 的坑**：STM32F1 的 RTC 是老版本（32 位计数器 + 闹钟），**没有** F4 那种 WakeUp Timer，
HAL 里也没有对应 API。所以 `lp.c` 直接操作 RTC/EXTI 寄存器：
`RCC->CSR` 开 LSI → `BDCR` 选 RTC 时钟 → `PRLL=39999` 得到 1Hz → 闹钟 = CNT+1。

**唤醒后的关键动作**：
1. 内核从 STOP 醒来跑在 HSI 8MHz，必须恢复 72MHz（`lp_clock_72()`）；
2. I2C/UART 的时序寄存器仍然是按 36MHz/72MHz 算的，恢复时钟后**不能**再调用 `MX_*_Init()`
   （HAL 在非 RESET 状态会返回错误，进而进 `Error_Handler`）；
3. 只需要把 UART 接收重新挂上、ADC+DMA 重启。

**两个容易忽略的点**：
- **IWDG 在 STOP 里继续计数**（LSI 独立时钟）。HAL_IWDG_Refresh 放在睡前和醒后各一次，
  2s 超时对 1s 左右的睡眠够用；如果 LSI 偏慢到 30kHz，RTC "1 秒"实际约 1.33s，仍在窗口内。
- **睡眠前要关库仑计**（`BQ_SetCcEnabled(false)`），否则 AFE 每 250ms 的 CC_READY
  会拉高 ALERT，把 MCU 每 250ms 叫醒一次，待机电流直接翻十几倍。此时无电流，关掉不丢积分。
