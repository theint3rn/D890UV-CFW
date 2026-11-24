/* D890UV analog outputs, as the stock V1.05 firmware uses them (docs/AUDIO_PATHS.md has the evidence):
 *
 *   PB9 = TIM4_CH4 PWM = MCU audio into the TX modulator. Stock plays MDC-style data (9600 samples/s, TIM5 IRQ ->
 *         FUN_080d7e54 -> CCR4) and APRS AFSK (76.8 kHz PWM, GPDMA ch0 -> CCR4, FUN_080195a0) through it; PB9 is AF2
 *         while playing (FUN_0811a730) and analog (hi-Z) otherwise (FUN_0811a6d4).
 *         cfw: TIM4 PWM at 48 kHz (ARR TX_PWM_TOP-1 = 2499, = ADC_FS), CCR4 reloaded every period by GPDMA1 channel 1
 *         (request 71 = tim4_upd) from the shared TX ring through a self-linked list (circular); the ring holds CCR
 *         values (hal.h DAC_CODE scales the 12-bit modulator samples). Read position = C1BR1.BNDT.
 *         Analog FM test tone (CFW_MCU_FM_TX) is allowed on every build; digital-mode TX needs CFW_DIGITAL_TX
 *         (emulator builds, or HW=1 HW_DTX=1 - dummy load only: PB9 coupling / filtering / pre-emphasis unknown).
 *   PA4 = DAC1 ch1 = TX power control (APC). Stock IRQ17_Handler (baseband TX line PD6): TX -> PE1 + PA15 high,
 *         DHR12R1 = APC << 4 (FUN_0811a75c value); RX -> PA15 + PE1 low, DAC 0. dac_apc() does it from the PTT path.
 *   PA5 = DAC1 ch2 = UHF RX front-end tune (FUN_0811c8f0 -> FUN_08010bf0); the VHF tune is RF chip 0 register 0x5C
 *         (FUN_0811cadc -> FUN_0801a35c). dac_rx_tune(hz) is called by rf_set_freq(0, hz).
 *   APC tables, tune points: RF calibration block NAND 0x04F00000 (A) / 0x04F40000 (B), FUN_080e0440. */
#include <string.h>
#include "stm32u5.h"
#include "board.h"
#include "hal.h"
#include "cal_default.h"

#define DAC1_BASE      0x46021800u
#define DAC_CR         REG32(DAC1_BASE + 0x00)
#define DAC_DHR12R1    REG32(DAC1_BASE + 0x08)
#define DAC_DHR12R2    REG32(DAC1_BASE + 0x14)
#define RCC_AHB3ENR_DAC1 (1u << 6)

#define TIM4_BASE      0x40000800u
#define TIM_CR1        REG32(TIM4_BASE + 0x00)
#define TIM_DIER       REG32(TIM4_BASE + 0x0C)
#define TIM_EGR        REG32(TIM4_BASE + 0x14)
#define TIM_CCMR2      REG32(TIM4_BASE + 0x1C)
#define TIM_CCER       REG32(TIM4_BASE + 0x20)
#define TIM_PSC        REG32(TIM4_BASE + 0x28)
#define TIM_ARR        REG32(TIM4_BASE + 0x2C)
#define TIM_CCR4       REG32(TIM4_BASE + 0x40)
#define TIM_CCR4_A     (TIM4_BASE + 0x40)
#define RCC_APB1ENR1_TIM4 (1u << 2)
#define TX_DMA_CH      1
#define REQ_TIM4_UPD   71u                                 /* RM0456 GPDMA1 request table */

#define LLR_UB1 (1u << 29)
#define LLR_USA (1u << 28)
#define LLR_ULL (1u << 16)

#ifndef CFW_APC_LEVEL
#define CFW_APC_LEVEL 0                                     /* != 0: force a stock power level; 0 = channel power (dac_set_power) */
#endif

static volatile uint16_t s_ring[DAC_RING] __attribute__((aligned(4)));
static volatile uint32_t s_node[3] __attribute__((aligned(4)));
static bool s_clk, s_streaming;
static uint32_t s_hz;                                       /* last main-chip frequency (RX or TX) */

