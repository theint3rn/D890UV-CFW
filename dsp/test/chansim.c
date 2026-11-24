#include <math.h>
#include <string.h>
#include <complex.h>
#include "chansim.h"

#define PT_RES  64          /* pulse table resolution: points per symbol */
#define PT_SPAN 6           /* +/- symbols */
#define PT_N    (2 * PT_SPAN * PT_RES + 1)

static double rrc(double t, double a)
{
    if(fabs(t) < 1e-9) { return 1.0 - a + 4.0 * a / M_PI; }
    if(fabs(fabs(t) - 1.0 / (4.0 * a)) < 1e-6) {
        return a / sqrt(2.0) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
    }
    return (sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

/* C4FM transmit shaping: raised cosine (alpha 0.2) x inverse sinc, by numeric inverse Fourier transform */
static double c4fm(double t)
{
    const double a = 0.2, fmax = (1 + a) / 2;
    const int steps = 1500;
    double acc = 0;
    for(int i = 0; i < steps; i++) {
        double f = (i + 0.5) * fmax / steps;
        double h = f < (1 - a) / 2 ? 1.0 : 0.5 * (1 + cos(M_PI / a * (f - (1 - a) / 2)));
        double inv = M_PI * f / sin(M_PI * f);
        acc += h * inv * cos(2 * M_PI * f * t);
    }
    return 2 * acc * fmax / steps;
}

/* GMSK frequency pulse: rectangle of one symbol convolved with a Gaussian, BT 0.5 */
static double gmsk(double t)
{
    double k = M_PI * 0.5 * sqrt(2.0 / log(2.0));
    return 0.5 * (erf(k * (t + 0.5)) - erf(k * (t - 0.5)));
}

typedef struct { double p[PT_N]; int kind; } ptab_t;

static void ptab_build(ptab_t *pt, int kind)
{
    for(int i = 0; i < PT_N; i++) {
        double t = (double)(i - PT_SPAN * PT_RES) / PT_RES;
        pt->p[i] = kind == 0 ? rrc(t, 0.2) : kind == 1 ? c4fm(t) : gmsk(t);
    }
    /* normalise: a run of equal symbols gives exactly the symbol level at the symbol instants */
    double s = 0;
    for(int k = -PT_SPAN; k <= PT_SPAN; k++) { s += pt->p[PT_SPAN * PT_RES + k * PT_RES]; }
    for(int i = 0; i < PT_N; i++) { pt->p[i] /= s; }
    pt->kind = kind;
}

static double ptab(const ptab_t *pt, double t)
{
    double x = (t + PT_SPAN) * PT_RES;
    if(x <= 0 || x >= PT_N - 1) { return 0; }
    int i = (int)x;
    double f = x - i;
    return pt->p[i] + f * (pt->p[i + 1] - pt->p[i]);
}

static uint32_t g_rng;
static double urand(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng + 0.5) / 4294967296.0; }
static double gauss(void) { return sqrt(-2.0 * log(urand())) * cos(2 * M_PI * urand()); }

static double level_of(dsp_mode_t m, uint8_t s)
{
    if(dsp_mode_info(m)->levels == 2) { return s ? 1.0 : -1.0; }
    return dsp_level_of(s);
}

#define IF_TAPS 65
static double g_if[IF_TAPS];
static void if_build(void)
{
    double fc = 6250.0 / SIM_FS, sum = 0;
    for(int i = 0; i < IF_TAPS; i++) {
        int k = i - IF_TAPS / 2;
        double w = 0.54 - 0.46 * cos(2 * M_PI * i / (IF_TAPS - 1));
        g_if[i] = (k == 0 ? 2 * fc : sin(2 * M_PI * fc * k) / (M_PI * k)) * w;
        sum += g_if[i];
    }
    for(int i = 0; i < IF_TAPS; i++) { g_if[i] /= sum; }
}

/* complex baseband transmitter: returns number of samples */
static size_t tx(dsp_mode_t mode, const uint8_t *syms, size_t n, const chan_t *ch, double complex *x, size_t max)
{
    static ptab_t pt;
    const dsp_mode_info_t *mi = dsp_mode_info(mode);
    double ts = (1.0 / mi->sym_rate) * (1.0 + ch->ppm * 1e-6);
    size_t ns = (size_t)(n * ts * SIM_FS);
    if(ns > max) { ns = max; }
    if(mode == DSP_MODE_P25P2) {
        /* H-DQPSK: phase steps level * pi/4, RRC 0.2 shaped complex symbols */
        static double complex c[400000];
        double th = 0;
        for(size_t k = 0; k < n && k < 400000; k++) { th += level_of(mode, syms[k]) * M_PI / 4; c[k] = cexp(I * th); }
        double pw = 0;
        for(size_t i = 0; i < ns; i++) {
            double u = i / (double)SIM_FS / ts;
            long k0 = (long)floor(u);
            double complex v = 0;
            for(long k = k0 - PT_SPAN; k <= k0 + PT_SPAN; k++) {
                if(k < 0 || k >= (long)n) { continue; }
                v += c[k] * rrc(u - k, 0.2);
            }
            x[i] = v * cexp(I * 2 * M_PI * ch->foff_hz * i / SIM_FS);
            pw += creal(v * conj(v));
        }
        pw /= ns;
        for(size_t i = 0; i < ns; i++) { x[i] /= sqrt(pw); }
        return ns;
    }
    int kind = mi->filter == DSP_F_RRC ? 0 : mi->filter == DSP_F_ID ? 1 : 2;
    if(pt.kind != kind || pt.p[PT_SPAN * PT_RES] == 0) { ptab_build(&pt, kind); }
    double unit = mi->outer_hz / (mi->levels == 4 ? 3.0 : 1.0) * (1.0 + ch->dev_err);
    double ph = 0;
    for(size_t i = 0; i < ns; i++) {
        double u = i / (double)SIM_FS / ts;
        long k0 = (long)floor(u);
        double f = 0;
        for(long k = k0 - PT_SPAN; k <= k0 + PT_SPAN; k++) {
            if(k < 0 || k >= (long)n) { continue; }
            f += level_of(mode, syms[k]) * ptab(&pt, u - k);
        }
        f = f * unit + ch->foff_hz;
        ph += 2 * M_PI * f / SIM_FS;
        x[i] = cexp(I * ph);
    }
    return ns;
}

static void channel(double complex *x, size_t ns, const chan_t *ch)
{
    g_rng = ch->seed ? ch->seed : 0x1234567u;
    if(ch->cnr_db < 99) {
        double cnr = pow(10.0, ch->cnr_db / 10.0);
        double sigma = sqrt(SIM_FS / 12500.0 / cnr / 2.0);
        for(size_t i = 0; i < ns; i++) { x[i] += sigma * (gauss() + I * gauss()); }
    }
}

static double complex g_x[1500000];
static double complex g_y[1500000];

size_t chansim_run(dsp_mode_t mode, const uint8_t *syms, size_t n, const chan_t *ch, uint16_t *out, size_t max)
{
    if(g_if[IF_TAPS / 2] == 0) { if_build(); }
    size_t ns = tx(mode, syms, n, ch, g_x, sizeof g_x / sizeof g_x[0]);
    channel(g_x, ns, ch);
    for(size_t i = 0; i < ns; i++) {
        double complex acc = 0;
        for(int t = 0; t < IF_TAPS; t++) { if(i >= (size_t)t) { acc += g_if[t] * g_x[i - t]; } }
        g_y[i] = acc;
    }
    double de = ch->deemph_tau_us > 0 ? 1.0 - exp(-1.0 / (ch->deemph_tau_us * 1e-6 * SIM_FS)) : 1.0, dy = 0;
    size_t k = 0;
    for(size_t i = 1; i < ns && k < max; i++) {
        double hz = carg(g_y[i] * conj(g_y[i - 1])) * SIM_FS / (2 * M_PI);
        dy += (hz - dy) * de;
        double c = 2048.0 + 0.25 * dy;
        out[k++] = (uint16_t)(c < 0 ? 0 : c > 4095 ? 4095 : lround(c));
    }
    return k;
}

size_t chansim_fm(const float *hz, size_t n, uint32_t fs_in, const chan_t *ch, uint16_t *out, size_t max)
{
    if(g_if[IF_TAPS / 2] == 0) { if_build(); }
    double r = (double)fs_in / SIM_FS / (1.0 + ch->ppm * 1e-6);   /* input samples per output sample */
    size_t ns = (size_t)((n - 1) / r);
    if(ns > sizeof g_x / sizeof g_x[0]) { ns = sizeof g_x / sizeof g_x[0]; }
    double ph = 0;
    for(size_t i = 0; i < ns; i++) {
        double x = i * r;
        size_t j = (size_t)x;
        double fr = x - j, f = hz[j] + fr * (hz[j + 1] - hz[j]);
        f = f * (1.0 + ch->dev_err) + ch->foff_hz;
        ph += 2 * M_PI * f / SIM_FS;
        g_x[i] = cexp(I * ph);
    }
    channel(g_x, ns, ch);
    for(size_t i = 0; i < ns; i++) {
        double complex acc = 0;
        for(int t = 0; t < IF_TAPS; t++) { if(i >= (size_t)t) { acc += g_if[t] * g_x[i - t]; } }
        g_y[i] = acc;
    }
    double de = ch->deemph_tau_us > 0 ? 1.0 - exp(-1.0 / (ch->deemph_tau_us * 1e-6 * SIM_FS)) : 1.0, dy = 0;
    size_t k = 0;
    for(size_t i = 1; i < ns && k < max; i++) {
        double h = carg(g_y[i] * conj(g_y[i - 1])) * SIM_FS / (2 * M_PI);
        dy += (h - dy) * de;
        double c = 2048.0 + 0.25 * dy;
        out[k++] = (uint16_t)(c < 0 ? 0 : c > 4095 ? 4095 : lround(c));
    }
    return k;
}

size_t chansim_dqpsk_coherent(const uint8_t *syms, size_t n, const chan_t *ch, uint8_t *out, size_t max)
{
    chan_t c = *ch;
    c.foff_hz = 0;                                /* ideal AFC */
    size_t ns = tx(DSP_MODE_P25P2, syms, n, &c, g_x, sizeof g_x / sizeof g_x[0]);
    channel(g_x, ns, &c);
    double ts = (1.0 / 6000) * (1.0 + c.ppm * 1e-6);
    double sps = ts * SIM_FS;
    int half = (int)(PT_SPAN * sps);
    double complex prev = 0;
    size_t k = 0;
    for(size_t s = 0; s < n && k < max; s++) {
        long ci = lround(s * sps);                /* genie timing */
        double complex acc = 0, norm = 0;
        for(int t = -half; t <= half; t++) {
            long i = ci + t;
            if(i < 0 || i >= (long)ns) { continue; }
            double h = rrc(t / sps, 0.2);
            acc += h * g_x[i];
            norm += h * h;
        }
        (void)norm;
        double d = carg(acc * conj(prev)) / (M_PI / 4);
        prev = acc;
        out[k++] = d >= 2 ? 1 : d >= 0 ? 0 : d >= -2 ? 2 : 3;
    }
    return k;
}
