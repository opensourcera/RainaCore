// Download OpenStreetMap tiles over WiFi to the SD card.
//
// Kept deliberately small and slow to respect the OSM tile usage policy: one
// connection, a pause between tiles, a hard tile cap, and existing tiles are
// skipped. The WiFi password stays in RAM for this download only. WiFi is
// switched off again afterwards, and the work runs in its own task so the mesh
// radio keeps running.

#include "UICommon.h"
#include "MapView.h"
#include "UIPlaces.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <math.h>
#include <esp_heap_caps.h>

#define TILE_HOST  "https://tile.openstreetmap.org"
#define TOPO_HOST  "https://tile.opentopomap.org"
#define TILE_AGENT "RainaCore/" RAINACORE_VERSION " (open source LoRa mesh device, small offline area)"
#define PLACE_URL  "https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=10&addressdetails=1&accept-language=en"
#define TILE_PAUSE_MS 500
#define TOPO_PAUSE_MS 1000     // a volunteer server that asks not to be hammered
#define DETAIL_ZMIN 9          // an oversized area loses top zooms, down to here
#define MAX_TILES 1200
#define MAX_NETS 12

// Every download starts wide: place and state names are part of the map
// images, and the low zooms are the ones that show states and countries.
#define OVERVIEW_ZMIN 5
#define OVERVIEW_ZMAX 8

struct DlArea { const char* label; float km; int zmax; };
static const DlArea dl_areas[] = {
  {"5 km around", 5.0f, 16},
  {"10 km around", 10.0f, 15},
  {"25 km around", 25.0f, 14},
  {"200 km, state view", 200.0f, 11},
};

enum DlState { DL_IDLE, DL_CONNECTING, DL_RUNNING, DL_DONE, DL_FAILED };
static volatile DlState dl_state = DL_IDLE;
static volatile int dl_done = 0;
static volatile int dl_total = 0;
static volatile bool dl_cancel = false;
static char dl_msg[56] = "";

static char dl_ssid[33] = "";
static char dl_pw[64] = "";
static char dl_loc[32] = "";
static double dl_lat, dl_lon;
static int dl_area = 0;
static int dl_zmax = 0;
static char dl_dir[16] = "maps";
static bool dl_bt_was_on = false;
static char dl_place[PLACE_NAME_MAX] = "";
static double dl_place_lat, dl_place_lon;
#define DL_LOOKUPS 12
#define PLACE_PAUSE_MS 1100   // the name service allows one request a second
static double dl_lookups[DL_LOOKUPS][2];
static int dl_lookup_n = 0;
static volatile bool dl_place_ready = false;

static lv_obj_t* scr = NULL;
static lv_obj_t* title_lbl;
static lv_obj_t* net_dd;
static lv_obj_t* area_dd;
static lv_obj_t* pw_lbl;
static lv_obj_t* loc_lbl;
static lv_obj_t* status_lbl;
static lv_obj_t* start_lbl;
static lv_obj_t* edit_scr;
static lv_obj_t* edit_ta;
static lv_obj_t* edit_kb;
static int edit_target = 0;     // 0 password, 1 location
static lv_timer_t* poll_timer = NULL;
static bool scanning = false;

static int lonToX(double lon, int z) {
  return (int) floor((lon + 180.0) / 360.0 * (1 << z));
}
static int latToY(double lat, int z) {
  double r = lat * M_PI / 180.0;
  return (int) floor((1.0 - asinh(tan(r)) / M_PI) / 2.0 * (1 << z));
}

struct TileRange { int x0, x1, y0, y1; };

static TileRange tileRange(const DlArea& a, int z) {
  double dlat = a.km / 111.0;
  double dlon = a.km / (111.0 * cos(dl_lat * M_PI / 180.0));
  TileRange r;
  r.x0 = lonToX(dl_lon - dlon, z);
  r.x1 = lonToX(dl_lon + dlon, z);
  r.y0 = latToY(dl_lat + dlat, z);   // north is the smaller row number
  r.y1 = latToY(dl_lat - dlat, z);
  if (z <= OVERVIEW_ZMAX) {   // one tile extra each way, to reach what is around
    r.x0--; r.x1++; r.y0--; r.y1++;
  }
  int last = (1 << z) - 1;
  if (r.x0 < 0) r.x0 = 0;
  if (r.y0 < 0) r.y0 = 0;
  if (r.x1 > last) r.x1 = last;
  if (r.y1 > last) r.y1 = last;
  return r;
}

