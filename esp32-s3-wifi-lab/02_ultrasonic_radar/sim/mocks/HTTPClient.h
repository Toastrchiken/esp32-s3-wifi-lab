#pragma once
#include "WiFi.h"
struct HTTPClient { bool begin(WiFiClient&, String){return true;} void addHeader(const char*,const char*){} int POST(uint8_t*,size_t){return 200;} void end(){} };
