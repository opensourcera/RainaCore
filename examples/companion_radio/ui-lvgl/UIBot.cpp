// RainaCore Bot + Dashboard tab.
// The bot sends one short message to the public #Test channel on a fixed
// schedule. It is off by default, its shortest interval is one hour, and the
// first automatic send waits a full interval so a reboot loop cannot flood.

#include "UICommon.h"
#include <helpers/sensors/LocationProvider.h>
#include <time.h>

#define BOT_CHANNEL "#Test"
#define BOT_TEXT    "Test"

static const int bot_hours[] = {1, 3, 6, 12, 24};
#define BOT_INTERVALS (sizeof(bot_hours) / sizeof(bot_hours[0]))

static bool bot_on = false;
static int bot_interval = 0;              // index into bot_hours
static uint32_t bot_due_ms = 0;
static uint32_t bot_sent = 0;
static uint32_t bot_failed = 0;
static uint32_t bot_last_ms = 0;
static bool bot_has_sent = false;

static lv_obj_t* bot_state_lbl;
static lv_obj_t* dash_lbl;

static uint32_t botIntervalMs() { return (uint32_t) bot_hours[bot_interval] * 3600000UL; }

static void botSchedule() { bot_due_ms = millis() + botIntervalMs(); }

// find #Test, or create it with the standard hashtag key (sha256 of the name)
static int botChannelIdx() {
  int empty = -1;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails t;
    if (!the_mesh.getChannel(i, t)) break;
    if (strcmp(t.name, BOT_CHANNEL) == 0) return i;
    if (t.name[0] == 0 && empty < 0) empty = i;
  }
  if (empty < 0) return -1;

  ChannelDetails ch;
  memset(&ch, 0, sizeof(ch));
  StrHelper::strncpy(ch.name, BOT_CHANNEL, sizeof(ch.name));
  mesh::Utils::sha256(ch.channel.secret, 16, (const uint8_t*) ch.name, strlen(ch.name));
  mesh::Utils::sha256(ch.channel.hash, sizeof(ch.channel.hash), ch.channel.secret, 16);
  if (!the_mesh.setChannel(empty, ch)) return -1;
  the_mesh.saveChannels();
  return empty;
}

static bool botSend() {
  int idx = botChannelIdx();
  ChannelDetails ch;
  bool ok = false;
  if (idx >= 0 && the_mesh.getChannel(idx, ch)) {
    uint32_t ts = rtc_clock.getCurrentTime();
    ok = the_mesh.sendGroupMessage(ts, ch.channel, ui->nodePrefs()->node_name, BOT_TEXT, strlen(BOT_TEXT));
    if (ok) {
      uint8_t key[6];
      makeChannelKey(idx, key);
      chatStorePush(key, true, ts, BOT_TEXT);
    }
  }
  if (ok) {
    bot_sent++;
    bot_has_sent = true;
    bot_last_ms = millis();
  } else {
    bot_failed++;
  }
  return ok;
}

static void bot_switch_cb(lv_event_t* e) {
  bot_on = lv_obj_has_state((lv_obj_t*) lv_event_get_target(e), LV_STATE_CHECKED);
  prefWriteInt("/bot_on", bot_on ? 1 : 0);
  if (bot_on) botSchedule();
  botRefresh();
}

static void bot_interval_cb(lv_event_t* e) {
  int i = (int) lv_dropdown_get_selected((lv_obj_t*) lv_event_get_target(e));
  if (i < 0 || i >= (int) BOT_INTERVALS) return;
  bot_interval = i;
  prefWriteInt("/bot_int", i);
  if (bot_on) botSchedule();
  botRefresh();
}

static void bot_send_now_cb(lv_event_t* e) {
  ui->showToast(botSend() ? "Sent to " BOT_CHANNEL : "Send failed");
  botRefresh();
}

static lv_obj_t* botCard(lv_obj_t* parent, const char* title) {
  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_set_style_pad_row(card, 6, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* hdr = lv_label_create(card);
  lv_label_set_text(hdr, title);
  lv_obj_set_style_text_font(hdr, &rc_font_16, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(COL_ACCENT), 0);
  return card;
}

static lv_obj_t* botRow(lv_obj_t* parent, const char* text) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), 34);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, text);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  return row;
}

