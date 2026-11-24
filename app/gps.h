/* GPS receiver (NMEA 0183 on USART2: $xxRMC, $xxGGA) + APRS position beacon (AX.25 UI frame, Bell 202 AFSK on the
 * analog FM TX path). The board UART driver feeds bytes with gps_uart_byte() from its interrupt. */
#ifndef CFW_GPS_H
#define CFW_GPS_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     valid;             /* RMC status A */
    int32_t  lat_e7, lon_e7;    /* degrees * 1e7, + = N / E */
    int32_t  alt_m;
    uint16_t speed_kn10, course;
    uint8_t  sats;
    uint32_t utc;               /* seconds since 1970 (RMC date + time), 0 = unknown */
    uint32_t sentences;
} gps_fix_t;

#ifndef CFW_GPS_TXT
#define CFW_GPS_TXT "USART2 NMEA 9600"
#endif

void gps_init(void);
void gps_poll(void);                         /* main loop / 10 ms: parse received sentences */
void gps_uart_byte(uint8_t b);               /* board UART IRQ */
void gps_uart_hw_init(void);                 /* board: USART2 9600 8N1 RX interrupt (weak no-op default) */
bool gps_present(void);                      /* NMEA received in the last 5 s */
const gps_fix_t *gps_fix(void);
void gps_fmt_lat(char *t, int32_t e7);       /* "32.71570 N" */
void gps_fmt_lon(char *t, int32_t e7);
void gps_fmt_loc(char *t, int32_t lat, int32_t lon);   /* Maidenhead locator, 6 chars */
uint32_t gps_beacons(void);
const char *gps_beacon_now(void);            /* NULL = sent, else the reason it was not */
bool gps_nmea_feed(const char *line);        /* parse one sentence (tests) */
#endif
