// Place names for the map's top bar.
//
// The device has no map of names to search, so a WiFi map sync asks
// OpenStreetMap's Nominatim for the name of the area it downloads and keeps it
// here. Away from every saved area the bar falls back to coordinates. Names are
// kept in SPIFFS, which a firmware update leaves alone.

#include "UIPlaces.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef MESHRAIN_PC_TEST
  #include <SPIFFS.h>
#endif

#define MAX_PLACES 32
#define PLACE_SAME_KM 1.0
#define PLACE_HERE_KM 15.0
#define PLACE_NEAR_KM 60.0

struct Place { double lat, lon; char name[PLACE_NAME_MAX]; };
static Place places[MAX_PLACES];
static int place_count = 0;
static bool loaded = false;

// copies a JSON string value, leaving out anything it cannot show
static bool jsonValue(const char* from, const char* until, const char* key, char* out, size_t sz) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char* p = strstr(from, pat);
  if (p == NULL || (until != NULL && p >= until)) return false;
  p += strlen(pat);
  size_t n = 0;
  while (*p != 0 && *p != '"' && n + 1 < sz) {
    if (*p == '\\') {
      p++;
      if (*p == 0 || *p == 'u') return false;
    }
    out[n++] = *p++;
  }
  if (*p != '"') {   // cut short: drop a split UTF-8 character
    while (n > 0 && ((unsigned char) out[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0 && ((unsigned char) out[n - 1] & 0xC0) == 0xC0) n--;
  }
  out[n] = 0;
  return n > 0;
}

bool placeParseReply(const char* json, char* out, size_t out_sz) {
  const char* addr = strstr(json, "\"address\":{");
  if (addr == NULL) return false;
  const char* end = strchr(addr, '}');
  if (end == NULL) return false;

  static const char* LOCAL[] = {"city", "town", "village", "hamlet", "municipality", "suburb", "county"};
  char local[PLACE_NAME_MAX] = "", region[PLACE_NAME_MAX] = "";
  for (const char* k : LOCAL) {
    if (jsonValue(addr, end, k, local, sizeof(local))) break;
  }

  char code[8] = "", iso[12] = "";
  jsonValue(addr, end, "country_code", code, sizeof(code));
  if ((strcmp(code, "us") == 0 || strcmp(code, "ca") == 0 || strcmp(code, "au") == 0) &&
      jsonValue(addr, end, "ISO3166-2-lvl4", iso, sizeof(iso)) && strchr(iso, '-') != NULL) {
    strncpy(region, strchr(iso, '-') + 1, sizeof(region) - 1);   // TX, not Texas: the bar is short
  } else if (!jsonValue(addr, end, "state", region, sizeof(region))) {
    jsonValue(addr, end, "country", region, sizeof(region));
  }

  if (local[0] && region[0]) snprintf(out, out_sz, "%s, %s", local, region);
  else if (local[0]) snprintf(out, out_sz, "%s", local);
  else if (region[0]) snprintf(out, out_sz, "%s", region);
  else return false;
  return true;
}

static double distanceKm(double lat1, double lon1, double lat2, double lon2) {
  double dy = (lat2 - lat1) * 111.2;
  double dlon = fabs(lon2 - lon1);
  if (dlon > 180.0) dlon = 360.0 - dlon;   // either side of the date line
  double dx = dlon * 111.2 * cos((lat1 + lat2) / 2.0 * M_PI / 180.0);
  return sqrt(dx * dx + dy * dy);
}

static void placesLoad() {
  if (loaded) return;
  loaded = true;
#ifndef MESHRAIN_PC_TEST
  File f = SPIFFS.open("/places", "r");
  if (!f) return;
  while (f.available() && place_count < MAX_PLACES) {
    String line = f.readStringUntil('\n');
    Place p;
    int used = 0;
    if (sscanf(line.c_str(), "%lf,%lf,%n", &p.lat, &p.lon, &used) != 2 || used == 0) continue;
    strncpy(p.name, line.c_str() + used, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = 0;
    if (p.name[0] != 0) places[place_count++] = p;
  }
  f.close();
#endif
}

static void placesSave() {
#ifndef MESHRAIN_PC_TEST
  File f = SPIFFS.open("/places", "w");
  if (!f) return;
  for (int i = 0; i < place_count; i++) {
    f.printf("%.4f,%.4f,%s\n", places[i].lat, places[i].lon, places[i].name);
  }
  f.close();
#endif
}

void placeAdd(double lat, double lon, const char* name) {
  placesLoad();
  int slot = -1;
  for (int i = 0; i < place_count; i++) {
    if (distanceKm(lat, lon, places[i].lat, places[i].lon) <= PLACE_SAME_KM) slot = i;
  }
  if (slot < 0) {
    if (place_count == MAX_PLACES) {   // oldest goes
      memmove(&places[0], &places[1], sizeof(Place) * (MAX_PLACES - 1));
      place_count--;
    }
    slot = place_count++;
  }
  places[slot].lat = lat;
  places[slot].lon = lon;
  strncpy(places[slot].name, name, sizeof(places[slot].name) - 1);
  places[slot].name[sizeof(places[slot].name) - 1] = 0;
  placesSave();
}

bool placeName(double lat, double lon, char* out, size_t out_sz) {
  placesLoad();
  int best = -1;
  double best_km = PLACE_NEAR_KM;
  for (int i = 0; i < place_count; i++) {
    double km = distanceKm(lat, lon, places[i].lat, places[i].lon);
    if (km <= best_km) { best_km = km; best = i; }
  }
  if (best < 0) return false;
  snprintf(out, out_sz, "%s%s", best_km <= PLACE_HERE_KM ? "" : "Near ", places[best].name);
  return true;
}