static void clocks(void)
{
    if(s_clk) { return; }
    RCC_AHB3ENR |= RCC_AHB3ENR_DAC1;
    RCC_AHB1ENR |= 1u << 0;                                 /* GPDMA1 */
    RCC_APB1ENR1 |= RCC_APB1ENR1_TIM4;
    (void)RCC_APB1ENR1;
    gpio_mode(GPIOA, APC_DAC_PIN, GPIO_ANALOG, PULL_NONE);
    gpio_mode(GPIOA, TUNE_DAC_PIN, GPIO_ANALOG, PULL_NONE);
    DAC_CR |= (1u << 0) | (1u << 16);                       /* EN1, EN2, no trigger: DHR -> output (stock FUN_08061f5c) */
    DAC_DHR12R1 = 0;
    gpio_mode(TX_PWM_PORT, TX_PWM, GPIO_ANALOG, PULL_NONE); /* PB9 hi-Z while the MCU does not drive TX audio */
    s_clk = true;
}

/* ---------------- RF calibration block (stock FUN_080e0440) ----------------
 * block0 = +0x000 (0x2A bytes, RAM 0x200152AC): +0x02..+0x05 UHF APC per level, +0x18/+0x1A/+0x1C UHF tune points
 *          (400 / 460 / 520 MHz, u16).
 * block1 = +0x100 (0x2C bytes, RAM 0x200152D6): +0x02..+0x05 VHF APC per level, +0x1E/+0x20/+0x22 VHF tune points
 *          (136 / 174 / 225 MHz).
 * APC tables, one byte per MHz: UHF levels 0..3 at +0xC00/+0xD00/+0xE00/+0xF00 (121 bytes, 400..520 MHz, reference
 *          index 40), VHF levels 0..3 at +0x1000/+0x1100/+0x1200/+0x1300 (45 bytes: 136..174 MHz, then 220..225 MHz at
 *          index 39.., reference index 9). */
#define CAL_A 0x04F00000u
#define CAL_B 0x04F40000u
static uint8_t s_b0[0x2A], s_b1[0x2C], s_apc_u[4][0x79], s_apc_v[4][0x2D];
static uint8_t s_apc_max_u, s_apc_max_v;                   /* F5 clamp, per band */
static bool s_apc_ok_u, s_apc_ok_v;
static bool apc_band(const uint8_t *cal, const uint8_t *t, int stride, int n, int ref, uint8_t *mx);
static uint16_t s_tu[13], s_tv[45];
/* squelch (stock FUN_080e373c, RF chip 0): bases u16 UHF +0x10, VHF +0x116, 200-300 MHz +0x124; per-frequency
 * correction tables UHF +0x1400 (5 MHz steps from 400 MHz, ref index 8), VHF +0x1500 (2 MHz steps from 136 MHz,
 * ref index 5, index 20 = 200-300 MHz). Default calibration (cal_default.c) until a NAND block is read. */
static uint16_t s_sqb[3];
static uint8_t s_sqt_u[0x19], s_sqt_v[0x19];
static bool s_sq;
static bool s_cal_ok;
static bool s_tune_ok;                                      /* RX tune tables set (NAND block or default calibration) */

