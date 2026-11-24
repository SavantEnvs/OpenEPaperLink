// Host stand-in for ESP32_AP-Flasher/include/web.h (which needs AsyncTCP): the one call v2transport.cpp makes.
#pragma once
#include <Arduino.h>
void wsErr(const String& text);
