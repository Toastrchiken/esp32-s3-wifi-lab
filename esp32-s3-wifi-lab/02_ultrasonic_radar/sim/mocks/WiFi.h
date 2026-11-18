#pragma once
#include "Arduino.h"
struct IPAddress { String toString() const {return "1.2.3.4";} };
#define WIFI_STA 1
#define WIFI_AP 2
#define WL_CONNECTED 3
struct WiFiC { void setHostname(const char*){} void mode(int){} void begin(const char*,const char*){} int status(){return 0;} bool isConnected(){return false;}
 void setAutoReconnect(bool){} IPAddress localIP(){return {};} IPAddress softAPIP(){return {};} int RSSI(){return -50;} int channel(){return 1;}
 bool disconnect(bool){return true;} bool softAP(const char*,const char*){return true;} } WiFi;
class WiFiClient {};
inline void configTime(long,int,const char*,const char*){}
