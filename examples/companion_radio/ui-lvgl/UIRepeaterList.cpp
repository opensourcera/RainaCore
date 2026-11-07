// Contacts > Rptr: repeaters by hops on the left, the chosen one on the right.
//
// Hops come from the last advert heard, which needs no traffic of our own, and
// fall back to the stored route. Nothing here redraws unless a value changed,
// and the list is only looked at every 20 s, so an open screen costs next to no
// battery. The place row uses names saved by a WiFi map sync; without one the
// row is simply left out.

#include "UICommon.h"
#include "MapView.h"
#include "UIPlaces.h"
#include <math.h>
#include <stdio.h>

#define RPT_MAX 40
#define HEARD_MAX 48
#define HOPS_UNKNOWN 255
#define DETAIL_ROWS 4
#define ROW_TICKS 10          // 2 s ticks between looks at the list

struct Row {
  uint8_t key[7];
  uint8_t hops;
  uint32_t heard;
  int idx;
  char name[32];
};
struct Heard {
  uint8_t key[7];
  uint8_t hops;
  uint32_t when;
};

static Row rows[RPT_MAX];
static int row_count = 0;
static Heard heard[HEARD_MAX];
static uint32_t rows_sig = 0;
static uint8_t sel_key[7] = {0};

static lv_obj_t* pane = NULL;
static lv_obj_t* list;
static lv_obj_t* empty_lbl;
static lv_obj_t* card;
static lv_obj_t* title_lbl;
static lv_obj_t* icon_lbl[DETAIL_ROWS];
static lv_obj_t* val_lbl[DETAIL_ROWS];
static lv_obj_t* row_btn[RPT_MAX];

static void setText(lv_obj_t* l, const char* t) {
  if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

// the mesh only remembers its last 16 adverts, so hops are kept here for longer
static void mergeHeard() {
  static AdvertPath recent[16];
  int n = the_mesh.getRecentlyHeard(recent, 16);
  for (int i = 0; i < n; i++) {
    if (recent[i].recv_timestamp == 0) continue;
    int slot = 0;
    for (int h = 0; h < HEARD_MAX; h++) {
      if (memcmp(heard[h].key, recent[i].pubkey_prefix, sizeof(heard[h].key)) == 0) { slot = h; break; }
      if (heard[h].when < heard[slot].when) slot = h;
    }
    memcpy(heard[slot].key, recent[i].pubkey_prefix, sizeof(heard[slot].key));
    heard[slot].hops = recent[i].path_len & 63;
    heard[slot].when = recent[i].recv_timestamp;
  }
}

static const Heard* findHeard(const uint8_t* pub_key) {
  for (int h = 0; h < HEARD_MAX; h++) {
    if (heard[h].when != 0 && memcmp(heard[h].key, pub_key, sizeof(heard[h].key)) == 0) return &heard[h];
  }
  return NULL;
}

static bool rowBefore(const Row& a, const Row& b) {
  if (a.hops != b.hops) return a.hops < b.hops;
  return a.heard > b.heard;
}

static void collectRows() {
  row_count = 0;
  for (int idx = MAX_ANON_CONTACTS; idx < the_mesh.getTotalContactSlots(); idx++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) break;
    if (c.name[0] == 0 || c.type != ADV_TYPE_REPEATER) continue;
    Row r;
    memcpy(r.key, c.id.pub_key, sizeof(r.key));
    const Heard* h = findHeard(c.id.pub_key);
    r.hops = h != NULL ? h->hops : c.out_path_len != OUT_PATH_UNKNOWN ? (c.out_path_len & 63) : HOPS_UNKNOWN;
    r.heard = h != NULL ? h->when : c.last_advert_timestamp;
    r.idx = idx;
    strlcpy(r.name, c.name, sizeof(r.name));
    // a long list keeps its closest repeaters
    if (row_count == RPT_MAX && !rowBefore(r, rows[RPT_MAX - 1])) continue;
    int j = row_count < RPT_MAX ? row_count++ : RPT_MAX - 1;
    while (j > 0 && rowBefore(r, rows[j - 1])) { rows[j] = rows[j - 1]; j--; }
    rows[j] = r;
  }
}

static uint32_t rowsSignature() {
  uint32_t h = 2166136261u + row_count;
  for (int i = 0; i < row_count; i++) {
    for (size_t k = 0; k < sizeof(rows[i].key); k++) h = (h ^ rows[i].key[k]) * 16777619u;
    h = (h ^ rows[i].hops) * 16777619u;
  }
  return h;
}