static bool block_valid(uint32_t base)                      /* stock FUN_08023ea8 */
{
    static const uint8_t ok[4] = { 0x55, 0x55, 0xAA, 0xAA }, bad[4] = { 0xAA, 0xAA, 0x55, 0x55 };
    uint8_t m[4];
    if(!nand_read(base + 0x3FFFCu, m, 4) || memcmp(m, ok, 4) != 0) { return false; }
    if(nand_read(base + 0x3FDF8u, m, 4) && memcmp(m, bad, 4) == 0) { return false; }
    return true;
}

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void tune_tables(void)
{
    /* FUN_08049fc4: UHF tune, 13 points every 10 MHz; the step is truncated to a byte as stock */
    uint16_t p0 = u16le(s_b0 + 0x18), p1 = u16le(s_b0 + 0x1A), p2 = u16le(s_b0 + 0x1C);
    uint8_t st = (uint8_t)(((int)p1 - (int)p0) / 6);
    s_tu[0] = p0; s_tu[6] = p1; s_tu[12] = p2;
    for(int i = 1; i < 6; i++) { s_tu[i] = (uint16_t)(s_tu[i - 1] + st); }
    st = (uint8_t)(((int)p2 - (int)p1) / 6);
    for(int i = 7; i < 12; i++) { s_tu[i] = (uint16_t)(s_tu[i - 1] + st); }
    /* FUN_0804a0a0: VHF tune, 136..174 MHz every 1 MHz (0..38), 220..225 MHz (39..44) counted back from p2 */
    p0 = u16le(s_b1 + 0x1E); p1 = u16le(s_b1 + 0x20); p2 = u16le(s_b1 + 0x22);
    s_tv[0] = p0; s_tv[38] = p1; s_tv[44] = p2;
    int16_t sv = (int16_t)(((int)p1 - (int)p0) / 38);
    for(int i = 1; i < 38; i++) { s_tv[i] = (uint16_t)(s_tv[i - 1] + sv); }
    sv = (int16_t)(((int)p2 - (int)p1) / 6);
    for(int i = 43; i > 38; i--) { s_tv[i] = (uint16_t)(s_tv[i + 1] - sv); }
    s_tune_ok = true;
}

/* No valid NAND calibration block: RX tune from the default RF calibration (drivers/cal_default.c). APC stays off:
 * s_cal_ok false = no TX (F5). */
_Static_assert(CAL_DEFAULT_LEN >= 0x100u + sizeof s_b1, "cal_default too short");
_Static_assert(CAL_DEFAULT_LEN >= 0x1500u + 0x19u, "cal_default has no squelch tables");
static void sq_set(const uint8_t *b0, const uint8_t *b1, const uint8_t *tu, const uint8_t *tv)
{
    s_sqb[0] = u16le(b0 + 0x10);
    s_sqb[1] = u16le(b1 + 0x16);
    s_sqb[2] = u16le(b1 + 0x24);
    memcpy(s_sqt_u, tu, sizeof s_sqt_u);
    memcpy(s_sqt_v, tv, sizeof s_sqt_v);
    s_sq = true;
}

static void sq_defaults(void)
{
    sq_set(cal_default, cal_default + 0x100u, cal_default + 0x1400u, cal_default + 0x1500u);
}

static bool cal_fallback(void)
{
    memcpy(s_b0, cal_default, sizeof s_b0);
    memcpy(s_b1, cal_default + 0x100u, sizeof s_b1);
    tune_tables();
    sq_defaults();
    return false;
}

bool dac_cal_load(void)
{
    s_cal_ok = false;
    s_tune_ok = false;
    uint32_t base;
    if(block_valid(CAL_A)) { base = CAL_A; }
    else if(block_valid(CAL_B)) { base = CAL_B; }
    else { cal_report(0, 0, 0, false, false); return cal_fallback(); }                         /* stock reads B anyway; cfw: no valid block = no TX (F5) */
    if(!nand_read(base, s_b0, sizeof s_b0) || !nand_read(base + 0x100u, s_b1, sizeof s_b1)) {
        cal_report(0, 0, 0, false, false);
        return cal_fallback();
    }
    for(uint32_t l = 0; l < 4; l++) {
        if(!nand_read(base + 0xC00u + 0x100u * l, s_apc_u[l], sizeof s_apc_u[l]) ||
           !nand_read(base + 0x1000u + 0x100u * l, s_apc_v[l], sizeof s_apc_v[l])) {
            cal_report(0, 0, 0, false, false);
            return cal_fallback();
        }
    }
    uint8_t tu[sizeof s_sqt_u], tv[sizeof s_sqt_v];
    if(nand_read(base + 0x1400u, tu, sizeof tu) && nand_read(base + 0x1500u, tv, sizeof tv)) { sq_set(s_b0, s_b1, tu, tv); }
    else { sq_defaults(); }
    tune_tables();
    s_apc_ok_u = apc_band(s_b0, &s_apc_u[0][0], 0x79, 0x79, 0x28, &s_apc_max_u);
    s_apc_ok_v = apc_band(s_b1, &s_apc_v[0][0], 0x2D, 0x2D, 9, &s_apc_max_v);
    s_cal_ok = true;
    cal_report(base == CAL_A ? 'A' : 'B', base, 0x2000u, s_apc_ok_u, s_apc_ok_v);
    return true;
}