static int countTiles(const DlArea& a, int zmax) {
  int n = 0;
  for (int z = OVERVIEW_ZMIN; z <= zmax; z++) {
    TileRange r = tileRange(a, z);
    n += (r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
  }
  return n;
}

static void ensureDir(const char* path) {
  if (!SD_MMC.exists(path)) SD_MMC.mkdir(path);
}

static void dlFinish(DlState state, const char* msg) {
  snprintf(dl_msg, sizeof(dl_msg), "%s", msg);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  memset(dl_pw, 0, sizeof(dl_pw));
  dl_state = state;
  vTaskDelete(NULL);
}

// a complete PNG starts with its signature and is never tiny
static bool looksLikePng(const char* path) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  uint8_t head[8] = {0};
  size_t size = f.size();
  f.read(head, sizeof(head));
  f.close();
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  return size > 64 && memcmp(head, sig, sizeof(sig)) == 0;
}

// One request per sync, rounded to about a kilometre. A failed lookup only
// leaves the top bar showing coordinates, so it never stops the map.
static void fetchPlace(double lat, double lon) {
  WiFiClientSecure client;
  client.setInsecure();      // a place name, nothing that is acted on
  HTTPClient http;
  http.setUserAgent(TILE_AGENT);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  char url[160];
  snprintf(url, sizeof(url), PLACE_URL "&lat=%.2f&lon=%.2f", lat, lon);
  dl_place_lat = lat;
  dl_place_lon = lon;
  int code = 0;
  if (http.begin(client, url)) {
    code = http.GET();
    if (code == HTTP_CODE_OK && placeParseReply(http.getString().c_str(), dl_place, sizeof(dl_place))) {
      dl_place_ready = true;
    }
    http.end();
  }
  Serial.printf("mapdl: place lookup code %d, %s\n", code, dl_place_ready ? dl_place : "no name");
}

