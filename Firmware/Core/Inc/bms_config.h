/**
 * @file    bms_config.h
 * @brief   3S 锂电 BMS 全部可调参数（改这里就够了）
 *
 * 硬件：STM32F103C8T6 + BQ76920 + 2mΩ 分流器 + 3S1P 18650
 * 约定：电流正 = 充电，负 = 放电（若实测相反，改 BMS_CURRENT_SIGN）
 */
#ifndef BMS_CONFIG_H
#define BMS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ========================= 1. 电池组 ========================= */
#define BMS_CELL_COUNT            3          /* 串联节数，BQ76920 支持 3~5 */
#define BMS_CELL_CAPACITY_MAH     3500.0f    /* 单体容量 mAh（3S1P 18650） */

/* ========================= 2. 硬件标定 ========================= */
#define BQ_I2C_ADDR              (0x08 << 1) /* BQ76920 7bit 地址 0x08 */
#define BQ_SHUNT_MOHM            2.0f        /* 分流电阻 mΩ，与实物一致 */
#define BQ_THERMISTOR_BETA       3950        /* TS1 上 10k NTC 的 B 值 */
#define BMS_CURRENT_SIGN         (+1)        /* 电流符号：+1 或 -1 */

/* 软件电压校准（多节一致性修正，默认 1.0 / 0）
 * 用万用表逐节对比后填：修正值 = 实测 / 读数 的微调 */
#define BMS_CAL_CELL_GAIN        1.0000f
#define BMS_CAL_CELL_OFFSET_MV   0

/* ========================= 3. 保护阈值 ========================= */
/* 3S NMC 18650：单体 4.20V 满 / 3.00V 空 */
#define CFG_OV_MV                4250        /* 单体过压跳闸（AFE 硬件） */
#define CFG_OV_RECOVER_MV        4100        /* 过压恢复 */
#define CFG_OV_DELAY_S           2

#define CFG_UV_MV                2800        /* 单体欠压跳闸 */
#define CFG_UV_RECOVER_MV        3000
#define CFG_UV_DELAY_S           4

#define CFG_OCD_MA               14000       /* 放电过流（AFE） */
#define CFG_OCD_DELAY_MS         80
/* AFE 的短路档位是离散的：2mΩ 时 44mV≈22A、67mV≈33.5A …
 * 写 34A 才落在 67mV 档；写 33A 会被向下取到 22A。 */
#define CFG_SCD_MA               34000       /* 放电短路（AFE），实际 ≈33.5A */
#define CFG_SCD_DELAY_US         200

/* 软件二级保护（比 AFE 稍紧，作为最后一道） */
#define CFG_SW_OV_MV             4300
#define CFG_SW_UV_MV             2750
#define CFG_SW_OCD_MA            12000

/* 温度（℃ x10） */
#define CFG_TEMP_CHG_MIN_C10     0           /* 0℃ 以下禁充 */
#define CFG_TEMP_CHG_MAX_C10     450         /* 45℃ 以上禁充 */
#define CFG_TEMP_DSG_MIN_C10     (-200)
#define CFG_TEMP_DSG_MAX_C10     600
#define CFG_TEMP_BAL_MIN_C10     100
#define CFG_TEMP_BAL_MAX_C10     450

/* 故障恢复时间 */
#define CFG_RECOVER_OV_MS        3000
#define CFG_RECOVER_UV_MS        3000
#define CFG_RECOVER_OCD_MS       10000
#define CFG_RECOVER_SCD_MS       30000

/* ========================= 4. 均衡 ========================= */
#define CFG_BAL_START_MV         3900        /* 高于此电压才均衡 */
#define CFG_BAL_DIFF_MV          30          /* 压差超过此值才动作 */
#define CFG_BAL_IDLE_MA          50          /* |I| 小于此值视为静置 */
#define CFG_BAL_IDLE_S           300         /* 静置多久后允许均衡 */
#define CFG_BAL_ON_MS            30000       /* 均衡开 30s */
#define CFG_BAL_OFF_MS           10000       /* 关 10s 重新测量 */

/* ========================= 5. SOC ========================= */
#define CFG_SOC_INIT_PCT         50.0f       /* 首次上电且电压不可信时的初值 */
#define CFG_SOC_CHG_EFF          0.995f      /* 充电库仑效率 */
#define CFG_SOC_DSG_EFF          0.985f      /* 放电库仑效率 */
#define CFG_SOC_IDLE_MA          50          /* 静置判定电流 */
#define CFG_SOC_OCV_IDLE_S       300         /* 静置多久用 OCV 修正 */
#define CFG_SOC_OCV_WEIGHT       0.20f       /* OCV 修正权重 */
#define CFG_SOC_MIN_PCT          0.0f
#define CFG_SOC_MAX_PCT          100.0f

/* ========================= 5.5 低功耗 ========================= */
#define CFG_LP_ENABLE            1   /* 1=静置 10s 后进 STOP；调试时置 0 保持常开 */
#define CFG_LP_IDLE_S            10  /* 静置多久进 STOP */

/* ========================= 6. 任务周期 ========================= */
#define BMS_TASK_FAST_MS         10          /* 通讯/按键/告警 */
#define BMS_TASK_MED_MS          100         /* 库仑计读取 */
#define BMS_TASK_SLOW_MS         1000        /* 全量采样/保护/上报 */
#define BMS_AFE_RETRY            3           /* I2C 重试次数 */
#define BMS_COMM_FAIL_LIMIT      5           /* 连续通讯失败多少次报故障 */

/* ========================= 7. 引脚 ========================= */
/* CubeMX 里把标签设成这些名字即可，未定义时用默认值兜底 */
#ifndef ALERT_Pin
#define ALERT_Pin            GPIO_PIN_0
#define ALERT_GPIO_Port      GPIOB
#endif
#ifndef BOOT_Pin
#define BOOT_Pin             GPIO_PIN_1
#define BOOT_GPIO_Port       GPIOB
#endif
#ifndef CHG_DET_Pin
#define CHG_DET_Pin          GPIO_PIN_5
#define CHG_DET_GPIO_Port    GPIOB
#endif
#ifndef LED_Pin
#define LED_Pin              GPIO_PIN_12
#define LED_GPIO_Port        GPIOB
#endif
#ifndef BUZZER_Pin
#define BUZZER_Pin           GPIO_PIN_13
#define BUZZER_GPIO_Port     GPIOB
#endif
#ifndef KEY_Pin
#define KEY_Pin              GPIO_PIN_14
#define KEY_GPIO_Port        GPIOB
#endif

/* ========================= 8. 上报 ========================= */
#define BMS_PROTO_SOF1           0x5A
#define BMS_PROTO_SOF2           0xA5
#define BMS_PROTO_VER            1
#define BMS_TEXT_PERIOD_MS       1000        /* 文本帧周期，0=关闭 */

#endif /* BMS_CONFIG_H */
