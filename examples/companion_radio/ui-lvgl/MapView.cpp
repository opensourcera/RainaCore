#include "MapView.h"
#include "UIPlaces.h"

#include <Arduino.h>
#include <math.h>
#include <SD_MMC.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <stdio.h>
#include "../MyMesh.h"
#include "target.h"
#include "UICommon.h"

// Wio Tracker L2 Pro SDIO 1-bit wiring; TF power rail is raised by board init
#define SD_PIN_CLK 2
#define SD_PIN_CMD 3
#define SD_PIN_D0  1

#define TILE_PX   256
#define GRID_COLS 3
#define GRID_ROWS 2
#define MAP_MIN_Z 5
#define MAP_MAX_Z 17

// The map fills the panel behind the status bar and sidebar, so the view is
// centered on the part those bars leave clear.
#define SCR_W   320
#define SCR_H   240
#define BAR_W   46
#define BAR_H   22
#define TOP_PLACE_W 124
#define TOP_WIFI_W  54
#define VIEW_CX ((SCR_W + BAR_W) / 2)
#define VIEW_CY ((SCR_H + BAR_H) / 2)
#define BAR_OPA LV_OPA_70
#define MAP_LABEL_MAX_W 78
#define CLUSTER_PX 30          // nodes closer than this on screen share a circle
#define CLUSTER_SPREAD_PX 90   // a tapped circle zooms until its nodes span this

static SensorManager* map_sensors = NULL;
static UITask* map_task = NULL;
static lv_obj_t* map_canvas = NULL;
static lv_obj_t* chrome_status = NULL;
static lv_obj_t* chrome_tabview = NULL;
static lv_obj_t* chrome_name = NULL;
static lv_obj_t* top_place = NULL;
static lv_obj_t* top_wifi = NULL;
static lv_obj_t* top_wifi_lbl = NULL;
static void topBarRefresh();
static lv_obj_t* tiles[GRID_COLS * GRID_ROWS];
static lv_obj_t* marker = NULL;
static lv_obj_t* status_lbl = NULL;
static lv_obj_t* zoom_lbl = NULL;

// radar view: rings around you, used wherever the card has no map tiles
#define RADAR_RINGS 4
static lv_obj_t* radar_rings[RADAR_RINGS];
static lv_obj_t* foot_lbl = NULL;     // ring scale, or the map credit
static bool fit_done = false;
static int tile_key_z = -1, tile_key_x = -1, tile_key_y = -1;
static bool tile_key_hit = false;
static bool sd_ok = false;

#define MAX_MAP_NODES 40
static lv_obj_t* node_marks[MAX_MAP_NODES];
static lv_obj_t* node_labels[MAX_MAP_NODES];
static lv_obj_t* node_counts[MAX_MAP_NODES];

// One marker on screen: a single node, or a circle standing for several.
struct MapCluster {
  int seed_x, seed_y;           // screen position of the first node
  int count;
  int first_idx;                // contact shown when count is 1
  bool any_repeater, any_other;
  double x0, y0, x1, y1;        // bounds of its nodes, in tiles at the current zoom
};
static MapCluster clusters[MAX_MAP_NODES];

static void clusterZoom(const MapCluster& c);

static void node_mark_cb(lv_event_t* e) {
  int slot = (int)(intptr_t) lv_event_get_user_data(e);
  if (slot < 0 || slot >= MAX_MAP_NODES) return;
  if (clusters[slot].count > 1) clusterZoom(clusters[slot]);
  else if (map_task != NULL) map_task->openContactDetail(clusters[slot].first_idx);
}

// view center in tile-float coordinates at the current zoom
static int map_z = 13;
static double center_fx, center_fy;
static bool center_initialized = false;

static void lonlatToTileF(double lon, double lat, int z, double* fx, double* fy) {
  double n = (double)(1 << z);
  *fx = (lon + 180.0) / 360.0 * n;
  double lat_r = lat * M_PI / 180.0;
  *fy = (1.0 - asinh(tan(lat_r)) / M_PI) / 2.0 * n;
}

static void savedPosLoad(double* lat, double* lon) {
  File f = SPIFFS.open("/lastpos", "r");
  if (!f) return;
  double a = f.parseFloat();
  double b = f.parseFloat();
  f.close();
  if (a != 0 || b != 0) { *lat = a; *lon = b; }
}
static void savedPosStore(double lat, double lon) {
  static unsigned long last_save = 0;
  if (last_save != 0 && millis() - last_save < 600000) return;   // at most every 10 min
  last_save = millis();
  File f = SPIFFS.open("/lastpos", "w");
  if (f) { f.printf("%.6f %.6f\n", lat, lon); f.close(); }
}

static void mapApplyTint(bool dim);

static void mapInitCenter() {
  if (center_initialized) return;
  double lat = 0, lon = 0;
  NodePrefs* np = map_task != NULL ? map_task->nodePrefs() : NULL;
  if (np != NULL && (np->node_lat != 0 || np->node_lon != 0)) { lat = np->node_lat; lon = np->node_lon; }
  else savedPosLoad(&lat, &lon);
  if (lat == 0 && lon == 0) {
    map_z = 5;   // nothing known yet: wide view until GPS or the phone sets a position
    lonlatToTileF(0.0, 20.0, map_z, &center_fx, &center_fy);
  } else {
    lonlatToTileF(lon, lat, map_z, &center_fx, &center_fy);
  }
  center_initialized = true;
}

// not exported through lvgl.h in this configuration
extern "C" void lv_image_cache_drop(const void* src);

#define SYMBOL_SUN  "\xEF\x86\x85"   /* U+F185 */
#define SYMBOL_MOON "\xEF\x86\x86"   /* U+F186 */

