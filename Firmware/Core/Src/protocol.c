/**
 * @file    protocol.c
 */
#include "bms_hal.h"
#include "bms_config.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>

#define RX_RING_SIZE 96
#define TX_BUF_SIZE  80

static proto_cb_t s_cb;
static uint8_t  s_ring[RX_RING_SIZE];
static volatile uint16_t s_head, s_tail;

/* 解析状态机 */
static uint8_t  s_state;
static uint8_t  s_cmd, s_len, s_idx;
static uint8_t  s_data[32];
static uint8_t  s_crc_hi;

void Proto_Init(proto_cb_t cb)
{
    s_cb = cb;
    s_head = s_tail = 0;
    s_state = 0;
}

/* ---------- CRC16 / CCITT-FALSE ---------- */
static uint16_t crc16_update(uint16_t crc, uint8_t d)
{
    crc ^= (uint16_t)d << 8;
    for (uint8_t i = 0; i < 8; i++) {
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t crc16_calc(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < n; i++) crc = crc16_update(crc, d[i]);
    return crc;
}

/* ---------- 接收 ---------- */
void Proto_RxByte(uint8_t b)
{
    uint16_t next = (uint16_t)((s_head + 1) % RX_RING_SIZE);
    if (next != s_tail) {          /* 满了就丢新字节 */
        s_ring[s_head] = b;
        s_head = next;
    }
}

static bool ring_pop(uint8_t *b)
{
    if (s_tail == s_head) return false;
    *b = s_ring[s_tail];
    s_tail = (uint16_t)((s_tail + 1) % RX_RING_SIZE);
    return true;
}

/* ---------- 发送 ---------- */
void Proto_SendFrame(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint8_t buf[TX_BUF_SIZE];
    uint8_t n = 0;

    if ((uint16_t)len + 6 > TX_BUF_SIZE) return;

    buf[n++] = BMS_PROTO_SOF1;
    buf[n++] = BMS_PROTO_SOF2;
    buf[n++] = cmd;
    buf[n++] = len;
    for (uint8_t i = 0; i < len; i++) buf[n++] = data[i];

    uint16_t crc = crc16_calc(&buf[2], (uint16_t)(len + 2));
    buf[n++] = (uint8_t)(crc >> 8);
    buf[n++] = (uint8_t)(crc & 0xFF);

    (void)HAL_UART_Transmit(&huart1, buf, n, 50);
}

void Proto_SendTelemetry(const proto_telemetry_t *t)
{
    uint8_t p[32];
    uint8_t n = 0;

    if (t == NULL) return;

    p[n++] = t->ver;
    p[n++] = t->cells;
    for (uint8_t i = 0; i < 5; i++) {
        p[n++] = (uint8_t)(t->cell_mv[i] >> 8);
        p[n++] = (uint8_t)(t->cell_mv[i] & 0xFF);
    }
    p[n++] = (uint8_t)(t->pack_mv >> 8);
    p[n++] = (uint8_t)(t->pack_mv & 0xFF);
    p[n++] = (uint8_t)((uint16_t)t->current_ma >> 8);
    p[n++] = (uint8_t)((uint16_t)t->current_ma & 0xFF);
    p[n++] = (uint8_t)((uint16_t)t->temp_c10 >> 8);
    p[n++] = (uint8_t)((uint16_t)t->temp_c10 & 0xFF);
    p[n++] = t->soc;
    p[n++] = (uint8_t)(t->remain_mah >> 8);
    p[n++] = (uint8_t)(t->remain_mah & 0xFF);
    p[n++] = (uint8_t)(t->faults >> 24);
    p[n++] = (uint8_t)(t->faults >> 16);
    p[n++] = (uint8_t)(t->faults >> 8);
    p[n++] = (uint8_t)(t->faults & 0xFF);
    p[n++] = t->fet;
    p[n++] = t->bal;

    Proto_SendFrame(PROTO_CMD_GET_TELEM | PROTO_ACK, p, n);
}

void Proto_SendText(const proto_telemetry_t *t)
{
    char buf[96];
    int n;

    if (t == NULL) return;

    n = snprintf(buf, sizeof(buf),
                 "$BMS,%u,%u,%u,%u,%d,%d,%u,%u,%04lX,%02X,%02X\r\n",
                 t->cell_mv[0], t->cell_mv[1], t->cell_mv[2],
                 t->pack_mv, t->current_ma, t->temp_c10,
                 t->soc, t->remain_mah,
                 (unsigned long)t->faults, t->fet, t->bal);

    if (n > 0 && n < (int)sizeof(buf)) {
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 50);
    }
}

/* ---------- 解析 ---------- */
void Proto_Poll(void)
{
    uint8_t b;

    while (ring_pop(&b)) {
        switch (s_state) {
        case 0:
            if (b == BMS_PROTO_SOF1) s_state = 1;
            break;
        case 1:
            s_state = (b == BMS_PROTO_SOF2) ? 2 : 0;
            break;
        case 2:
            s_cmd = b;
            s_state = 3;
            break;
        case 3:
            if (b > sizeof(s_data)) { s_state = 0; break; }
            s_len = b;
            s_idx = 0;
            s_state = (b == 0) ? 5 : 4;
            break;
        case 4:
            s_data[s_idx++] = b;
            if (s_idx >= s_len) s_state = 5;
            break;
        case 5:
            s_crc_hi = b;
            s_state = 6;
            break;
        case 6: {
            uint16_t crc_rx = (uint16_t)(((uint16_t)s_crc_hi << 8) | b);
            uint8_t  frame[34];
            uint16_t flen = (uint16_t)(s_len + 2);
            uint16_t crc_calc;

            frame[0] = s_cmd;
            frame[1] = s_len;
            memcpy(&frame[2], s_data, s_len);
            crc_calc = crc16_calc(frame, flen);

            if (crc_calc == crc_rx) {
                uint8_t resp[32];
                uint8_t rlen = 0;

                if (s_cb) s_cb(s_cmd, s_data, s_len, resp, &rlen);
                if (rlen > 0) Proto_SendFrame((uint8_t)(s_cmd | PROTO_ACK), resp, rlen);
            }
            s_state = 0;
            break;
        }
        default:
            s_state = 0;
            break;
        }
    }
}
