/**
 * @file    bms.c
 * @brief   BMS 应用层：任务调度 + 保护/均衡/上报联动
 *
 * 调用方式：
 *   main() 里 BMS_Init() 之后，while(1) 里不断调用 BMS_Task()
 */
#include "bms_hal.h"
#include "bms_config.h"
#include "bms.h"
#include "soc.h"
#include "balancing.h"
#include "adc_ntc.h"
#include "nvm.h"
#include "lp.h"

#include <string.h>

static bq_sample_t       s_sample;
static prot_state_t      s_prot;
static bal_state_t       s_bal;
static proto_telemetry_t s_tel;

static int32_t  s_current_ma;
static uint8_t  s_afe_status;
static bool     s_afe_ok;
static bool     s_alert_pending;
static uint8_t  s_rx_byte;
static uint8_t  s_fet_state;
static bool     s_text_mode;
static uint16_t s_vmin;

static uint32_t s_t_fast, s_t_med, s_t_slow, s_t_led, s_t_save;
static bool     s_led_level;
static bool     s_full_reported;

/* NVM：循环次数、历史故障、放电累计 */
static uint16_t s_cycles_x10;
static uint16_t s_fault_hist;
static uint32_t s_uptime_base;
static float    s_disch_mah;
static uint32_t s_last_rx_ms;      /* 最近一次收到上位机字节的时间 */
static uint32_t s_idle_since;      /* 进入静置的时间 */
static bool     s_idle_tracking;
static bool     s_lp_enabled;

/* ---------------- 小工具 ---------------- */
static uint16_t cells_min(void)
{
    uint16_t v = 0xFFFF;
    for (uint8_t i = 0; i < BMS_CELL_COUNT; i++) {
        if (s_sample.cell_mv[i] < v) v = s_sample.cell_mv[i];
    }
    return (v == 0xFFFF) ? 0 : v;
}

static uint16_t cells_max(void)
{
    uint16_t v = 0;
    for (uint8_t i = 0; i < BMS_CELL_COUNT; i++) {
        if (s_sample.cell_mv[i] > v) v = s_sample.cell_mv[i];
    }
    return v;
}

static void update_telemetry(void)
{
    s_tel.ver    = BMS_PROTO_VER;
    s_tel.cells  = s_sample.cell_count;

    for (uint8_t i = 0; i < 5; i++) s_tel.cell_mv[i] = s_sample.cell_mv[i];

    s_tel.pack_mv  = s_sample.pack_mv;

    int32_t i = s_current_ma;
    if (i > 32767)  i = 32767;
    if (i < -32768) i = -32768;
    s_tel.current_ma = (int16_t)i;

    s_tel.temp_c10 = s_sample.temp_c10;

    float soc = SOC_GetPercent();
    if (soc < 0.0f)   soc = 0.0f;
    if (soc > 100.0f) soc = 100.0f;
    s_tel.soc = (uint8_t)(soc + 0.5f);

    float rem = SOC_GetRemainingMah();
    s_tel.remain_mah = (uint16_t)((rem > 65535.0f) ? 65535.0f : rem);

    /* 低 16 位=当前故障，高 16 位=历史故障 */
    s_tel.faults = (s_prot.active & 0xFFFFu) | ((s_prot.latched & 0xFFFFu) << 16);
    s_tel.fet    = s_fet_state;
    s_tel.bal    = s_bal.mask;
}

/* ---------------- AFE 故障位处理 ---------------- */
static uint16_t sample_vmax(void)
{
    uint16_t v = 0;
    for (uint8_t i = 0; i < s_sample.cell_count && i < 5; i++) {
        if (s_sample.cell_mv[i] > v) v = s_sample.cell_mv[i];
    }
    return v;
}

static uint16_t sample_vmin(void)
{
    uint16_t v = 0xFFFF;
    for (uint8_t i = 0; i < s_sample.cell_count && i < 5; i++) {
        if (s_sample.cell_mv[i] < v) v = s_sample.cell_mv[i];
    }
    return (v == 0xFFFF) ? 0 : v;
}

/* AFE 的 SYS_STAT 是"写 1 清"的锁存位。
 * 清除条件必须建立在"测量值已恢复"上，而且要先清位、让保护状态机下一拍才能看到变化。
 * （早期版本用 s_prot.active 做判据，导致位永远清不掉 -> 充电锁死，已修正） */
