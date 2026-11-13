/* 主机端自测：假 AFE + 驱动 / 保护 / 协议 / SOC / 主循环冒烟 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bms.h"
#include "bq76920.h"
#include "soc.h"
#include "protection.h"
#include "protocol.h"

void stub_afe_reset(void);
void stub_afe_set_cell(int idx, uint16_t mv);
void stub_afe_set_pack(uint16_t mv, int cells);
void stub_afe_set_current_ma(int32_t ma, float shunt_mohm);
void stub_afe_set_temp_c(int16_t c);
void stub_set_tick(uint32_t t);
void stub_clear_uart(void);
extern uint8_t  g_uart_tx[512];
extern uint8_t  g_regs[256];
extern uint16_t g_uart_tx_len;

static int g_fail;

#define OK(cond, msg) do { \
    if (cond) printf("  ok   %s\n", msg); \
    else { printf("  FAIL %s\n", msg); g_fail++; } } while (0)

static uint16_t crc16(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---------------- 1. 驱动 ---------------- */
static void test_driver(void)
{
    bq_sample_t s;
    int32_t ma = 0;
    bool fresh = false;
    bool chg = false, dsg = false;

    printf("[1] BQ76920 driver\n");
    stub_afe_reset();
    stub_afe_set_cell(0, 4000);
    stub_afe_set_cell(1, 3960);
    stub_afe_set_cell(2, 3920);
    stub_afe_set_pack(11880, 3);
    stub_afe_set_temp_c(25);

    OK(BQ_Init(), "BQ_Init  (I2C 通、增益/偏置已读)");

    OK(BQ_ReadAll(&s), "BQ_ReadAll");
    OK(abs((int)s.cell_mv[0] - 4000) <= 2 && abs((int)s.cell_mv[1] - 3960) <= 2 &&
       abs((int)s.cell_mv[2] - 3920) <= 2, "单体电压换算 (4000/3960/3920 mV)");
    OK(abs((int)s.pack_mv - 11880) <= 6, "总压换算 (11880 mV)");
    OK(s.cell_count == 3, "节数识别 = 3");
    OK(s.temp_valid && abs((int)s.temp_c10 - 250) <= 20, "NTC 测温 ≈ 25.0C");

    stub_afe_set_current_ma(-5000, 2.0f);
    OK(BQ_ReadCurrent(&ma, &fresh) && fresh, "库仑计有新数据");
    OK(abs((int)ma + 5000) <= 10, "电流换算 (-5000 mA)");

    OK(BQ_SetFets(true, false), "BQ_SetFets");
    OK(BQ_GetFets(&chg, &dsg) && chg && !dsg, "FET 状态回读");
}

/* ---------------- 2. 保护 ---------------- */
static void test_protection(void)
{
    bq_sample_t s;
    prot_state_t p;

    printf("[2] protection\n");
    memset(&s, 0, sizeof(s));
    s.cell_count = 3;
    s.cell_mv[0] = s.cell_mv[1] = s.cell_mv[2] = 4000;
    s.temp_c10 = 250;

    Prot_Init();
    Prot_Update(&s, 250, &p, 0);
    OK(p.chg_ok && p.dsg_ok, "正常电压下充放电都允许");

    s.cell_mv[0] = 4300;
    Prot_Update(&s, 250, &p, 100);
    OK((p.active & PROT_F_OV) == 0, "过压确认延时内不动作");
    Prot_Update(&s, 250, &p, 400);
    OK((p.active & PROT_F_OV) != 0 && !p.chg_ok, "过压 200ms 后动作并禁充");

    s.cell_mv[0] = 4100;
    Prot_Update(&s, 250, &p, 1000);
    Prot_Update(&s, 250, &p, 3900);
    OK((p.active & PROT_F_OV) != 0, "恢复计时未到 3s 仍保持故障");
    Prot_Update(&s, 250, &p, 4100);
    OK((p.active & PROT_F_OV) == 0 && p.chg_ok, "回差恢复后重新允许充电");

    memset(&s, 0, sizeof(s));
    s.cell_count = 3;
    s.cell_mv[0] = s.cell_mv[1] = s.cell_mv[2] = 3900;
    s.temp_c10 = -100;
    Prot_Init();
    Prot_Update(&s, -100, &p, 0);
    Prot_Update(&s, -100, &p, 1500);
    OK((p.active & PROT_F_CELL_COLD) != 0 && !p.chg_ok && p.dsg_ok,
       "0℃ 以下禁充但允许放电");
}