static void dlTask(void* arg) {
  const DlArea& a = dl_areas[dl_area];

  WiFi.persistent(false);    // never store the password in flash
  WiFi.mode(WIFI_STA);
  WiFi.begin(dl_ssid, dl_pw);
  unsigned long deadline = millis() + 25000;
  while (WiFi.status() != WL_CONNECTED) {
    if (dl_cancel) dlFinish(DL_FAILED, "Cancelled");
    if ((long)(millis() - deadline) > 0) dlFinish(DL_FAILED, "WiFi did not connect. Check the password.");
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  Serial.printf("mapdl: wifi up, internal free %u, largest %u\n",
                (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  dl_state = DL_RUNNING;
  fetchPlace(dl_lat, dl_lon);
  for (int i = 0; i < dl_lookup_n && !dl_cancel; i++) {   // names for the repeater list
    vTaskDelay(pdMS_TO_TICKS(PLACE_PAUSE_MS));
    while (dl_place_ready && !dl_cancel) vTaskDelay(pdMS_TO_TICKS(100));
    fetchPlace(dl_lookups[i][0], dl_lookups[i][1]);
  }
  bool terrain = strcmp(dl_dir, "maps") != 0;
  const char* host = terrain ? TOPO_HOST : TILE_HOST;
  char path[40];
  snprintf(path, sizeof(path), "/%s", dl_dir);
  ensureDir(path);

  WiFiClientSecure client;
  client.setInsecure();      // public map images only, each one is checked as a PNG below
  HTTPClient http;
  http.setReuse(true);
  http.setUserAgent(TILE_AGENT);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);

  int failures = 0;
  for (int z = OVERVIEW_ZMIN; z <= dl_zmax; z++) {
    TileRange r = tileRange(a, z);
    snprintf(path, sizeof(path), "/%s/%d", dl_dir, z);
    ensureDir(path);
    for (int x = r.x0; x <= r.x1; x++) {
      snprintf(path, sizeof(path), "/%s/%d/%d", dl_dir, z, x);
      ensureDir(path);
      for (int y = r.y0; y <= r.y1; y++) {
        if (dl_cancel) dlFinish(DL_FAILED, "Cancelled. Tiles so far are kept.");

        snprintf(path, sizeof(path), "/%s/%d/%d/%d.png", dl_dir, z, x, y);
        if (SD_MMC.exists(path)) { dl_done = dl_done + 1; continue; }

        char url[80];
        snprintf(url, sizeof(url), "%s/%d/%d/%d.png", host, z, x, y);
        char tmp[44];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);

        bool ok = false;
        int code = 0;
        if (http.begin(client, url)) {
          code = http.GET();
          if (code == HTTP_CODE_OK) {
            File f = SD_MMC.open(tmp, FILE_WRITE);
            if (f) {
              int written = http.writeToStream(&f);
              f.close();
              ok = written > 0 && looksLikePng(tmp);
            }
          }
          http.end();
        }

        if (code == 403 || code == 429) {
          dlFinish(DL_FAILED, "The map server refused. Try again later.");
        }
        if (ok) {
          SD_MMC.rename(tmp, path);
          failures = 0;
        } else {
          SD_MMC.remove(tmp);
          Serial.printf("mapdl: tile failed, code %d, internal free %u\n", code,
                        (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
          if (++failures >= 5) {
            dlFinish(DL_FAILED, "Download failed. Tiles so far are kept.");
          }
        }
        dl_done = dl_done + 1;
        vTaskDelay(pdMS_TO_TICKS(terrain ? TOPO_PAUSE_MS : TILE_PAUSE_MS));
      }
    }
  }
  dlFinish(DL_DONE, "Map saved to the SD card");
}

static void setBusy(bool busy) {
  lv_label_set_text(start_lbl, busy ? LV_SYMBOL_CLOSE " Cancel" : LV_SYMBOL_DOWNLOAD " Sync");
}

static void scanStart() {
  if (scanning || dl_state == DL_CONNECTING || dl_state == DL_RUNNING) return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.scanNetworks(true);
  scanning = true;
  lv_dropdown_set_options(net_dd, "Searching...");
}

static void scanPoll() {
  if (!scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanning = false;
  if (n <= 0) {
    lv_dropdown_set_options(net_dd, "No networks found");
    if (n < 0) WiFi.scanDelete();
    return;
  }
  char opts[MAX_NETS * 34] = "";
  int added = 0;
  for (int i = 0; i < n && added < MAX_NETS; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0 || strstr(opts, s.c_str()) != NULL) continue;
    if (added > 0) strlcat(opts, "\n", sizeof(opts));
    strlcat(opts, s.c_str(), sizeof(opts));
    added++;
  }
  WiFi.scanDelete();
  lv_dropdown_set_options(net_dd, opts);
}

static void refreshFields() {
  lv_label_set_text(pw_lbl, dl_pw[0] ? "Password  ********" : "Password  tap to enter");
  lv_label_set_text_fmt(loc_lbl, "Center  %s", dl_loc[0] ? dl_loc : "tap to enter lat, lon");
}

static void pollCb(lv_timer_t* t) {
  scanPoll();
  if (dl_place_ready) {
    placeAdd(dl_place_lat, dl_place_lon, dl_place);
    dl_place_ready = false;
  }
  if (dl_state == DL_CONNECTING) {
    lv_label_set_text(status_lbl, "Connecting to WiFi...");
  } else if (dl_state == DL_RUNNING) {
    lv_label_set_text_fmt(status_lbl, "Downloading %d of %d tiles", (int) dl_done, (int) dl_total);
  } else if (dl_state == DL_DONE || dl_state == DL_FAILED) {
    lv_label_set_text(status_lbl, dl_msg);
    dl_state = DL_IDLE;
    setBusy(false);
    refreshFields();
    mapViewReload();
    if (dl_bt_was_on) ui->enableBluetooth();
    if (lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN)) {   // finished in the background
      ui->showToast(dl_msg);
      lv_timer_delete(poll_timer);
      poll_timer = NULL;
    }
  }
}

static void startCb(lv_event_t* e) {
  if (dl_state == DL_CONNECTING || dl_state == DL_RUNNING) {
    dl_cancel = true;
    lv_label_set_text(status_lbl, "Stopping...");
    return;
  }
  if (!mapViewSdOk()) { lv_label_set_text(status_lbl, "No SD card found"); return; }
  if (scanning) { lv_label_set_text(status_lbl, "Still searching for WiFi"); return; }
  lv_dropdown_get_selected_str(net_dd, dl_ssid, sizeof(dl_ssid));
  if (strcmp(dl_ssid, "Searching...") == 0 || strcmp(dl_ssid, "No networks found") == 0) {
    lv_label_set_text(status_lbl, "No WiFi network selected");
    return;
  }
  if (sscanf(dl_loc, "%lf , %lf", &dl_lat, &dl_lon) != 2 || fabs(dl_lat) > 84 || fabs(dl_lon) > 180) {
    lv_label_set_text(status_lbl, "Enter the center as lat, lon");
    return;
  }
  dl_area = (int) lv_dropdown_get_selected(area_dd);
  // far from the equator tiles cover less ground, so an area can need many
  // more: give up the closest zooms rather than refuse it
  dl_zmax = dl_areas[dl_area].zmax;
  dl_total = countTiles(dl_areas[dl_area], dl_zmax);
  while (dl_total > MAX_TILES && dl_zmax > DETAIL_ZMIN) {
    dl_zmax--;
    dl_total = countTiles(dl_areas[dl_area], dl_zmax);
  }
  if (dl_total > MAX_TILES) {
    lv_label_set_text(status_lbl, "That area is too big, pick a smaller one");
    return;
  }
  strlcpy(dl_dir, mapStyleDir(), sizeof(dl_dir));
  dl_lookup_n = rptPlacesNeeded(dl_lookups, DL_LOOKUPS);

  dl_done = 0;
  dl_cancel = false;
  dl_bt_was_on = ui->isBluetoothEnabled();
  if (dl_bt_was_on) ui->disableBluetooth();   // frees radio time for WiFi

  dl_state = DL_CONNECTING;
  setBusy(true);
  if (xTaskCreatePinnedToCore(dlTask, "mapdl", 12288, NULL, 1, NULL, 0) != pdPASS) {
    dl_state = DL_IDLE;
    setBusy(false);
    if (dl_bt_was_on) ui->enableBluetooth();
    lv_label_set_text(status_lbl, "Not enough memory to start");
  }
}

static void closeCb(lv_event_t* e) {
  if (dl_state == DL_CONNECTING || dl_state == DL_RUNNING) {
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);   // carries on; the top bar shows progress
    return;
  }
  if (poll_timer != NULL) { lv_timer_delete(poll_timer); poll_timer = NULL; }
  scanning = false;
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  memset(dl_pw, 0, sizeof(dl_pw));
  lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

static void rescanCb(lv_event_t* e) { scanStart(); }

static void editOpen(int target) {
  edit_target = target;
  lv_textarea_set_password_mode(edit_ta, target == 0);
  lv_textarea_set_text(edit_ta, target == 0 ? dl_pw : dl_loc);
  lv_textarea_set_placeholder_text(edit_ta, target == 0 ? "WiFi password" : "32.77, -96.79");
  lv_textarea_set_max_length(edit_ta, target == 0 ? 63 : 30);
  lv_keyboard_set_mode(edit_kb, target == 0 ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_SPECIAL);
  lv_obj_remove_flag(edit_scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(edit_scr);
}

static void pwCb(lv_event_t* e) { editOpen(0); }
static void locCb(lv_event_t* e) { editOpen(1); }

static void editTaCb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
  if (code == LV_EVENT_READY) {
    const char* txt = lv_textarea_get_text(edit_ta);
    if (edit_target == 0) strlcpy(dl_pw, txt, sizeof(dl_pw));
    else strlcpy(dl_loc, txt, sizeof(dl_loc));
  }
  lv_textarea_set_text(edit_ta, "");
  lv_obj_add_flag(edit_scr, LV_OBJ_FLAG_HIDDEN);
  refreshFields();
}

static lv_obj_t* fieldRow(lv_obj_t* parent, int y, lv_event_cb_t cb) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, 304, 30);
  lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, y);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &rc_font_14, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  return l;
}

