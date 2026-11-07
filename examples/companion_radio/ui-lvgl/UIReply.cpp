// What you can do with a message you received: the menu behind the sender's
// name, quick replies with {variables} from that packet, and the packet's
// route and signal. Quick replies stay plain text in /qreplies.txt.

#include "UICommon.h"
#include "ReplyExpand.h"
#include <helpers/sensors/LocationProvider.h>
#include <math.h>
#include <time.h>

#define QR_MAX 12
#define QR_TEXT_LEN 80
#define REPLY_MAX_LEN 110   // same limit as typing in a thread

static char qr_texts[QR_MAX][QR_TEXT_LEN];
static int qr_count = 0;

static void qrDefaults() {
  static const char* defs[] = {
    "Copy that", "On my way", "Yes", "No", "At camp, all good",
    "Heard you, {hops} hops, SNR {snr} dB",
    "Route to you: {path}",
  };
  qr_count = 0;
  for (auto d : defs) StrHelper::strncpy(qr_texts[qr_count++], d, QR_TEXT_LEN);
}

void qrLoad() {
  File f = SPIFFS.open("/qreplies.txt", "r");
  if (!f) { qrDefaults(); return; }
  qr_count = 0;
  while (f.available() && qr_count < QR_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) StrHelper::strncpy(qr_texts[qr_count++], line.c_str(), QR_TEXT_LEN);
  }
  f.close();
}

static void qrSave() {
  File f = SPIFFS.open("/qreplies.txt", "w");
  if (!f) return;
  for (int i = 0; i < qr_count; i++) {
    f.print(qr_texts[i]);
    f.print('\n');
  }
  f.close();
}

// ---- message text helpers ----

// Channel text arrives as "Sender: message". Returns the message part and puts
// the sender in who. In a direct thread the sender is the thread itself.
const char* msgSplitSender(const char* text, bool channel, const char* thread_name, char* who, size_t who_sz) {
  who[0] = 0;
  if (!channel) {
    StrHelper::strncpy(who, thread_name, who_sz);
    return text;
  }
  const char* sep = strstr(text, ": ");
  if (sep == NULL || sep == text || (size_t)(sep - text) >= who_sz) return text;
  memcpy(who, text, sep - text);
  who[sep - text] = 0;
  return sep + 2;
}

// shows the mesh mention form @[Name] as @Name
void msgTidy(char* out, size_t out_sz, const char* in) {
  size_t o = 0;
  while (*in != 0 && o + 1 < out_sz) {
    if (in[0] == '@' && in[1] == '[') {
      const char* end = strchr(in, ']');
      if (end != NULL) {
        out[o++] = '@';
        for (const char* p = in + 2; p < end && o + 1 < out_sz; p++) out[o++] = *p;
        in = end + 1;
        continue;
      }
    }
    out[o++] = *in++;
  }
  out[o] = 0;
}

// ---- who and what the actions refer to ----

static ChatMsg tgt_msg;               // copied, the chat store may reuse the slot
static bool tgt_has_msg = false;
static bool tgt_channel = false;
static bool tgt_mention = false;      // start replies with @[name]
static char tgt_name[33];
static char tgt_thread[32];
static ReplyVars rv;

static int contactByName(const char* name, ContactInfo* out) {
  for (int idx = MAX_ANON_CONTACTS; idx < the_mesh.getTotalContactSlots(); idx++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) break;
    if (c.name[0] != 0 && strcmp(c.name, name) == 0) {
      if (out != NULL) *out = c;
      return idx;
    }
  }
  return -1;
}

static bool tgtContact(ContactInfo* out) {
  if (tgt_has_msg && !tgt_channel) {
    ContactInfo* c = the_mesh.lookupContactByPubKey(tgt_msg.prefix, 6);
    if (c == NULL) return false;
    *out = *c;
    return true;
  }
  return tgt_name[0] != 0 && contactByName(tgt_name, out) >= 0;
}

