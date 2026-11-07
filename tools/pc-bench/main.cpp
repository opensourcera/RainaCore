// Drives the real MapView.cpp on a PC: real LVGL, lodepng and tiles from a card folder.
#include <stdlib.h>
#include <stdio.h>
#include "UICommon.h"
#include "MapView.h"
#include <vector>
#include <SD_MMC.h>
#include "UIPlaces.h"
RtcStub rtc_clock; UITask* ui; Mesh the_mesh; SpiffsFS SPIFFS; SdFS SD_MMC; bool units_miles = false;
static unsigned long now_ms = 0;
unsigned long millis() { return now_ms; }
static int dl_opens = 0; static const char* wifi_txt = "Off"; static bool wifi_on = false;
void mapDlOpen(lv_event_t*) { dl_opens++; }
bool mapDlWifiStatus(char* o, size_t n) { strlcpy(o, wifi_txt, n); return wifi_on; }
static int opened = -1;
void UITask::openContactDetail(int i) { opened = i; printf("OPEN %s\n", the_mesh.c[i].name); }
void UITask::showToast(const char* t) { printf("TOAST %s\n", t); }
static char managed[32] = "";
void UITask::openRepeaterManager(const ContactInfo& c) { strcpy(managed, c.name); }
void benchState(int* z, double* fx, double* fy);
int benchClusters(int* total);
int benchFindCircle(int* x, int* y);
static uint16_t fb[320 * 240];
static uint8_t db[320 * 240 * 2];
static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  int w = lv_area_get_width(a); uint16_t* p = (uint16_t*) px;
  for (int y = a->y1; y <= a->y2; y++) { memcpy(&fb[y * 320 + a->x1], p, w * 2); p += w; }
  lv_display_flush_ready(d);
}
static int tx, ty, pressed;
static void rd(lv_indev_t*, lv_indev_data_t* d) { d->point.x = tx; d->point.y = ty; d->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static void pump(int ms = 120) { for (int i = 0; i < ms / 10; i++) { lv_tick_inc(10); now_ms += 10; lv_timer_handler(); } }
static void shot(const char* n) { pump(); lv_refr_now(NULL); char p[80]; snprintf(p, 80, "%s.rgb", n); FILE* f = fopen(p, "wb"); fwrite(fb, 1, sizeof(fb), f); fclose(f); }
static void tap(int x, int y) { tx = x; ty = y; pressed = 1; pump(100); pressed = 0; pump(100); }
static void drag(int x0, int y0, int x1, int y1) { tx = x0; ty = y0; pressed = 1; pump(60);
  for (int i = 1; i <= 8; i++) { tx = x0 + (x1 - x0) * i / 8; ty = y0 + (y1 - y0) * i / 8; pump(30); } pressed = 0; pump(60); }
static int pass_n = 0, fail_n = 0;
static void chk(bool ok, const char* w) { printf("%s %s\n", ok ? "PASS" : "FAIL", w); if (ok) pass_n++; else fail_n++; }
static void add(const char* n, int t, double la, double lo) { ContactInfo c; memset(&c, 0, sizeof c); strncpy(c.name, n, 31); c.type = t; c.out_path_len = OUT_PATH_UNKNOWN; c.id.pub_key[0] = (uint8_t) the_mesh.c.size() + 1; c.gps_lat = (int32_t)(la * 1e6); c.gps_lon = (int32_t)(lo * 1e6); the_mesh.c.push_back(c); }
static lv_obj_t* tabview;
static void changed(lv_event_t*) { mapViewShow(lv_tabview_get_tab_active(tabview) == 2); mapViewRefresh(); }

int main() {
  lv_init();
  lv_display_t* disp = lv_display_create(320, 240);
  lv_display_set_flush_cb(disp, flush);
  lv_display_set_buffers(disp, db, NULL, sizeof db, LV_DISPLAY_RENDER_MODE_FULL);
  lv_indev_t* in = lv_indev_create(); lv_indev_set_type(in, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(in, rd);
  lv_theme_t* th = lv_theme_default_init(disp, lv_color_hex(COL_ACCENT), lv_color_hex(0xFFB454), true, &fa_icons_14);
  lv_display_set_theme(disp, th);

  // a downtown group, a pair to the north, spread repeaters, one far town
  add("Mission Peak Rptr", 2, 37.7790, -122.4190); add("Twin Peaks", 2, 37.7545, -122.4476);
  add("Bernal Hill", 2, 37.7437, -122.4144); add("Sutro Tower Repeater North", 2, 37.7552, -122.4528);
  add("Alex", 1, 37.7750, -122.4170); add("Sam", 1, 37.7755, -122.4160); add("Jo", 1, 37.7746, -122.4182);
  add("Rio", 1, 37.7741, -122.4155); add("Mo", 1, 37.7760, -122.4176); add("Kai \xF0\x9F\x9A\x80", 1, 37.7735, -122.4190);
  add("Pat", 1, 37.8000, -122.4100); add("Lee", 1, 37.8010, -122.4080);
  add("Far Town", 1, 37.9000, -122.2000);
  UITask task; ui = &task; task.np.node_lat = 37.771; task.np.node_lon = -122.417;
  SensorManager sensors;

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_text_color(scr, lv_color_hex(COL_TXT), 0);
  // same construction as UITask::buildShell
  lv_obj_t* sb = lv_obj_create(scr);
  lv_obj_set_size(sb, 320, 22); lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(sb, lv_color_hex(COL_CARD), 0); lv_obj_set_style_border_width(sb, 0, 0);
  lv_obj_set_style_radius(sb, 0, 0); lv_obj_set_style_pad_all(sb, 2, 0); lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* nm = lv_label_create(sb); lv_label_set_text(nm, "12:45 pm"); lv_obj_set_style_text_color(nm, lv_color_hex(COL_MUTED), 0);
  lv_obj_set_style_pad_left(nm, rc_logo.header.w + 8, 0); lv_obj_set_style_min_height(nm, rc_logo.header.h, 0); lv_obj_align(nm, LV_ALIGN_LEFT_MID, 2, 0);
  { lv_obj_t* logo = lv_image_create(nm); lv_image_set_src(logo, &rc_logo); lv_obj_set_pos(logo, -(int) rc_logo.header.w - 8, 0); }
  lv_obj_t* rt = lv_label_create(sb); lv_label_set_text(rt, LV_SYMBOL_BLUETOOTH " 12:45p  " LV_SYMBOL_BATTERY_FULL " 100%"); lv_obj_align(rt, LV_ALIGN_RIGHT_MID, -4, 0);
  tabview = lv_tabview_create(scr);
  lv_obj_set_size(tabview, 320, 240 - 22); lv_obj_align(tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_tabview_set_tab_bar_position(tabview, LV_DIR_LEFT); lv_tabview_set_tab_bar_size(tabview, 46);
  lv_obj_set_style_bg_color(tabview, lv_color_hex(COL_BG), 0);
  lv_obj_t* t0 = lv_tabview_add_tab(tabview, LV_SYMBOL_ENVELOPE); lv_obj_t* t1 = lv_tabview_add_tab(tabview, LV_SYMBOL_LIST);
  lv_obj_t* tm = lv_tabview_add_tab(tabview, LV_SYMBOL_GPS); lv_tabview_add_tab(tabview, LV_SYMBOL_LOOP); lv_tabview_add_tab(tabview, LV_SYMBOL_SETTINGS);
  lv_obj_t* side = lv_tabview_get_tab_bar(tabview);
  lv_obj_set_style_bg_color(side, lv_color_hex(COL_CARD), 0); lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0); lv_obj_set_style_pad_all(side, 0, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_count(side); i++) { lv_obj_t* b = lv_obj_get_child(side, i);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_set_style_text_color(b, lv_color_hex(COL_MUTED), 0); lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED); lv_obj_set_style_bg_color(b, lv_color_hex(COL_BG), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_ACCENT), LV_STATE_CHECKED); lv_obj_set_style_border_color(b, lv_color_hex(COL_ACCENT), LV_STATE_CHECKED);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED); lv_obj_set_style_border_width(b, 3, LV_STATE_CHECKED); }
  lv_obj_add_event_cb(tabview, changed, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_remove_flag(lv_tabview_get_content(tabview), LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* l = lv_label_create(t0); lv_label_set_text(l, "Chats tab content");
  lv_obj_set_flex_flow(t1, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_all(t1, 4, 0); lv_obj_set_style_pad_row(t1, 4, 0);
  { lv_obj_t* ch = lv_obj_create(t1); lv_obj_set_size(ch, LV_PCT(100), 28); }
  rptBuild(t1);
  mapViewBuild(scr, &sensors, &task);
  mapViewChrome(sb, tabview, tm, nm);

  int z, z2, z3, total, cx, cy, n; double fx, fy, gx, gy;
  { // the real reply from Nominatim, and shapes it can take
    char o[PLACE_NAME_MAX];
    const char* sf = "{\"place_id\":323430119,\"licence\":\"Data \xC2\xA9 OSM\",\"addresstype\":\"city\",\"name\":\"San Francisco\",\"display_name\":\"San Francisco, California, United States\",\"address\":{\"city\":\"San Francisco\",\"state\":\"California\",\"ISO3166-2-lvl4\":\"US-CA\",\"country\":\"United States\",\"country_code\":\"us\"},\"boundingbox\":[\"37.64\"]}";
    chk(placeParseReply(sf, o, sizeof o) && !strcmp(o, "San Francisco, CA"), "US reply gives city, state code"); puts(o);
    chk(placeParseReply("{\"address\":{\"town\":\"S\xC3\xA8te\",\"state\":\"Occitania\",\"ISO3166-2-lvl4\":\"FR-OCC\",\"country_code\":\"fr\"}}", o, sizeof o) && !strcmp(o, "S\xC3\xA8te, Occitania"), "other countries keep the state name"); puts(o);
    chk(placeParseReply("{\"address\":{\"country\":\"Iceland\",\"country_code\":\"is\"}}", o, sizeof o) && !strcmp(o, "Iceland"), "country alone");
    chk(!placeParseReply("{\"error\":\"Unable to geocode\"}", o, sizeof o), "open ocean gives no name");
    chk(!placeParseReply("{\"name\":\"x\",\"state\":\"y\"}", o, sizeof o), "no address block gives no name");
    char tiny[8]; placeParseReply("{\"address\":{\"city\":\"Caf\xC3\xA9\xC3\xA9\xC3\xA9\"}}", tiny, sizeof tiny); chk(strlen(tiny) < 8, "long name stays in its buffer");
    chk(!placeName(37.771, -122.417, o, sizeof o), "no saved places, no name");
    shot("s0_chats_before");
    placeAdd(37.8, -122.4, "San Francisco, CA");
    chk(placeName(37.771, -122.417, o, sizeof o) && !strcmp(o, "San Francisco, CA"), "inside the synced area");
    chk(placeName(38.1, -122.4, o, sizeof o) && !strcmp(o, "Near San Francisco, CA"), "33 km away says near");
    chk(!placeName(40.0, -122.4, o, sizeof o), "far away gives no name");
    placeAdd(37.8001, -122.4001, "Renamed"); placeName(37.8, -122.4, o, sizeof o); chk(!strcmp(o, "Renamed"), "same spot replaces its name");
    placeAdd(37.8, -122.4, "San Francisco, CA");
    for (int i = 0; i < 40; i++) { char nm2[16]; snprintf(nm2, 16, "P%d", i); placeAdd(10 + i, 10, nm2); }
    chk(!placeName(37.8, -122.4, o, sizeof o), "oldest of a full list goes");
    chk(placeName(49, 10, o, sizeof o) && !strcmp(o, "P39"), "newest stays");
    placeAdd(37.8, -122.4, "San Francisco, CA");
  }
  shot("s1_chats");
  tap(23, 22 + 109);  // Map: third of five in the sidebar
  benchState(&z, &fx, &fy);
  shot("s2_map");
  { // repeater pane. Mission Peak has a stored 3 hop route, Twin Peaks was heard direct,
    // Bernal Hill heard over 2, and Sutro is unknown.
    the_mesh.c[0].out_path_len = 3; the_mesh.c[0].last_advert_timestamp = 1800000000u - 7200;
    AdvertPath a; memset(&a, 0, sizeof a); a.pubkey_prefix[0] = 2; a.recv_timestamp = 1800000000u - 300; a.path_len = 0; the_mesh.h.push_back(a);
    a.pubkey_prefix[0] = 3; a.recv_timestamp = 1800000000u - 90000; a.path_len = 2 | 0x40; the_mesh.h.push_back(a);
    tap(23, 22 + 66); rptShow(true); rptRefresh(true); pump(); shot("s23_rptr");
    lv_obj_t* pane = lv_obj_get_child(t1, 1); lv_obj_t* lst = lv_obj_get_child(pane, 0);
    auto rowName = [&](int i) { return lv_label_get_text(lv_obj_get_child(lv_obj_get_child(lst, i), 0)); };
    auto rowHops = [&](int i) { return lv_label_get_text(lv_obj_get_child(lv_obj_get_child(lst, i), 1)); };
    chk(lv_obj_get_child_count(lst) == 4, "only repeaters are listed");
    chk(!strcmp(rowName(0), "Twin Peaks") && !strcmp(rowHops(0), "0"), "direct repeater is first");
    chk(!strcmp(rowName(1), "Bernal Hill") && !strcmp(rowHops(1), "2"), "hop count ignores the hash size bits");
    chk(!strncmp(rowName(2), "Mission", 7) && !strcmp(rowHops(2), "3"), "stored route is the fallback");
    chk(!strcmp(rowHops(3), "-"), "unknown hops sort last");
    lv_area_t ar; lv_obj_get_coords(lv_obj_get_child(lst, 1), &ar);
    tap((ar.x1 + ar.x2) / 2, (ar.y1 + ar.y2) / 2); shot("s24_rptr_second");
    lv_obj_t* card = lv_obj_get_child(pane, 2);
    chk(!strcmp(lv_label_get_text(lv_obj_get_child(card, 0)), "Bernal Hill"), "tapping a row shows it on the right");
    int hid = 0; for (int i = 0; i < 8; i++) if (lv_obj_has_flag(lv_obj_get_child(card, 1 + i), LV_OBJ_FLAG_HIDDEN)) hid++;
    chk(hid == 0, "synced area gives a place row");
    lv_obj_t* man = lv_obj_get_child(card, 9); lv_obj_get_coords(man, &ar);
    tap((ar.x1 + ar.x2) / 2, (ar.y1 + ar.y2) / 2); chk(!strcmp(managed, "Bernal Hill"), "Manage opens the selected repeater");
    lv_obj_t* first = lv_obj_get_child(lst, 0); rptRefresh(false); chk(lv_obj_get_child(lst, 0) == first, "no change, no rebuild");
    double need[12][2]; chk(rptPlacesNeeded(need, 12) == 0, "named area needs no lookup");
    the_mesh.c[3].gps_lat = 10000000; the_mesh.c[3].gps_lon = 170000000;
    rptRefresh(true); chk(rptPlacesNeeded(need, 12) == 1, "unnamed repeater is looked up");
    the_mesh.c[3].gps_lat = 37755200; the_mesh.c[3].gps_lon = -122452800;
    the_mesh.c[0].gps_lat = 0; the_mesh.c[0].gps_lon = 0; rptRefresh(true); shot("s25_rptr_nopos"); the_mesh.c[0].gps_lat = 37779000; the_mesh.c[0].gps_lon = -122419000;
    rptShow(false); tap(23, 22 + 109); }
  chk(lv_obj_has_flag(nm, LV_OBJ_FLAG_HIDDEN), "map bar replaces the node name");
  { int was = dl_opens; tap(170, 11); chk(dl_opens == was + 1, "tapping WiFi in the bar opens sync");
    wifi_txt = "100%"; wifi_on = true; mapViewRefresh(); shot("s21_syncing"); wifi_txt = "Off"; wifi_on = false; mapViewRefresh();
    benchState(&z2, &gx, &gy); chk(gx == fx && gy == fy, "tapping WiFi does not move the map");
    tap(23, 22 + 22 + 44); chk(!lv_obj_has_flag(nm, LV_OBJ_FLAG_HIDDEN), "leaving the map brings the name back");
    was = dl_opens; tap(170, 11); chk(dl_opens == was, "WiFi chip is gone on other tabs"); shot("s22_other_tab");
    tap(23, 22 + 109); }
  { int m = benchClusters(&total); printf("z%d markers %d nodes %d\n", z, m, total); }
  drag(200, 140, 170, 120); benchState(&z, &gx, &gy);
  chk(gx > fx && gy > fy, "drag through the tab layers pans the map");
  shot("s3_panned");
  tap(296, 222); benchState(&z2, &fx, &fy); chk(z2 == z - 1, "minus button works through the layers");
  shot("s4_zoom_out");
  { int m = benchClusters(&total); printf("z%d markers %d nodes %d\n", z2, m, total); }
  tap(296, 186); tap(296, 186); benchState(&z2, &fx, &fy); chk(z2 == z + 1, "plus button"); shot("s5_zoom_in");
  { int m = benchClusters(&total); printf("z%d markers %d nodes %d\n", z2, m, total); }
  tap(296, 222); tap(296, 222); tap(296, 222); benchState(&z2, &fx, &fy); shot("s6_wide");
  { int m = benchClusters(&total); printf("z%d markers %d nodes %d\n", z2, m, total); }
  n = benchFindCircle(&cx, &cy); printf("circle n=%d at %d,%d\n", n, cx, cy);
  chk(n > 1, "zoomed out shows a count circle");
  tap(cx, cy); benchState(&z3, &fx, &fy); printf("tap circle z%d->z%d\n", z2, z3);
  chk(z3 > z2, "tapping the circle zooms in");
  chk(opened < 0, "tapping a circle does not open a contact");
  shot("s7_after_circle");
  { int m = benchClusters(&total); printf("z%d markers %d nodes %d\n", z3, m, total); }
  for (int g = 0; g < 6 && (n = benchFindCircle(&cx, &cy)) > 1; g++) { tap(cx, cy); char s[20]; snprintf(s, 20, "s%d_tap", 8 + g); shot(s); }
  benchState(&z3, &fx, &fy); printf("final z%d, biggest group %d\n", z3, benchFindCircle(&cx, &cy));
  chk(opened < 0, "still no contact opened by circles");
  tap(23, 22 + 22 + 44); shot("s14_tab_contacts");   // leave the map
  tap(23, 22 + 109); shot("s15_back");
  benchState(&z, &fx, &fy);
  tap(73, 38); shot("s161_terrain");
  tap(144, 38); shot("s162_flat");
  tap(112, 38); shot("s163_map");
  benchState(&z2, &gx, &gy);
  chk(z == z2 && fx == gx && fy == gy, "style toggle keeps the view");
  chk(opened < 0, "style toggle opens nothing");
  // out past the tiles on the card: the radar view
  for (int i = 0; i < 9; i++) tap(296, 222);
  benchState(&z2, &fx, &fy); shot("s16_radar"); printf("radar z%d\n", z2); chk(z2 == 7, "zooming out past the tiles stays where it was put");
  tap(73, 38); printf("-- expect a no-tiles toast above --\n"); tap(112, 38);
  // a full contact list, tightly packed then spread: nobody may go missing
  the_mesh.c.clear();
  srand(7);
  for (int i = 0; i < 350; i++) add("Node", i % 3 == 0 ? 2 : 1, 37.74 + (rand() % 700) / 10000.0, -122.45 + (rand() % 700) / 10000.0);
  tap(258, 41);  // Fit
  benchState(&z2, &fx, &fy); mapViewRefresh(); shot("s17_full_list");
  { int m = benchClusters(&total); printf("z%d markers %d nodes %d\n", z2, m, total); chk(total == 350, "every node of a full list is on a marker"); chk(m <= 40, "marker slots hold"); }
  for (int i = 0; i < 3; i++) tap(296, 186);
  shot("s18_full_list_closer"); { int m = benchClusters(&total); printf("markers %d nodes %d\n", m, total); chk(m <= 40, "marker slots hold when spread"); }
  printf("%d passed %d failed\n", pass_n, fail_n);
  return fail_n != 0;
}
