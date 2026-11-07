#pragma once
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "MyMesh.h"
#define COL_BG 0x0E1216
#define COL_CARD 0x1B2128
#define COL_ACCENT 0x2EC4A6
#define COL_ACCENT_D 0x176B5C
#define COL_TXT 0xF1F4F6
#define COL_MUTED 0x98A4AE
#define COL_WARN 0xFFB454
#define COL_REPEATER 0xB794FF
#define SYMBOL_TOWER LV_SYMBOL_WIFI
LV_FONT_DECLARE(fa_icons_14);
LV_FONT_DECLARE(rc_font_12);
LV_FONT_DECLARE(rc_font_14);
LV_IMAGE_DECLARE(rc_logo);
extern bool units_miles;
class UITask { public:
  NodePrefs np; NodePrefs* nodePrefs() { return &np; }
  void openContactDetail(int i);
  void showToast(const char* t);
  void openRepeaterManager(const ContactInfo& c);
};
extern UITask* ui;
void rptBuild(lv_obj_t* parent); void rptShow(bool on); void rptRefresh(bool force); void rptTick(bool t); int rptPlacesNeeded(double out[][2], int max);
#include <string.h>
#ifndef BENCH_STRLCPY
#define BENCH_STRLCPY
static inline size_t strlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
#endif