static double distanceM(double lat1, double lon1, double lat2, double lon2) {
  double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
  double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180.0;
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static int tgtHops() {   // -1 when the packet does not say
  if (!tgt_has_msg || !tgt_msg.has_rx) return -1;
  if (tgt_msg.rx_flood) return tgt_msg.rx_path_len & 63;
  if (tgt_channel) return 0;   // zero-hop send
  // direct packets use their path up on the way here, the saved route is the best guide
  ContactInfo c;
  if (tgtContact(&c) && c.out_path_len != OUT_PATH_UNKNOWN) return c.out_path_len & 63;
  return -1;
}

static void pathHex(char* out, size_t sz) {
  int hops = tgt_msg.rx_path_len & 63;
  int hsz = (tgt_msg.rx_path_len >> 6) + 1;
  int kept = (int) sizeof(tgt_msg.rx_path) / hsz;
  size_t o = 0;
  out[0] = 0;
  for (int h = 0; h < hops && h < kept && o + hsz * 2 + 4 < sz; h++) {
    if (h > 0) out[o++] = '>';
    for (int b = 0; b < hsz; b++) o += snprintf(&out[o], sz - o, "%02x", tgt_msg.rx_path[h * hsz + b]);
  }
  if (hops > kept && o + 3 < sz) snprintf(&out[o], sz - o, ">..");
}

static void varsBuild() {
  strcpy(rv.hops, "?"); strcpy(rv.path, "?"); strcpy(rv.snr, "?"); strcpy(rv.rssi, "?");
  strcpy(rv.time, "?"); strcpy(rv.dist, "?"); strcpy(rv.loc, "?");
  StrHelper::strncpy(rv.name, tgt_name[0] != 0 ? tgt_name : "?", sizeof(rv.name));
  StrHelper::strncpy(rv.me, ui->nodePrefs()->node_name, sizeof(rv.me));
  uint16_t mv = ui->getBattMilliVolts();
  snprintf(rv.batt, sizeof(rv.batt), "%u.%02uV", mv / 1000, (mv % 1000) / 10);

  double my_lat = 0, my_lon = 0;
  bool have_pos = false;
  SensorManager* s = ui->sensors();
  LocationProvider* loc = s != NULL ? s->getLocationProvider() : NULL;
  if (loc != NULL && loc->isValid()) {
    my_lat = loc->getLatitude() / 1000000.0;
    my_lon = loc->getLongitude() / 1000000.0;
    have_pos = true;
    snprintf(rv.loc, sizeof(rv.loc), "%.5f,%.5f", my_lat, my_lon);
  }

  if (!tgt_has_msg) return;
  if (tgt_msg.timestamp > 1600000000UL) {
    time_t local = (time_t) tgt_msg.timestamp + (time_t) settingsTzOffset() * 3600;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    snprintf(rv.time, sizeof(rv.time), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  }

  ContactInfo c;
  if (have_pos && tgtContact(&c) && (c.gps_lat != 0 || c.gps_lon != 0)) {
    double m = distanceM(my_lat, my_lon, c.gps_lat / 1000000.0, c.gps_lon / 1000000.0);
    if (units_miles) snprintf(rv.dist, sizeof(rv.dist), "%.1f mi", m / 1609.344);
    else snprintf(rv.dist, sizeof(rv.dist), "%.1f km", m / 1000.0);
  }

  if (!tgt_msg.has_rx) return;
  snprintf(rv.snr, sizeof(rv.snr), "%.1f", tgt_msg.rx_snr / 4.0);
  snprintf(rv.rssi, sizeof(rv.rssi), "%d", (int) tgt_msg.rx_rssi);
  int hops = tgtHops();
  if (hops >= 0) snprintf(rv.hops, sizeof(rv.hops), "%d", hops);
  if (hops == 0) strcpy(rv.path, "direct");
  else if (tgt_msg.rx_flood) pathHex(rv.path, sizeof(rv.path));
}

static void varsSample() {   // for previews when no message is selected
  strcpy(rv.name, "Alex");   strcpy(rv.hops, "2");       strcpy(rv.path, "a1>3f");
  strcpy(rv.snr, "6.5");     strcpy(rv.rssi, "-92");     strcpy(rv.time, "14:32");
  strcpy(rv.dist, "3.2 km"); strcpy(rv.loc, "0.00000,0.00000");
  strcpy(rv.batt, "3.95V");  StrHelper::strncpy(rv.me, ui->nodePrefs()->node_name, sizeof(rv.me));
}

static void tgtSet(const ChatMsg* m, const char* thread_name, bool channel, bool mention) {
  tgt_channel = channel;
  tgt_has_msg = m != NULL;
  StrHelper::strncpy(tgt_thread, thread_name, sizeof(tgt_thread));
  tgt_name[0] = 0;
  if (m != NULL) {
    tgt_msg = *m;
    msgSplitSender(m->text, channel, thread_name, tgt_name, sizeof(tgt_name));
  }
  tgt_mention = mention && channel && tgt_name[0] != 0;
  varsBuild();
}

// the text that would be sent for a quick reply
static void replyRender(const char* tmpl, char* out, size_t sz) {
  size_t used = 0;
  if (tgt_mention) {
    used = snprintf(out, sz, "@[%s] ", tgt_name);
    if (used >= sz) used = 0;
  }
  replyExpand(tmpl, rv, out + used, sz - used);
}

// ---- shared bits ----

static lv_obj_t* overlayCreate(lv_obj_t** title, lv_event_cb_t back_cb) {
  lv_obj_t* scr = lv_obj_create(lv_layer_top());
  lv_obj_set_size(scr, 320, 240);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* back = lv_button_create(scr);
  lv_obj_set_size(back, 40, 24);
  lv_obj_set_pos(back, 2, 2);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(back);
  lv_label_set_text(l, LV_SYMBOL_LEFT);
  lv_obj_center(l);

  *title = lv_label_create(scr);
  lv_label_set_text(*title, "");
  lv_label_set_long_mode(*title, LV_LABEL_LONG_DOT);
  lv_obj_set_size(*title, 224, 20);
  lv_obj_set_pos(*title, 48, 5);
  lv_obj_set_style_text_color(*title, lv_color_hex(COL_ACCENT), 0);
  return scr;
}

static lv_obj_t* smallBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* data, int w, int h) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

// ---- route and signal ----

static lv_obj_t* route_scr;
static lv_obj_t* route_title;
static lv_obj_t* route_lbl;

static void route_back_cb(lv_event_t* e) { lv_obj_add_flag(route_scr, LV_OBJ_FLAG_HIDDEN); }

// names a path hop from a repeater's hash; a short hash can match several
static void hopName(const uint8_t* hash, int hsz, char* out, size_t sz) {
  int matches = 0;
  char first[33] = "";
  for (int idx = MAX_ANON_CONTACTS; idx < the_mesh.getTotalContactSlots(); idx++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) break;
    if (c.name[0] == 0 || c.type != ADV_TYPE_REPEATER) continue;
    if (memcmp(c.id.pub_key, hash, hsz) == 0) {
      if (matches++ == 0) StrHelper::strncpy(first, c.name, sizeof(first));
    }
  }
  if (matches == 1) { snprintf(out, sz, "%s", first); return; }
  if (matches > 1) { snprintf(out, sz, "%s?", first); return; }
  size_t o = 0;
  for (int b = 0; b < hsz; b++) o += snprintf(&out[o], sz - o, "%02x", hash[b]);
}

