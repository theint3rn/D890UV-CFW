/* Tone measurement (shared by the firmware RX tone meter and the host tests), see tonemeter.h.
 * Float only (the M33 FPU is single precision). Frequency: interpolated rising zero crossings.
 * SINAD: least-squares fit of a*cos + b*sin + c at that frequency (3 refinement steps), residual = N+D. */
#include <math.h>
#include "tonemeter.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float fit(const int16_t *x, size_t n, float fs, float f, float mean, float *amp)
{
    float sc = 0, ss = 0, scs = 0, xc = 0, xs = 0, sx2 = 0;
    float w = 2.0f * (float)M_PI * f / fs;
    float c = 1, s = 0, cw = cosf(w), sw = sinf(w);
    for(size_t i = 0; i < n; i++) {
        float v = (float)x[i] - mean;
        sc += c * c; ss += s * s; scs += c * s; xc += v * c; xs += v * s; sx2 += v * v;
        float c2 = c * cw - s * sw;                       /* rotate (renormalise every 256 steps) */
        s = s * cw + c * sw; c = c2;
        if((i & 255) == 255) { float r = 1.0f / sqrtf(c * c + s * s); c *= r; s *= r; }
    }
    float det = sc * ss - scs * scs;
    if(det <= 0) { *amp = 0; return sx2; }
    float a = (xc * ss - xs * scs) / det, b = (xs * sc - xc * scs) / det;
    *amp = sqrtf(a * a + b * b);
    float res = sx2 - (a * xc + b * xs);
    return res > 0 ? res : 0;
}

tone_meas_t tone_measure(const int16_t *x, size_t n, float fs)
{
    tone_meas_t r = { 0, 0, 0 };
    if(n < 64) { return r; }
    float mean = 0;
    for(size_t i = 0; i < n; i++) { mean += (float)x[i]; }
    mean /= (float)n;
    float first = -1, last = -1;
    int cnt = 0;
    for(size_t i = 1; i < n; i++) {
        float a = (float)x[i - 1] - mean, b = (float)x[i] - mean;
        if(a < 0 && b >= 0) {
            float t = (float)(i - 1) + a / (a - b);
            if(first < 0) { first = t; } else { cnt++; }
            last = t;
        }
    }
    if(cnt < 2) { return r; }
    float f = (float)cnt * fs / (last - first);
    float amp, best = fit(x, n, fs, f, mean, &amp), bf = f, ba = amp;
    for(int it = -3; it <= 3; it++) {
        if(!it) { continue; }
        float ff = f * (1.0f + (float)it * 5e-5f);
        float res = fit(x, n, fs, ff, mean, &amp);
        if(res < best) { best = res; bf = ff; ba = amp; }
    }
    float sigp = ba * ba / 2.0f * (float)n;
    r.freq = bf;
    r.amp = ba;
    r.sinad_db = 10.0f * log10f(sigp / (best > 1e-3f ? best : 1e-3f));
    return r;
}