static void afe_fault_service(uint32_t now_ms)
{
    static uint32_t t_xr, t_ovrd;
    static uint32_t t_ov, t_uv, t_ocd, t_scd;

    if (!s_afe_ok || s_afe_status == 0) return;

    uint16_t vmax = sample_vmax();
    uint16_t vmin = sample_vmin();
    int32_t  i_ma = s_current_ma;

    /* 过压：所有单体回落到恢复阈值以下并保持 3s */
    if (s_afe_status & 0x04) {
        if (vmax > 0 && vmax < CFG_OV_RECOVER_MV) {
            if ((uint32_t)(now_ms - t_ov) >= CFG_RECOVER_OV_MS) {
                (void)BQ_ClearStatus(0x04);
                t_ov = now_ms;
            }
        } else {
            t_ov = now_ms;
        }
    }

    /* 欠压：最低单体回到恢复阈值以上并保持 3s */
    if (s_afe_status & 0x08) {
        if (vmin > CFG_UV_RECOVER_MV) {
            if ((uint32_t)(now_ms - t_uv) >= CFG_RECOVER_UV_MS) {
                (void)BQ_ClearStatus(0x08);
                t_uv = now_ms;
            }
        } else {
            t_uv = now_ms;
        }
    }

    /* 放电过流：电流回落到 200mA 以内并保持 10s */
    if (s_afe_status & 0x01) {
        if (i_ma > -200 && i_ma < 200) {
            if ((uint32_t)(now_ms - t_ocd) >= CFG_RECOVER_OCD_MS) {
                (void)BQ_ClearStatus(0x01);
                t_ocd = now_ms;
            }
        } else {
            t_ocd = now_ms;
        }
    }

    /* 短路：电流回落后保持 5s 再清 AFE 位，软件层自己还有 30s 恢复计时 */
    if (s_afe_status & 0x02) {
        if (i_ma > -200 && i_ma < 200) {
            if ((uint32_t)(now_ms - t_scd) >= 5000u) {
                (void)BQ_ClearStatus(0x02);
                t_scd = now_ms;
            }
        } else {
            t_scd = now_ms;
        }
    }

    /* XREADY：TI 建议等几秒后重试清除 */
    if (s_afe_status & 0x20) {
        if ((uint32_t)(now_ms - t_xr) > 3000u) {
            (void)BQ_ClearStatus(0x20);
            t_xr = now_ms;
        }
    }
    /* OVRD_ALERT：10s 后尝试清除 */
    if (s_afe_status & 0x10) {
        if ((uint32_t)(now_ms - t_ovrd) > 10000u) {
            (void)BQ_ClearStatus(0x10);
            t_ovrd = now_ms;
        }
    }
}

/* ---------------- MOS 控制 ---------------- */
static void fet_service(void)
{
    static uint8_t last_want = 0xFF;
    uint8_t want = 0;

    bool chg = s_prot.chg_ok && ((s_afe_status & 0x04) == 0);
    bool dsg = s_prot.dsg_ok && ((s_afe_status & 0x0B) == 0);

    if (chg) want |= 0x01;
    if (dsg) want |= 0x02;

    if (want != last_want && s_afe_ok) {
        (void)BQ_SetFets((want & 0x01) != 0, (want & 0x02) != 0);
        last_want = want;
    }
    s_fet_state = want;
}

/* ---------------- 按键 / LED / 蜂鸣器 ---------------- */
static void key_service(uint32_t now_ms)
{
    static bool     last_pressed;
    static bool     timing;
    static uint32_t t_press;

    bool pressed = (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_RESET);

    if (pressed && !last_pressed) {
        timing  = true;
        t_press = now_ms;
    } else if (!pressed && last_pressed && timing) {
        uint32_t dur = (uint32_t)(now_ms - t_press);
        timing = false;
        if (dur > 3000u) {          /* 长按：清故障锁存 */
            Prot_ClearLatch();
            (void)BQ_ClearStatus(0x3F);
        } else {                    /* 短按：切换文本/二进制上报 */
            s_text_mode = !s_text_mode;
        }
    }
    last_pressed = pressed;
}

