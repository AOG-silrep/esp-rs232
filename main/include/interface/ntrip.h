#ifndef ESP32_XBEE_NTRIP_H
#define ESP32_XBEE_NTRIP_H

#include <stdbool.h>

#define NTRIP_CLIENT_NAME "ESP32-RS232_Client"

#define NEWLINE "\r\n"
#define NEWLINE_LENGTH 2

void ntrip_client_init();

bool ntrip_response_ok(void *response);
bool ntrip_response_sourcetable_ok(void *response);

#endif //ESP32_XBEE_NTRIP_H
