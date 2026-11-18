#pragma once
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
typedef struct { int8_t rssi; } wifi_sta_info_t;
typedef struct { wifi_sta_info_t sta[10]; int num; } wifi_sta_list_t;
inline esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t* l){ l->num=0; return ESP_OK; }
