#pragma once
#include <stdint.h>
constexpr unsigned ESP_MAC_WIFI_STA = 0;
int esp_read_mac(uint8_t *mac, unsigned type);
