/* SCT3288 command allow-list, used by bb_send() of both radios (drivers/baseband.c). Only vocoder, codec and status
 * commands that the stock firmware itself uses at run time (cfw/docs/SCT3288_VOCODER.md section 2) pass. Everything
 * else is refused: flash, boot, program, activation / licence, memory read / write, reset, RF transmit / call control
 * (WORK_MODE TX, CALL_START, CONNECT, SEND_HEAD ...), and anything unknown. */
#ifndef CFW_BB_ALLOW_H
#define CFW_BB_ALLOW_H
#include <stdint.h>
#include <stdbool.h>

static inline bool bb_allowed(uint8_t b0, uint8_t id, const uint8_t *d, uint16_t len)
{
    const uint8_t v = len ? d[0] : 0xFF;
    switch(b0) {
    case 0x00:
    case 0x05:                                              /* the same control fields; stock uses 05 for some */
        switch(id) {
        case 0x00: return b0 == 0 && len == 322 && v == 0x00 && d[1] == 0xA0;          /* speech packet (PCM) */
        case 0x01:                                          /* CHAN_D: voice frames / frame format only */
            if(b0) { return false; }
            return (v == 0x24 && len == 37) || (v == 0x1B && len == 28) ||
                   (v == 0xFF && len == 2 && (d[1] == 0x00 || d[1] == 0x02 || d[1] == 0x06));
        case 0x10: return b0 == 0 && len == 1 && (v == 0x04 || v == 0x0C || v == 0x10);  /* VOCODER_SEL */
        case 0x18:                                          /* WORK_MODE: idle, or duplex with the vocoder loop */
            return len == 3 && d[2] == 0 && ((v == 0x00 && d[1] == 0x00) || (v == 0x03 && d[1] == 0x03));
        case 0x19: return len == 1 && v == 0x00;            /* carrier lost */
        case 0x1A: return len == 1 && (v == 0x02 || v == 0x80);                        /* PROCESS_MODE */
        case 0x2D: return b0 == 0 && len == 1 && v == 0x04; /* CODEC_SELECT, the stock value */
        case 0x3E: return b0 == 0 && len == 1;              /* VOCODER_IO_SET */
        case 0x68:                                          /* digital mic gain */
        case 0x69: return b0 == 0 && len == 2;              /* digital speaker gain */
        default:   return false;
        }
    case 0x20: return len == 0 && (id == 0x10 || id == 0x1A);                          /* read VOCODER_SEL / PROCESS_MODE */
    case 0x23: return len == 0 && id >= 0x30 && id <= 0x33; /* identity / version reads */
    case 0x40:                                              /* audio codec, the registers stock writes */
        if(len != 1) { return false; }
        return id == 0x08 || id == 0x09 || id == 0x0B || id == 0x0F || id == 0x10 || id == 0x11;
    case 0x80:                                              /* external AMBE chip: PRODID, VERSTRING, rate */
        return (len == 0 && (id == 0x30 || id == 0x31)) || (len == 1 && id == 0x09) || (len == 12 && id == 0x0A);
    case 0x81: return id == 0x01 && len >= 2 && len <= 10;
    case 0x82: return id == 0x00 && len == 321;
    default:   return false;
    }
}
#endif
