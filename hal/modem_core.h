/* Platform side of the modem: proto registry, mode switching, symbol plumbing between backends and the
 * active proto, vocoder dispatch. (Protocol libraries only need hal/modem.h.) */
#ifndef CFW_MODEM_CORE_H
#define CFW_MODEM_CORE_H
#include "modem.h"

void  modem_init(void);
int   modem_proto_count(void);
const cfw_proto_t *modem_proto_at(int i);
const cfw_proto_t *modem_find(cfw_mode_t mode);
const char *modem_mode_name(cfw_mode_t mode);

/* Enter a mode on a frequency. FM needs no proto. Returns false if no proto / backend is available
 * (the UI then shows "no proto"/"no modem"; RF stays in FM receive). */
bool  modem_set_mode(cfw_mode_t mode, const cfw_chan_cfg_t *cfg);
void  modem_configure(const cfw_chan_cfg_t *cfg);        /* channel settings edited: pass to the proto */
typedef enum { MODEM_OK, MODEM_NO_PROTO, MODEM_NO_DEMOD } modem_avail_t;
modem_avail_t modem_availability(void);                   /* why the last modem_set_mode() failed */
const char *modem_backend_for(cfw_mode_t mode);           /* backend that would run this mode, NULL = none */
uint32_t modem_rx_hz(void);                               /* frequency the modem is on (trunking may move it) */
cfw_mode_t modem_mode(void);
bool  modem_active(void);                    /* a digital proto is running */
const char *modem_backend_name(void);

void  modem_ptt(bool down, const cfw_tx_req_t *req);
bool  modem_transmitting(void);
void  modem_poll(void);                      /* main loop, every pass */
void  modem_tick(void);                      /* every 10 ms */

size_t modem_status(char *buf, size_t len);  /* proto status line */
const cfw_call_info_t *modem_call_info(void);
uint32_t modem_voice_frames(void);           /* decoded voice frames since mode entry */
uint32_t modem_rx_symbol_count(void);

/* for backends */
void   modem_rx_push(const uint8_t *sym, const int8_t *soft, size_t n);
size_t modem_tx_pull(uint8_t *sym, size_t max);
void   modem_tx_done(void);                      /* backend: TX tail sent, back to receive */
void   modem_register_backend(const cfw_modem_backend_t *b);
void   modem_register_vocoder(const cfw_vocoder_ops_t *v);

/* backends provided by the platform */
extern const cfw_modem_backend_t g_backend_sct3288;   /* SCT3288 baseband (not functional yet) */
extern const cfw_modem_backend_t g_backend_loopback;  /* TX symbols -> RX, for testing protos */
extern const cfw_modem_backend_t g_backend_emusyms;
extern const cfw_modem_backend_t g_backend_mcudsp;    /* ADC1 ch1 discriminator + cfw/dsp (hal/dsp_backend.c) */   /* emulator symbol FIFO (make EMU=1 only), hal/emu_syms.c */

/* proto -> platform requests the main loop acts on */
extern volatile bool g_modem_tx_request;
extern volatile uint32_t g_modem_tune_rx, g_modem_tune_tx;

#endif