static void routeOpen() {
  char buf[420];
  int o = 0;
  const char* who = tgt_name[0] != 0 ? tgt_name : "Unknown";
  lv_label_set_text(route_title, who);

  o += snprintf(&buf[o], sizeof(buf) - o, "In  %s\nArrived  %s\n", tgt_thread, rv.time);

  if (!tgt_has_msg || !tgt_msg.has_rx) {
    o += snprintf(&buf[o], sizeof(buf) - o, "\nThis message was saved before route details were kept.");
  } else {
    int hops = tgtHops();
    if (tgt_msg.rx_flood && hops > 0) {
      o += snprintf(&buf[o], sizeof(buf) - o, "Route  %d repeater%s\n", hops, hops == 1 ? "" : "s");
    } else if (hops == 0) {
      o += snprintf(&buf[o], sizeof(buf) - o, "Route  heard directly\n");
    } else if (hops > 0) {
      o += snprintf(&buf[o], sizeof(buf) - o, "Route  saved path, %d hop%s\n", hops, hops == 1 ? "" : "s");
    } else {
      o += snprintf(&buf[o], sizeof(buf) - o, "Route  not in this packet\n");
    }

    o += snprintf(&buf[o], sizeof(buf) - o, "Signal  SNR %s dB, RSSI %s dBm\n", rv.snr, rv.rssi);
    if (hops > 0) o += snprintf(&buf[o], sizeof(buf) - o, "(signal is the last hop only)\n");
    if (strcmp(rv.dist, "?") != 0) o += snprintf(&buf[o], sizeof(buf) - o, "Distance  %s\n", rv.dist);

    if (tgt_msg.rx_flood && hops > 0) {
      int hsz = (tgt_msg.rx_path_len >> 6) + 1;
      int kept = (int) sizeof(tgt_msg.rx_path) / hsz;
      o += snprintf(&buf[o], sizeof(buf) - o, "\nPath\n%s", who);
      for (int h = 0; h < hops && h < kept && o < (int) sizeof(buf) - 44; h++) {
        char nm[36];
        hopName(&tgt_msg.rx_path[h * hsz], hsz, nm, sizeof(nm));
        o += snprintf(&buf[o], sizeof(buf) - o, " " LV_SYMBOL_RIGHT " %s", nm);
      }
      if (hops > kept && o < (int) sizeof(buf) - 12) o += snprintf(&buf[o], sizeof(buf) - o, " " LV_SYMBOL_RIGHT " ...");
      if (o < (int) sizeof(buf) - 12) snprintf(&buf[o], sizeof(buf) - o, " " LV_SYMBOL_RIGHT " You");
    }
  }
  lv_label_set_text(route_lbl, buf);
  lv_obj_scroll_to_y(lv_obj_get_parent(route_lbl), 0, LV_ANIM_OFF);
  lv_obj_remove_flag(route_scr, LV_OBJ_FLAG_HIDDEN);
}