static void build() {
  scr = lv_obj_create(lv_screen_active());
  lv_obj_set_size(scr, 320, 240);
  lv_obj_align(scr, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  lv_obj_set_style_pad_all(scr, 8, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  title_lbl = lv_label_create(scr);
  lv_obj_set_style_text_font(title_lbl, &rc_font_16, 0);
  lv_obj_set_style_text_color(title_lbl, lv_color_hex(COL_ACCENT), 0);
  lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  net_dd = lv_dropdown_create(scr);
  lv_obj_set_size(net_dd, 262, 32);
  lv_obj_align(net_dd, LV_ALIGN_TOP_LEFT, 0, 24);

  lv_obj_t* rb = lv_button_create(scr);
  lv_obj_set_size(rb, 36, 32);
  lv_obj_align(rb, LV_ALIGN_TOP_RIGHT, 0, 24);
  lv_obj_set_style_bg_color(rb, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(rb, rescanCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* rl = lv_label_create(rb);
  lv_label_set_text(rl, LV_SYMBOL_REFRESH);
  lv_obj_center(rl);

  pw_lbl = fieldRow(scr, 60, pwCb);
  loc_lbl = fieldRow(scr, 94, locCb);

  area_dd = lv_dropdown_create(scr);
  lv_obj_set_size(area_dd, 304, 32);
  lv_obj_align(area_dd, LV_ALIGN_TOP_LEFT, 0, 128);
  char opts[64] = "";
  for (size_t i = 0; i < sizeof(dl_areas) / sizeof(dl_areas[0]); i++) {
    if (i > 0) strlcat(opts, "\n", sizeof(opts));
    strlcat(opts, dl_areas[i].label, sizeof(opts));
  }
  lv_dropdown_set_options(area_dd, opts);

  status_lbl = lv_label_create(scr);
  lv_label_set_text(status_lbl, "");
  lv_obj_set_width(status_lbl, 304);
  lv_obj_set_style_text_font(status_lbl, &rc_font_12, 0);
  lv_obj_set_style_text_color(status_lbl, lv_color_hex(COL_MUTED), 0);
  lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 0, 166);

  lv_obj_t* sb = lv_button_create(scr);
  lv_obj_set_size(sb, 180, 34);
  lv_obj_align(sb, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(sb, startCb, LV_EVENT_CLICKED, NULL);
  start_lbl = lv_label_create(sb);
  lv_obj_center(start_lbl);

  lv_obj_t* cb = lv_button_create(scr);
  lv_obj_set_size(cb, 112, 34);
  lv_obj_align(cb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(cb, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(cb, closeCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* cl = lv_label_create(cb);
  lv_label_set_text(cl, "Close");
  lv_obj_center(cl);

  edit_scr = lv_obj_create(lv_screen_active());
  lv_obj_set_size(edit_scr, 320, 240);
  lv_obj_align(edit_scr, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(edit_scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(edit_scr, 0, 0);
  lv_obj_set_style_radius(edit_scr, 0, 0);
  lv_obj_set_style_pad_all(edit_scr, 4, 0);
  lv_obj_remove_flag(edit_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(edit_scr, LV_OBJ_FLAG_HIDDEN);
  edit_ta = lv_textarea_create(edit_scr);
  lv_obj_set_size(edit_ta, 312, 42);
  lv_obj_align(edit_ta, LV_ALIGN_TOP_MID, 0, 0);
  lv_textarea_set_one_line(edit_ta, true);
  lv_obj_add_event_cb(edit_ta, editTaCb, LV_EVENT_ALL, NULL);
  edit_kb = lv_keyboard_create(edit_scr);
  lv_keyboard_set_textarea(edit_kb, edit_ta);
  lv_obj_set_size(edit_kb, 320, KB_HEIGHT + 20);
  lv_obj_align(edit_kb, LV_ALIGN_BOTTOM_MID, 0, 4);
  kbAttachShiftBehavior(edit_kb);
}

bool mapDlWifiStatus(char* out, size_t out_sz) {
  if (dl_state == DL_RUNNING) {
    snprintf(out, out_sz, "%d%%", dl_total > 0 ? (int) dl_done * 100 / (int) dl_total : 0);
    return true;
  }
  bool on = dl_state == DL_CONNECTING || WiFi.getMode() != WIFI_OFF;
  strlcpy(out, on ? "On" : "Off", out_sz);
  return on;
}

void mapDlOpen(lv_event_t* e) {
  if (scr == NULL) build();
  if (dl_loc[0] == 0) {
    double lat, lon;
    if (mapViewPosition(&lat, &lon)) snprintf(dl_loc, sizeof(dl_loc), "%.4f, %.4f", lat, lon);
  }
  bool busy = dl_state == DL_CONNECTING || dl_state == DL_RUNNING;
  setBusy(busy);
  refreshFields();
  if (!busy) {
    lv_label_set_text_fmt(title_lbl, "Sync map: %s", mapStyleName());
    lv_label_set_text(status_lbl, "Saved to the card. WiFi turns off when finished.");
  }
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(scr);
  if (poll_timer == NULL) poll_timer = lv_timer_create(pollCb, 500, NULL);
  scanStart();
}
