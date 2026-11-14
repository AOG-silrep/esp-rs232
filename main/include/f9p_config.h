#ifndef ESP32_XBEE_F9P_CONFIG_H
#define ESP32_XBEE_F9P_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

// F9P UART1 to the ESP32 at F9P_UART1_BAUD: GGA, VTG, RMC, GSA and GST every navigation epoch (10 Hz), GSV,
// NAV-SAT and NAV-SIG at 1 Hz. The RS-232 output picks its own rates from these. F9P UART2 is left as it is.
#define F9P_UART1_BAUD 460800

#define F9P_REJECTED_MAX 16

typedef struct f9p_result {
    int keys; // Keys the receiver accepted
    int total; // Keys in the config

    // Keys the receiver refused
    int rejected_count;
    uint32_t rejected[F9P_REJECTED_MAX];

    const char *error; // NULL if the whole config was written
} f9p_result_t;

// Panel settings as found in one layer of the receiver
typedef struct f9p_state {
    bool present; // False if the layer doesn't hold all of them, e.g. nothing saved to flash

    uint32_t uart1_baud;
    bool out_ubx; // UART1 protocols
    bool out_nmea;

    // Measurement rate, and UART1 output rates (0 = off)
    double nav_hz;
    double gga_hz;
    double vtg_hz;
    double rmc_hz;
    double gsa_hz;
    double gsv_hz;
    double gst_hz;
    double nav_sat_hz;
    double nav_sig_hz;
} f9p_state_t;

#define F9P_DIFFERENCES_MAX 16

typedef struct f9p_difference {
    uint32_t key;
    uint64_t expected;
    uint64_t actual;
    bool missing; // Not in this layer of the receiver
} f9p_difference_t;

// Comparison of one layer of the receiver with what a write would set
typedef struct f9p_check {
    int total;
    int differ;
    int missing;

    // The first differences
    int count;
    f9p_difference_t keys[F9P_DIFFERENCES_MAX];
} f9p_check_t;

void f9p_config_init();

// Writes the config to the receiver's RAM, BBR and flash over the UART, blocks until done.
// The UART1 baud rate goes last, then the ESP32 UART follows it.
esp_err_t f9p_config_write(f9p_result_t *result);

// Each of these first finds the receiver's baud rate if the ESP32 UART doesn't match it.

// Reads the panel settings from the receiver's RAM (active) and flash (saved)
esp_err_t f9p_config_read(f9p_state_t *ram, f9p_state_t *flash, const char **error);

// Compares every key in the receiver's RAM and flash with what a write would set
esp_err_t f9p_config_check(f9p_check_t *ram, f9p_check_t *flash, const char **error);

#endif //ESP32_XBEE_F9P_CONFIG_H
