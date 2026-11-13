/**
 * @file    protection.c
 * @brief   保护状态机（带确认延时、回差恢复、故障锁存）
 */
#include "bms_config.h"
#include "protection.h"

#include <stddef.h>

enum {
    IDX_OV = 0, IDX_UV, IDX_OCD, IDX_SCD,
    IDX_HOT, IDX_COLD, IDX_MOS, IDX_COMM, IDX_AFE,
    IDX_NUM
};

static const uint32_t kDwell[IDX_NUM] = {
    200, 200, 50, 5, 500, 1000, 500, 100, 100
};

static const uint32_t kClear[IDX_NUM] = {
    CFG_RECOVER_OV_MS, CFG_RECOVER_UV_MS,
    CFG_RECOVER_OCD_MS, CFG_RECOVER_SCD_MS,
    5000, 2000, 10000, 1000, 1000
};

static prot_state_t s_state;
static uint32_t s_t_on[IDX_NUM];
static uint32_t s_t_off[IDX_NUM];
static bool     s_on_set[IDX_NUM];
static bool     s_off_set[IDX_NUM];

void Prot_Init(void)
{
    s_state.active  = 0;
    s_state.latched = 0;
    s_state.chg_ok  = true;
    s_state.dsg_ok  = true;
    s_state.bal_ok  = false;

    for (uint8_t i = 0; i < IDX_NUM; i++) {
        s_t_on[i] = s_t_off[i] = 0;
        s_on_set[i] = s_off_set[i] = false;
    }
}

void Prot_ClearLatch(void)
{
    s_state.latched = 0;
}

static uint32_t bit_of(uint8_t idx)
{
    switch (idx) {
    case IDX_OV:   return PROT_F_OV;
    case IDX_UV:   return PROT_F_UV;
    case IDX_OCD:  return PROT_F_OCD;
    case IDX_SCD:  return PROT_F_SCD;
    case IDX_HOT:  return PROT_F_CELL_HOT;
    case IDX_COLD: return PROT_F_CELL_COLD;
    case IDX_MOS:  return PROT_F_MOS_HOT;
    case IDX_COMM: return PROT_F_COMM;
    case IDX_AFE:  return PROT_F_AFE;
    default:       return 0;
    }
}

void Prot_Update(const bq_sample_t *s, int16_t mos_c10,
                 prot_state_t *out, uint32_t now_ms)
{
    uint16_t vmax = 0, vmin = 0xFFFF;
    uint32_t cond = 0;

    if (s == NULL || out == NULL) return;

    for (uint8_t i = 0; i < BMS_CELL_COUNT; i++) {
        if (s->cell_mv[i] > vmax) vmax = s->cell_mv[i];
        if (s->cell_mv[i] < vmin) vmin = s->cell_mv[i];
    }
    /* ---- 条件判定 ----
     * 注意：OV/UV/OCD 只用"测量值"判断，绝对不要把 AFE 的 SYS_STAT 锁存位写进条件。
     * AFE 的锁存位只能由 bms.c 在电压/电流恢复后显式清除；若这里再依赖它，
     * 就会出现"位没清 -> 条件一直成立 -> 永远不清"的死锁（充电永久锁死）。
     * SCD 例外：软件无法在 200µs 内测到短路，只能引用 AFE 的 SCD 位，
     * 由 bms.c 在电流恢复后清除。 */
    if (vmax >= CFG_SW_OV_MV)                                        cond |= 1u << IDX_OV;
    if (vmin <= CFG_SW_UV_MV)                                        cond |= 1u << IDX_UV;
    if (s->current_ma <= -CFG_SW_OCD_MA)                             cond |= 1u << IDX_OCD;
    if (s->sys_stat & 0x02)                                          cond |= 1u << IDX_SCD;
    if (s->temp_c10 >= CFG_TEMP_DSG_MAX_C10)                         cond |= 1u << IDX_HOT;
    if (s->temp_c10 <= CFG_TEMP_CHG_MIN_C10)                         cond |= 1u << IDX_COLD;
    if (mos_c10 >= 700)                                              cond |= 1u << IDX_MOS;
    if (BQ_CommFails() >= BMS_COMM_FAIL_LIMIT ||
        s->cell_count < BMS_CELL_COUNT)                              cond |= 1u << IDX_COMM;
    if (s->sys_stat & 0x20)                                          cond |= 1u << IDX_AFE;

    /* ---- 每项独立的确认 / 恢复计时 ---- */
    for (uint8_t i = 0; i < IDX_NUM; i++) {
        uint32_t bit = bit_of(i);

        if (cond & (1u << i)) {
            s_off_set[i] = false;
            s_t_off[i]   = 0;

            if (!(s_state.active & bit)) {
                if (!s_on_set[i]) {
                    s_on_set[i] = true;
                    s_t_on[i]   = now_ms;
                }
                if ((uint32_t)(now_ms - s_t_on[i]) >= kDwell[i]) {
                    s_state.active |= bit;
                    s_state.latched |= bit;
                }
            }
        } else {
            s_on_set[i]  = false;
            s_t_on[i]    = 0;

            if (s_state.active & bit) {
                if (!s_off_set[i]) {
                    s_off_set[i] = true;
                    s_t_off[i]   = now_ms;
                }
                if ((uint32_t)(now_ms - s_t_off[i]) >= kClear[i]) {
                    s_state.active &= ~bit;
                    s_off_set[i]   = false;
                }
            }
        }
    }

    /* ---- 输出允许标志 ---- */
    uint32_t chg_block = PROT_F_OV | PROT_F_SCD | PROT_F_OCD |
                         PROT_F_CELL_HOT | PROT_F_CELL_COLD |
                         PROT_F_MOS_HOT | PROT_F_COMM | PROT_F_AFE;
    uint32_t dsg_block = PROT_F_UV | PROT_F_SCD | PROT_F_OCD |
                         PROT_F_CELL_HOT | PROT_F_MOS_HOT |
                         PROT_F_COMM | PROT_F_AFE;

    s_state.chg_ok = (s_state.active & chg_block) == 0;
    s_state.dsg_ok = (s_state.active & dsg_block) == 0;

    /* 温度窗口（独立于故障位，直接判测量值） */
    if (s->temp_c10 < CFG_TEMP_CHG_MIN_C10 ||
        s->temp_c10 > CFG_TEMP_CHG_MAX_C10) s_state.chg_ok = false;
    if (s->temp_c10 < CFG_TEMP_DSG_MIN_C10 ||
        s->temp_c10 > CFG_TEMP_DSG_MAX_C10) s_state.dsg_ok = false;

    s_state.bal_ok = (s_state.active == 0) &&
                     s->temp_c10 >= CFG_TEMP_BAL_MIN_C10 &&
                     s->temp_c10 <= CFG_TEMP_BAL_MAX_C10;

    *out = s_state;
}

const char *Prot_Name(uint32_t bit)
{
    switch (bit) {
    case PROT_F_OV:        return "OV";
    case PROT_F_UV:        return "UV";
    case PROT_F_OCD:       return "OCD";
    case PROT_F_SCD:       return "SCD";
    case PROT_F_CELL_HOT:  return "CELL_HOT";
    case PROT_F_CELL_COLD: return "CELL_COLD";
    case PROT_F_MOS_HOT:   return "MOS_HOT";
    case PROT_F_COMM:      return "COMM";
    case PROT_F_AFE:       return "AFE";
    case PROT_F_BAL_HOT:   return "BAL_HOT";
    default:               return "?";
    }
}
