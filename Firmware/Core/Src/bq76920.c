/**
 * @file    bq76920.c
 * @brief   BQ76920 驱动实现
 *
 * 关键换算（来自数据手册）：
 *   单体电压 mV = raw14 * ADCGAIN(µV/LSB) / 1000 + ADCOFFSET(mV)
 *   总压    mV = 4 * ADCGAIN * raw14 / 1000 + 节数 * ADCOFFSET
 *   电流    mA = CC(int16) * 8.44µV / R_shunt(mΩ)      // 8.44µV/LSB
 *   温度：TS1 分压 → R_ntc = 10k*Vts/(3.3V-Vts) → Beta 公式
 */
#include "bms_hal.h"
#include "bms_config.h"
#include "bq76920.h"

#include <string.h>
#include <math.h>

/* ---------------- 寄存器地址 ---------------- */
#define REG_SYS_STAT     0x00
#define REG_CELLBAL1     0x01
#define REG_SYS_CTRL1    0x04
#define REG_SYS_CTRL2    0x05
#define REG_PROTECT1     0x06
#define REG_PROTECT2     0x07
#define REG_PROTECT3     0x08
#define REG_OV_TRIP      0x09
#define REG_UV_TRIP      0x0A
#define REG_CC_CFG       0x0B
#define REG_VC1_HI       0x0C
#define REG_BAT_HI       0x2A
#define REG_TS1_HI       0x2C
#define REG_CC_HI        0x32
#define REG_ADCGAIN1     0x50
#define REG_ADCOFFSET    0x51
#define REG_ADCGAIN2     0x59

/* SYS_STAT 位 */
#define STAT_CC_READY    0x80
#define STAT_DEVICE_XR   0x20
#define STAT_OVRD_ALERT  0x10
#define STAT_UV          0x08
#define STAT_OV          0x04
#define STAT_SCD         0x02
#define STAT_OCD         0x01

/* 保护阈值档位表（PROTECT1/2） */
static const uint16_t kScdMv[8]  = {44, 67, 89, 111, 133, 155, 178, 200};
static const uint16_t kScdUs[4]  = {70, 100, 200, 400};
static const uint16_t kOcdMv[16] = {17, 22, 28, 33, 39, 44, 50, 56,
                                    61, 67, 72, 78, 83, 89, 94, 100};
static const uint16_t kOcdMs[8]  = {8, 20, 40, 80, 160, 320, 640, 1280};
static const uint16_t kOvDelayS[4] = {1, 2, 4, 8};
static const uint16_t kUvDelayS[4] = {1, 4, 8, 16};

static uint16_t s_gain   = 365;   /* µV/LSB */
static int16_t  s_offset = 0;     /* mV，有符号 */
static uint8_t  s_fails  = 0;
static uint16_t s_last_err = 0;

/* ---------------- 底层 I2C ---------------- */
static bool bq_wr(uint8_t reg, uint8_t val)
{
    for (uint8_t i = 0; i < BMS_AFE_RETRY; i++) {
        if (HAL_I2C_Mem_Write(&hi2c1, BQ_I2C_ADDR, reg,
                              I2C_MEMADD_SIZE_8BIT, &val, 1, 20) == HAL_OK) {
            s_fails = 0;
            return true;
        }
    }
    if (s_fails < 255) s_fails++;
    s_last_err = __LINE__;
    return false;
}

static bool bq_rd(uint8_t reg, uint8_t *buf, uint16_t len)
{
    for (uint8_t i = 0; i < BMS_AFE_RETRY; i++) {
        if (HAL_I2C_Mem_Read(&hi2c1, BQ_I2C_ADDR, reg,
                             I2C_MEMADD_SIZE_8BIT, buf, len, 20) == HAL_OK) {
            s_fails = 0;
            return true;
        }
    }
    if (s_fails < 255) s_fails++;
    s_last_err = __LINE__;
    return false;
}

/* 在升序表里找“不超过 v 的最大档位” */
static uint8_t pick_index(const uint16_t *tbl, uint8_t n, uint32_t v)
{
    uint8_t idx = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (v >= tbl[i]) idx = i;
    }
    return idx;
}

/* BOOT 脉冲：拉高 5ms 唤醒 AFE，然后释放为高阻（手册要求，否则影响测温） */
static void bq_boot_pulse(void)
{
    GPIO_InitTypeDef g = {0};

    g.Pin   = BOOT_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOOT_GPIO_Port, &g);

    HAL_GPIO_WritePin(BOOT_GPIO_Port, BOOT_Pin, GPIO_PIN_SET);
    HAL_Delay(5);
    HAL_GPIO_WritePin(BOOT_GPIO_Port, BOOT_Pin, GPIO_PIN_RESET);

    g.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(BOOT_GPIO_Port, &g);

    HAL_Delay(10);   /* 器件启动 <10ms */
}

