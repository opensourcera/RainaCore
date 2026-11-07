#pragma once

// Internals shared by the ui-lvgl translation units. Not part of the UITask
// interface; only the ui-lvgl sources include this.

#include "UITask.h"
#include "../MyMesh.h"
#include "target.h"
#include <helpers/TxtDataHelpers.h>
#include <SPIFFS.h>
#include "ChatStore.h"
#include "Sound.h"
#include "SettingsUI.h"

// theme
#define COL_BG       0x0E1216   // deep navy
#define COL_CARD     0x1B2128   // panel/card
#define COL_ACCENT   0x2EC4A6   // RainaCore teal
#define COL_ACCENT_D 0x176B5C   // outgoing bubble
#define COL_TXT      0xF1F4F6
#define COL_MUTED    0x98A4AE
#define COL_WARN     0xFFB454
#define COL_REPEATER 0xB794FF   // map: repeaters

#define KB_HEIGHT 160

#ifndef RAINACORE_VERSION
  #define RAINACORE_VERSION "dev"
#endif

// UI font: the icon glyphs the screens use, falling back to montserrat for text.
LV_FONT_DECLARE(fa_icons_14);
LV_FONT_DECLARE(rc_font_12);
LV_FONT_DECLARE(rc_font_14);
LV_FONT_DECLARE(rc_font_16);
LV_FONT_DECLARE(rc_font_28);
LV_IMAGE_DECLARE(rc_logo);

#define SYMBOL_STAR      "\xE2\x98\x85"   /* U+2605, from the Inter fonts */
#define SYMBOL_LOCK      "\xF0\x9F\x94\x92"   /* U+1F512, from the emoji font */
#define SYMBOL_GLOBE     "\xF0\x9F\x8C\x90"   /* U+1F310, from the emoji font */
#define SYMBOL_TOWER     "\xEF\x94\x99"   /* U+F519 broadcast tower */
#define SYMBOL_ADDR_BOOK "\xEF\x8A\xB9"   /* U+F2B9 address book */

extern UITask* ui;   // singleton for LVGL callbacks
extern bool units_miles;

// UITask.cpp
int  prefReadInt(const char* path, int dflt);
void prefWriteInt(const char* path, int v);

// UIOverlays.cpp
void buildSplash();
void buildPolishOverlays();
void showNotifBanner(const uint8_t key[6], const char* from_name, const char* text);
void wizardOpen();
void aboutOpen();

// UIThread.cpp
bool threadOpen();
void threadCompose(const char* prefill);
lv_obj_t* threadScreen();

// UIReply.cpp
void qrLoad();
void qrEditorBuild();
void qrEditorOpen();                  // manage quick replies from Settings
void replyMenuOpen(const ChatMsg* m, const char* thread_name, bool channel);
void replyMenuClose();
void myInfoOpen(const ChatMsg* m, const char* thread_name, bool channel);
void replyPresetsForThread(const ChatMsg* newest, const char* thread_name, bool channel);
const char* msgSplitSender(const char* text, bool channel, const char* thread_name, char* who, size_t who_sz);
void msgTidy(char* out, size_t out_sz, const char* in);

// UIRepeaterList.cpp: the two-pane Contacts > Rptr view
void rptBuild(lv_obj_t* parent);
void rptShow(bool on);
void rptRefresh(bool force);
void rptTick(bool tab_open);
int  rptPlacesNeeded(double out[][2], int max);   // repeaters with no saved place name

// UIContacts.cpp
void contactPathOpen(int contact_idx);

// UIGeo.cpp
struct GeoGuess {
  int tz_hours;       // UTC offset right now, daylight saving included
  char tz_name[28];
  int preset;         // radio preset index, -1 when unknown
};
void geoGuess(double lat, double lon, uint32_t utc, GeoGuess* g);
bool geoFromGps(GeoGuess* g);   // false until there is a fix
void geoLoad();
void geoTick();

// UIChannels.cpp
#define CHAT_CHIPS 5
void chatChipsBuild(lv_obj_t* parent, lv_event_cb_t cb, lv_obj_t** chips);
bool channelIsPublic(const ChannelDetails& ch);
int  channelsUsed();
void channelShareOpen(int idx);
void addchBuild();
void addchOpen();

// UIBot.cpp
void botBuild(lv_obj_t* parent);
void botTick();
void botRefresh();

// UIRepeater.cpp
void repeaterStateLoad();
void repeaterScrShow();
void repeaterScrHide();
