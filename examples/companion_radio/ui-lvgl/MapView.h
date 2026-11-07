#pragma once

#include <lvgl.h>
#include <helpers/SensorManager.h>

// Slippy-map viewer: renders OSM raster tiles from the microSD card
// (/sdcard/maps/{z}/{x}/{y}.png via LVGL's POSIX FS + lodepng decoder),
// with drag panning, zoom buttons, and an own-position marker from GPS.

class UITask;
void mapViewBuild(lv_obj_t* parent, SensorManager* sensors, UITask* task);
// Fills the panel behind the status bar and tabview: call Chrome once after the
// tabs exist, then Show as the Map tab opens and closes.
void mapViewChrome(lv_obj_t* status_bar, lv_obj_t* tabview, lv_obj_t* map_page, lv_obj_t* name_lbl);
void mapViewShow(bool on);
const char* mapStyleDir();    // card folder of the chosen style, for downloads
const char* mapStyleName();   // its name as shown on the download screen
void mapViewRefresh();        // reposition/reload tiles + marker
bool mapViewSdOk();
void mapViewReload();                            // after tiles change on the card
bool mapViewPosition(double* lat, double* lon);  // GPS, else last known position
void mapDlOpen(lv_event_t* e);                   // WiFi map sync screen
bool mapDlWifiStatus(char* out, size_t out_sz);  // "Off", "On", "42%"; true while WiFi is on
