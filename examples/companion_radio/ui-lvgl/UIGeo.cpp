// Timezone and radio region from the GPS position, with no internet.
//
// This is a coarse offline lookup. North America and Europe use approximate
// zone boundaries and their daylight saving rules. Everywhere else falls back
// to longitude / 15, which can be an hour out and knows no daylight saving.
// Places near a boundary can be wrong, so manual +/- always stays available.

#include "UICommon.h"
#include <helpers/sensors/LocationProvider.h>
#include <math.h>
#include <time.h>

static bool tz_auto = true;
static char tz_name[32] = "";

// days since 1970-01-01 for a civil date
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t) doe - 719468;
}

static int weekday(int32_t days) { return (int)(((days + 4) % 7 + 7) % 7); }   // 0 = Sunday

// day number of the nth Sunday of a month, counting from 1
static int32_t nthSunday(int year, unsigned month, int n) {
  int32_t first = daysFromCivil(year, month, 1);
  return first + (7 - weekday(first)) % 7 + (n - 1) * 7;
}

static int32_t lastSunday(int year, unsigned month) {
  int32_t next = month == 12 ? daysFromCivil(year + 1, 1, 1) : daysFromCivil(year, month + 1, 1);
  int32_t last = next - 1;
  return last - weekday(last);
}

static bool usDaylight(uint32_t utc, int std_hours) {
  time_t t = utc;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  int year = tmv.tm_year + 1900;
  int64_t start = (int64_t) nthSunday(year, 3, 2) * 86400 + (2 - std_hours) * 3600;
  int64_t end = (int64_t) nthSunday(year, 11, 1) * 86400 + (2 - (std_hours + 1)) * 3600;
  return utc >= start && utc < end;
}

static bool euDaylight(uint32_t utc) {
  time_t t = utc;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  int year = tmv.tm_year + 1900;
  int64_t start = (int64_t) lastSunday(year, 3) * 86400 + 3600;
  int64_t end = (int64_t) lastSunday(year, 10) * 86400 + 3600;
  return utc >= start && utc < end;
}

static void setZone(GeoGuess* g, int std_hours, bool dst, const char* name) {
  g->tz_hours = std_hours + (dst ? 1 : 0);
  snprintf(g->tz_name, sizeof(g->tz_name), "%s", name);
}

void geoGuess(double lat, double lon, uint32_t utc, GeoGuess* g) {
  g->preset = -1;

  if (lat >= 18 && lat <= 23 && lon >= -161 && lon <= -154) {
    g->preset = 0;
    setZone(g, -10, false, "Hawaii");
  } else if (lat >= 51 && lon >= -170 && lon < -130) {
    g->preset = 0;
    setZone(g, -9, usDaylight(utc, -9), "Alaska");
  } else if (lat >= 24 && lat <= 72 && lon >= -141 && lon <= -52) {
    g->preset = 0;
    if (lat >= 31.3 && lat <= 37 && lon >= -115 && lon <= -109) setZone(g, -7, false, "Arizona");
    else if (lon < -114.5) setZone(g, -8, usDaylight(utc, -8), "Pacific");
    else if (lon < -101.5) setZone(g, -7, usDaylight(utc, -7), "Mountain");
    else if (lon < -85.5) setZone(g, -6, usDaylight(utc, -6), "Central");
    else setZone(g, -5, usDaylight(utc, -5), "Eastern");
  } else if (lat >= 36 && lat <= 71 && lon >= -11 && lon <= 40) {
    g->preset = 2;
    bool dst = euDaylight(utc);
    if ((lon < 2 && lat > 49.5) || (lon < -6 && lat < 43)) setZone(g, 0, dst, "UK, Ireland, Portugal");
    else if (lon < 22) setZone(g, 1, dst, "Central Europe");
    else setZone(g, 2, dst, "Eastern Europe");
  } else {
    if (lat >= -48 && lat <= -10 && lon >= 112 && lon <= 180) g->preset = 3;
    int h = (int) lround(lon / 15.0);
    if (h < -12) h = -12;
    if (h > 14) h = 14;
    setZone(g, h, false, "estimated");
  }
}

bool geoFromGps(GeoGuess* g) {
  SensorManager* s = ui != NULL ? ui->sensors() : NULL;
  LocationProvider* loc = s != NULL ? s->getLocationProvider() : NULL;
  if (loc == NULL || !loc->isValid()) return false;
  uint32_t utc = rtc_clock.getCurrentTime();
  if (utc < 1600000000UL) utc = (uint32_t) loc->getTimestamp();
  if (utc < 1600000000UL) return false;
  geoGuess(loc->getLatitude() / 1000000.0, loc->getLongitude() / 1000000.0, utc, g);
  return true;
}

void geoLoad() {
  tz_auto = prefReadInt("/tz_auto", 1) == 1;
}

bool geoTzAuto() { return tz_auto; }

void geoSetTzAuto(bool on) {
  tz_auto = on;
  prefWriteInt("/tz_auto", on ? 1 : 0);
  if (!on) tz_name[0] = 0;
  geoTick();
}

const char* geoTzName() { return tz_name; }

// keeps the offset right, including when daylight saving changes
void geoTick() {
  if (!tz_auto) return;
  GeoGuess g;
  if (!geoFromGps(&g)) return;
  snprintf(tz_name, sizeof(tz_name), "%s", g.tz_name);
  if (g.tz_hours != settingsTzOffset()) settingsSetTzOffset(g.tz_hours);
}
