#pragma once
#include "Arduino.h"
enum HTTPMethod { HTTP_GET };
struct WebServer { WebServer(int){} void on(const char*,HTTPMethod,std::function<void()>){} void onNotFound(std::function<void()>){} void begin(){} void handleClient(){}
 bool hasArg(const char*){return false;} String arg(const char*){return "";} void send(int,const char*,const String&){} void send(int,const char*,const char*){} void send_P(int,PGM_P,PGM_P){} };
