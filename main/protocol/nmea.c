/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#include "protocol/nmea.h"

#define NMEA_SENTENCE_MAX_LENGTH 128
#define NMEA_MAX_FIELDS 20

uint8_t nmea_calculate_checksum(char *sentence) {
    uint8_t checksum = 0;
    unsigned int length = strlen(sentence);
    for (unsigned int i = 1; i < length; i++) {
        checksum ^= (uint8_t) sentence[i];
    }

    return checksum;
}

int nmea_asprintf(char **strp, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    int l = nmea_vasprintf(strp, fmt, args);

    va_end(args);

    return l;
}

int nmea_vasprintf(char **strp, const char *fmt, va_list args) {
    char *sentence;
    vasprintf(&sentence, fmt, args);
    uint8_t checksum = nmea_calculate_checksum(sentence);
    int l = asprintf(strp, "%s*%02X\r\n", sentence, checksum);
    free(sentence);

    return l;
}

// Copies sentence of the given type, verifies checksum and splits it into comma separated fields
static int nmea_split(const char *sentence, const char *type, char *copy, size_t copy_size, char **fields, int max_fields) {
    size_t length = strlen(sentence);
    if (length < 7 || length >= copy_size || sentence[0] != '$') return -1;
    if (strncmp(sentence + 3, type, 3) != 0 || sentence[6] != ',') return -1;

    memcpy(copy, sentence, length + 1);

    char *asterisk = strrchr(copy, '*');
    if (asterisk == NULL || !isxdigit((unsigned char) asterisk[1]) || !isxdigit((unsigned char) asterisk[2])) return -1;

    char checksum_hex[3] = {asterisk[1], asterisk[2], '\0'};
    uint8_t checksum = strtoul(checksum_hex, NULL, 16);

    *asterisk = '\0';
    if (nmea_calculate_checksum(copy) != checksum) return -1;

    int count = 0;
    char *rest = copy;
    char *field;
    while (count < max_fields && (field = strsep(&rest, ",")) != NULL) {
        fields[count++] = field;
    }

    return count;
}

static double nmea_field_double(const char *field) {
    if (*field == '\0') return NAN;

    char *end;
    double value = strtod(field, &end);

    return *end == '\0' ? value : NAN;
}

// ddmm.mmmm / dddmm.mmmm to signed degrees
static double nmea_field_coordinate(const char *field, const char *hemisphere) {
    double value = nmea_field_double(field);
    if (isnan(value)) return NAN;

    double degrees = floor(value / 100);
    degrees += (value - degrees * 100) / 60;

    return (hemisphere[0] == 'S' || hemisphere[0] == 'W') ? -degrees : degrees;
}

bool nmea_parse_gga(const char *sentence, nmea_gga_t *gga) {
    char copy[NMEA_SENTENCE_MAX_LENGTH];
    char *fields[NMEA_MAX_FIELDS];

    // $--GGA,time,lat,N,lon,E,quality,sats,hdop,alt,M,sep,M,age,station
    int count = nmea_split(sentence, "GGA", copy, sizeof(copy), fields, NMEA_MAX_FIELDS);
    if (count < 12) return false;

    *gga = (nmea_gga_t) {
            .latitude = nmea_field_coordinate(fields[2], fields[3]),
            .longitude = nmea_field_coordinate(fields[4], fields[5]),
            .fix_quality = atoi(fields[6]),
            .satellites = atoi(fields[7]),
            .hdop = nmea_field_double(fields[8]),
            .altitude = nmea_field_double(fields[9]),
            .geoid_separation = nmea_field_double(fields[11]),
            .correction_age = count > 13 ? nmea_field_double(fields[13]) : NAN,
            .station_id = (count > 14 && fields[14][0] != '\0') ? atoi(fields[14]) : -1
    };

    return true;
}

bool nmea_parse_gst(const char *sentence, nmea_gst_t *gst) {
    char copy[NMEA_SENTENCE_MAX_LENGTH];
    char *fields[NMEA_MAX_FIELDS];

    // $--GST,time,rms,major,minor,orientation,lat,lon,alt
    int count = nmea_split(sentence, "GST", copy, sizeof(copy), fields, NMEA_MAX_FIELDS);
    if (count < 9) return false;

    *gst = (nmea_gst_t) {
            .sigma_latitude = nmea_field_double(fields[6]),
            .sigma_longitude = nmea_field_double(fields[7]),
            .sigma_altitude = nmea_field_double(fields[8])
    };

    return true;
}

bool nmea_parse_gsa(const char *sentence, nmea_gsa_t *gsa) {
    char copy[NMEA_SENTENCE_MAX_LENGTH];
    char *fields[NMEA_MAX_FIELDS];

    // $--GSA,opMode,navMode,svid x12,pdop,hdop,vdop[,systemId], one per GNSS in multi-GNSS operation
    int count = nmea_split(sentence, "GSA", copy, sizeof(copy), fields, NMEA_MAX_FIELDS);
    if (count < 18) return false;

    int satellites = 0;
    for (int i = 3; i <= 14; i++) {
        if (fields[i][0] != '\0') satellites++;
    }

    *gsa = (nmea_gsa_t) {
            .pdop = nmea_field_double(fields[15]),
            .hdop = nmea_field_double(fields[16]),
            .vdop = nmea_field_double(fields[17]),
            .satellites = satellites,
            .system_id = count > 18 ? atoi(fields[18]) : 0
    };

    return true;
}

bool nmea_checksum_valid(const char *sentence, size_t length) {
    if (length < 4 || sentence[0] != '$') return false;

    const char *asterisk = memchr(sentence, '*', length);
    if (asterisk == NULL || (size_t) (asterisk - sentence) + 3 > length) return false;
    if (!isxdigit((unsigned char) asterisk[1]) || !isxdigit((unsigned char) asterisk[2])) return false;

    uint8_t checksum = 0;
    for (const char *c = sentence + 1; c < asterisk; c++) checksum ^= (uint8_t) *c;

    char checksum_hex[3] = {asterisk[1], asterisk[2], '\0'};
    return strtoul(checksum_hex, NULL, 16) == checksum;
}

int nmea_gga_set_satellites(const char *sentence, int satellites, char *out, size_t out_size) {
    const char *asterisk = strchr(sentence, '*');
    if (asterisk == NULL || sentence[0] != '$') return -1;

    // Satellites are field 7, between the 7th and 8th comma
    const char *start = sentence;
    for (int i = 0; i < 7; i++) {
        start = memchr(start, ',', asterisk - start);
        if (start == NULL) return -1;
        start++;
    }
    const char *end = memchr(start, ',', asterisk - start);
    if (end == NULL) return -1;

    if (satellites < 0) satellites = 0;
    if (satellites > 99) satellites = 99;

    int length = snprintf(out, out_size, "%.*s%02d%.*s", (int) (start - sentence), sentence, satellites,
            (int) (asterisk - end), end);
    if (length < 0 || (size_t) length + 6 > out_size) return -1;

    uint8_t checksum = 0;
    for (int i = 1; i < length; i++) checksum ^= (uint8_t) out[i];

    return length + snprintf(out + length, out_size - length, "*%02X\r\n", checksum);
}