bool dac_cal_ok(void) { return s_cal_ok; }

/* stock FUN_080e373c for RF chip 0, f in 10 Hz: reg 0x49 = base + level offset (stock table 0x0814AE05, index
 * level - 1; level 0 = index 0) + table[reference] - table[f], u16 arithmetic as stock. The table index is clamped
 * (stock: unbounded). Not ported: the stock temperature term FUN_0804a988 (subtracted only below 0 degC) and the
 * special-mode branch that uses the base alone. */
uint16_t dac_sql_threshold(uint32_t hz, int level)
{
    static const uint8_t off[5] = { 0, 7, 13, 19, 23 };
    if(!s_sq) { sq_defaults(); }
    uint32_t f = hz / 10u;
    int li = level < 1 ? 0 : level > 5 ? 4 : level - 1;
    if(f > 30000000u) {
        uint32_t i = f < 40000000u ? 0 : (f - 40000000u) / 500000u;
        if(i > 0x18u) { i = 0x18u; }
        return (uint16_t)(s_sqb[0] + off[li] + s_sqt_u[8] - s_sqt_u[i]);
    }
    if(f < 20000000u) {
        uint32_t i = f < 13600000u ? 0 : (f - 13600000u) / 200000u;
        if(i > 0x18u) { i = 0x18u; }
        return (uint16_t)(s_sqb[1] + off[li] + s_sqt_v[5] - s_sqt_v[i]);
    }
    return (uint16_t)(s_sqb[2] + off[li] + s_sqt_v[5] - s_sqt_v[20]);
}

/* stock FUN_0811a75c, f in 10 Hz units; level 0 in 200..300 MHz as stock. Pre-flash audit F5: the stock u16 sum
 * "table + cal - reference" wraps to ~0xFFFF (then masked to 12 bits: near full scale) when the table point is below
 * the reference by more than cal; here it is computed signed and clamped to 0..max, max = the highest value the
 * stock formula gives anywhere in that band at any power level (apc_band). No valid calibration: 0, and TX is
 * refused before keying (board_apc_ok -> hal/safety.c). */
static int clampi(int v, int mx) { return v < 0 ? 0 : v > mx ? mx : v; }

uint16_t dac_apc_value(uint32_t hz)
{
    if(!s_cal_ok) { return 0; }
    uint32_t f = hz / 10u;
    int lvl = (f >= 20000000u && f <= 30000000u) ? 0 : (CFW_APC_LEVEL ? (CFW_APC_LEVEL & 3) : dac_power());
    int cal, ref, t;
    if(f > 30000000u) {
        uint32_t i = (f - 40000000u) / 100000u & 0xFFu;    /* stock does not bound the index: clamped here */
        t = s_apc_u[lvl][i < 0x79u ? i : 0x78u];
        ref = s_apc_u[lvl][0x28];
        cal = s_b0[2 + lvl];
        return (uint16_t)clampi(t + cal - ref, s_apc_max_u);
    }
    uint32_t i = (f <= 17400000u ? (f - 13600000u) / 100000u : (f - 22000000u) / 100000u + 0x27u) & 0xFFu;
    t = s_apc_v[lvl][i < 0x2Du ? i : 0x2Cu];
    ref = s_apc_v[lvl][9];
    cal = s_b1[2 + lvl];
    return (uint16_t)clampi(t + cal - ref, s_apc_max_v);
}