void botBuild(lv_obj_t* parent) {
  bot_on = prefReadInt("/bot_on", 0) == 1;
  bot_interval = prefReadInt("/bot_int", 0);
  if (bot_interval < 0 || bot_interval >= (int) BOT_INTERVALS) bot_interval = 0;
  botSchedule();

  lv_obj_set_style_pad_all(parent, 4, 0);
  lv_obj_set_style_pad_row(parent, 6, 0);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* dash = botCard(parent, "Dashboard");
  dash_lbl = lv_label_create(dash);
  lv_obj_set_width(dash_lbl, LV_PCT(100));
  lv_obj_set_style_text_font(dash_lbl, &rc_font_14, 0);

  lv_obj_t* bot = botCard(parent, "Bot");

  lv_obj_t* row = botRow(bot, "Send to " BOT_CHANNEL);
  lv_obj_t* sw = lv_switch_create(row);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  if (bot_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, bot_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = botRow(bot, "Every");
  lv_obj_t* dd = lv_dropdown_create(row);
  lv_dropdown_set_options(dd, "1 hour\n3 hours\n6 hours\n12 hours\n24 hours");
  lv_dropdown_set_selected(dd, bot_interval);
  lv_obj_set_size(dd, 120, 32);
  lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(dd, bot_interval_cb, LV_EVENT_VALUE_CHANGED, NULL);

  bot_state_lbl = lv_label_create(bot);
  lv_obj_set_width(bot_state_lbl, LV_PCT(100));
  lv_obj_set_style_text_font(bot_state_lbl, &rc_font_12, 0);
  lv_obj_set_style_text_color(bot_state_lbl, lv_color_hex(COL_MUTED), 0);

  lv_obj_t* btn = lv_button_create(bot);
  lv_obj_set_size(btn, LV_PCT(100), 34);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_add_event_cb(btn, bot_send_now_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(btn);
  lv_label_set_text(bl, "Send one now");
  lv_obj_center(bl);

  botRefresh();
}

void botTick() {
  if (!bot_on) return;
  if ((int32_t)(millis() - bot_due_ms) < 0) return;
  botSend();
  botSchedule();
}

void botRefresh() {
  if (dash_lbl == NULL) return;

  int chats = 0, repeaters = 0, mapped = 0;
  for (int idx = MAX_ANON_CONTACTS; idx < the_mesh.getTotalContactSlots(); idx++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(idx, c)) break;
    if (c.name[0] == 0) continue;
    if (c.type == ADV_TYPE_REPEATER) {
      repeaters++;
      if (c.gps_lat != 0 || c.gps_lon != 0) mapped++;
    } else if (c.type == ADV_TYPE_CHAT) {
      chats++;
    }
  }

  char gps[24] = "off";
  SensorManager* s = ui->sensors();
  LocationProvider* loc = s != NULL ? s->getLocationProvider() : NULL;
  if (loc != NULL) {
    if (loc->isValid()) snprintf(gps, sizeof(gps), "fix, %ld sats", loc->satellitesCount());
    else snprintf(gps, sizeof(gps), "searching");
  }

  char tzline[52];
  uint32_t now = rtc_clock.getCurrentTime();
  if (now > 1600000000UL) {
    time_t local = (time_t) now + (time_t) settingsTzOffset() * 3600;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    snprintf(tzline, sizeof(tzline), "%02d:%02d  UTC%+d %s", tmv.tm_hour, tmv.tm_min,
             settingsTzOffset(), geoTzName());
  } else {
    snprintf(tzline, sizeof(tzline), "not set yet, waiting for GPS");
  }

  uint32_t up = millis() / 60000UL;
  uint16_t mv = ui->getBattMilliVolts();
  char buf[280];
  snprintf(buf, sizeof(buf),
           "Time  %s\n"
           "Battery  %u.%02u V\n"
           "Nodes  %d chat, %d repeaters (%d on map)\n"
           "Last signal  %d dBm, SNR %.1f\n"
           "GPS  %s\n"
           "Air time  %lu s\n"
           "Up  %luh %lum",
           tzline, mv / 1000, (mv % 1000) / 10, chats, repeaters, mapped,
           (int) radio_driver.getLastRSSI(), radio_driver.getLastSNR(), gps,
           (unsigned long) (the_mesh.getTotalAirTime() / 1000),
           (unsigned long) (up / 60), (unsigned long) (up % 60));
  lv_label_set_text(dash_lbl, buf);

  char last[24] = "never";
  if (bot_has_sent) snprintf(last, sizeof(last), "%lum ago", (unsigned long) ((millis() - bot_last_ms) / 60000UL));
  char next[24] = "off";
  if (bot_on) {
    int32_t left = (int32_t)(bot_due_ms - millis());
    snprintf(next, sizeof(next), "in %ldm", (long) ((left > 0 ? left : 0) / 60000L));
  }
  lv_label_set_text_fmt(bot_state_lbl, "Next: %s   Last: %s\nSent %lu, failed %lu since restart",
                        next, last, (unsigned long) bot_sent, (unsigned long) bot_failed);
}
