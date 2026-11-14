#ifndef ESP32_XBEE_GNSS_SATS_H
#define ESP32_XBEE_GNSS_SATS_H

#include <stdbool.h>
#include <stdint.h>

// UBX gnssId
#define GNSS_ID_GPS 0
#define GNSS_ID_SBAS 1
#define GNSS_ID_GALILEO 2
#define GNSS_ID_BEIDOU 3
#define GNSS_ID_QZSS 5
#define GNSS_ID_GLONASS 6

#define GNSS_SATS_MAX 72
#define GNSS_SAT_SIGNALS_MAX 3

typedef struct gnss_signal {
    uint8_t sig_id; // UBX sigId within the GNSS, e.g. GPS 0 = L1C/A, 3 = L2 CL, 4 = L2 CM
    uint8_t cno; // dB-Hz
    uint8_t quality; // UBX qualityInd 0-7
    bool used; // Pseudorange used in the navigation solution
} gnss_signal_t;

typedef struct gnss_sat {
    uint8_t gnss_id;
    uint8_t sv_id;
    uint8_t cno; // dB-Hz, strongest signal
    int8_t elevation; // Degrees, -91 if unknown
    int16_t azimuth; // Degrees
    uint8_t quality; // UBX qualityInd 0-7
    uint8_t orbit_source; // 0 none, 1 ephemeris, 2 almanac, 3 AssistNow offline, 4 AssistNow autonomous
    bool used; // Used in the navigation solution
    bool healthy;

    int signal_count; // From NAV-SIG, 0 if it isn't output
    gnss_signal_t signals[GNSS_SAT_SIGNALS_MAX];
} gnss_sat_t;

typedef struct gnss_sats {
    int count;
    gnss_sat_t sats[GNSS_SATS_MAX];
    double age; // Seconds since the last NAV-SAT
    double signals_age; // Seconds since the last NAV-SIG, negative if never
} gnss_sats_t;

void gnss_sats_init();

// Copy of the last NAV-SAT (with NAV-SIG signals), false if none was received yet
bool gnss_sats_get(gnss_sats_t *sats);

// Satellites used in the navigation solution from the last NAV-SAT, false if none was received yet
bool gnss_sats_used(int *used, int64_t *age_us);

#endif //ESP32_XBEE_GNSS_SATS_H
