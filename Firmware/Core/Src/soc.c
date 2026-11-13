/**
 * @file    soc.c
 * @brief   3S NMC 18650 的 SOC 估算
 *
 * 思路：
 *   1) 上电用开路电压给初值（静置 >5min 才可信，否则用配置里的 50%）
 *   2) 运行时用库仑计积分，充/放电各用不同库仑效率
 *   3) 静置超过 5 分钟且电流很小 → 用 OCV 表做 20% 权重的慢修正
 *   4) 充满（CV 末端）时强制对齐 100%，抑制长期漂移
 */
#include "bms_config.h"
#include "soc.h"

#define LUT_N 21

/* 单体 OCV-SOC 表（3.7V 标称 NMC），从 0% 到 100%，电压单位 mV */
static const uint16_t kOcvMv[LUT_N] = {
    3000, 3480, 3530, 3570, 3600, 3620, 3640, 3660, 3680, 3700, 3730,
    3760, 3800, 3840, 3890, 3940, 4000, 4060, 4130, 4170, 4200
};

static float    s_soc_pct      = CFG_SOC_INIT_PCT;
static float    s_cap_mah      = BMS_CELL_CAPACITY_MAH;
static float    s_remain_mah   = 0.0f;
static uint32_t s_last_ms      = 0;
static uint32_t s_idle_ms      = 0;      /* 静置累计时间 */
static bool     s_inited       = false;
static bool     s_calibrated   = false;

/* 电压 → SOC，线性插值 */
float SOC_OcvToPercent(uint16_t mv)
{
    if (mv <= kOcvMv[0])            return 0.0f;
    if (mv >= kOcvMv[LUT_N - 1])    return 100.0f;

    for (uint8_t i = 0; i < LUT_N - 1; i++) {
        if (mv >= kOcvMv[i] && mv <= kOcvMv[i + 1]) {
            float span_mv  = (float)(kOcvMv[i + 1] - kOcvMv[i]);
            float span_soc = 100.0f / (float)(LUT_N - 1);
            float ratio    = (float)(mv - kOcvMv[i]) / span_mv;
            return (float)i * span_soc + ratio * span_soc;
        }
    }
    return 50.0f;
}

void SOC_Init(uint32_t now_ms, uint16_t cell_min_mv, bool voltage_valid)
{
    s_cap_mah    = BMS_CELL_CAPACITY_MAH;
    s_last_ms    = now_ms;
    s_idle_ms    = 0;
    s_inited     = true;
    s_calibrated = false;

    if (voltage_valid && cell_min_mv > 2500 && cell_min_mv < 4300) {
        s_soc_pct = SOC_OcvToPercent(cell_min_mv);
    } else {
        s_soc_pct = CFG_SOC_INIT_PCT;
    }

    if (s_soc_pct < CFG_SOC_MIN_PCT) s_soc_pct = CFG_SOC_MIN_PCT;
    if (s_soc_pct > CFG_SOC_MAX_PCT) s_soc_pct = CFG_SOC_MAX_PCT;

    s_remain_mah = s_cap_mah * s_soc_pct / 100.0f;
}

void SOC_MarkFull(void)
{
    s_soc_pct    = 100.0f;
    s_remain_mah = s_cap_mah;
    s_calibrated = true;
}

void SOC_SetCapacity(float mah)
{
    if (mah < 100.0f) return;
    s_cap_mah    = mah;
    s_remain_mah = s_cap_mah * s_soc_pct / 100.0f;
}

void SOC_Update(int32_t current_ma, uint16_t cell_min_mv, uint32_t now_ms)
{
    if (!s_inited) {
        SOC_Init(now_ms, cell_min_mv, true);
        return;
    }

    uint32_t dt_ms = now_ms - s_last_ms;      /* 无符号相减，天然处理回绕 */
    s_last_ms = now_ms;
    if (dt_ms > 5000) dt_ms = 5000;           /* 调试断点后不要一次性补太多 */

    /* --- 1. 安时积分 --- */
    float dt_h = (float)dt_ms / 3600000.0f;
    float i_ma = (float)current_ma;
    float eff  = (i_ma >= 0.0f) ? CFG_SOC_CHG_EFF : CFG_SOC_DSG_EFF;

    s_remain_mah += i_ma * eff * dt_h;

    if (s_remain_mah > s_cap_mah) s_remain_mah = s_cap_mah;
    if (s_remain_mah < 0.0f)      s_remain_mah = 0.0f;

    s_soc_pct = s_remain_mah / s_cap_mah * 100.0f;

    /* --- 2. 静置检测与 OCV 修正 --- */
    if (current_ma > -CFG_SOC_IDLE_MA && current_ma < CFG_SOC_IDLE_MA) {
        s_idle_ms += dt_ms;
    } else {
        s_idle_ms = 0;
    }

    if (s_idle_ms >= (uint32_t)CFG_SOC_OCV_IDLE_S * 1000u &&
        cell_min_mv > 2500 && cell_min_mv < 4300) {

        float ocv_soc = SOC_OcvToPercent(cell_min_mv);
        float diff    = ocv_soc - s_soc_pct;

        /* 只在偏差大于 2% 时慢修正，避免电压噪声抖动 SOC */
        if (diff > 2.0f || diff < -2.0f) {
            s_soc_pct += diff * CFG_SOC_OCV_WEIGHT;
            if (s_soc_pct < CFG_SOC_MIN_PCT) s_soc_pct = CFG_SOC_MIN_PCT;
            if (s_soc_pct > CFG_SOC_MAX_PCT) s_soc_pct = CFG_SOC_MAX_PCT;
            s_remain_mah = s_cap_mah * s_soc_pct / 100.0f;
        }
        s_idle_ms = 0;       /* 修正一次后重新计时 */
    }

    if (s_soc_pct > 99.5f) s_calibrated = true;
}

float SOC_GetPercent(void)      { return s_soc_pct; }
float SOC_GetRemainingMah(void) { return s_remain_mah; }
float SOC_GetCapacityMah(void)  { return s_cap_mah; }
bool  SOC_IsCalibrated(void)    { return s_calibrated; }