// ---- your own info, from the Me button on a message you sent ----

void myInfoOpen(const ChatMsg* m, const char* thread_name, bool channel) {
  tgtSet(m, thread_name, channel, false);
  NodePrefs* p = ui->nodePrefs();
  const uint8_t* pk = the_mesh.self_id.pub_key;
  char buf[420];
  int o = 0;

  lv_label_set_text_fmt(route_title, "Me: %s", p->node_name);

  // this message first, it is why the screen was opened
  o += snprintf(&buf[o], sizeof(buf) - o, "Sent  %s to %s\n", rv.time, thread_name);
  if (channel) {
    o += snprintf(&buf[o], sizeof(buf) - o, "Delivery  channels do not confirm\n");
  } else if (m->status == MSG_STATUS_DELIVERED) {
    o += snprintf(&buf[o], sizeof(buf) - o, "Delivery  confirmed\n");
  } else if (m->status == MSG_STATUS_PENDING) {
    o += snprintf(&buf[o], sizeof(buf) - o, "Delivery  waiting\n");
  } else if (m->status == MSG_STATUS_NO_ACK) {
    o += snprintf(&buf[o], sizeof(buf) - o, "Delivery  no confirmation\n");
  }

  o += snprintf(&buf[o], sizeof(buf) - o, "\nID  %02x%02x%02x%02x\n", pk[0], pk[1], pk[2], pk[3]);
  o += snprintf(&buf[o], sizeof(buf) - o, "Radio  %.3f MHz, BW %.1f, SF%d, CR%d\nPower  %d dBm\nBattery  %s\n",
                p->freq, p->bw, p->sf, p->cr, p->tx_power_dbm, rv.batt);

  SensorManager* s = ui->sensors();
  LocationProvider* loc = s != NULL ? s->getLocationProvider() : NULL;
  if (loc == NULL) o += snprintf(&buf[o], sizeof(buf) - o, "GPS  off\n");
  else if (loc->isValid()) o += snprintf(&buf[o], sizeof(buf) - o, "GPS  %s, %ld sats\n", rv.loc, (long) loc->satellitesCount());
  else o += snprintf(&buf[o], sizeof(buf) - o, "GPS  searching\n");
  o += snprintf(&buf[o], sizeof(buf) - o, "Location in adverts  %s\n", p->advert_loc_policy == ADVERT_LOC_NONE ? "not shared" : "shared");
#ifndef STANDALONE_NO_BT
  snprintf(&buf[o], sizeof(buf) - o, "Bluetooth  %s", ui->isBluetoothEnabled() ? "on" : "off");
#endif

  lv_label_set_text(route_lbl, buf);
  lv_obj_scroll_to_y(lv_obj_get_parent(route_lbl), 0, LV_ANIM_OFF);
  lv_obj_remove_flag(route_scr, LV_OBJ_FLAG_HIDDEN);
}