// Terrain is its own tile set. Flat is the Map set softened on the device, so
// it needs no download of its own.
enum { STYLE_MAP, STYLE_TERRAIN, STYLE_FLAT, STYLE_COUNT };
static int map_style = STYLE_MAP;
static const char* const STYLE_NAMES[STYLE_COUNT] = { "Map", "Terrain", "Flat" };
static const char* const STYLE_CREDITS[STYLE_COUNT] = {
  "\xC2\xA9 OpenStreetMap",
  "\xC2\xA9 OSM, SRTM, OpenTopoMap\nCC-BY-SA",
  "\xC2\xA9 OpenStreetMap",
};
#define FLAT_TINT 0xEEF1F3

const char* mapStyleDir() { return map_style == STYLE_TERRAIN ? "maps_terrain" : "maps"; }
const char* mapStyleName() { return map_style == STYLE_FLAT ? STYLE_NAMES[STYLE_MAP] : STYLE_NAMES[map_style]; }

static bool map_night = false;     // defined here: the pak loader needs it
static bool dark_set_ok = false;   // a dark set was seen on the card
static bool day_set_ok = false;    // a day set was seen on the card
static int  dark_hits = 0;         // tiles served from maps_dark this refresh

// Packed tiles: maps/{z}/{x}.pak = 'TPK1' | y0 | y1 | offsets[n+1] | PNGs.
// One file per tile column, since a FAT card copies millions of small files
// far slower than a few thousand large ones. Loose z/x/y.png still works.
static const char* tileDir() {
  if (map_style == STYLE_TERRAIN) return "maps_terrain";
  return map_night ? "maps_dark" : "maps";
}

static uint8_t* pak_buf[GRID_COLS * GRID_ROWS];
static size_t pak_cap[GRID_COLS * GRID_ROWS];
static lv_image_dsc_t pak_dsc[GRID_COLS * GRID_ROWS];
static int pak_key_z[GRID_COLS * GRID_ROWS];
static int pak_key_x[GRID_COLS * GRID_ROWS];
static int pak_key_y[GRID_COLS * GRID_ROWS];

static bool loadPakTile(lv_obj_t* img, int z, int tx, int ty, int slot) {
  bool terrain = map_style == STYLE_TERRAIN;
  int zkey = z | (map_night ? 0x100 : 0) | (terrain ? 0x200 : 0);
  if (pak_key_z[slot] == zkey && pak_key_x[slot] == tx && pak_key_y[slot] == ty) {
    return true;   // this slot already shows exactly this tile (and this set)
  }
  const char* first = tileDir();
  const char* second = terrain ? NULL : map_night ? "maps" : "maps_dark";
  bool from_dark = !terrain && map_night;
  char path[48];
  snprintf(path, sizeof(path), "/sdcard/%s/%d/%d.pak", first, z, tx);
  FILE* f = fopen(path, "rb");
  if (f == NULL && second != NULL) {
    snprintf(path, sizeof(path), "/sdcard/%s/%d/%d.pak", second, z, tx);
    f = fopen(path, "rb");
    from_dark = !map_night;
  }
  if (f == NULL) return false;
  uint8_t hdr[12];
  uint32_t y0, y1;
  if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "TPK1", 4) != 0) { fclose(f); return false; }
  memcpy(&y0, hdr + 4, 4);
  memcpy(&y1, hdr + 8, 4);
  if (ty < (int) y0 || ty > (int) y1) { fclose(f); return false; }
  uint32_t off[2];
  if (fseek(f, 12 + 4 * (ty - (int) y0), SEEK_SET) != 0 || fread(off, 4, 2, f) != 2) { fclose(f); return false; }
  uint32_t len = off[1] - off[0];
  if (len == 0 || len > 262144) { fclose(f); return false; }
  if (pak_cap[slot] < len) {
    uint8_t* nb = (uint8_t*) heap_caps_realloc(pak_buf[slot], len, MALLOC_CAP_SPIRAM);
    if (nb == NULL) { fclose(f); return false; }
    pak_buf[slot] = nb;
    pak_cap[slot] = len;
  }
  bool ok = fseek(f, off[0], SEEK_SET) == 0 && fread(pak_buf[slot], 1, len, f) == len;
  fclose(f);
  if (!ok) return false;

  lv_image_cache_drop(&pak_dsc[slot]);   // same dsc pointer, new bytes
  memset(&pak_dsc[slot], 0, sizeof(pak_dsc[slot]));
  pak_dsc[slot].header.magic = LV_IMAGE_HEADER_MAGIC;
  pak_dsc[slot].header.cf = LV_COLOR_FORMAT_RAW;
  pak_dsc[slot].header.w = TILE_PX;
  pak_dsc[slot].header.h = TILE_PX;
  pak_dsc[slot].data_size = len;
  pak_dsc[slot].data = pak_buf[slot];
  lv_image_set_src(img, &pak_dsc[slot]);
  lv_obj_invalidate(img);
  pak_key_z[slot] = zkey; pak_key_x[slot] = tx; pak_key_y[slot] = ty;
  if (from_dark) { dark_hits++; dark_set_ok = true; }
  return true;
}

static bool sdMount() {
  if (sd_ok) return true;
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);
  sd_ok = SD_MMC.begin("/sdcard", true /* 1-bit mode */);
  MESH_DEBUG_PRINTLN("map: SD %s", sd_ok ? "mounted" : "mount FAILED");
  if (sd_ok) {
    File d = SD_MMC.open("/maps");
    day_set_ok = d && d.isDirectory();
    MESH_DEBUG_PRINTLN("map: tile store %s", day_set_ok ? "present" : "missing");
    if (d) d.close();
    File dd = SD_MMC.open("/maps_dark");
    dark_set_ok = dd && dd.isDirectory();
    if (dd) dd.close();
    MESH_DEBUG_PRINTLN("map: dark set %s", dark_set_ok ? "present" : "not on card");
    if (dark_set_ok && !day_set_ok) map_night = true;   // dark-only card: start dark
  }
  return sd_ok;
}

static double tileYToLat(double fy, int z) {
  double n = M_PI * (1.0 - 2.0 * fy / (double)(1 << z));
  return atan(sinh(n)) * 180.0 / M_PI;
}