static void led_service(uint32_t now_ms)
{
    uint32_t period = (s_prot.active != 0 || !s_afe_ok) ? 100u : 500u;

    if ((uint32_t)(now_ms - s_t_led) >= period) {
        s_t_led = now_ms;
        s_led_level = !s_led_level;
        HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin,
                          s_led_level ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

static void buzzer_service(void)
{
    /* 放电且电量 <5% 时鸣叫提醒，其余情况静音 */
    bool low = (SOC_GetPercent() < 5.0f) && (s_current_ma < -50);
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin,
                      low ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ---------------- 命令处理 ---------------- */
static void bms_cmd_handler(uint8_t cmd, const uint8_t *data, uint8_t len,
                            uint8_t *resp, uint8_t *rlen)
{
    *rlen = 0;

    switch (cmd) {
    case PROTO_CMD_GET_TELEM:
        update_telemetry();
        Proto_SendTelemetry(&s_tel);
        break;

    case PROTO_CMD_SET_FETS:
        if (len >= 1 && s_afe_ok) {
            (void)BQ_SetFets((data[0] & 0x01) != 0, (data[0] & 0x02) != 0);
            resp[0] = data[0];
            *rlen = 1;
        }
        break;

    case PROTO_CMD_CLEAR_FAULT:
        Prot_ClearLatch();
        if (s_afe_ok) (void)BQ_ClearStatus(0x3F);
        resp[0] = 1;
        *rlen = 1;
        break;

    case PROTO_CMD_BAL_TEST:
        if (len >= 1 && s_afe_ok) {
            (void)BQ_SetBalancing(data[0]);
            resp[0] = data[0];
            *rlen = 1;
        }
        break;

    case PROTO_CMD_GET_VERSION: {
        static const char ver[] = "BMS3S-F103 v1.0";
        uint8_t l = (uint8_t)(sizeof(ver) - 1);
        memcpy(resp, ver, l);
        *rlen = l;
        break;
    }

    case PROTO_CMD_TEXT_MODE:
        if (len >= 1) {
            s_text_mode = (data[0] != 0);
            resp[0] = (uint8_t)s_text_mode;
            *rlen = 1;
        }
        break;

    default:
        resp[0] = 0xEE;
        *rlen = 1;
        break;
    }
}

/* ---------------- 低功耗 ---------------- */
static bool charger_present(void)
{
    return HAL_GPIO_ReadPin(CHG_DET_GPIO_Port, CHG_DET_Pin) == GPIO_PIN_SET;
}

/* 静置 10s（无充放电、无故障、上位机不通讯）后进 STOP：
 * RTC 每秒唤醒一次做完整周期，ALERT 拉高立即唤醒。 */
static void low_power_service(uint32_t now_ms)
{
    if (!s_lp_enabled) return;

    bool quiet = (s_prot.active == 0) &&
                 (s_current_ma > -CFG_BAL_IDLE_MA) &&
                 (s_current_ma < CFG_BAL_IDLE_MA) &&
                 !charger_present() &&
                 ((uint32_t)(now_ms - s_last_rx_ms) > 10000u);

    if (!quiet) { s_idle_tracking = false; return; }
    if (!s_idle_tracking) { s_idle_tracking = true; s_idle_since = now_ms; return; }
    if ((uint32_t)(now_ms - s_idle_since) < 10000u) return;

    LP_SleepUntilEvent();

    /* 醒来：让主循环立刻再跑一次完整周期 */
    s_alert_pending = true;
    s_t_slow = HAL_GetTick() - BMS_TASK_SLOW_MS;
}

/* ---------------- 1s 主任务 ---------------- */
static void slow_service(uint32_t now_ms)
{
    int16_t mos_c10 = NTC_GetTempC10(0);
    bq_sample_t tmp;

    /* 读失败时保留上一次的有效数据：否则全 0 的 cell_mv 会让保护层误判欠压，
     * 白白锁死一整套故障位。通讯故障本身由 BQ_CommFails() 统计。 */
    if (BQ_ReadAll(&tmp)) {
        s_sample     = tmp;
        s_afe_ok     = true;
        s_afe_status = s_sample.sys_stat;
        s_vmin       = cells_min();
    } else {
        s_afe_ok     = false;
        s_afe_status = 0xFF;      /* FET 全关，等通讯恢复 */
    }

    /* 电流由 100ms 任务维护，采样结构里补回去 */
    s_sample.current_ma = s_current_ma;

    /* AFE 的 NTC 没接或读不到时，用板载 NTC 兜底，否则低温保护会误触发 */
    if (!s_sample.temp_valid) {
        s_sample.temp_c10 = NTC_GetTempC10(1);
    }

    Prot_Update(&s_sample, mos_c10, &s_prot, now_ms);
    afe_fault_service(now_ms);
    fet_service();
    Bal_Update(&s_sample, &s_prot, now_ms, &s_bal);

    /* 充满判定：单体接近 4.15V 且充电电流已经很小 */
    if (s_afe_ok && s_prot.chg_ok && cells_max() >= 4150 &&
        s_current_ma > 20 && s_current_ma < 200) {
        if (!s_full_reported) {
            SOC_MarkFull();
            s_full_reported = true;
        }
    } else if (s_current_ma < 0 || cells_max() < 4050) {
        s_full_reported = false;
    }

    /* 故障历史变化立即落盘；平时 5 分钟落一次 */
    if (s_fault_hist != (uint16_t)(s_prot.latched & 0xFFFFu)) {
        s_fault_hist = (uint16_t)(s_prot.latched & 0xFFFFu);
        (void)BMS_SaveState();
        s_t_save = now_ms;
    } else if ((uint32_t)(now_ms - s_t_save) >= 300000u) {
        (void)BMS_SaveState();
        s_t_save = now_ms;
    }

    update_telemetry();
    buzzer_service();

    if (s_text_mode) Proto_SendText(&s_tel);
    else             Proto_SendTelemetry(&s_tel);

    led_service(now_ms);

    /* 放最后：一旦进 STOP 就会阻塞到下次唤醒 */
    low_power_service(now_ms);
}

/* ---------------- 对外接口 ---------------- */
void BMS_Init(void)
{
    uint32_t now = HAL_GetTick();

    s_t_fast = s_t_med = s_t_slow = s_t_led = now;
    s_current_ma = 0;
    s_fet_state  = 0;
    s_text_mode  = false;
    s_vmin       = 0;

    Proto_Init(bms_cmd_handler);
    NTC_Start();

    s_afe_ok = BQ_Init();
    memset(&s_sample, 0, sizeof(s_sample));

    if (s_afe_ok) {
        /* 首轮 ADC 可能还没出数：最多重试 5 次、共 500ms */
        for (uint8_t i = 0; i < 5; i++) {
            if (BQ_ReadAll(&s_sample) && s_sample.cell_count == BMS_CELL_COUNT) break;
            HAL_Delay(100);
            HAL_IWDG_Refresh(&hiwdg);
        }
        (void)BQ_SetBalancing(0);
    }

    s_vmin = cells_min();

    bool v_valid = (s_afe_ok && s_sample.cell_count == BMS_CELL_COUNT &&
                    s_vmin > 2500 && s_vmin < 4300);
    SOC_Init(now, s_vmin, v_valid);

    Prot_Init();
    Bal_Init();

    /* 从 Flash 恢复：SOC/容量、循环次数、历史故障 */
    {
        nvm_state_t nst;
        if (NVM_Load(&nst)) {
            SOC_Restore(nst.soc_pct, nst.remain_mah, now);
            if (nst.cap_mah > 500.0f) SOC_SetCapacity(nst.cap_mah);
            s_cycles_x10  = nst.cycles_x10;
            s_fault_hist  = nst.faults_hist;
            s_uptime_base = nst.uptime_s;
        }
    }
    s_t_save = now;

    if (s_afe_ok) {
        (void)BQ_SetFets(true, true);
        s_fet_state = 0x03;
    }

    update_telemetry();

    (void)HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);

    s_last_rx_ms   = now;
    s_idle_tracking = false;

    /* RTC + STOP 低功耗（初始化失败就自动退回常开模式） */
    s_lp_enabled = LP_Init();

    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
    s_led_level = true;
}

void BMS_Task(void)
{
    uint32_t now = HAL_GetTick();

    HAL_IWDG_Refresh(&hiwdg);

    /* 10ms：通讯、按键、告警响应 */
    if ((uint32_t)(now - s_t_fast) >= BMS_TASK_FAST_MS) {
        s_t_fast = now;
        Proto_Poll();
        key_service(now);

        if (s_alert_pending) {
            s_alert_pending = false;

            /* 这里千万不能读 CC！BQ 每 250ms 置一次 CC_READY，如果中断里把样本读走，
             * 100ms 任务就永远拿不到 fresh，SOC 会一直停在初值（实测 bug）。
             * ALERT 只用来把"故障类"告警提前处理：清标志位、让下一拍立刻跑完整流程。
             * 正常的库仑计采样统一交给 100ms 任务。 */
            uint8_t st = BQ_Status();
            if (st & 0x3F) {
                s_afe_status = st;
                s_t_slow = 0;          /* 下一拍立即做完整采样/保护 */
            }
        }
    }

    /* 100ms：库仑计积分（BQ 每 250ms 出一次数，这里只取有新数据的那次） */
    if ((uint32_t)(now - s_t_med) >= BMS_TASK_MED_MS) {
        s_t_med = now;

        int32_t ma = s_current_ma;
        bool fresh = false;

        if (BQ_ReadCurrent(&ma, &fresh) && fresh) {
            s_current_ma = ma;
            SOC_Update(s_current_ma, s_vmin, now);

            /* 放电量累计 -> 循环次数（满容量放完算 1 次，x10 记录） */
            if (ma < 0) {
                float cap;
                s_disch_mah += (float)(-ma) * 0.1f / 3600.0f;
                cap = SOC_GetCapacityMah();
                if (cap > 100.0f && s_disch_mah >= cap) {
                    s_disch_mah -= cap;
                    if (s_cycles_x10 < 60000u) s_cycles_x10 += 10u;
                }
            }
        }
    }

    /* 1s：全量采样 + 保护 + 均衡 + 上报 */
    if ((uint32_t)(now - s_t_slow) >= BMS_TASK_SLOW_MS) {
        s_t_slow = now;
        slow_service(now);
    }
}

void BMS_OnAlertIrq(void)
{
    s_alert_pending = true;
}

bool BMS_SaveState(void)
{
    nvm_state_t st;

    st.soc_pct     = SOC_GetPercent();
    st.remain_mah  = SOC_GetRemainingMah();
    st.cap_mah     = SOC_GetCapacityMah();
    st.cycles_x10  = s_cycles_x10;
    st.faults_hist = s_fault_hist;
    st.uptime_s    = s_uptime_base + HAL_GetTick() / 1000u;

    return NVM_Save(&st);
}

const proto_telemetry_t *BMS_GetTelemetry(void) { return &s_tel; }
const prot_state_t      *BMS_GetProtection(void) { return &s_prot; }
const bq_sample_t       *BMS_GetSample(void) { return &s_sample; }

/* ================= HAL 回调（若与 CubeMX 生成的重名，保留一份即可） ================= */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        Proto_RxByte(s_rx_byte);
        s_last_rx_ms = HAL_GetTick();
        (void)HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);
    }
}

void BMS_RestartRx(void)
{
    (void)HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ALERT_Pin) {
        BMS_OnAlertIrq();
    }
}

/* 串口溢出/帧错误后 HAL 会停掉接收，这里重新挂上，否则上位机命令通道会"装死" */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
#ifdef __HAL_UART_CLEAR_OREFLAG
        __HAL_UART_CLEAR_OREFLAG(huart);
#endif
        (void)HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);
    }
}

/* 说明：ALERT 默认只做"引脚配置"，不打开 NVIC 中断。
 * 原因：CubeMX 没有把 EXTI0 生成到 stm32f1xx_it.c，自己补一个 handler 会与启动文件
 * 里的弱符号冲突（Keil AC5 报 L6200E）。硬件保护由 BQ76920 独立完成，不依赖这个中断；
 * 需要毫秒级故障上报时，在 CubeMX 的 NVIC 里勾上 "EXTI line0 interrupt" 重新生成即可，
 * 生成的 handler 会自动调用下面的 HAL_GPIO_EXTI_Callback。 */