static void detailRow(int i, int* y, const char* icon, const char* text) {
  if (text == NULL) {
    lv_obj_add_flag(icon_lbl[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(val_lbl[i], LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(icon_lbl[i], LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(val_lbl[i], LV_OBJ_FLAG_HIDDEN);
  setText(icon_lbl[i], icon);
  setText(val_lbl[i], text);
  lv_obj_set_y(icon_lbl[i], *y);
  lv_obj_set_y(val_lbl[i], *y + 1);
  *y += 21;
}

static void showDetail(const Row* r) {
  ContactInfo c;
  if (r == NULL || !the_mesh.getContactByIdx(r->idx, c)) {
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
  setText(title_lbl, c.name);

  char hops[20], when[20], where[32], place[PLACE_NAME_MAX + 6];
  if (r->hops == HOPS_UNKNOWN) strlcpy(hops, "Hops unknown", sizeof(hops));
  else if (r->hops == 0) strlcpy(hops, "Direct, no relay", sizeof(hops));
  else snprintf(hops, sizeof(hops), "%d hop%s", r->hops, r->hops == 1 ? "" : "s");

  if (r->heard == 0) {
    strlcpy(when, "Not heard yet", sizeof(when));
  } else {
    long secs = (long) rtc_clock.getCurrentTime() - (long) r->heard;
    if (secs < 0) secs = 0;
    if (secs < 3600) snprintf(when, sizeof(when), "%ld min ago", secs / 60);
    else if (secs < 86400) snprintf(when, sizeof(when), "%ld h ago", secs / 3600);
    else snprintf(when, sizeof(when), "%ld day%s ago", secs / 86400, secs < 172800 ? "" : "s");
  }

  bool has_pos = c.gps_lat != 0 || c.gps_lon != 0;
  double clat = c.gps_lat / 1000000.0, clon = c.gps_lon / 1000000.0;
  double mlat, mlon;
  if (has_pos && mapViewPosition(&mlat, &mlon)) {
    double dlat = (clat - mlat) * M_PI / 180.0, dlon = (clon - mlon) * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(mlat * M_PI / 180.0) * cos(clat * M_PI / 180.0) * sin(dlon / 2) * sin(dlon / 2);
    double km = 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1 - a));
    double y = sin(dlon) * cos(clat * M_PI / 180.0);
    double x = cos(mlat * M_PI / 180.0) * sin(clat * M_PI / 180.0) -
               sin(mlat * M_PI / 180.0) * cos(clat * M_PI / 180.0) * cos(dlon);
    int brg = ((int)(atan2(y, x) * 180.0 / M_PI) + 360) % 360;
    static const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    snprintf(where, sizeof(where), "%.1f %s %s", units_miles ? km * 0.621371 : km,
             units_miles ? "mi" : "km", dirs[((brg + 22) / 45) % 8]);
  } else if (has_pos) {
    snprintf(where, sizeof(where), "%.3f, %.3f", clat, clon);
  }
  bool named = has_pos && placeName(clat, clon, place, sizeof(place));

  int y = 24;
  detailRow(0, &y, LV_SYMBOL_SHUFFLE, hops);
  detailRow(1, &y, LV_SYMBOL_REFRESH, when);
  detailRow(2, &y, LV_SYMBOL_GPS, has_pos ? where : NULL);
  detailRow(3, &y, LV_SYMBOL_HOME, named ? place : NULL);
}

static const Row* selectedRow() {
  for (int i = 0; i < row_count; i++) {
    if (memcmp(rows[i].key, sel_key, sizeof(sel_key)) == 0) return &rows[i];
  }
  return row_count > 0 ? &rows[0] : NULL;
}

static void markSelected() {
  const Row* s = selectedRow();
  for (int i = 0; i < row_count; i++) {
    lv_obj_set_style_bg_color(row_btn[i], lv_color_hex(&rows[i] == s ? COL_ACCENT_D : COL_CARD), 0);
  }
}

static void row_cb(lv_event_t* e) {
  int i = (int)(intptr_t) lv_event_get_user_data(e);
  if (i >= row_count) return;
  memcpy(sel_key, rows[i].key, sizeof(sel_key));
  markSelected();
  showDetail(selectedRow());
}

static void row_long_cb(lv_event_t* e) {
  int i = (int)(intptr_t) lv_event_get_user_data(e);
  if (i < row_count) ui->openContactDetail(rows[i].idx);
}

static void manage_cb(lv_event_t* e) {
  const Row* r = selectedRow();
  ContactInfo c;
  if (r != NULL && the_mesh.getContactByIdx(r->idx, c)) ui->openRepeaterManager(c);
}

static void buildRows() {
  lv_obj_clean(list);
  for (int i = 0; i < row_count; i++) {
    lv_obj_t* b = lv_button_create(list);
    lv_obj_set_size(b, LV_PCT(100), 28);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_add_event_cb(b, row_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t) i);
    lv_obj_add_event_cb(b, row_long_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t) i);
    lv_obj_t* name = lv_label_create(b);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, 84, 16);
    lv_obj_set_style_text_font(name, &rc_font_12, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(COL_TXT), 0);
    lv_label_set_text(name, rows[i].name);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 1);
    lv_obj_t* hops = lv_label_create(b);
    lv_obj_set_style_text_font(hops, &rc_font_12, 0);
    lv_obj_set_style_text_color(hops, lv_color_hex(rows[i].hops == 0 ? COL_ACCENT : COL_MUTED), 0);
    if (rows[i].hops == HOPS_UNKNOWN) lv_label_set_text(hops, "-");
    else lv_label_set_text_fmt(hops, "%d", rows[i].hops);
    lv_obj_align(hops, LV_ALIGN_RIGHT_MID, 0, 1);
    row_btn[i] = b;
  }
  lv_obj_update_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN, row_count > 0);
}