/* ---------------- 保护阈值下发 ---------------- */
static void bq_apply_protection(void)
{
    /* OV / UV：寄存器里存的是 14bit ADC 码的 bit13..4 */
    int32_t raw_ov = ((int32_t)CFG_OV_MV - s_offset) * 1000 / s_gain;
    int32_t raw_uv = ((int32_t)CFG_UV_MV - s_offset) * 1000 / s_gain;
    uint8_t ov_trip = (uint8_t)((raw_ov >> 4) & 0xFF);
    uint8_t uv_trip = (uint8_t)(((raw_uv >> 4) & 0xFF) + 1);  /* 向下取整，宁早勿晚 */

    (void)bq_wr(REG_OV_TRIP, ov_trip);
    (void)bq_wr(REG_UV_TRIP, uv_trip);

    /* 短路保护：mV = I * R；RSNS=1 表示使用 ≥2mΩ 档 */
    uint32_t scd_mv = (uint32_t)((float)CFG_SCD_MA * BQ_SHUNT_MOHM / 1000.0f);
    uint8_t  scd_th = pick_index(kScdMv, 8, scd_mv);
    uint8_t  scd_dl = pick_index(kScdUs, 4, CFG_SCD_DELAY_US);
    (void)bq_wr(REG_PROTECT1, (uint8_t)(0x80 | (scd_dl << 3) | scd_th));

    /* 过流保护 */
    uint32_t ocd_mv = (uint32_t)((float)CFG_OCD_MA * BQ_SHUNT_MOHM / 1000.0f);
    uint8_t  ocd_th = pick_index(kOcdMv, 16, ocd_mv);
    uint8_t  ocd_dl = pick_index(kOcdMs, 8, CFG_OCD_DELAY_MS);
    (void)bq_wr(REG_PROTECT2, (uint8_t)((ocd_dl << 4) | ocd_th));

    /* 过压/欠压确认延时：PROTECT3 的 OV_DELAY=bit5:4, UV_DELAY=bit7:6 */
    uint8_t ov_dl = pick_index(kOvDelayS, 4, CFG_OV_DELAY_S);
    uint8_t uv_dl = pick_index(kUvDelayS, 4, CFG_UV_DELAY_S);
    (void)bq_wr(REG_PROTECT3, (uint8_t)((ov_dl << 4) | (uv_dl << 6)));
}

/* ---------------- 对外接口 ---------------- */
bool BQ_Init(void)
{
    uint8_t v = 0;

    s_fails = 0;

    bq_boot_pulse();

    /* CC_CFG 是“可写可读”的探针寄存器，写 0x19 能回读说明 I2C 通了 */
    if (!bq_wr(REG_CC_CFG, 0x19)) return false;
    if (!bq_rd(REG_CC_CFG, &v, 1) || v != 0x19) return false;

    /* ADC_EN | TEMP_SEL(外部 NTC)
     * 打开 ADC 后第一轮转换要约 250ms 才出数，期间寄存器是 0，
     * 这里先等一轮，否则上电第一次读到的单体电压全是 0。 */
    (void)bq_wr(REG_SYS_CTRL1, 0x18);
    HAL_Delay(250);
    /* CC_EN 打开库仑计 */
    (void)bq_wr(REG_SYS_CTRL2, 0x40);

    /* 读出厂增益与偏置 */
    uint8_t g1 = 0, g2 = 0, off = 0;
    if (bq_rd(REG_ADCGAIN1, &g1, 1) &&
        bq_rd(REG_ADCGAIN2, &g2, 1) &&
        bq_rd(REG_ADCOFFSET, &off, 1)) {
        s_gain   = (uint16_t)(365 + (((uint16_t)(g1 & 0x0C) << 1) |
                                     ((uint16_t)(g2 & 0xE0) >> 5)));
        s_offset = (int16_t)(int8_t)off;
    }

    bq_apply_protection();

    (void)bq_wr(REG_SYS_STAT, 0x3F);   /* 清历史故障（写 1 清） */

    return true;
}

