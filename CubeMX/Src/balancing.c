/**
 * @file    balancing.c
 *
 * 规则（保守版，适合第一次做板子）：
 *   1) 无任何故障、温度 10~45℃
 *   2) |I| < 50mA 且持续 5 分钟（避免把 IR drop 当压差）
 *   3) 最高节电压 > 3.90V，且与最低节压差 > 30mV
 *   4) 只均衡高于 (vmin + 30mV) 的节；相邻节不同时开（BQ 内部 FET 共用引脚）
 *   5) 30s 开 / 10s 关，关的那 10s 用来重新测量真实电压
 */
#include "bms_config.h"
#include "balancing.h"

#include <stddef.h>

static uint8_t  s_mask;
static bool     s_active;
static uint32_t s_idle_start;
static bool     s_idle_valid;
static uint32_t s_phase_start;
static bool     s_phase_on;
static bool     s_inited;

void Bal_Init(void)
{
    s_mask       = 0;
    s_active     = false;
    s_idle_start = 0;
    s_idle_valid = false;
    s_phase_start = 0;
    s_phase_on   = false;
    s_inited     = true;
}

void Bal_Update(const bq_sample_t *s, const prot_state_t *p,
                uint32_t now_ms, bal_state_t *out)
{
    if (out == NULL) return;
    if (!s_inited) Bal_Init();
    if (s == NULL || p == NULL) {
        out->mask = 0;
        out->active = false;
        return;
    }

    bool idle = (s->current_ma > -CFG_BAL_IDLE_MA) &&
                (s->current_ma <  CFG_BAL_IDLE_MA);

    /* 静置计时 */
    if (!idle || !p->bal_ok) {
        s_idle_valid = false;
        if (s_mask != 0) {
            s_mask = 0;
            (void)BQ_SetBalancing(0);
        }
        s_active = false;
        out->mask = 0;
        out->active = false;
        return;
    }

    if (!s_idle_valid) {
        s_idle_valid = true;
        s_idle_start = now_ms;
        /* 刚静置下来先关均衡，让电压回落 */
        s_mask = 0;
        s_active = false;
        (void)BQ_SetBalancing(0);
    }

    if ((uint32_t)(now_ms - s_idle_start) < (uint32_t)CFG_BAL_IDLE_S * 1000u) {
        out->mask = 0;
        out->active = false;
        return;
    }

    /* 找最高/最低节 */
    uint16_t vmax = 0, vmin = 0xFFFF;
    for (uint8_t i = 0; i < BMS_CELL_COUNT; i++) {
        if (s->cell_mv[i] > vmax) vmax = s->cell_mv[i];
        if (s->cell_mv[i] < vmin) vmin = s->cell_mv[i];
    }

    bool need = (vmax >= CFG_BAL_START_MV) &&
                ((uint16_t)(vmax - vmin) > CFG_BAL_DIFF_MV);

    if (!need) {
        if (s_mask != 0) {
            s_mask = 0;
            (void)BQ_SetBalancing(0);
        }
        s_active = false;
        out->mask = 0;
        out->active = false;
        return;
    }

    /* 占空比：开 30s / 关 10s */
    uint32_t phase_ms = s_phase_on ? CFG_BAL_ON_MS : CFG_BAL_OFF_MS;
    if ((uint32_t)(now_ms - s_phase_start) >= phase_ms) {
        s_phase_start = now_ms;
        s_phase_on = !s_phase_on;

        if (!s_phase_on) {
            /* 进入测量窗口，先全部关掉 */
            s_mask = 0;
            (void)BQ_SetBalancing(0);
        } else {
            /* 重新计算掩码：从高到低选，跳过相邻 */
            uint8_t mask = 0;
            for (int8_t i = (int8_t)BMS_CELL_COUNT - 1; i >= 0; i--) {
                if (s->cell_mv[i] < CFG_BAL_START_MV) continue;
                if ((uint16_t)(s->cell_mv[i] - vmin) <= CFG_BAL_DIFF_MV) continue;
                if (mask & (uint8_t)(1u << (i + 1))) continue;   /* 相邻 */
                if (i > 0 && (mask & (uint8_t)(1u << (i - 1)))) continue;
                mask |= (uint8_t)(1u << i);
            }
            s_mask = mask;
            (void)BQ_SetBalancing(s_mask);
        }
    }

    s_active   = (s_mask != 0);
    out->mask  = s_mask;
    out->active = s_active;
}
