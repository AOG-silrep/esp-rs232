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

// NMEA from the receiver out of a second UART for an RS-232 device (through a MAX3232).
// NMEA 0183 GGA tops out at 12 satellites, so its satellites field is replaced with the real number used.
// The receiver sends every sentence at the navigation rate (GSV at 1 Hz), each goes out at its own lower rate.

#include <string.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "rs232.h"
#include "config.h"
#include "gnss_sats.h"
#include "protocol/nmea.h"
#include "rtk_status.h"
#include "stream_stats.h"
#include "tasks.h"
#include "uart.h"

static const char *TAG = "RS232";

#define LINE_MAX_LENGTH 128
#define LINE_QUEUE_LENGTH 32
#define RS232_TX_BUFFER_SIZE 2048
#define RS232_RX_BUFFER_SIZE 256

// A satellite count older than this isn't used, GGA then goes out as the receiver sent it
#define SATS_MAX_AGE_US 2500000

// Sentences of one type closer together than this are one group (the GSAs of an epoch, a GSV set),
// all sent or all skipped
#define GROUP_GAP_US 40000

// A group goes out once its period has passed, less this: half a 10 Hz epoch, for the UART timing jitter
#define PERIOD_MARGIN_US 50000

typedef struct line {
    uint8_t length;
    uint8_t sentence; // rs232_sentence_t
    char data[LINE_MAX_LENGTH];
} line_t;

const char *const RS232_SENTENCE_NAMES[RS232_SENTENCE_COUNT] = {"GGA", "VTG", "RMC", "GSA", "GST", "GSV"};

static const char *const RATE_KEYS[RS232_SENTENCE_COUNT] = {
        KEY_CONFIG_RS232_GGA_HZ, KEY_CONFIG_RS232_VTG_HZ, KEY_CONFIG_RS232_RMC_HZ,
        KEY_CONFIG_RS232_GSA_HZ, KEY_CONFIG_RS232_GST_HZ, KEY_CONFIG_RS232_GSV_HZ
};

static uart_port_t port = -1;
static uint32_t baud = 0;
static uint8_t rate_hz[RS232_SENTENCE_COUNT];
static bool gga_sats = true;

// Only accessed from rs232_task: per sentence, the last one seen, the last group sent, and the decision
// for the current group
static struct {
    int64_t last_seen;
    int64_t last_sent;
    bool sending;
} groups[RS232_SENTENCE_COUNT];

static QueueHandle_t line_queue;
static stream_stats_handle_t stream_stats;

static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static int gga_sats_in = -1;
static int gga_sats_out = -1;
static const char *sats_source = "GGA";
static uint32_t sentences = 0;
static uint32_t dropped = 0;

// Only accessed from UART read event handler
static line_t line;

static void rs232_line_complete() {
    // $ttsss,...*hh
    if (line.length < 10 || line.data[1] == 'P') return;

    for (int s = 0; s < RS232_SENTENCE_COUNT; s++) {
        if (rate_hz[s] == 0 || strncmp(line.data + 3, RS232_SENTENCE_NAMES[s], 3) != 0 || line.data[6] != ',') {
            continue;
        }

        line.sentence = s;
        xQueueSend(line_queue, &line, 0);
        return;
    }
}

static void rs232_uart_handler(void *handler_args, esp_event_base_t base, int32_t length, void *buffer) {
    const char *data = buffer;
    for (int32_t i = 0; i < length; i++) {
        char c = data[i];

        if (c == '$') {
            line.length = 0;
        } else if (line.length == 0) {
            continue;
        } else if (c == '\r' || c == '\n') {
            rs232_line_complete();
            line.length = 0;
            continue;
        } else if (line.length >= LINE_MAX_LENGTH - 3) {
            line.length = 0;
            continue;
        }

        line.data[line.length++] = c;
    }
}

// Satellites used, from NAV-SAT if the receiver outputs it, otherwise from the epoch's GSAs
static int satellites_used(const char **source) {
    int satellites;
    int64_t age;

    if (gnss_sats_used(&satellites, &age) && age <= SATS_MAX_AGE_US) {
        *source = "NAV-SAT";
        return satellites;
    }

    if (rtk_status_gsa_satellites(&satellites, &age) && age <= SATS_MAX_AGE_US && satellites > 0) {
        *source = "GSA";
        return satellites;
    }

    *source = "GGA";
    return -1;
}

static void rs232_send(const char *data, size_t length) {
    size_t free_size = 0;
    if (uart_get_tx_buffer_free_size(port, &free_size) != ESP_OK || free_size < length) {
        taskENTER_CRITICAL(&status_lock);
        dropped++;
        taskEXIT_CRITICAL(&status_lock);
        return;
    }

    uart_write_bytes(port, data, length);
    stream_stats_increment(stream_stats, 0, length);

    taskENTER_CRITICAL(&status_lock);
    sentences++;
    taskEXIT_CRITICAL(&status_lock);
}

