// Host stand-in for the Arduino/ESP32 core, only what ESP32_AP-Flasher/src/v2transport.cpp and the
// headers it includes use. The clock, the serial ports and the FreeRTOS calls are provided by
// mayhem/v2/v2_host.cpp, which feeds the radio's serial bytes from the fuzz input.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#ifndef __packed
#define __packed __attribute__((packed))
#endif

uint32_t millis();

class String {
   public:
    String(const char* s = "") : s_(s ? s : "") {}
    const char* c_str() const { return s_.c_str(); }

   private:
    std::string s_;
};

class IPAddress {
   public:
    IPAddress(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) : v_{a, b, c, d} {}

   private:
    uint8_t v_[4];
};

// Serial: the console log; Serial1: the UART to the radio (AP_SERIAL_PORT in radiotransport.h)
class HostSerial {
   public:
    explicit HostSerial(int port) : port_(port) {}
    size_t write(const uint8_t* buf, size_t n);
    size_t print(const char* s);
    size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    int available();

   private:
    int port_;
};
extern HostSerial Serial;
extern HostSerial Serial1;

// FreeRTOS, single-threaded: the reader task runs inside vTaskDelay (see v2_host.cpp)
typedef void* SemaphoreHandle_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY 0xFFFFFFFFu
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vTaskDelay(uint32_t ticks);
