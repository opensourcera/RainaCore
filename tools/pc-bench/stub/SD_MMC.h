#pragma once
#include "SPIFFS.h"
struct SdFS {
  void setPins(int, int, int) {} bool begin(const char*, bool) { return true; }
  File open(const char*) { return File(); } bool exists(const char*) { return true; }
};
extern SdFS SD_MMC;
