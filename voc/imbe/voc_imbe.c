/* cfw vocoder backend "imbe" (see voc_imbe.h). */
#include <string.h>
#include "voc_imbe.h"
#include "imbe.h"

static imbe_enc_t s_enc;
static imbe_dec_t s_dec;
static bool s_ready;

void voc_imbe_reset_rx(void) { imbe_dec_init(&s_dec); }
void voc_imbe_reset_tx(void) { imbe_enc_init(&s_enc); }

void voc_imbe_init(void)
{
    voc_imbe_reset_rx();
    voc_imbe_reset_tx();
    s_ready = true;
}

size_t voc_imbe_encode(const int16_t pcm[160], uint8_t bits[11])
{
    if (!s_ready) voc_imbe_init();
    imbe_encode(&s_enc, pcm, bits);
    return 88;
}

int voc_imbe_decode(const uint8_t bits[11], int errs, int16_t pcm[160])
{
    if (!s_ready) voc_imbe_init();
    return imbe_decode(&s_dec, bits, errs, pcm);
}

static int ops_decode(const uint8_t *bits, size_t nbits, int16_t pcm[160])
{
    if (nbits != 88) return -1;
    voc_imbe_decode(bits, 0, pcm);
    return 0;
}

static size_t ops_encode(const int16_t pcm[160], uint8_t *bits, size_t nbits)
{
    if (nbits < 88) return 0;
    return voc_imbe_encode(pcm, bits);
}

const cfw_vocoder_ops_t g_voc_imbe = {
    .id = CFW_VOC_IMBE_88,
    .name = "imbe",
    .interoperable = true,
    .decode = ops_decode,
    .encode = ops_encode,
};
