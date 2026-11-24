/* cfw/dsp modulators, see dsp_mod.h. */
#include <math.h>
#include <string.h>
#include "dsp_mod.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float rrc(float t, float a)
{
    if(fabsf(t) < 1e-6f) { return 1.0f - a + 4.0f * a / (float)M_PI; }
    if(fabsf(fabsf(t) - 1.0f / (4.0f * a)) < 1e-4f) {
        return a / sqrtf(2.0f) * ((1 + 2 / (float)M_PI) * sinf((float)M_PI / (4 * a)) + (1 - 2 / (float)M_PI) * cosf((float)M_PI / (4 * a)));
    }
    return (sinf((float)M_PI * t * (1 - a)) + 4 * a * t * cosf((float)M_PI * t * (1 + a))) /
           ((float)M_PI * t * (1 - 16 * a * a * t * t));
}

static float c4fm(float t)            /* RC 0.2 x pi f T / sin(pi f T), inverse Fourier (numeric) */
{
    const float a = 0.2f, fmax = (1 + a) / 2;
    const int steps = 240;
    float acc = 0;
    for(int i = 0; i < steps; i++) {
        float f = ((float)i + 0.5f) * fmax / (float)steps;
        float h = f < (1 - a) / 2 ? 1.0f : 0.5f * (1 + cosf((float)M_PI / a * (f - (1 - a) / 2)));
        acc += h * ((float)M_PI * f / sinf((float)M_PI * f)) * cosf(2 * (float)M_PI * f * t);
    }
    return 2 * acc * fmax / (float)steps;
}

static float gmsk(float t)
{
    float k = (float)M_PI * 0.5f * sqrtf(2.0f / logf(2.0f));
    return 0.5f * (erff(k * (t + 0.5f)) - erff(k * (t - 0.5f)));
}

static float level_of(dsp_mode_t m, uint8_t s)
{
    if(s == DSP_SYM_GAP) { return 0; }                  /* TDMA gap: carrier, no modulation */
    if(dsp_mode_info(m)->levels == 2) { return s ? 1.0f : -1.0f; }
    return dsp_level_of(s);
}

void dsp_mod_init(dsp_mod_t *m, dsp_mode_t mode, uint32_t fs)
{
    memset(m, 0, sizeof *m);
    m->mode = mode;
    m->fs = (float)fs;
    const dsp_mode_info_t *mi = dsp_mode_info(mode);
    m->step = (float)mi->sym_rate / (float)fs;
    m->unit = mi->outer_hz / (mi->levels == 4 ? 3.0f : 1.0f);
    for(int i = 0; i < DSP_MOD_TAB; i++) {
        float t = (float)(i - DSP_MOD_SPAN * DSP_MOD_RES) / DSP_MOD_RES;
        float v;
        if(mode == DSP_MODE_P25P2) { v = (t >= 0 && t < 1) ? 1.0f - cosf(2.0f * (float)M_PI * t) : 0; }  /* 1RC */
        else if(mi->filter == DSP_F_RRC) { v = rrc(t, 0.2f); }
        else if(mi->filter == DSP_F_GAUSS) { v = gmsk(t); }
        else { v = c4fm(t); }
        m->tab[i] = v;
    }
    if(mode != DSP_MODE_P25P2) {   /* a run of equal symbols = exactly the level at the symbol instants */
        float s = 0;
        for(int k = -DSP_MOD_SPAN; k <= DSP_MOD_SPAN; k++) { s += m->tab[DSP_MOD_SPAN * DSP_MOD_RES + k * DSP_MOD_RES]; }
        for(int i = 0; i < DSP_MOD_TAB; i++) { m->tab[i] /= s; }
    }
}

static float pulse(const dsp_mod_t *m, float t)
{
    float x = (t + DSP_MOD_SPAN) * DSP_MOD_RES;
    if(x <= 0 || x >= DSP_MOD_TAB - 1) { return 0; }
    int i = (int)x;
    float f = x - (float)i;
    return m->tab[i] + f * (m->tab[i + 1] - m->tab[i]);
}

/* shift the symbol window by one, pulling the next symbol (level 0 after the end) */
static void advance(dsp_mod_t *m, dsp_mod_pull_t pull, void *user)
{
    const int nl = 2 * DSP_MOD_SPAN + 2;
    memmove(m->lv, m->lv + 1, sizeof(float) * (size_t)(nl - 1));
    uint8_t s;
    if(!m->ended && pull(user, &s, 1) == 1) { m->lv[nl - 1] = level_of(m->mode, s); return; }
    m->lv[nl - 1] = 0;
    if(++m->ended > 2 * DSP_MOD_SPAN + 2) { m->done = true; }
}

size_t dsp_mod_run(dsp_mod_t *m, float *hz, size_t n, dsp_mod_pull_t pull, void *user)
{
    size_t k = 0;
    while(k < n && !m->done) {
        /* lv[SPAN] is the symbol whose instant is u = 0 */
        float f = 0;
        for(int j = -DSP_MOD_SPAN; j <= DSP_MOD_SPAN + 1; j++) {
            float a = m->lv[j + DSP_MOD_SPAN];
            if(a != 0) { f += a * pulse(m, m->u - (float)j); }
        }
        hz[k++] = f * m->unit;
        m->u += m->step;
        while(m->u >= 1.0f) { m->u -= 1.0f; advance(m, pull, user); }
    }
    return k;
}
