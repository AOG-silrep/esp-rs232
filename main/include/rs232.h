#ifndef ESP32_XBEE_RS232_H
#define ESP32_XBEE_RS232_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RS232_SENTENCE_GGA = 0,
    RS232_SENTENCE_VTG,
    RS232_SENTENCE_RMC,
    RS232_SENTENCE_GSA,
    RS232_SENTENCE_GST,
    RS232_SENTENCE_GSV,
    RS232_SENTENCE_COUNT
} rs232_sentence_t;

extern const char *const RS232_SENTENCE_NAMES[RS232_SENTENCE_COUNT];

typedef struct rs232_status {
    bool active;
    uint32_t baud;
    uint8_t rate_hz[RS232_SENTENCE_COUNT]; // Output rate of each sentence, 0 = off

    // Last GGA: satellites the receiver reported, and what went out (-1 if no GGA yet)
    int gga_sats_in;
    int gga_sats_out;
    const char *sats_source; // "NAV-SAT", "GSA" or "GGA" (passed through unchanged)

    uint32_t sentences; // Sent
    uint32_t dropped; // Didn't fit the TX buffer, the baud rate is too low for the enabled sentences
} rs232_status_t;

void rs232_init();

void rs232_get_status(rs232_status_t *status);

#endif //ESP32_XBEE_RS232_H