void rptRefresh(bool force) {
  if (pane == NULL || lv_obj_has_flag(pane, LV_OBJ_FLAG_HIDDEN)) return;
  if (force) mergeHeard();
  collectRows();
  uint32_t sig = rowsSignature();
  if (force || sig != rows_sig) {
    rows_sig = sig;
    buildRows();
  }
  markSelected();
  showDetail(selectedRow());
}

void rptShow(bool on) {
  if (pane != NULL) lv_obj_update_flag(pane, LV_OBJ_FLAG_HIDDEN, !on);
}

void rptTick(bool tab_open) {
  static uint8_t div = 0;
  if (++div < ROW_TICKS) return;
  div = 0;
  mergeHeard();
  if (tab_open) rptRefresh(false);
}

int rptPlacesNeeded(double out[][2], int max) {
  int n = 0;
  for (int idx = MAX_ANON_CONTACTS; idx < the_mesh.getTotalContactSlots() && n < max; idx++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) break;
    if (c.name[0] == 0 || c.type != ADV_TYPE_REPEATER || (c.gps_lat == 0 && c.gps_lon == 0)) continue;
    double lat = c.gps_lat / 1000000.0, lon = c.gps_lon / 1000000.0;
    char name[PLACE_NAME_MAX + 6];
    if (placeName(lat, lon, name, sizeof(name))) continue;
    bool dup = false;
    for (int i = 0; i < n; i++) {
      if (fabs(out[i][0] - lat) < 0.15 && fabs(out[i][1] - lon) < 0.15) dup = true;
    }
    if (dup) continue;
    out[n][0] = lat;
    out[n][1] = lon;
    n++;
  }
  return n;
}

void rptBuild(lv_obj_t* parent) {
  pane = lv_obj_create(parent);
  lv_obj_set_width(pane, LV_PCT(100));
  lv_obj_set_flex_grow(pane, 1);
  lv_obj_set_style_bg_opa(pane, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pane, 0, 0);
  lv_obj_set_style_pad_all(pane, 0, 0);
  lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pane, LV_OBJ_FLAG_HIDDEN);

  list = lv_obj_create(pane);
  lv_obj_set_size(list, 110, LV_PCT(100));
  lv_obj_align(list, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_pad_row(list, 3, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

  empty_lbl = lv_label_create(pane);
  lv_label_set_text(empty_lbl, "No repeaters heard yet");
  lv_obj_set_style_text_font(empty_lbl, &rc_font_12, 0);
  lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COL_MUTED), 0);
  lv_obj_center(empty_lbl);
  lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);

  card = lv_obj_create(pane);
  lv_obj_set_size(card, 152, LV_PCT(100));
  lv_obj_align(card, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  title_lbl = lv_label_create(card);
  lv_label_set_text(title_lbl, "");
  lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_size(title_lbl, 136, 18);
  lv_obj_set_style_text_font(title_lbl, &rc_font_14, 0);
  lv_obj_set_style_text_color(title_lbl, lv_color_hex(COL_ACCENT), 0);

  for (int i = 0; i < DETAIL_ROWS; i++) {
    icon_lbl[i] = lv_label_create(card);
    lv_label_set_text(icon_lbl[i], "");
    lv_obj_set_style_text_color(icon_lbl[i], lv_color_hex(COL_MUTED), 0);
    lv_obj_set_x(icon_lbl[i], 0);
    val_lbl[i] = lv_label_create(card);
    lv_label_set_text(val_lbl[i], "");
    lv_label_set_long_mode(val_lbl[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(val_lbl[i], 114, 16);
    lv_obj_set_style_text_font(val_lbl[i], &rc_font_12, 0);
    lv_obj_set_style_text_color(val_lbl[i], lv_color_hex(COL_TXT), 0);
    lv_obj_set_x(val_lbl[i], 22);
  }

  lv_obj_t* b = lv_button_create(card);
  lv_obj_set_size(b, 136, 28);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, manage_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, "Manage");
  lv_obj_set_style_text_font(l, &rc_font_12, 0);
  lv_obj_center(l);
}