/* ---------------- 3. 协议 ---------------- */
static uint8_t s_cmd_seen;
static void cmd_cb(uint8_t cmd, const uint8_t *d, uint8_t len,
                   uint8_t *resp, uint8_t *rlen)
{
    (void)d; (void)len;
    s_cmd_seen = cmd;
    resp[0] = 0x42;
    *rlen = 1;
}

static void test_protocol(void)
{
    uint8_t body[2] = {0x01, 0x00};
    uint16_t c = crc16(body, 2);
    uint8_t frame[6] = {0x5A, 0xA5, 0x01, 0x00, (uint8_t)(c >> 8), (uint8_t)(c & 0xFF)};

    printf("[3] protocol\n");
    Proto_Init(cmd_cb);
    stub_clear_uart();
    for (uint8_t i = 0; i < sizeof(frame); i++) Proto_RxByte(frame[i]);
    Proto_Poll();

    OK(s_cmd_seen == 0x01, "命令 0x01 解析正确");
    OK(g_uart_tx_len == 7 && g_uart_tx[0] == 0x5A && g_uart_tx[1] == 0xA5 &&
       g_uart_tx[2] == 0x81, "回包帧头/命令正确");

    /* 坏 CRC 不应回包 */
    Proto_Init(cmd_cb);
    stub_clear_uart();
    s_cmd_seen = 0;
    frame[5] ^= 0xFF;
    for (uint8_t i = 0; i < sizeof(frame); i++) Proto_RxByte(frame[i]);
    Proto_Poll();
    OK(s_cmd_seen == 0 && g_uart_tx_len == 0, "CRC 错误丢帧");
}

/* ---------------- 4. SOC ---------------- */
static void test_soc(void)
{
    printf("[4] SOC\n");
    SOC_Init(0, 4000, true);
    float s0 = SOC_GetPercent();
    OK(fabsf(s0 - 80.0f) < 2.0f, "OCV 查表：4000mV ≈ 80%");

    for (uint32_t i = 0; i < 36000; i++) {
        SOC_Update(-1000, 4000, i * 100u);
    }
    float s1 = SOC_GetPercent();
    printf("       1h @ -1A 后 SOC = %.1f%%（理论 51.9%%）\n", s1);
    OK(fabsf(s1 - 51.9f) < 2.5f, "安时积分 + 库仑效率");
}

/* ---------------- 5. 主循环冒烟 ---------------- */
static void test_bms(void)
{
    printf("[5] BMS 主循环\n");
    stub_afe_reset();
    stub_afe_set_cell(0, 4050);
    stub_afe_set_cell(1, 4060);
    stub_afe_set_cell(2, 4040);
    stub_afe_set_pack(12150, 3);
    stub_afe_set_temp_c(30);
    stub_afe_set_current_ma(-1200, 2.0f);
    stub_set_tick(2000);

    BMS_Init();
    stub_clear_uart();

    for (int i = 1; i <= 15; i++) {
        stub_set_tick((uint32_t)(2000 + i * 1000));
        BMS_Task();
    }

    const proto_telemetry_t *t = BMS_GetTelemetry();
    const prot_state_t *p = BMS_GetProtection();

    printf("       cell=%u/%u/%u pack=%u I=%d T=%d soc=%u flt=0x%08lX\n",
           t->cell_mv[0], t->cell_mv[1], t->cell_mv[2], t->pack_mv,
           t->current_ma, t->temp_c10, t->soc, (unsigned long)t->faults);

    OK(t->cells == 3, "上报节数 = 3");
    OK(abs((int)t->cell_mv[1] - 4060) <= 3, "上报单体电压正确");
    OK(t->fet == 0x03, "默认 CHG/DSG 都打开");
    OK(p->active == 0, "无故障");
    OK(g_uart_tx_len > 0, "每秒有遥测帧发出");
}

