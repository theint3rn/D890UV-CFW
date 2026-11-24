#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp_demod.h"
#include "chansim.h"
static uint8_t ref[400000], out[400000]; static int8_t soft[400000]; static uint16_t adc[1600000]; static float f[1600000];
int main(int argc, char **argv)
{
    int m = atoi(argv[1]); float cnr = atof(argv[3]);
    FILE *fp = fopen(argv[2], "rb"); size_t n = fread(ref, 1, sizeof ref, fp); fclose(fp);
    chan_t ch = { cnr, argc > 4 ? atof(argv[4]) : 0, argc > 5 ? atof(argv[5]) : 0, 0, 0, 5 };
    size_t na = chansim_run((dsp_mode_t)m, ref, n, &ch, adc, sizeof adc / 2);
    static dsp_demod_t d; dsp_init(&d, (dsp_mode_t)m, 48000, NULL);
    for(size_t i = 0; i < na; i++) f[i] = adc[i] - 2048.0f;
    size_t no = 0;
    for(size_t i = 0; i < na; i += 480) {
        size_t k = na - i < 480 ? na - i : 480;
        no += dsp_process(&d, f + i, k, out + no, soft + no, sizeof out - no);
        if((i / 480) % 50 == 0) printf("t=%.2fs syms=%zu dc=%.1f gain=%.2f tint=%.4f evm=%.3f syncs=%u corr=%.2f\n", i/48000.0, no, d.dc, d.gain, d.t_int, d.evm, d.syncs, d.last_corr);
    }
    printf("nref %zu nout %zu\n", n, no);
    /* best offset over a middle window and error histogram */
    for(size_t j0 = 1000; j0 + 300 < no; j0 += (no-1300)/6) {
        int bo=0,bm=-1; for(int o=-800;o<=800;o++){int mm=0; for(size_t j=j0;j<j0+300;j++){long i=(long)j+o; if(i>=0&&i<(long)n&&ref[i]==out[j])mm++;} if(mm>bm){bm=mm;bo=o;}}
        printf("j0=%zu off=%d match=%d/300\n", j0, bo, bm);
    }
    {   /* error positions with off from first window */
        int bo=0,bm=-1; for(int o=-800;o<=800;o++){int mm=0; for(size_t j=1000;j<1300;j++){long i=(long)j+o; if(i>=0&&i<(long)n&&ref[i]==out[j])mm++;} if(mm>bm){bm=mm;bo=o;}}
        int cnt=0; for(size_t j=0;j<no;j++){long i=(long)j+bo; if(i>=0&&i<(long)n&&ref[i]!=out[j]){ if(cnt<30) printf("err j=%zu ",j); cnt++;}} printf("\ntotal sym errs %d\n",cnt);
    }
    return 0;
}