// Whether this sentence goes out at its rate. A sentence right after another of the same type is part of its
// group and goes the same way, a new group goes once the rate's period has passed since the last one sent.
static bool rate_due(rs232_sentence_t sentence, int64_t now) {
    typeof(groups[0]) *group = &groups[sentence];

    bool same_group = group->last_seen != 0 && now - group->last_seen < GROUP_GAP_US;
    group->last_seen = now;
    if (same_group) return group->sending;

    int64_t period = 1000000 / rate_hz[sentence];
    group->sending = group->last_sent == 0 || now - group->last_sent >= period - PERIOD_MARGIN_US;
    if (group->sending) group->last_sent = now;

    return group->sending;
}

static void rs232_task(void *ctx) {
    line_t received;
    char out[LINE_MAX_LENGTH + 8];

    while (true) {
        if (xQueueReceive(line_queue, &received, portMAX_DELAY) != pdTRUE) continue;
        if (!nmea_checksum_valid(received.data, received.length)) continue;
        if (!rate_due(received.sentence, esp_timer_get_time())) continue;

        received.data[received.length] = '\0';

        if (strncmp(received.data + 3, "GGA,", 4) == 0) {
            nmea_gga_t gga;
            int sats_in = nmea_parse_gga(received.data, &gga) ? gga.satellites : -1;

            const char *source = "GGA";
            int sats = gga_sats ? satellites_used(&source) : -1;

            int length = sats >= 0 ? nmea_gga_set_satellites(received.data, sats, out, sizeof(out)) : -1;
            if (length < 0) {
                source = "GGA";
                sats = sats_in;
                length = snprintf(out, sizeof(out), "%s\r\n", received.data);
            }

            taskENTER_CRITICAL(&status_lock);
            gga_sats_in = sats_in;
            gga_sats_out = sats;
            sats_source = source;
            taskEXIT_CRITICAL(&status_lock);

            rs232_send(out, length);
        } else {
            received.data[received.length++] = '\r';
            received.data[received.length++] = '\n';
            rs232_send(received.data, received.length);
        }
    }
}

void rs232_get_status(rs232_status_t *status) {
    taskENTER_CRITICAL(&status_lock);
    *status = (rs232_status_t) {
            .active = port >= 0,
            .baud = baud,
            .gga_sats_in = gga_sats_in,
            .gga_sats_out = gga_sats_out,
            .sats_source = sats_source,
            .sentences = sentences,
            .dropped = dropped
    };
    taskEXIT_CRITICAL(&status_lock);

    memcpy(status->rate_hz, rate_hz, sizeof(rate_hz));
}

// The configured rate, or the nearest allowed one
static uint8_t rate_allowed(rs232_sentence_t sentence, uint8_t hz) {
    if (sentence == RS232_SENTENCE_GSV) return hz > 0 ? 1 : 0;
    if (hz == 0) return sentence == RS232_SENTENCE_GGA ? 1 : 0;
    if (hz <= 2) return hz;
    if (hz <= 5) return 5;
    return 10;
}

void rs232_init() {
    baud = config_get_u32(CONF_ITEM(KEY_CONFIG_RS232_BAUD_RATE));
    gga_sats = config_get_bool1(CONF_ITEM(KEY_CONFIG_RS232_GGA_SATS));

    for (int s = 0; s < RS232_SENTENCE_COUNT; s++) {
        rate_hz[s] = rate_allowed(s, config_get_u8(CONF_ITEM(RATE_KEYS[s])));
    }

    // UART2, or UART1 if the receiver is moved to UART2
    uart_port_t rs232_port = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_NUM)) == UART_NUM_2 ? UART_NUM_1 : UART_NUM_2;

    uart_config_t uart_config = {
            .baud_rate = baud,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT
    };

    esp_err_t err = uart_param_config(rs232_port, &uart_config);
    if (err == ESP_OK) {
        err = uart_set_pin(rs232_port, config_get_u8(CONF_ITEM(KEY_CONFIG_RS232_TX_PIN)),
                config_get_u8(CONF_ITEM(KEY_CONFIG_RS232_RX_PIN)), UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_driver_install(rs232_port, RS232_RX_BUFFER_SIZE, RS232_TX_BUFFER_SIZE, 0, NULL, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set up UART%d: %s", rs232_port, esp_err_to_name(err));
        return;
    }

    line_queue = xQueueCreate(LINE_QUEUE_LENGTH, sizeof(line_t));
    stream_stats = stream_stats_new("rs232");

    xTaskCreate(rs232_task, "rs232_task", 4096, NULL, TASK_PRIORITY_RS232, NULL);

    port = rs232_port;
    uart_register_read_handler(rs232_uart_handler);

    ESP_LOGI(TAG, "NMEA out on UART%d TX GPIO%d at %lu baud, GGA %d VTG %d RMC %d GSA %d GST %d GSV %d Hz", port,
            config_get_u8(CONF_ITEM(KEY_CONFIG_RS232_TX_PIN)), (unsigned long) baud,
            rate_hz[RS232_SENTENCE_GGA], rate_hz[RS232_SENTENCE_VTG], rate_hz[RS232_SENTENCE_RMC],
            rate_hz[RS232_SENTENCE_GSA], rate_hz[RS232_SENTENCE_GST], rate_hz[RS232_SENTENCE_GSV]);
}
