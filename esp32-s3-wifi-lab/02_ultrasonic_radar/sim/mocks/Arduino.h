#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <functional>
#include <algorithm>
using std::min; using std::max;
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#define PROGMEM
typedef const char* PGM_P;
#define OUTPUT 1
#define INPUT 0
#define LOW 0
#define HIGH 1
#define RGB_BUILTIN 97
inline void pinMode(int,int){} inline void digitalWrite(int,int){} inline void delayMicroseconds(int){}
unsigned long pulseIn(int,int,unsigned long);
extern uint32_t g_ms; inline uint32_t millis(){return g_ms;} inline void delay(int d){g_ms+=d;}
inline void neopixelWrite(uint8_t,uint8_t,uint8_t,uint8_t){}
inline bool ledcSetup(int,int,int){return true;} inline void ledcAttachPin(int,int){} inline bool ledcAttach(int,int,int){return true;} extern uint32_t g_duty; inline void ledcWrite(int,uint32_t d){g_duty=d;}
class String { public: std::string s; String(){} String(const char*c):s(c){} String(int v):s(std::to_string(v)){}
 unsigned length() const {return s.size();} const char* c_str() const {return s.c_str();} void reserve(size_t n){s.reserve(n);}
 long toInt() const {return atol(s.c_str());} float toFloat() const {return atof(s.c_str());}
 bool operator==(const char*o) const {return s==o;} String& operator+=(const char*c){s+=c;return *this;} String& operator+=(const String&o){s+=o.s;return *this;}
 friend String operator+(const String&a,const char*b){String r=a; r.s+=b; return r;} friend String operator+(const char*a,const String&b){String r(a); r.s+=b.s; return r;} };
struct SerialC { void begin(long){} template<class...A> int printf(const char* f, A... a){return ::printf(f,a...);} void println(const char*s=""){(void)s;} void print(char){} } Serial;
typedef void* TaskHandle_t; typedef int BaseType_t;
#define pdPASS 1
inline BaseType_t xTaskCreate(void(*)(void*),const char*,uint32_t,void*,int,TaskHandle_t*){return pdPASS;} inline void vTaskDelete(void*){}
