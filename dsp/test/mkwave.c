/* Emulator sample files: proto test calls -> channel simulator (C/N 25 dB, +500 Hz offset, +50 ppm clock)
 * -> discriminator ADC samples at 48 kHz.
 *   NAME.adc16  uint16 LE, 12-bit ADC counts (2048 + 0.25 count/Hz) for `analog Stream @file 48000` (cfw-emu)
 *   NAME.adc8   one byte per sample = counts >> 4, for the temporary path `syms Load @file 48000` (make EMU=1) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp_demod.h"
#include "chansim.h"

static uint8_t g_sym[400000];
static uint16_t g_adc[1600000];
static uint8_t g_b[1600000];

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static const struct { dsp_mode_t m; const char *in, *out; } k[] = {
        { DSP_MODE_DMR,    "dmr_call.sym",   "dmr_call" },
        { DSP_MODE_DMR,    "dmr_t3.sym",     "dmr_t3" },
        { DSP_MODE_P25P1,  "p25_call.sym",   "p25_call" },
        { DSP_MODE_P25P1,  "p25_trunk.sym",  "p25_trunk" },
        { DSP_MODE_P25P2,  "p25p2_call.sym", "p25p2_call" },
        { DSP_MODE_NXDN48, "nxdn_call.sym",  "nxdn_call" },
        { DSP_MODE_DSTAR,  "dstar_call.sym", "dstar_call" },
        { DSP_MODE_YSF,    "ysf_call.sym",   "ysf_call" },
    };
    for(size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, k[i].in);
        FILE *f = fopen(p, "rb");
        if(!f) { perror(p); return 1; }
        size_t n = fread(g_sym, 1, sizeof g_sym, f);
        fclose(f);
        chan_t ch = { 25.0f, 500.0f, 50.0f, 0.0f, 0.0f, 4242u + (uint32_t)i };
        size_t na = chansim_run(k[i].m, g_sym, n, &ch, g_adc, sizeof g_adc / sizeof g_adc[0]);
        snprintf(p, sizeof p, "%s/%s.adc16", dir, k[i].out);
        f = fopen(p, "wb"); fwrite(g_adc, 2, na, f); fclose(f);
        for(size_t j = 0; j < na; j++) { g_b[j] = (uint8_t)(g_adc[j] >> 4); }
        snprintf(p, sizeof p, "%s/%s.adc8", dir, k[i].out);
        f = fopen(p, "wb"); fwrite(g_b, 1, na, f); fclose(f);
        printf("%-12s %7zu samples %.1f s @ 48 kHz (%s)\n", k[i].out, na, na / 48000.0, dsp_mode_info(k[i].m)->name);
    }
    return 0;
}
