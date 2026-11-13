/**
 * @file    protocol.h
 * @brief   上位机通讯协议：0x5A 0xA5 + CMD + LEN + DATA + CRC16
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#define PROTO_CMD_GET_TELEM   0x01
#define PROTO_CMD_SET_FETS    0x02
#define PROTO_CMD_CLEAR_FAULT 0x03
#define PROTO_CMD_BAL_TEST    0x04
#define PROTO_CMD_GET_VERSION 0x05
#define PROTO_CMD_TEXT_MODE   0x06

#define PROTO_ACK             0x80
#define PROTO_ERR             0xFF

typedef struct {
    uint8_t  ver;
    uint8_t  cells;
    uint16_t cell_mv[5];
    uint16_t pack_mv;
    int16_t  current_ma;
    int16_t  temp_c10;
    uint8_t  soc;
    uint16_t remain_mah;
    uint32_t faults;
    uint8_t  fet;     /* bit0 CHG, bit1 DSG */
    uint8_t  bal;     /* 均衡掩码 */
} proto_telemetry_t;

/* 命令回调：填充 resp/resp_len 即回包（长度 0 表示不回） */
typedef void (*proto_cb_t)(uint8_t cmd, const uint8_t *data, uint8_t len,
                           uint8_t *resp, uint8_t *resp_len);

void Proto_Init(proto_cb_t cb);
void Proto_RxByte(uint8_t b);          /* 在 UART 接收中断里调用 */
void Proto_Poll(void);                 /* 主循环里调用 */
void Proto_SendTelemetry(const proto_telemetry_t *t);
void Proto_SendText(const proto_telemetry_t *t);
void Proto_SendFrame(uint8_t cmd, const uint8_t *data, uint8_t len);

#endif /* PROTOCOL_H */