static bool centerTileOnCard() {
  if (!sd_ok) return false;
  int tx = (int) floor(center_fx), ty = (int) floor(center_fy);
  if (tx == tile_key_x && ty == tile_key_y && map_z == tile_key_z) return tile_key_hit;
  char path[64];
  struct stat st;
  snprintf(path, sizeof(path), "/sdcard/%s/%d/%d/%d.png", mapStyleDir(), map_z, tx, ty);
  bool hit = stat(path, &st) == 0;
  if (!hit) {
    snprintf(path, sizeof(path), "/sdcard/%s/%d/%d.pak", mapStyleDir(), map_z, tx);
    hit = stat(path, &st) == 0;
  }
  tile_key_z = map_z; tile_key_x = tx; tile_key_y = ty; tile_key_hit = hit;
  return hit;
}

bool mapViewPosition(double* lat, double* lon) {
  LocationProvider* nmea = map_sensors != NULL ? map_sensors->getLocationProvider() : NULL;
  if (nmea != NULL && nmea->isValid()) {
    *lat = nmea->getLatitude() / 1000000.0;
    *lon = nmea->getLongitude() / 1000000.0;
    return true;
  }
  // read flash once: this runs on every drag step while panning
  static bool saved_loaded = false;
  static double saved_lat = 0, saved_lon = 0;
  if (!saved_loaded) {
    savedPosLoad(&saved_lat, &saved_lon);
    saved_loaded = true;
  }
  if (saved_lat == 0 && saved_lon == 0) return false;
  *lat = saved_lat; *lon = saved_lon;
  return true;
}

// zoom and center so you and every located contact fit on screen
static bool mapFitAll() {
  double min_lat = 90, max_lat = -90, min_lon = 180, max_lon = -180;
  int points = 0;
  double lat, lon;
  if (mapViewPosition(&lat, &lon)) {
    min_lat = max_lat = lat; min_lon = max_lon = lon;
    points++;
  }
  for (int idx = MAX_ANON_CONTACTS; idx < the_mesh.getTotalContactSlots(); idx++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) break;
    if (c.name[0] == 0 || (c.gps_lat == 0 && c.gps_lon == 0)) continue;
    double clat = c.gps_lat / 1000000.0, clon = c.gps_lon / 1000000.0;
    if (clat < min_lat) min_lat = clat;
    if (clat > max_lat) max_lat = clat;
    if (clon < min_lon) min_lon = clon;
    if (clon > max_lon) max_lon = clon;
    points++;
  }
  if (points == 0) return false;

  int cw = SCR_W - BAR_W, chh = SCR_H - BAR_H;
  int z = points == 1 ? 12 : MAP_MAX_Z;
  double x0, y0, x1, y1;
  for (; z > MAP_MIN_Z; z--) {
    lonlatToTileF(min_lon, max_lat, z, &x0, &y0);
    lonlatToTileF(max_lon, min_lat, z, &x1, &y1);
    if ((x1 - x0) * TILE_PX <= cw - 70 && (y1 - y0) * TILE_PX <= chh - 70) break;
  }
  lonlatToTileF(min_lon, max_lat, z, &x0, &y0);
  lonlatToTileF(max_lon, min_lat, z, &x1, &y1);
  map_z = z;
  center_fx = (x0 + x1) / 2.0;
  center_fy = (y0 + y1) / 2.0;
  return true;
}

