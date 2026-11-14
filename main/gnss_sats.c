/*
 * This file is part of the ESP32 RS-232 distribution, based on ESP32-XBee (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2026 Reuben Rissler.
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

// Satellites and signals from the receiver's UBX NAV-SAT and NAV-SIG on the UART, for the signal view

#include <string.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "gnss_sats.h"
#include "uart.h"

#define UBX_SYNC_1 0xB5
#define UBX_SYNC_2 0x62
#define UBX_HEADER_LENGTH 6
#define UBX_CLASS_NAV 0x01
#define UBX_ID_NAV_SAT 0x35
#define UBX_ID_NAV_SIG 0x43

// NAV-SIG of 128 signals is the largest frame we keep
#define SIGNALS_MAX 128
#define PAYLOAD_MAX (8 + 16 * SIGNALS_MAX)

typedef struct signal_entry {
    uint8_t gnss_id;
    uint8_t sv_id;
    gnss_signal_t signal;
} signal_entry_t;

static SemaphoreHandle_t lock;

// Only accessed while holding lock
static int sat_count = 0;
static gnss_sat_t sats[GNSS_SATS_MAX];
static int64_t sats_time = 0;
static int sats_used = 0;
static int signal_count = 0;
static signal_entry_t signals[SIGNALS_MAX];
static int64_t signals_time = 0;

// Only accessed from UART read event handler
static struct {
    size_t position;
    uint8_t msg_class;
    uint8_t msg_id;
    uint16_t length;
    uint8_t ck_a;
    uint8_t ck_b;
    uint8_t received_ck_a;
    uint8_t payload[PAYLOAD_MAX];
} parser;

static uint16_t u2(const uint8_t *p) {
    return p[0] | p[1] << 8;
}

static uint32_t u4(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

static void nav_sat_decode(const uint8_t *payload, uint16_t length) {
    if (length < 8 || payload[4] != 1) return; // Version 1
    int count = payload[5];
    if (length < 8 + 12 * count) return;

    if (xSemaphoreTake(lock, 0) != pdTRUE) return;

    sat_count = 0;
    sats_used = 0;
    for (int i = 0; i < count && sat_count < GNSS_SATS_MAX; i++) {
        const uint8_t *block = payload + 8 + 12 * i;
        uint32_t flags = u4(block + 8);

        gnss_sat_t *sat = &sats[sat_count++];
        *sat = (gnss_sat_t) {
                .gnss_id = block[0],
                .sv_id = block[1],
                .cno = block[2],
                .elevation = (int8_t) block[3],
                .azimuth = (int16_t) u2(block + 4),
                .quality = flags & 0x07,
                .used = (flags >> 3) & 0x01,
                .healthy = ((flags >> 4) & 0x03) == 1,
                .orbit_source = (flags >> 8) & 0x07
        };

        if (sat->used) sats_used++;
    }
    sats_time = esp_timer_get_time();

    xSemaphoreGive(lock);
}

static void nav_sig_decode(const uint8_t *payload, uint16_t length) {
    if (length < 8 || payload[4] != 0) return; // Version 0
    int count = payload[5];
    if (length < 8 + 16 * count) return;

    if (xSemaphoreTake(lock, 0) != pdTRUE) return;

    signal_count = 0;
    for (int i = 0; i < count && signal_count < SIGNALS_MAX; i++) {
        const uint8_t *block = payload + 8 + 16 * i;
        uint16_t flags = u2(block + 10);

        signals[signal_count++] = (signal_entry_t) {
                .gnss_id = block[0],
                .sv_id = block[1],
                .signal = {
                        .sig_id = block[2],
                        .cno = block[6],
                        .quality = block[7],
                        .used = (flags >> 3) & 0x01
                }
        };
    }
    signals_time = esp_timer_get_time();

    xSemaphoreGive(lock);
}

static void frame_complete() {
    if (parser.msg_class != UBX_CLASS_NAV) return;

    if (parser.msg_id == UBX_ID_NAV_SAT) {
        nav_sat_decode(parser.payload, parser.length);
    } else if (parser.msg_id == UBX_ID_NAV_SIG) {
        nav_sig_decode(parser.payload, parser.length);
    }
}

static void gnss_sats_uart_handler(void *handler_args, esp_event_base_t base, int32_t length, void *buffer) {
    const uint8_t *data = buffer;
    for (int32_t i = 0; i < length; i++) {
        uint8_t c = data[i];
        size_t position = parser.position++;

        if (position == 0) {
            if (c != UBX_SYNC_1) parser.position = 0;
            continue;
        } else if (position == 1) {
            if (c != UBX_SYNC_2) parser.position = c == UBX_SYNC_1 ? 1 : 0;
            parser.ck_a = 0;
            parser.ck_b = 0;
            continue;
        }

        size_t payload_end = UBX_HEADER_LENGTH + parser.length;
        if (position < UBX_HEADER_LENGTH || position < payload_end) {
            parser.ck_a += c;
            parser.ck_b += parser.ck_a;
        }

        if (position == 2) {
            parser.msg_class = c;
        } else if (position == 3) {
            parser.msg_id = c;
        } else if (position == 4) {
            parser.length = c;
        } else if (position == 5) {
            parser.length |= c << 8;

            // Only NAV-SAT and NAV-SIG are kept, skip anything else or too long and look for the next frame
            bool wanted = parser.msg_class == UBX_CLASS_NAV &&
                    (parser.msg_id == UBX_ID_NAV_SAT || parser.msg_id == UBX_ID_NAV_SIG);
            if (!wanted || parser.length > PAYLOAD_MAX) parser.position = 0;
        } else if (position < payload_end) {
            parser.payload[position - UBX_HEADER_LENGTH] = c;
        } else if (position == payload_end) {
            parser.received_ck_a = c;
        } else {
            parser.position = 0;
            if (parser.received_ck_a == parser.ck_a && c == parser.ck_b) frame_complete();
        }
    }
}

bool gnss_sats_get(gnss_sats_t *out) {
    if (lock == NULL) return false;

    xSemaphoreTake(lock, portMAX_DELAY);

    if (sats_time == 0) {
        xSemaphoreGive(lock);
        return false;
    }

    int64_t now = esp_timer_get_time();
    out->count = sat_count;
    memcpy(out->sats, sats, sat_count * sizeof(gnss_sat_t));
    out->age = (now - sats_time) / 1e6;
    out->signals_age = signals_time == 0 ? -1 : (now - signals_time) / 1e6;

    for (int s = 0; s < out->count; s++) {
        gnss_sat_t *sat = &out->sats[s];
        sat->signal_count = 0;

        for (int i = 0; i < signal_count && sat->signal_count < GNSS_SAT_SIGNALS_MAX; i++) {
            if (signals[i].gnss_id != sat->gnss_id || signals[i].sv_id != sat->sv_id) continue;
            sat->signals[sat->signal_count++] = signals[i].signal;
        }
    }

    xSemaphoreGive(lock);

    return true;
}

bool gnss_sats_used(int *used, int64_t *age_us) {
    if (lock == NULL) return false;

    xSemaphoreTake(lock, portMAX_DELAY);
    int64_t time = sats_time;
    int count = sats_used;
    xSemaphoreGive(lock);

    if (time == 0) return false;

    *used = count;
    *age_us = esp_timer_get_time() - time;
    return true;
}

void gnss_sats_init() {
    lock = xSemaphoreCreateMutex();

    uart_register_read_handler(gnss_sats_uart_handler);
}