// ---- quick replies list ----

static lv_obj_t* qrl_scr;
static lv_obj_t* qrl_title;
static lv_obj_t* qrl_hint;
static lv_obj_t* qrl_list;
static bool qrl_can_send = false;    // false when opened from Settings
static int qrl_armed = -1;           // row whose trash was tapped once

static void qrEditOpen(int idx);
static void qrlRefresh();

static void qrl_back_cb(lv_event_t* e) { lv_obj_add_flag(qrl_scr, LV_OBJ_FLAG_HIDDEN); }

static void qrl_add_cb(lv_event_t* e) {
  if (qr_count >= QR_MAX) {
    lv_label_set_text_fmt(qrl_hint, "List is full (%d). Delete one first.", QR_MAX);
    return;
  }
  qrEditOpen(-1);
}

static void qrl_row_cb(lv_event_t* e) {
  int i = (int)(intptr_t) lv_event_get_user_data(e);
  if (i < 0 || i >= qr_count) return;
  if (!qrl_can_send) { qrEditOpen(i); return; }
  char out[REPLY_MAX_LEN + 1];
  replyRender(qr_texts[i], out, sizeof(out));
  lv_obj_add_flag(qrl_scr, LV_OBJ_FLAG_HIDDEN);
  if (out[0] != 0) ui->sendFromThread(out);
}

static void qrl_edit_cb(lv_event_t* e) { qrEditOpen((int)(intptr_t) lv_event_get_user_data(e)); }

static void qrl_del_cb(lv_event_t* e) {
  int i = (int)(intptr_t) lv_event_get_user_data(e);
  if (i != qrl_armed) {      // first tap arms it, nothing is lost by a stray touch
    qrl_armed = i;
    lv_obj_t* btn = (lv_obj_t*) lv_event_get_target(e);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_WARN), 0);
    lv_label_set_text(qrl_hint, "Tap the trash again to delete");
    return;
  }
  for (int k = i; k < qr_count - 1; k++) strcpy(qr_texts[k], qr_texts[k + 1]);
  qr_count--;
  qrSave();
  qrlRefresh();
}

static void qrl_loc_cb(lv_event_t* e) {
  if (strcmp(rv.loc, "?") == 0) {
    lv_label_set_text(qrl_hint, "No GPS fix yet");
    return;
  }
  char msg[48];
  snprintf(msg, sizeof(msg), "I'm at %s", rv.loc);
  lv_obj_add_flag(qrl_scr, LV_OBJ_FLAG_HIDDEN);
  ui->sendFromThread(msg);
}