// rings are centered on you when your position is known
static void radarUpdate(bool show) {
  if (!show) {
    for (int i = 0; i < RADAR_RINGS; i++) lv_obj_add_flag(radar_rings[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(foot_lbl, STYLE_CREDITS[map_style]);
    return;
  }
  double lat = tileYToLat(center_fy, map_z);
  double mpp = 40075016.686 * cos(lat * M_PI / 180.0) / ((double) TILE_PX * (double)(1 << map_z));
  double want = 48.0 * mpp;
  double mag = pow(10.0, floor(log10(want)));
  double step = mag;
  if (want >= 5 * mag) step = 5 * mag;
  else if (want >= 2 * mag) step = 2 * mag;

  int cx = VIEW_CX, cy = VIEW_CY;
  double mlat, mlon;
  if (mapViewPosition(&mlat, &mlon)) {
    double fx, fy;
    lonlatToTileF(mlon, mlat, map_z, &fx, &fy);
    cx = (int)((fx - center_fx) * TILE_PX) + VIEW_CX;
    cy = (int)((fy - center_fy) * TILE_PX) + VIEW_CY;
  }
  for (int i = 0; i < RADAR_RINGS; i++) {
    int r = (int)((i + 1) * step / mpp);
    lv_obj_set_size(radar_rings[i], r * 2, r * 2);
    lv_obj_set_pos(radar_rings[i], cx - r, cy - r);
    lv_obj_remove_flag(radar_rings[i], LV_OBJ_FLAG_HIDDEN);
  }
  double shown = units_miles ? step / 1609.344 : step / 1000.0;
  lv_label_set_text_fmt(foot_lbl, "Rings %s%d %s",
                        shown < 1.0 ? "0." : "",
                        shown < 1.0 ? (int)(shown * 10.0 + 0.5) : (int)(shown + 0.5),
                        units_miles ? "mi" : "km");
}

void mapViewReload() {
  for (int k = 0; k < GRID_COLS * GRID_ROWS; k++) pak_key_z[k] = -1;
  tile_key_z = -1;
  day_set_ok = sd_ok && SD_MMC.exists("/maps");
  mapViewRefresh();
}

static void fit_cb(lv_event_t* e) {
  if (mapFitAll()) mapViewRefresh();
  else if (map_task != NULL) map_task->showToast("No locations yet");
}

// screen areas held by the buttons, zoom tag and credit, kept free of names
static const lv_area_t MAP_CONTROLS[] = {
  { BAR_W + 2, BAR_H + 2, BAR_W + 114, BAR_H + 46 },   // style toggle, zoom tag
  { SCR_W - 82, BAR_H + 2, SCR_W - 1, BAR_H + 36 },    // fit, repeater
  { BAR_W + 2, SCR_H - 72, BAR_W + 42, SCR_H - 1 },    // night, locate
  { SCR_W - 42, SCR_H - 72, SCR_W - 1, SCR_H - 1 },    // zoom buttons
  { VIEW_CX - 90, SCR_H - 36, VIEW_CX + 90, SCR_H - 1 },   // credit or ring scale
};

// a name goes right of its dot, or left if that is taken; otherwise the dot
// stays alone and a tap on it still opens the node
static bool labelSpotFree(const lv_area_t* a, const lv_area_t* taken, int n) {
  if (a->x1 < BAR_W || a->x2 >= SCR_W || a->y1 < BAR_H || a->y2 >= SCR_H) return false;
  for (const lv_area_t& t : MAP_CONTROLS) {
    if (a->x1 <= t.x2 && a->x2 >= t.x1 && a->y1 <= t.y2 && a->y2 >= t.y1) return false;
  }
  for (int i = 0; i < n; i++) {
    if (a->x1 <= taken[i].x2 && a->x2 >= taken[i].x1 && a->y1 <= taken[i].y2 && a->y2 >= taken[i].y1) return false;
  }
  return true;
}

// Groups by screen distance. Once the marker slots run out a node joins the
// nearest group, so nothing in view is ever left off the map.
static int clusterNodes() {
  int n = 0;
  const int radius = map_z >= MAP_MAX_Z ? 0 : CLUSTER_PX;   // top zoom shows every node
  for (int pass = 0; pass < 2; pass++)   // repeaters first: they get the labels
  for (int idx = MAX_ANON_CONTACTS; idx < the_mesh.getTotalContactSlots(); idx++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) break;
    if (c.name[0] == 0 || (c.gps_lat == 0 && c.gps_lon == 0)) continue;
    bool is_rep = c.type == ADV_TYPE_REPEATER;
    if (is_rep != (pass == 0)) continue;
    double fx, fy;
    lonlatToTileF(c.gps_lon / 1000000.0, c.gps_lat / 1000000.0, map_z, &fx, &fy);
    int px = (int)((fx - center_fx) * TILE_PX) + VIEW_CX;
    int py = (int)((fy - center_fy) * TILE_PX) + VIEW_CY;
    if (px < -20 || py < -20 || px > SCR_W + 20 || py > SCR_H + 20) continue;

    int join = -1;
    long best = 0;
    for (int k = 0; k < n; k++) {
      long dx = px - clusters[k].seed_x, dy = py - clusters[k].seed_y;
      long d = dx * dx + dy * dy;
      bool near = labs(dx) < radius && labs(dy) < radius;
      if ((near || n >= MAX_MAP_NODES) && (join < 0 || d < best)) { join = k; best = d; }
    }
    if (join < 0) {
      join = n++;
      MapCluster& g = clusters[join];
      g.seed_x = px; g.seed_y = py;
      g.count = 0;
      g.first_idx = idx;
      g.any_repeater = g.any_other = false;
      g.x0 = g.x1 = fx;
      g.y0 = g.y1 = fy;
    }
    MapCluster& g = clusters[join];
    g.count++;
    if (is_rep) g.any_repeater = true; else g.any_other = true;
    if (fx < g.x0) g.x0 = fx;
    if (fx > g.x1) g.x1 = fx;
    if (fy < g.y0) g.y0 = fy;
    if (fy > g.y1) g.y1 = fy;
  }

  // groups grow past their first node, so two can end up touching: merge them
  for (int i = 0; radius > 0 && i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      double dx = ((clusters[i].x0 + clusters[i].x1) - (clusters[j].x0 + clusters[j].x1)) / 2.0 * TILE_PX;
      double dy = ((clusters[i].y0 + clusters[i].y1) - (clusters[j].y0 + clusters[j].y1)) / 2.0 * TILE_PX;
      if (fabs(dx) >= radius || fabs(dy) >= radius) continue;
      MapCluster& a = clusters[i];
      const MapCluster& b = clusters[j];
      a.count += b.count;
      a.any_repeater = a.any_repeater || b.any_repeater;
      a.any_other = a.any_other || b.any_other;
      if (b.x0 < a.x0) a.x0 = b.x0;
      if (b.x1 > a.x1) a.x1 = b.x1;
      if (b.y0 < a.y0) a.y0 = b.y0;
      if (b.y1 > a.y1) a.y1 = b.y1;
      clusters[j] = clusters[--n];
      i = -1;   // a is now larger: look at every pair again
      break;
    }
  }
  return n;
}

// centers on the circle's nodes and zooms in far enough to pull them apart
static void clusterZoom(const MapCluster& c) {
  double span = (c.x1 - c.x0) * TILE_PX;
  double high = (c.y1 - c.y0) * TILE_PX;
  if (high > span) span = high;
  int z = map_z + 1;
  double scale = 2.0;
  while (z < MAP_MAX_Z && span * scale < CLUSTER_SPREAD_PX) { z++; scale *= 2.0; }
  if (z > MAP_MAX_Z) return;
  center_fx = (c.x0 + c.x1) / 2.0 * scale;
  center_fy = (c.y0 + c.y1) / 2.0 * scale;
  map_z = z;
  mapViewRefresh();
}