/* band usable: not erased (all 0xFF) or blank, and some level gives a non-zero drive; *mx = band maximum */
static bool apc_band(const uint8_t *cal, const uint8_t *t, int stride, int n, int ref, uint8_t *mx)
{
    int m = 0;
    bool ff = true, zero = true;
    for(int l = 0; l < 4; l++) {
        const uint8_t *tl = t + stride * l;
        for(int i = 0; i < n; i++) {
            if(tl[i] != 0xFFu) { ff = false; }
            if(tl[i] != 0u) { zero = false; }
            int v = (int)cal[2 + l] + tl[i] - tl[ref];
            if(v > m) { m = v; }
        }
    }
    *mx = (uint8_t)(m > 0xFF ? 0xFF : m);
    return !ff && !zero && m > 0;
}

bool board_apc_ok(uint32_t hz)
{
    return s_cal_ok && (hz / 10u > 30000000u ? s_apc_ok_u : s_apc_ok_v);
}

/* stock FUN_0811c8f0 (>= 300 MHz) / FUN_0811cadc (< 300 MHz), f in 10 Hz units */
uint16_t dac_tune_value(uint32_t hz)
{
    uint32_t f = hz / 10u;
    if(f >= 30000000u) {
        uint32_t u = f - 40000000u;
        uint16_t i = (uint16_t)(u / 1000000u);
        if(i >= 12) { return s_tu[12]; }
        uint16_t d = (uint16_t)((uint16_t)(s_tu[i + 1] - s_tu[i]) * (uint16_t)((uint16_t)(u / 100000u) % 10u));
        return (uint16_t)(s_tu[i] + d / 10u);
    }
    if(f < 20000000u) {
        uint32_t u = f - 13600000u;
        uint16_t i = (uint16_t)(u / 100000u);
        if(i >= 0x26) { return s_tv[0x26]; }
        uint16_t d = (uint16_t)((uint16_t)(s_tv[i + 1] - s_tv[i]) * (uint16_t)((uint16_t)(u / 10000u) % 10u));
        return (uint16_t)(s_tv[i] + d / 10u);
    }
    uint32_t u = f - 22000000u;
    uint16_t i = (uint16_t)(u / 100000u);
    if(i >= 5) { return s_tv[44]; }
    uint16_t d = (uint16_t)((uint16_t)(s_tv[39 + i + 1] - s_tv[39 + i]) * (uint16_t)((u / 10000u) % 10u));
    return (uint16_t)(s_tv[39 + i] + d / 10u);
}

void dac_rx_tune(uint32_t hz)
{
    clocks();
    s_hz = hz;
    if(!s_tune_ok) { DAC_DHR12R2 = 0x800u; return; }        /* no tune tables: mid-scale, RF reg 0x5C untouched */
    uint16_t v = dac_tune_value(hz);
    if(hz >= 300000000u) { DAC_DHR12R2 = (uint32_t)(v << 4) & 0xFFFu; }
    else { rf_write(0, 0x5C, (uint16_t)(((v & 0xFFu) << 4) | 0x1000u)); }   /* stock FUN_0801a35c */
}

/* Fault / SysTick TX stop (hal/safety.h): register writes only. APC 0, PA15 + PE1 low, TIM4 PWM off and PB9 hi-Z;
 * rf: RF chip 0 out of TX (drivers/rf.c). */
void rf_tx_kill(void);
void board_tx_kill(bool rf)
{
    DAC_DHR12R1 = 0;
    GPIO_BSRR(TX_EN0_PORT) = 1u << (TX_EN0 + 16);
    GPIO_BSRR(TX_EN1_PORT) = 1u << (TX_EN1 + 16);
    TIM_DIER = 0;
    TIM_CR1 = 0;
    TIM_CCER &= ~(1u << 12);
    GPIO_MODER(TX_PWM_PORT) |= 3u << (TX_PWM * 2);
    if(rf) { rf_tx_kill(); }
}

/* APC level + TX enables PE1 / PA15 (stock IRQ17_Handler on the baseband TX edge; the cfw does it at PTT) */
void dac_apc(bool tx)
{
    clocks();
    if(tx && safety_tx_block(s_hz)) { tx = false; }       /* backstop: a refused TX never keys the PA (hal/safety.c) */
    DAC_DHR12R1 = tx ? ((uint32_t)dac_apc_value(s_hz) << 4) & 0xFFFu : 0u;
    gpio_write(TX_EN1_PORT, TX_EN1, tx);
    gpio_write(TX_EN0_PORT, TX_EN0, tx);
    safety_tx_on(tx);                                      /* SysTick TX cap / stall watch */
}

