/* IMBE 7200x4400 dequantisation tables (imbe_tables.c, generated from mbelib, ISC). Index [L-9]. */
#ifndef IMBE_TABLES_H
#define IMBE_TABLES_H
#include <stdint.h>

extern const float   imbe_quantstep[10];   /* step for B bits: [B-1] */
extern const float   imbe_standdev[9];     /* std dev of DCT coefficient k: [k-2] */
extern const float   imbe_b2[64];          /* G1 (b2) codebook */
extern const uint8_t imbe_ba_bits[48][5];  /* bits of b3..b7 (G2..G6) */
extern const float   imbe_ba_step[48][5];  /* step of b3..b7 */
extern const uint8_t imbe_hoba[48][50];    /* bits of b8.. (higher-order DCT coefficients) */
extern const uint8_t imbe_bo_m[48][79];    /* info bit 6+i -> parameter index m (b_m) */
extern const uint8_t imbe_bo_b[48][79];    /*                -> bit number inside b_m (0 = LSB) */
extern const uint8_t imbe_ji[48][6];       /* residual block lengths J_1..J_6 */

#endif