bool BQ_ReadAll(bq_sample_t *s)
{
    uint8_t buf[10];

    if (s == NULL) return false;
    memset(s, 0, sizeof(*s));

    if (!bq_rd(REG_VC1_HI, buf, (uint16_t)(2 * BMS_CELL_COUNT))) return false;

    for (uint8_t i = 0; i < BMS_CELL_COUNT; i++) {
        uint16_t raw = (uint16_t)(((uint16_t)(buf[2 * i] & 0x3F) << 8) |
                                  buf[2 * i + 1]);
        int32_t mv = (int32_t)((uint32_t)raw * s_gain / 1000u) + s_offset;
        mv = (int32_t)((float)mv * BMS_CAL_CELL_GAIN) + BMS_CAL_CELL_OFFSET_MV;
        if (mv < 0) mv = 0;
        s->cell_mv[i] = (uint16_t)mv;
        if (mv > 500) s->cell_count++;
    }

    uint8_t pb[2];
    if (bq_rd(REG_BAT_HI, pb, 2)) {
        uint16_t raw = (uint16_t)(((uint16_t)(pb[0] & 0x3F) << 8) | pb[1]);
        int32_t mv = (int32_t)((uint32_t)4u * s_gain * raw / 1000u)
                     + (int32_t)s_offset * s->cell_count;
        s->pack_mv = (mv > 0 && mv < 40000) ? (uint16_t)mv : 0;
    }

    s->temp_valid = BQ_ReadTemp(&s->temp_c10);
    s->sys_stat   = BQ_Status();
    return true;
}

bool BQ_ReadTemp(int16_t *c10)
{
    uint8_t b[2];

    if (c10 == NULL) return false;
    if (!bq_rd(REG_TS1_HI, b, 2)) return false;

    uint16_t adc = (uint16_t)(((uint16_t)(b[0] & 0x3F) << 8) | b[1]);
    float vts = (float)adc * 0.382f;                 /* mV */
    if (vts <= 10.0f || vts >= 3290.0f) return false; /* 未接 NTC / 短路 */

    float r = 10000.0f * vts / (3300.0f - vts);      /* Ω */
    if (r < 50.0f || r > 2000000.0f) return false;

    float inv_t = 1.0f / (273.15f + 25.0f)
                + logf(r / 10000.0f) / (float)BQ_THERMISTOR_BETA;
    float t = 1.0f / inv_t - 273.15f;
    if (t < -60.0f) t = -60.0f;
    if (t > 120.0f) t = 120.0f;

    *c10 = (int16_t)lroundf(t * 10.0f);
    return true;
}

bool BQ_ReadCurrent(int32_t *ma, bool *fresh)
{
    uint8_t st = BQ_Status();
    bool ready = (st & STAT_CC_READY) != 0;

    if (fresh) *fresh = ready;
    if (ma == NULL) return false;
    if (!ready) return true;      /* 没有新数据，调用方保留上次值 */

    uint8_t b[2];
    if (!bq_rd(REG_CC_HI, b, 2)) return false;

    int16_t raw = (int16_t)(((uint16_t)b[0] << 8) | b[1]);
    int32_t v = (int32_t)lroundf((float)raw * 8.44f / BQ_SHUNT_MOHM) * BMS_CURRENT_SIGN;
    if (v > -10 && v < 10) v = 0;

    *ma = v;
    (void)bq_wr(REG_SYS_STAT, STAT_CC_READY);   /* 写 1 清 CC_READY */
    return true;
}

uint8_t BQ_Status(void)
{
    uint8_t v = 0;
    (void)bq_rd(REG_SYS_STAT, &v, 1);
    return v;
}

bool BQ_ClearStatus(uint8_t mask)
{
    return bq_wr(REG_SYS_STAT, mask);
}

bool BQ_SetFets(bool chg_on, bool dsg_on)
{
    uint8_t v = 0;
    if (!bq_rd(REG_SYS_CTRL2, &v, 1)) return false;

    if (chg_on) v |= 0x01; else v &= (uint8_t)~0x01;
    if (dsg_on) v |= 0x02; else v &= (uint8_t)~0x02;

    return bq_wr(REG_SYS_CTRL2, v);
}

bool BQ_GetFets(bool *chg_on, bool *dsg_on)
{
    uint8_t v = 0;
    if (!bq_rd(REG_SYS_CTRL2, &v, 1)) return false;
    if (chg_on) *chg_on = (v & 0x01) != 0;
    if (dsg_on) *dsg_on = (v & 0x02) != 0;
    return true;
}

bool BQ_SetBalancing(uint8_t mask)
{
    mask &= (uint8_t)((1u << BMS_CELL_COUNT) - 1u);
    return bq_wr(REG_CELLBAL1, mask);
}

void BQ_EnterShipMode(void)
{
    (void)bq_wr(REG_SYS_CTRL1, 0x00);
    HAL_Delay(2);
    (void)bq_wr(REG_SYS_CTRL1, 0x01);
    HAL_Delay(2);
    (void)bq_wr(REG_SYS_CTRL1, 0x02);
}

uint8_t  BQ_CommFails(void) { return s_fails; }
uint16_t BQ_LastError(void) { return s_last_err; }