static void qrlRefresh() {
  qrl_armed = -1;
  lv_obj_clean(qrl_list);
  lv_label_set_text(qrl_hint, qrl_can_send ? "Tap one to send it" : "Tap one to edit it");

  if (qrl_can_send) {
    lv_obj_t* row = lv_button_create(qrl_list);
    lv_obj_set_size(row, 310, 32);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_add_event_cb(row, qrl_loc_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(row);
    lv_label_set_text(l, LV_SYMBOL_GPS " Send my location");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  }

  for (int i = 0; i < qr_count; i++) {
    lv_obj_t* row = lv_obj_create(qrl_list);
    lv_obj_set_size(row, 310, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* b = lv_button_create(row);
    lv_obj_set_size(b, 234, 32);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_add_event_cb(b, qrl_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t) i);
    lv_obj_t* l = lv_label_create(b);
    char shown[REPLY_MAX_LEN + 1];
    if (qrl_can_send) {
      char raw[REPLY_MAX_LEN + 1];
      replyRender(qr_texts[i], raw, sizeof(raw));
      msgTidy(shown, sizeof(shown), raw);
    } else {
      StrHelper::strncpy(shown, qr_texts[i], sizeof(shown));
    }
    lv_label_set_text(l, shown);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, 222, 18);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* ed = smallBtn(row, LV_SYMBOL_EDIT, qrl_edit_cb, (void*)(intptr_t) i, 34, 32);
    lv_obj_set_pos(ed, 238, 0);
    lv_obj_t* del = smallBtn(row, LV_SYMBOL_TRASH, qrl_del_cb, (void*)(intptr_t) i, 34, 32);
    lv_obj_set_pos(del, 276, 0);
  }
  lv_obj_scroll_to_y(qrl_list, 0, LV_ANIM_OFF);
}

static void qrlOpen(bool can_send) {
  qrl_can_send = can_send;
  if (!can_send) lv_label_set_text(qrl_title, "Quick replies");
  else if (tgt_mention) lv_label_set_text_fmt(qrl_title, "Reply to %s", tgt_name);
  else lv_label_set_text_fmt(qrl_title, "Quick reply in %s", tgt_thread);
  qrlRefresh();
  lv_obj_remove_flag(qrl_scr, LV_OBJ_FLAG_HIDDEN);
}

void qrEditorOpen() {
  tgtSet(NULL, "", false, false);
  varsSample();
  qrlOpen(false);
}

void replyPresetsForThread(const ChatMsg* newest, const char* thread_name, bool channel) {
  tgtSet(newest, thread_name, channel, false);
  qrlOpen(true);
}

// ---- quick reply editor ----

static lv_obj_t* qre_scr;
static lv_obj_t* qre_preview;
static lv_obj_t* qre_ta;
static int qre_idx = -1;             // -1 adds a new one

static void qre_close() { lv_obj_add_flag(qre_scr, LV_OBJ_FLAG_HIDDEN); }
static void qre_back_cb(lv_event_t* e) { qre_close(); }

static void qre_save() {
  const char* t = lv_textarea_get_text(qre_ta);
  while (*t == ' ') t++;
  if (*t == 0) { lv_label_set_text(qre_preview, "Type something first"); return; }
  int i = qre_idx >= 0 ? qre_idx : qr_count;
  if (i >= QR_MAX) return;
  StrHelper::strncpy(qr_texts[i], t, QR_TEXT_LEN);
  if (qre_idx < 0) qr_count++;
  qrSave();
  qre_close();
  qrlRefresh();
}

static void qre_save_cb(lv_event_t* e) { qre_save(); }

static void qre_preview_update() {
  char out[REPLY_MAX_LEN + 1];
  replyExpand(lv_textarea_get_text(qre_ta), rv, out, sizeof(out));
  lv_label_set_text(qre_preview, out[0] != 0 ? out : (qre_idx >= 0 ? "Edit reply" : "New reply"));
}

static void qre_ta_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) qre_preview_update();
  else if (code == LV_EVENT_READY) qre_save();
}

static void qre_chip_cb(lv_event_t* e) {
  int i = (int)(intptr_t) lv_event_get_user_data(e);
  char v[12];
  snprintf(v, sizeof(v), "{%s}", REPLY_VARS[i].key);
  lv_textarea_add_text(qre_ta, v);
}

static void qrEditOpen(int idx) {
  qre_idx = idx;
  lv_textarea_set_text(qre_ta, idx >= 0 ? qr_texts[idx] : "");
  qre_preview_update();
  lv_obj_remove_flag(qre_scr, LV_OBJ_FLAG_HIDDEN);
}