/* ---------------- 6. AFE 锁存故障恢复（回归测试） ---------------- */
/* 这个用例专门盯一个真实 bug：保护条件里如果引用 AFE 的 SYS_STAT 锁存位，
 * 而清位又以"软件故障已恢复"为前提，就会出现互锁 —— 充电永久打不开。 */
static void test_afe_recovery(void)
{
    printf("[6] AFE fault recovery\n");

    stub_afe_reset();
    stub_afe_set_cell(0, 4320);      /* 高于软件 OV 阈值 4300 */
    stub_afe_set_cell(1, 4320);
    stub_afe_set_cell(2, 4320);
    stub_afe_set_pack(12960, 3);
    stub_afe_set_temp_c(25);
    stub_afe_set_current_ma(0, 2.0f);

    stub_set_tick(30000);
    BMS_Init();                      /* 初始化会清一次 SYS_STAT */

    g_regs[0] |= 0x04;               /* 初始化之后才模拟 AFE 硬件过压跳闸 */
    BQ_ClearStatus(0);               /* 占位：不改变任何位（写 0 无效） */

    uint32_t t = 30000;
    for (int i = 0; i < 300; i++) {           /* 3s */
        t += 10;
        stub_set_tick(t);
        BMS_Task();
    }
    OK((BMS_GetProtection()->active & PROT_F_OV) != 0, "过压时软件层也能识别");
    OK((BMS_GetTelemetry()->fet & 0x01) == 0, "过压期间 CHG 关闭");
    OK((g_regs[0] & 0x04) != 0, "电压没恢复前，AFE 的 OV 位不能被清");

    /* 电压回落到恢复阈值以下 */
    stub_afe_set_cell(0, 4050);
    stub_afe_set_cell(1, 4050);
    stub_afe_set_cell(2, 4050);
    stub_afe_set_pack(12150, 3);

    for (int i = 0; i < 1000; i++) {          /* 10s */
        t += 10;
        stub_set_tick(t);
        BMS_Task();
    }

    OK((g_regs[0] & 0x04) == 0, "电压恢复后 AFE 的 OV 位被清掉");
    OK((BMS_GetProtection()->active & PROT_F_OV) == 0, "软件 OV 故障解除");
    OK((BMS_GetTelemetry()->fet & 0x01) != 0, "CHG 重新打开（没有锁死）");
}

/* ---------------- 7. ALERT 不能抢走库仑计样本 ---------------- */
/* 另一个真实 bug：10ms 的 ALERT 分支如果读 CC，就会把 250ms 一次的样本读走，
 * 100ms 任务永远拿不到 fresh，SOC 冻结。这里模拟 ALERT 每 10ms 来一次。 */
static void test_alert_keeps_soc(void)
{
    printf("[7] ALERT 不抢库仑计\n");

    stub_afe_reset();
    stub_afe_set_cell(0, 3900);
    stub_afe_set_cell(1, 3900);
    stub_afe_set_cell(2, 3900);
    stub_afe_set_pack(11700, 3);
    stub_afe_set_temp_c(25);
    stub_afe_set_current_ma(-1000, 2.0f);

    stub_set_tick(100000);
    BMS_Init();

    float s0 = SOC_GetPercent();
    uint32_t t = 100000;

    for (int i = 0; i < 6000; i++) {          /* 60s */
        if (i % 25 == 0) stub_afe_set_current_ma(-1000, 2.0f);  /* 250ms 一个新样本 */
        BMS_OnAlertIrq();                     /* ALERT 每 10ms 都来 */
        t += 10;
        stub_set_tick(t);
        BMS_Task();
    }

    float s1 = SOC_GetPercent();
    printf("       SOC %.2f%% -> %.2f%%（理论 -0.47%%）\n", s0, s1);
    OK((s0 - s1) > 0.30f, "ALERT 频繁时 SOC 仍正常积分");
}

int main(void)
{
    test_driver();
    test_protection();
    test_protocol();
    test_soc();
    test_bms();
    test_afe_recovery();
    test_alert_keeps_soc();

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
