/* Bare-metal Cortex-M33 benchmark of the IMBE codec (runs in Renode, see run_bench.sh).
 * MODE 0: setup only, 1: encode FRAMES frames, 2: encode + decode FRAMES frames, 3: FRAMES FFTs. */
#include <stdint.h>
#include <math.h>
#include "imbe.h"
#ifndef MODE
#define MODE 2
#endif
#define FRAMES 50
extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);
void Reset_Handler(void)
{
    *(volatile uint32_t *)0xE000ED88 |= 0xFu << 20;           /* CPACR: FPU on */
    __asm volatile("dsb; isb");
    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) *d++ = *s++;
    for (uint32_t *d = &_sbss; d < &_ebss;) *d++ = 0;
    main();
    for (;;) __asm volatile("wfi");
}
static void loop(void) { for (;;) ; }
__attribute__((section(".isr_vector"), used)) const void *const vectors[16] = {
    &_estack, Reset_Handler, loop, loop, loop, loop, loop, 0, 0, 0, 0, loop, loop, 0, loop, loop };

volatile uint32_t g_done, g_check;
static imbe_enc_t enc;
static imbe_dec_t dec;
static int16_t pcm[160], out[160];

int main(void)
{
    imbe_enc_init(&enc);
    imbe_dec_init(&dec);
    float ph1 = 0, ph2 = 0;
    for (int f = 0; f < ((MODE == 1 || MODE == 2) ? FRAMES : 0); f++) {
        for (int i = 0; i < 160; i++) {                       /* vowel-like: 2 partials + noise-ish */
            ph1 += 0.0942f; ph2 += 0.6283f;
            if (ph1 > 6.2831853f) ph1 -= 6.2831853f;
            if (ph2 > 6.2831853f) ph2 -= 6.2831853f;
            pcm[i] = (int16_t)(6000.0f * sinf(ph1) + 2000.0f * sinf(ph2) + (float)((i * 7919) % 401 - 200));
        }
        uint8_t b[11];
        imbe_encode(&enc, pcm, b);
        if (MODE == 2) imbe_decode(&dec, b, 0, out);
        g_check += b[0] + (uint32_t)out[80];
    }
#if MODE == 3
    static float re[512], im[512];
    for (int f = 0; f < FRAMES; f++) { re[f] = 1; imbe_fft512(re, im, false); }
    g_check += (uint32_t)re[3];
#endif
    g_done = 1;
    return 0;
}