void qrEditorBuild() {
  // route
  route_scr = overlayCreate(&route_title, route_back_cb);
  lv_obj_t* box = lv_obj_create(route_scr);
  lv_obj_set_size(box, 316, 208);
  lv_obj_set_pos(box, 2, 30);
  lv_obj_set_style_bg_color(box, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_set_style_pad_all(box, 8, 0);
  route_lbl = lv_label_create(box);
  lv_obj_set_width(route_lbl, LV_PCT(100));
  lv_label_set_long_mode(route_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(route_lbl, 3, 0);

  // list
  qrl_scr = overlayCreate(&qrl_title, qrl_back_cb);
  lv_obj_t* add = lv_button_create(qrl_scr);
  lv_obj_set_size(add, 40, 24);
  lv_obj_set_pos(add, 278, 2);
  lv_obj_add_event_cb(add, qrl_add_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* al = lv_label_create(add);
  lv_label_set_text(al, LV_SYMBOL_PLUS);
  lv_obj_center(al);

  qrl_hint = lv_label_create(qrl_scr);
  lv_label_set_text(qrl_hint, "");
  lv_obj_set_style_text_font(qrl_hint, &rc_font_12, 0);
  lv_obj_set_style_text_color(qrl_hint, lv_color_hex(COL_MUTED), 0);
  lv_obj_set_pos(qrl_hint, 6, 28);

  qrl_list = lv_obj_create(qrl_scr);
  lv_obj_set_size(qrl_list, 320, 240 - 46);
  lv_obj_set_pos(qrl_list, 0, 46);
  lv_obj_set_style_bg_opa(qrl_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(qrl_list, 0, 0);
  lv_obj_set_style_pad_all(qrl_list, 4, 0);
  lv_obj_set_style_pad_row(qrl_list, 4, 0);
  lv_obj_set_flex_flow(qrl_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(qrl_list, LV_SCROLLBAR_MODE_OFF);

  // editor: 80 px above the keyboard for the preview, the text and the variables
  qre_scr = overlayCreate(&qre_preview, qre_back_cb);
  lv_obj_set_size(qre_preview, 226, 16);
  lv_obj_set_style_text_font(qre_preview, &rc_font_12, 0);
  lv_obj_set_style_text_color(qre_preview, lv_color_hex(COL_MUTED), 0);
  lv_obj_set_pos(qre_preview, 48, 7);

  lv_obj_t* ok = lv_button_create(qre_scr);
  lv_obj_set_size(ok, 40, 24);
  lv_obj_set_pos(ok, 278, 2);
  lv_obj_add_event_cb(ok, qre_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* okl = lv_label_create(ok);
  lv_label_set_text(okl, LV_SYMBOL_OK);
  lv_obj_center(okl);

  qre_ta = lv_textarea_create(qre_scr);
  lv_textarea_set_one_line(qre_ta, true);
  lv_textarea_set_max_length(qre_ta, QR_TEXT_LEN - 1);
  lv_textarea_set_placeholder_text(qre_ta, "Type, then add a variable");
  lv_obj_set_width(qre_ta, 316);
  lv_obj_set_height(qre_ta, 26);
  lv_obj_set_pos(qre_ta, 2, 28);
  lv_obj_set_style_pad_ver(qre_ta, 3, 0);
  lv_obj_set_style_pad_left(qre_ta, 6, 0);
  lv_obj_set_scroll_dir(qre_ta, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(qre_ta, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_state(qre_ta, LV_STATE_FOCUSED);
  lv_obj_add_event_cb(qre_ta, qre_ta_cb, LV_EVENT_ALL, NULL);

  lv_obj_t* chips = lv_obj_create(qre_scr);
  lv_obj_set_size(chips, 320, 24);
  lv_obj_set_pos(chips, 0, 56);
  lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chips, 0, 0);
  lv_obj_set_style_pad_all(chips, 0, 0);
  lv_obj_set_style_pad_left(chips, 2, 0);
  lv_obj_set_style_pad_column(chips, 4, 0);
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
  for (size_t i = 0; i < REPLY_VAR_COUNT; i++) {
    lv_obj_t* c = lv_button_create(chips);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 22);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_ACCENT_D), 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_hor(c, 7, 0);
    lv_obj_set_style_pad_ver(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(c, qre_chip_cb, LV_EVENT_CLICKED, (void*)(intptr_t) i);
    lv_obj_t* l = lv_label_create(c);
    lv_label_set_text(l, REPLY_VARS[i].key);
    lv_obj_set_style_text_font(l, &rc_font_12, 0);
    lv_obj_center(l);
  }

  lv_obj_t* kb = lv_keyboard_create(qre_scr);
  lv_keyboard_set_textarea(kb, qre_ta);
  kbAttachShiftBehavior(kb);
  lv_obj_set_size(kb, 320, KB_HEIGHT);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// ---- the menu behind a sender's name ----

static lv_obj_t* menu_scrim = NULL;

void replyMenuClose() {
  if (menu_scrim == NULL) return;
  lv_obj_delete_async(menu_scrim);
  menu_scrim = NULL;
}

static void menu_scrim_cb(lv_event_t* e) {
  if (lv_event_get_target(e) == lv_event_get_current_target(e)) replyMenuClose();
}

static void menu_reply_cb(lv_event_t* e) {
  replyMenuClose();
  char start[40] = "";
  if (tgt_mention) snprintf(start, sizeof(start), "@[%s] ", tgt_name);
  threadCompose(start);
}

static void menu_private_cb(lv_event_t* e) {
  replyMenuClose();
  ContactInfo c;
  if (tgtContact(&c)) ui->openContactThread(c);
}

static void menu_presets_cb(lv_event_t* e) {
  replyMenuClose();
  qrlOpen(true);
}

static void menu_route_cb(lv_event_t* e) {
  replyMenuClose();
  routeOpen();
}

static void menuBtn(lv_obj_t* card, const char* text, lv_event_cb_t cb) {
  lv_obj_t* b = lv_button_create(card);
  lv_obj_set_size(b, LV_PCT(100), 36);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 2, 0);
}

void replyMenuOpen(const ChatMsg* m, const char* thread_name, bool channel) {
  replyMenuClose();
  tgtSet(m, thread_name, channel, true);

  menu_scrim = lv_obj_create(threadScreen());
  lv_obj_set_size(menu_scrim, 320, 240);
  lv_obj_set_pos(menu_scrim, 0, 0);
  lv_obj_set_style_bg_color(menu_scrim, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(menu_scrim, LV_OPA_50, 0);
  lv_obj_set_style_border_width(menu_scrim, 0, 0);
  lv_obj_set_style_radius(menu_scrim, 0, 0);
  lv_obj_remove_flag(menu_scrim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(menu_scrim, menu_scrim_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* card = lv_obj_create(menu_scrim);
  lv_obj_set_size(card, 250, LV_SIZE_CONTENT);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT_D), 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_all(card, 7, 0);
  lv_obj_set_style_pad_row(card, 4, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* t = lv_label_create(card);
  lv_label_set_text(t, tgt_name[0] != 0 ? tgt_name : "Unknown");
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_size(t, LV_PCT(100), 22);
  lv_obj_set_style_text_font(t, &rc_font_16, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(COL_ACCENT), 0);

  lv_obj_t* sub = lv_label_create(card);
  if (m->has_rx) lv_label_set_text_fmt(sub, "%s hops   SNR %s dB   %s", rv.hops, rv.snr, rv.time);
  else lv_label_set_text_fmt(sub, "Arrived %s", rv.time);
  lv_obj_set_style_text_font(sub, &rc_font_12, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(COL_MUTED), 0);

  menuBtn(card, LV_SYMBOL_EDIT "  Reply", menu_reply_cb);
  ContactInfo c;
  if (channel && tgtContact(&c)) menuBtn(card, LV_SYMBOL_ENVELOPE "  Private message", menu_private_cb);
  menuBtn(card, LV_SYMBOL_LIST "  Quick replies", menu_presets_cb);
  menuBtn(card, LV_SYMBOL_SHUFFLE "  Route & signal", menu_route_cb);
}
