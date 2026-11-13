/**
 * @file    balancing.h
 * @brief   被动均衡调度：只在静置、压差够大、电压够高时放电阻
 */
#ifndef BALANCING_H
#define BALANCING_H

#include <stdint.h>
#include <stdbool.h>
#include "bq76920.h"
#include "protection.h"

typedef struct {
    uint8_t mask;      /* 正在均衡的节（bit0=Cell1） */
    bool    active;
} bal_state_t;

void Bal_Init(void);
void Bal_Update(const bq_sample_t *s, const prot_state_t *p,
                uint32_t now_ms, bal_state_t *out);

#endif /* BALANCING_H */
