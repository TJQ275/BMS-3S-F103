# 3S 锂电 BMS（STM32F103 + BQ76920）

一个能真正做出来的 **3 串锂电池管理系统**：测量、SOC、保护、被动均衡、上位机通讯，
全部代码都在本目录，已在主机端用"假 AFE"跑通 33 项自测，并用 Keil 编译通过（0 error / 0 warning）。

```
3S 电池组 ──┬── BQ76920 (AFE) ── I2C ── STM32F103 ── UART ── 上位机
            │      │  │
            │    CHG DSG  →  两个 N-MOS（低边）
            └──  2mΩ 采样电阻（库仑计）
```

## 1. 指标

| 项目 | 值 |
|---|---|
| 电池 | 3S1P 18650 NMC，11.1V 标称 / 12.6V 满 / 9.0V 空，3500mAh |
| 持续放电 | 8A（峰值 15A） |
| 充电 | 外部 12.6V CC/CV 充电器，2A |
| 单体过压 / 欠压 | 4.25V / 2.80V（AFE 硬件跳闸） |
| 过流 / 短路 | 14A@80ms / ≈33.5A@200µs（档位离散，见 03 文档） |
| 均衡 | 被动，3.90V 起，压差 30mV，约 35mA/节 |
| 上报 | UART 115200，1Hz 二进制帧 + 可选文本帧 |
| 采样 | 单体电压 ±2mV、电流 ±1%、温度 ±0.5℃（校准后） |

## 2. 目录

```
BMS-3S-F103/
├── Firmware/Core/{Inc,Src}     纯代码，可挂到任意工程
├── CubeMX/                     ★ 已生成好的 CubeMX 工程（.ioc + Keil，编译通过）
│   ├── BMS-3S-F103.ioc
│   ├── Inc/ Src/ Drivers/
│   └── MDK-ARM/BMS-3S-F103.uvprojx + .hex
├── docs/                       选型 / CubeMX / 代码 / 测试
├── tools/bms_monitor.py        上位机
└── host_check/                 PC 端自测
```

## 3. 最快上手（工程已经生成好）

1. 双击 [CubeMX/BMS-3S-F103.ioc](<CubeMX/BMS-3S-F103.ioc>) → 看配置 / 改配置 → GENERATE CODE；
2. 或者直接双击 [CubeMX/MDK-ARM/BMS-3S-F103.uvprojx](<CubeMX/MDK-ARM/BMS-3S-F103.uvprojx>) → F7 编译（已验证 0 error / 0 warning）。

```
Program Size: Code=19298 RO-data=650 RW-data=136 ZI-data=2216
```

## 4. 五分钟跑通（不用硬件）

```powershell
cd BMS-3S-F103
gcc -std=c99 -I host_check -I Firmware/Core/Inc host_check/test_main.c host_check/stub_hal.c ^
    Firmware/Core/Src/bq76920.c Firmware/Core/Src/soc.c Firmware/Core/Src/protection.c ^
    Firmware/Core/Src/balancing.c Firmware/Core/Src/protocol.c Firmware/Core/Src/bms.c ^
    Firmware/Core/Src/adc_ntc.c -lm -o host_check/build/test.exe
./host_check/build/test.exe
```

会看到驱动换算、保护延时/回差、协议 CRC、SOC 积分、BMS 主循环五项全部通过。

## 5. 上真实硬件的顺序

1. **只焊电源部分**（AFE + LDO + 采样电阻），不接电池，用 12V 限流电源从 PACK+ 供电，
   测 3.3V、REGOUT、I2C 上拉是否正常；
2. 接 **单节 18650 + 假负载**（或电子负载），确认 CHG/DSG 能开关；
3. 接满 3S 电池，跑 `tools/bms_monitor.py` 看电压/电流/SOC；
4. 按 `docs/04-test-calibration.md` 做校准和故障注入。

**第一次上电务必串保险丝 + 用限流电源**，锂电短路电流可达几十安。

## 6. 关键设计决定（为什么这么做）

- **AFE 做硬件保护，MCU 只做"二级 + 策略"**：过压/欠压/过流/短路由 BQ76920 独立跳闸，
  即使 MCU 死机也不会炸；MCU 负责温度、通讯、SOC、均衡策略。
- **用 AFE 的库仑计，不用霍尔**：ACS712 之类的霍尔零点漂移大、分辨率低，
  做 SOC 积分几小时就飘；BQ 的 CC 是 8.44µV/LSB，配 2mΩ 分流器约 4mA 分辨率。
- **BMS ≠ 充电器**：CC/CV 由外部充电器完成，BMS 只决定"允不允许充"。
- **均衡只在静置时做**：带载时测到的压差含 IR drop，会把好电芯误放掉。
- **低功耗是这套方案的短板**：F103@72MHz 约 30mA，长期待机不合适。
  要装进电池供电的产品，换 STM32L431 / G030，或让 MCU 定时唤醒（见 docs/03）。

## 7. 自测覆盖

| # | 项目 | 断言 |
|---|---|---|
| 1 | 驱动 | 单体/总压/温度/电流换算、FET 回读 |
| 2 | 保护 | 确认延时、回差恢复、低温禁充 |
| 3 | 协议 | CRC 校验、命令解析、坏帧丢弃 |
| 4 | SOC | OCV 查表、1h@1A 积分误差 <2.5% |
| 5 | 主循环 | 1s 调度、遥测帧、默认 FET 状态 |
| 6 | AFE 故障恢复 | 电压恢复后锁存位能清掉、CHG 不锁死 |
| 7 | ALERT 抢占 | ALERT 每 10ms 来一次，SOC 仍正常积分 |

## 8. 安全声明

本项目涉及锂电池，**过充、过放、短路、机械损伤都可能导致热失控**。

- 本仓库是个人学习/原型项目，**不构成安全认证产品**，直接用于商业产品需自行完成 IEC 62133 / GB 31241 等认证；
- 首次上电务必串保险丝、用限流电源，并在有人看护的环境下进行；
- 任何因使用本项目造成的电池损坏、火灾或人身伤害，作者不承担责任。

## 9. 许可证

本仓库代码采用 **MIT**；`CubeMX/Drivers` 下的 ST HAL/CMSIS 归 STMicroelectronics，遵循其 BSD-3-Clause。详见 [LICENSE](LICENSE)。