/* ---------------- TX audio: TIM4_CH4 PWM on PB9 ---------------- */
volatile uint16_t *dac_ring(void) { return s_ring; }

void dac_stream_start(void)
{
    clocks();
    for(uint32_t i = 0; i < DAC_RING; i++) { s_ring[i] = DAC_CODE(2048u); }
    TIM_CR1 = 0;
    TIM_DIER = 0;
    uint32_t lla = (uint32_t)(uintptr_t)s_node;
    s_node[0] = sizeof s_ring;                                     /* BR1 */
    s_node[1] = (uint32_t)(uintptr_t)s_ring;                       /* SAR */
    s_node[2] = LLR_UB1 | LLR_USA | LLR_ULL | (lla & 0xFFFCu);     /* LLR -> itself */
    GPDMA_C(TX_DMA_CH, GPDMA_CR) = 0;
    GPDMA_C(TX_DMA_CH, GPDMA_FCR) = 0x7F00u;
    GPDMA_C(TX_DMA_CH, GPDMA_LBAR) = lla & 0xFFFF0000u;
    GPDMA_C(TX_DMA_CH, GPDMA_TR1) = (1u << 0) | (1u << 3) | (1u << 16);   /* SDW halfword, SINC, DDW halfword */
    GPDMA_C(TX_DMA_CH, GPDMA_TR2) = REQ_TIM4_UPD | (1u << 10);            /* request from the destination (DREQ) */
    GPDMA_C(TX_DMA_CH, GPDMA_BR1) = sizeof s_ring;
    GPDMA_C(TX_DMA_CH, GPDMA_SAR) = (uint32_t)(uintptr_t)s_ring;
    GPDMA_C(TX_DMA_CH, GPDMA_DAR) = TIM_CCR4_A;
    GPDMA_C(TX_DMA_CH, GPDMA_LLR) = s_node[2];
    GPDMA_C(TX_DMA_CH, GPDMA_CR) = 1u;                             /* EN */
    TIM_PSC = 0;
    TIM_ARR = TX_PWM_TOP - 1u;                             /* 120 MHz / 2500 = 48 kHz = ADC_FS */
    TIM_CCR4 = DAC_CODE(2048u);
    TIM_CCMR2 = (TIM_CCMR2 & 0x00FFu) | (6u << 12) | (1u << 11);   /* OC4M = PWM mode 1 (as stock 0x60), OC4PE */
    TIM_CCER |= 1u << 12;                                  /* CC4E, active high */
    TIM_DIER = 1u << 8;                                    /* UDE: update -> GPDMA request 71 */
    TIM_EGR = 1u;
    TIM_CR1 = (1u << 7) | 1u;                              /* ARPE, CEN */
    gpio_af(TX_PWM_PORT, TX_PWM, TX_PWM_AF);
    gpio_mode(TX_PWM_PORT, TX_PWM, GPIO_AF, PULL_NONE);    /* stock FUN_0811a730 */
    s_streaming = true;
}

void dac_stream_stop(void)
{
    if(!s_streaming) { return; }
    s_streaming = false;
    TIM_DIER = 0;
    GPDMA_C(TX_DMA_CH, GPDMA_CR) = 0;
    TIM_CCR4 = DAC_CODE(2048u);
    TIM_CR1 = 0;
    TIM_CCER &= ~(1u << 12);
    gpio_mode(TX_PWM_PORT, TX_PWM, GPIO_ANALOG, PULL_NONE);   /* stock FUN_0811a6d4 */
}

uint32_t dac_read_pos(void)
{
    uint32_t left = GPDMA_C(TX_DMA_CH, GPDMA_BR1) & 0xFFFFu;
    uint32_t total = DAC_RING * 2u;
    if(left == 0 || left > total) { return 0; }
    return ((total - left) / 2u) % DAC_RING;
}
