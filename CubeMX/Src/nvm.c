/**
 * @file    nvm.c
 * @brief   掉电保存核心逻辑（与平台无关，PC 上可测）
 */
#include "nvm.h"
#include "nvm_port.h"

#include <string.h>

#define REC_SIZE      32u
#define REC_MAGIC     0x4D42u     /* 'BM' */
#define SLOT_COUNT    (NVM_PAGE_SIZE / REC_SIZE)

static uint32_t s_erase_count;

/* ---------- 记录编解码（手工拼字节，避免结构体对齐/端序坑） ---------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static float rd_f32(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
static void wr_f32(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(p, u); }

static uint16_t crc16(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool rec_is_valid(const uint8_t *r)
{
    if (rd16(r) != REC_MAGIC) return false;
    return crc16(r, REC_SIZE - 2) == rd16(r + REC_SIZE - 2);
}

static void rec_pack(uint8_t *r, const nvm_state_t *st, uint16_t seq)
{
    memset(r, 0, REC_SIZE);
    wr16(r + 0, REC_MAGIC);
    wr16(r + 2, seq);
    wr32(r + 4, st->uptime_s);
    wr_f32(r + 8, st->soc_pct);
    wr_f32(r + 12, st->remain_mah);
    wr_f32(r + 16, st->cap_mah);
    wr16(r + 20, st->cycles_x10);
    wr16(r + 22, st->faults_hist);
    wr16(r + REC_SIZE - 2, crc16(r, REC_SIZE - 2));
}

static void rec_unpack(const uint8_t *r, nvm_state_t *st)
{
    st->uptime_s   = rd32(r + 4);
    st->soc_pct    = rd_f32(r + 8);
    st->remain_mah = rd_f32(r + 12);
    st->cap_mah    = rd_f32(r + 16);
    st->cycles_x10 = rd16(r + 20);
    st->faults_hist = rd16(r + 22);
}

/* 序号比较，处理 16 位回绕 */
static bool seq_newer(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }

/* 扫描整页，返回最后一个有效记录的下标；无有效记录返回 -1 */
static int find_last(uint16_t *last_seq)
{
    uint8_t r[REC_SIZE];
    int last = -1;
    uint16_t best = 0;

    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        if (!NVM_PortRead(i * REC_SIZE, r, REC_SIZE)) break;
        if (!rec_is_valid(r)) continue;
        uint16_t seq = rd16(r + 2);
        if (last < 0 || seq_newer(seq, best)) { best = seq; last = (int)i; }
    }
    if (last_seq) *last_seq = best;
    return last;
}

void NVM_Init(void)
{
    s_erase_count = 0;
    (void)find_last(NULL);          /* 预热一次，失败也不影响后续 */
}

bool NVM_Load(nvm_state_t *st)
{
    uint8_t r[REC_SIZE];
    int idx;

    if (st == NULL) return false;

    idx = find_last(NULL);
    if (idx < 0) return false;

    if (!NVM_PortRead((uint32_t)idx * REC_SIZE, r, REC_SIZE)) return false;
    if (!rec_is_valid(r)) return false;

    rec_unpack(r, st);

    /* 合理性检查：坏数据当没存过，避免用错误 SOC 启动 */
    if (st->soc_pct < 0.0f || st->soc_pct > 100.0f) return false;
    if (st->cap_mah < 100.0f || st->cap_mah > 50000.0f) return false;
    if (st->remain_mah < 0.0f || st->remain_mah > st->cap_mah + 1.0f) return false;

    return true;
}

bool NVM_Save(const nvm_state_t *st)
{
    uint8_t r[REC_SIZE];
    uint16_t last_seq = 0;
    int last = find_last(&last_seq);
    uint32_t slot = (last < 0) ? 0u : (uint32_t)(last + 1);

    if (st == NULL) return false;

    if (slot >= SLOT_COUNT) {           /* 页写满：整页擦一次，从头写 */
        if (!NVM_PortErase()) return false;
        s_erase_count++;
        slot = 0;
    }

    rec_pack(r, st, (uint16_t)(last_seq + 1));
    if (!NVM_PortWrite(slot * REC_SIZE, r, REC_SIZE)) return false;

    /* 回读校验：写失败（掉电/坏块）时上层能知道 */
    {
        uint8_t chk[REC_SIZE];
        if (!NVM_PortRead(slot * REC_SIZE, chk, REC_SIZE)) return false;
        if (memcmp(chk, r, REC_SIZE) != 0) return false;
    }
    return true;
}

bool NVM_EraseAll(void)
{
    if (!NVM_PortErase()) return false;
    s_erase_count++;
    return true;
}

uint32_t NVM_EraseCount(void) { return s_erase_count; }
