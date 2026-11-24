// Host stand-in for ESP32_AP-Flasher/include/tag_db.h (which needs ArduinoJson and the web server):
// only the members of Config, tagRecord and the tag database lock that v2transport.cpp reads.
#pragma once
#include <Arduino.h>

#include <vector>


struct Config {
    uint8_t led;
    uint8_t lock;
};
class tagRecord {
   public:
    uint8_t mac[8];
    uint8_t version;
};
class TagDBLock {
   public:
    TagDBLock() {}
};
extern Config config;
extern std::vector<tagRecord*> tagDB;