void mapViewRefresh() {
  topBarRefresh();
  if (map_canvas == NULL) return;
  mapInitCenter();
  if (!fit_done && !centerTileOnCard() && mapFitAll()) fit_done = true;
  dark_hits = 0;

  int n = 1 << map_z;
  // start from the tile under the top-left corner, so the grid always reaches
  // the far edges of the panel
  int base_tx = (int)floor(center_fx - (double) VIEW_CX / TILE_PX);
  int base_ty = (int)floor(center_fy - (double) VIEW_CY / TILE_PX);

  char src[48];
  for (int j = 0; j < GRID_ROWS; j++) {
    for (int i = 0; i < GRID_COLS; i++) {
      lv_obj_t* img = tiles[j * GRID_COLS + i];
      int tx = base_tx + i;
      int ty = base_ty + j;
      int px = (int)((tx - center_fx) * TILE_PX) + VIEW_CX;
      int py = (int)((ty - center_fy) * TILE_PX) + VIEW_CY;
      lv_obj_set_pos(img, px, py);
      int slot = j * GRID_COLS + i;
      if (sd_ok && tx >= 0 && ty >= 0 && tx < n && ty < n) {
        if (!loadPakTile(img, map_z, tx, ty, slot)) {
          // loose-tile fallback; the leading slash matters after "/sdcard"
          pak_key_z[slot] = -1;
          snprintf(src, sizeof(src), "A:/%s/%d/%d/%d.png", tileDir(), map_z, tx, ty);
          lv_image_set_src(img, src);
          // a missing file leaves the old picture in place: never show that
          // tile here, it belongs to another spot or zoom
          const void* now = lv_image_get_src(img);
          if (now == NULL || lv_image_src_get_type(now) != LV_IMAGE_SRC_FILE
              || strcmp((const char*) now, src) != 0) {
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            continue;
          }
        }
        lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
      } else {
        pak_key_z[slot] = -1;
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  mapApplyTint(map_night && dark_hits == 0);
  radarUpdate(!centerTileOnCard());
  lv_label_set_text_fmt(zoom_lbl, map_night ? "z%d dark" : "z%d", map_z);
  lv_obj_add_flag(status_lbl, LV_OBJ_FLAG_HIDDEN);

  int used = clusterNodes();
  static lv_area_t taken[MAX_MAP_NODES];
  int taken_n = 0;
  const int label_h = lv_font_get_line_height(&rc_font_12) + 2;
  for (int i = 0; i < used; i++) {
    const MapCluster& g = clusters[i];
    double fx = (g.x0 + g.x1) / 2.0, fy = (g.y0 + g.y1) / 2.0;
    int px = (int)((fx - center_fx) * TILE_PX) + VIEW_CX;
    int py = (int)((fy - center_fy) * TILE_PX) + VIEW_CY;
    lv_obj_t* mark = node_marks[i];

    if (g.count > 1) {
      int d = g.count < 10 ? 20 : g.count < 100 ? 24 : 28;
      lv_obj_set_size(mark, d, d);
      lv_obj_set_pos(mark, px - d / 2, py - d / 2);
      lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(mark, lv_color_hex(!g.any_other ? COL_REPEATER
                                                   : !g.any_repeater ? COL_WARN : 0xD5DCE2), 0);
      lv_label_set_text_fmt(node_counts[i], "%d", g.count);
      lv_obj_remove_flag(node_counts[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }

    ContactInfo c;
    if (!the_mesh.getContactByIdx(g.first_idx, c)) {
      lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_add_flag(node_counts[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(mark, 10, 10);
    lv_obj_set_pos(mark, px - 5, py - 5);
    lv_obj_set_style_bg_color(mark, lv_color_hex(g.any_repeater ? COL_REPEATER : COL_WARN), 0);
    lv_obj_set_style_radius(mark, g.any_repeater ? 2 : LV_RADIUS_CIRCLE, 0);   // square = repeater
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);

    int label_w = lv_text_get_width(c.name, strlen(c.name), &rc_font_12, 0) + 6;
    if (label_w > MAP_LABEL_MAX_W) label_w = MAP_LABEL_MAX_W;
    lv_area_t box;
    box.y1 = py - label_h / 2;
    box.y2 = box.y1 + label_h - 1;
    box.x1 = px + 7;
    box.x2 = box.x1 + label_w - 1;
    bool fits = labelSpotFree(&box, taken, taken_n);
    if (!fits) {
      box.x1 = px - 7 - label_w;
      box.x2 = box.x1 + label_w - 1;
      fits = labelSpotFree(&box, taken, taken_n);
    }
    if (fits) {
      taken[taken_n++] = box;
      lv_label_set_text(node_labels[i], c.name);
      lv_obj_set_size(node_labels[i], label_w, label_h);
      lv_obj_set_pos(node_labels[i], box.x1, box.y1);
      lv_obj_remove_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  for (int i = used; i < MAX_MAP_NODES; i++) {
    lv_obj_add_flag(node_marks[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
  }

  bool marker_shown = false;
  if (map_sensors != NULL) {
    LocationProvider* nmea = map_sensors->getLocationProvider();
    if (nmea != NULL && nmea->isValid()) {
      savedPosStore(nmea->getLatitude() / 1000000.0, nmea->getLongitude() / 1000000.0);
      double gfx, gfy;
      lonlatToTileF(nmea->getLongitude() / 1000000.0, nmea->getLatitude() / 1000000.0,
                    map_z, &gfx, &gfy);
      int mx = (int)((gfx - center_fx) * TILE_PX) + VIEW_CX;
      int my = (int)((gfy - center_fy) * TILE_PX) + VIEW_CY;
      if (mx >= 0 && my >= 0 && mx < SCR_W && my < SCR_H) {
        lv_obj_set_pos(marker, mx - 6, my - 6);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
        marker_shown = true;
      }
    }
  }
  if (!marker_shown) lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
}

static void map_press_cb(lv_event_t* e) {
  lv_indev_t* indev = lv_indev_active();
  if (indev == NULL) return;
  lv_point_t vect;
  lv_indev_get_vect(indev, &vect);
  if (vect.x == 0 && vect.y == 0) return;
  fit_done = true;   // the view is theirs now: no automatic jump
  center_fx -= (double)vect.x / TILE_PX;
  center_fy -= (double)vect.y / TILE_PX;
  double n = (double)(1 << map_z);
  if (center_fx < 0) center_fx = 0;
  if (center_fy < 0) center_fy = 0;
  if (center_fx > n) center_fx = n;
  if (center_fy > n) center_fy = n;
  mapViewRefresh();
}

static void zoom_cb(lv_event_t* e) {
  int dir = (int)(intptr_t) lv_event_get_user_data(e);
  int new_z = map_z + dir;
  if (new_z < MAP_MIN_Z || new_z > MAP_MAX_Z) return;
  fit_done = true;   // the view is theirs now: no automatic jump
  double scale = dir > 0 ? 2.0 : 0.5;
  center_fx *= scale;
  center_fy *= scale;
  map_z = new_z;
  mapViewRefresh();
}

static void locate_cb(lv_event_t* e) {
  if (map_sensors == NULL) return;
  LocationProvider* nmea = map_sensors->getLocationProvider();
  if (nmea == NULL || !nmea->isValid()) return;
  lonlatToTileF(nmea->getLongitude() / 1000000.0, nmea->getLatitude() / 1000000.0,
                map_z, &center_fx, &center_fy);
  mapViewRefresh();
}

// jump to the next repeater that has shared a location
static int rep_cursor = -1;
static void next_repeater_cb(lv_event_t* e) {
  int slots = the_mesh.getTotalContactSlots();
  for (int step = 1; step <= slots; step++) {
    int idx = (rep_cursor + step + slots) % slots;
    if (idx < MAX_ANON_CONTACTS) continue;
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) continue;
    if (c.name[0] == 0 || c.type != ADV_TYPE_REPEATER || (c.gps_lat == 0 && c.gps_lon == 0)) continue;
    rep_cursor = idx;
    lonlatToTileF(c.gps_lon / 1000000.0, c.gps_lat / 1000000.0, map_z, &center_fx, &center_fy);
    mapViewRefresh();
    if (map_task != NULL) map_task->showToast(c.name);
    return;
  }
  if (map_task != NULL) map_task->showToast("No repeater has shared a location");
}

// with a dark set on the card the toggle swaps sets, otherwise it recolors
static lv_obj_t* night_btn_lbl = NULL;

// Dimming is only a fallback for cards without a dark set, so it is applied
// after a refresh, once it is known whether any tile came from maps_dark.
static void mapApplyTint(bool dim) {
  uint32_t color = dim ? 0x0A1428 : FLAT_TINT;
  lv_opa_t opa = dim ? LV_OPA_60 : map_style == STYLE_FLAT ? LV_OPA_50 : LV_OPA_TRANSP;
  for (int k = 0; k < GRID_COLS * GRID_ROWS; k++) {
    lv_obj_set_style_image_recolor(tiles[k], lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(tiles[k], opa, 0);
  }
}

static void mapApplyNight() {
  for (int k = 0; k < GRID_COLS * GRID_ROWS; k++) pak_key_z[k] = -1;   // reload
  if (night_btn_lbl != NULL) lv_label_set_text(night_btn_lbl, map_night ? SYMBOL_SUN : SYMBOL_MOON);
  mapViewRefresh();
}

static void mapNightLoad() {
  File f = SPIFFS.open("/mapnight", "r");
  if (f) { map_night = f.parseInt() != 0; f.close(); }
  File st = SPIFFS.open("/mapstyle", "r");
  if (st) {
    int v = st.parseInt();
    st.close();
    if (v >= 0 && v < STYLE_COUNT) map_style = v;
  }
}

// the toggle at the top of the map
static const int STYLE_ORDER[STYLE_COUNT] = { STYLE_TERRAIN, STYLE_MAP, STYLE_FLAT };
static const int STYLE_WIDTHS[STYLE_COUNT] = { 46, 32, 32 };
static lv_obj_t* style_btns[STYLE_COUNT];

static void styleToggleShow() {
  for (int i = 0; i < STYLE_COUNT; i++) {
    bool on = STYLE_ORDER[i] == map_style;
    lv_obj_set_style_bg_color(style_btns[i], lv_color_hex(on ? COL_ACCENT : 0x101820), 0);
    lv_obj_set_style_bg_opa(style_btns[i], on ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(style_btns[i], 0), lv_color_hex(on ? 0x101820 : 0xFFFFFF), 0);
  }
}

static void style_cb(lv_event_t* e) {
  map_style = (int)(intptr_t) lv_event_get_user_data(e);
  File f = SPIFFS.open("/mapstyle", "w");
  if (f) { f.print(map_style); f.close(); }
  styleToggleShow();
  tile_key_z = -1;
  for (int k = 0; k < GRID_COLS * GRID_ROWS; k++) pak_key_z[k] = -1;
  mapViewRefresh();
  if (map_task != NULL && !centerTileOnCard()) {
    static char note[48];
    snprintf(note, sizeof(note), "No %s tiles here yet. Tap WiFi.", mapStyleName());
    map_task->showToast(note);
  }
}

static void styleToggleBuild() {
  int x = BAR_W + 4;
  for (int i = 0; i < STYLE_COUNT; i++) {
    lv_obj_t* b = lv_button_create(map_canvas);
    lv_obj_set_size(b, STYLE_WIDTHS[i], 24);
    lv_obj_set_pos(b, x, BAR_H + 4);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_add_event_cb(b, style_cb, LV_EVENT_CLICKED, (void*)(intptr_t) STYLE_ORDER[i]);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, STYLE_NAMES[STYLE_ORDER[i]]);
    lv_obj_set_style_text_font(l, &rc_font_12, 0);
    lv_obj_center(l);
    style_btns[i] = b;
    x += STYLE_WIDTHS[i];
  }
}

static void night_cb(lv_event_t* e) {
  map_night = !map_night;
  File f = SPIFFS.open("/mapnight", "w");
  if (f) { f.print(map_night ? 1 : 0); f.close(); }
  mapApplyNight();
  if (map_task != NULL) {
    if (!map_night) map_task->showToast("Day tiles");
    else if (dark_hits > 0) map_task->showToast("Dark tiles");
    else map_task->showToast("No dark tiles here - dimming instead");
  }
}

static lv_obj_t* makeMapBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, void* ud,
                            lv_align_t align, int x_ofs, int y_ofs) {
  lv_obj_t* btn = lv_button_create(parent);
  lv_obj_set_size(btn, 36, 30);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_align(btn, align, x_ofs, y_ofs);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lbl);
  return btn;
}

void mapViewBuild(lv_obj_t* parent, SensorManager* sensors, UITask* task) {
  map_sensors = sensors;
  map_task = task;
  sdMount();

  map_canvas = lv_obj_create(parent);
  lv_obj_set_size(map_canvas, SCR_W, SCR_H);
  lv_obj_set_pos(map_canvas, 0, 0);
  lv_obj_move_background(map_canvas);   // under the status bar and tabview
  lv_obj_add_flag(map_canvas, LV_OBJ_FLAG_HIDDEN);   // until the Map tab opens
  lv_obj_set_style_bg_color(map_canvas, lv_color_hex(0x101820), 0);
  lv_obj_set_style_border_width(map_canvas, 0, 0);
  lv_obj_set_style_radius(map_canvas, 0, 0);
  lv_obj_set_style_pad_all(map_canvas, 0, 0);
  lv_obj_remove_flag(map_canvas, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(map_canvas, LV_OBJ_FLAG_SCROLL_CHAIN);   // drags pan the map, never the tabview
  lv_obj_add_flag(map_canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(map_canvas, map_press_cb, LV_EVENT_PRESSING, NULL);
  lv_obj_set_style_clip_corner(map_canvas, true, 0);

  for (int k = 0; k < GRID_COLS * GRID_ROWS; k++) {
    pak_key_z[k] = pak_key_x[k] = pak_key_y[k] = -1;
    tiles[k] = lv_image_create(map_canvas);
    lv_obj_set_size(tiles[k], TILE_PX, TILE_PX);
    lv_obj_remove_flag(tiles[k], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tiles[k], LV_OBJ_FLAG_HIDDEN);
  }

  for (int i = 0; i < RADAR_RINGS; i++) {
    radar_rings[i] = lv_obj_create(map_canvas);
    lv_obj_set_style_radius(radar_rings[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(radar_rings[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(radar_rings[i], lv_color_hex(0x2F6B60), 0);
    lv_obj_set_style_border_width(radar_rings[i], 1, 0);
    lv_obj_set_style_pad_all(radar_rings[i], 0, 0);
    lv_obj_remove_flag(radar_rings[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(radar_rings[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(radar_rings[i], LV_OBJ_FLAG_HIDDEN);
  }

  for (int i = 0; i < MAX_MAP_NODES; i++) {
    node_marks[i] = lv_obj_create(map_canvas);
    lv_obj_set_size(node_marks[i], 10, 10);
    lv_obj_set_style_radius(node_marks[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(node_marks[i], lv_color_hex(0xFFB454), 0);   // orange = other nodes
    lv_obj_set_style_border_color(node_marks[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(node_marks[i], 1, 0);
    lv_obj_set_style_pad_all(node_marks[i], 0, 0);
    lv_obj_remove_flag(node_marks[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(node_marks[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(node_marks[i], 8);   // fingertip-sized hit box
    lv_obj_add_event_cb(node_marks[i], node_mark_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    lv_obj_add_flag(node_marks[i], LV_OBJ_FLAG_HIDDEN);

    node_counts[i] = lv_label_create(node_marks[i]);
    lv_label_set_text(node_counts[i], "");
    lv_obj_set_style_text_font(node_counts[i], &rc_font_12, 0);
    lv_obj_set_style_text_color(node_counts[i], lv_color_hex(0x101820), 0);
    lv_obj_center(node_counts[i]);
    lv_obj_add_flag(node_counts[i], LV_OBJ_FLAG_HIDDEN);

    node_labels[i] = lv_label_create(map_canvas);
    lv_label_set_text(node_labels[i], "");
    lv_label_set_long_mode(node_labels[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(node_labels[i], &rc_font_12, 0);
    lv_obj_set_style_text_color(node_labels[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(node_labels[i], lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(node_labels[i], LV_OPA_50, 0);
    lv_obj_set_style_radius(node_labels[i], 3, 0);
    lv_obj_set_style_pad_hor(node_labels[i], 3, 0);
    lv_obj_set_style_pad_ver(node_labels[i], 1, 0);
    lv_obj_add_flag(node_labels[i], LV_OBJ_FLAG_HIDDEN);
  }

  marker = lv_obj_create(map_canvas);
  lv_obj_set_size(marker, 12, 12);
  lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(marker, lv_color_hex(0x2EC4A6), 0);
  lv_obj_set_style_border_color(marker, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(marker, 2, 0);
  lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);

  status_lbl = lv_label_create(map_canvas);
  lv_label_set_text(status_lbl, "");
  lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x98A4AE), 0);
  lv_obj_align(status_lbl, LV_ALIGN_CENTER, 0, 0);

  zoom_lbl = lv_label_create(map_canvas);
  lv_label_set_text(zoom_lbl, "");
  lv_obj_set_style_text_font(zoom_lbl, &rc_font_12, 0);
  lv_obj_set_style_text_color(zoom_lbl, lv_color_hex(0xF1F4F6), 0);
  lv_obj_set_style_bg_color(zoom_lbl, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(zoom_lbl, LV_OPA_50, 0);
  lv_obj_set_style_radius(zoom_lbl, 3, 0);
  lv_obj_set_style_pad_hor(zoom_lbl, 3, 0);
  lv_obj_align(zoom_lbl, LV_ALIGN_TOP_LEFT, BAR_W + 4, BAR_H + 31);

  makeMapBtn(map_canvas, LV_SYMBOL_PLUS, zoom_cb, (void*)(intptr_t)+1, LV_ALIGN_BOTTOM_RIGHT, -4, -40);
  makeMapBtn(map_canvas, LV_SYMBOL_MINUS, zoom_cb, (void*)(intptr_t)-1, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
  makeMapBtn(map_canvas, LV_SYMBOL_GPS, locate_cb, NULL, LV_ALIGN_BOTTOM_LEFT, BAR_W + 4, -4);
  lv_obj_t* nb = makeMapBtn(map_canvas, LV_SYMBOL_EYE_OPEN, night_cb, NULL, LV_ALIGN_BOTTOM_LEFT, BAR_W + 4, -40);
  night_btn_lbl = lv_obj_get_child(nb, 0);
  lv_obj_t* rptr = makeMapBtn(map_canvas, "Rptr", next_repeater_cb, NULL, LV_ALIGN_TOP_RIGHT, -4, BAR_H + 4);
  lv_obj_set_style_text_font(lv_obj_get_child(rptr, 0), &rc_font_12, 0);
  lv_obj_t* fit = makeMapBtn(map_canvas, "Fit", fit_cb, NULL, LV_ALIGN_TOP_RIGHT, -44, BAR_H + 4);
  lv_obj_set_style_text_font(lv_obj_get_child(fit, 0), &rc_font_12, 0);

  foot_lbl = lv_label_create(map_canvas);
  lv_label_set_text(foot_lbl, "");
  lv_obj_set_style_text_font(foot_lbl, &rc_font_12, 0);
  lv_obj_set_style_text_align(foot_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(foot_lbl, lv_color_hex(0xF1F4F6), 0);
  lv_obj_set_style_bg_color(foot_lbl, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(foot_lbl, LV_OPA_50, 0);
  lv_obj_set_style_radius(foot_lbl, 3, 0);
  lv_obj_set_style_pad_hor(foot_lbl, 3, 0);
  lv_obj_align(foot_lbl, LV_ALIGN_BOTTOM_MID, BAR_W / 2, -6);
  styleToggleBuild();
  mapNightLoad();
  styleToggleShow();
  mapApplyNight();

  mapViewRefresh();
}

// While the map is open the status bar names where you are and shows WiFi,
// which also opens the map sync.
static void topBarBuild(lv_obj_t* bar) {
  top_place = lv_label_create(bar);
  lv_label_set_text(top_place, "");
  lv_label_set_long_mode(top_place, LV_LABEL_LONG_DOT);
  lv_obj_set_size(top_place, TOP_PLACE_W, 16);
  lv_obj_set_style_text_font(top_place, &rc_font_12, 0);
  lv_obj_set_style_text_color(top_place, lv_color_hex(COL_ACCENT), 0);
  lv_obj_align(top_place, LV_ALIGN_LEFT_MID, 2, 1);
  lv_obj_add_flag(top_place, LV_OBJ_FLAG_HIDDEN);

  top_wifi = lv_button_create(bar);
  lv_obj_set_size(top_wifi, TOP_WIFI_W, 18);
  lv_obj_align(top_wifi, LV_ALIGN_LEFT_MID, TOP_PLACE_W + 4, 0);
  lv_obj_set_style_radius(top_wifi, 4, 0);
  lv_obj_set_style_shadow_width(top_wifi, 0, 0);
  lv_obj_set_style_pad_all(top_wifi, 0, 0);
  lv_obj_set_style_bg_color(top_wifi, lv_color_hex(0x101820), 0);
  lv_obj_set_ext_click_area(top_wifi, 3);
  lv_obj_add_event_cb(top_wifi, mapDlOpen, LV_EVENT_CLICKED, NULL);
  top_wifi_lbl = lv_label_create(top_wifi);
  lv_label_set_text(top_wifi_lbl, "");
  lv_obj_center(top_wifi_lbl);
  lv_obj_add_flag(top_wifi, LV_OBJ_FLAG_HIDDEN);
}

static void topBarRefresh() {
  if (top_place == NULL || lv_obj_has_flag(top_place, LV_OBJ_FLAG_HIDDEN)) return;
  char place[PLACE_NAME_MAX + 6];
  double lat, lon;
  if (!mapViewPosition(&lat, &lon)) strlcpy(place, "No location yet", sizeof(place));
  else if (!placeName(lat, lon, place, sizeof(place))) snprintf(place, sizeof(place), "%.3f, %.3f", lat, lon);
  if (strcmp(lv_label_get_text(top_place), place) != 0) lv_label_set_text(top_place, place);

  char state[8], text[20];
  bool on = mapDlWifiStatus(state, sizeof(state));
  snprintf(text, sizeof(text), LV_SYMBOL_WIFI " %s", state);
  if (strcmp(lv_label_get_text(top_wifi_lbl), text) != 0) {
    lv_label_set_text(top_wifi_lbl, text);
    lv_obj_set_style_text_color(top_wifi_lbl, lv_color_hex(on ? COL_ACCENT : COL_MUTED), 0);
  }
}

// The map sits under these objects, so they stop catching touches here and
// let drags through to it. The tabs and sidebar buttons still take theirs.
void mapViewChrome(lv_obj_t* status_bar, lv_obj_t* tabview, lv_obj_t* map_page, lv_obj_t* name_lbl) {
  chrome_status = status_bar;
  chrome_tabview = tabview;
  chrome_name = name_lbl;
  topBarBuild(status_bar);
  lv_obj_t* content = lv_tabview_get_content(tabview);
  lv_obj_remove_flag(tabview, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(map_page, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(map_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(map_page, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(map_page, 0, 0);
}

void mapViewShow(bool on) {
  if (map_canvas == NULL || chrome_tabview == NULL) return;
  if (on) lv_obj_remove_flag(map_canvas, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(map_canvas, LV_OBJ_FLAG_HIDDEN);
  lv_obj_update_flag(top_place, LV_OBJ_FLAG_HIDDEN, !on);
  lv_obj_update_flag(top_wifi, LV_OBJ_FLAG_HIDDEN, !on);
  lv_obj_update_flag(chrome_name, LV_OBJ_FLAG_HIDDEN, on);
  topBarRefresh();
  lv_opa_t bar = on ? BAR_OPA : LV_OPA_COVER;
  lv_obj_set_style_bg_opa(chrome_status, bar, 0);
  lv_obj_set_style_bg_opa(chrome_tabview, on ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  lv_obj_t* side = lv_tabview_get_tab_bar(chrome_tabview);
  lv_obj_set_style_bg_opa(side, bar, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_count(side); i++) {
    lv_obj_set_style_bg_opa(lv_obj_get_child(side, i), bar, LV_STATE_CHECKED);
  }
}

bool mapViewSdOk() { return sd_ok; }
