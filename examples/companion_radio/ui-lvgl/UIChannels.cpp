// Creating, joining and sharing channels.
//
// Private: a random key, readable only by people given the key or QR code.
// Public: a #name whose key comes from the name, so anyone can join.
// Both are plain channels. A room server is a separate always-on device.

#include "UICommon.h"
#include "ChannelShare.h"
#include <esp_random.h>

// from base64.hpp, which is already compiled into BaseChatMesh.cpp
unsigned int decode_base64(const unsigned char input[], unsigned int len, unsigned char output[]);

static const uint8_t PUBLIC_KEY[CHANNEL_KEY_LEN] = {
  0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};

// public means the key is no secret: the default key, or derived from the name
bool channelIsPublic(const ChannelDetails& ch) {
  if (memcmp(ch.channel.secret, PUBLIC_KEY, CHANNEL_KEY_LEN) == 0) return true;
  if (ch.name[0] != '#') return false;
  uint8_t derived[CHANNEL_KEY_LEN];
  mesh::Utils::sha256(derived, CHANNEL_KEY_LEN, (const uint8_t*) ch.name, strlen(ch.name));
  return memcmp(derived, ch.channel.secret, CHANNEL_KEY_LEN) == 0;
}

int channelsUsed() {
  int used = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails t;
    if (!the_mesh.getChannel(i, t)) break;
    if (t.name[0] != 0) used++;
  }
  return used;
}

// hardware random numbers, stirred with radio noise and timing
static void channelNewKey(uint8_t* key) {
  struct { uint8_t hw[32]; uint32_t us[8]; float rssi[8]; } seed;
  esp_fill_random(seed.hw, sizeof(seed.hw));
  for (int i = 0; i < 8; i++) {
    seed.us[i] = micros();
    seed.rssi[i] = radio_driver.getCurrentRSSI();
  }
  mesh::Utils::sha256(key, CHANNEL_KEY_LEN, (const uint8_t*) &seed, sizeof(seed));
  memset(&seed, 0, sizeof(seed));
}

// ---- Chats tab filter chips, with + to add a channel ----

static void chip_add_cb(lv_event_t* e) { addchOpen(); }

void chatChipsBuild(lv_obj_t* parent, lv_event_cb_t cb, lv_obj_t** chips) {
  static const char* names[CHAT_CHIPS] = {"All", "Private", "Public", "Repeaters", LV_SYMBOL_PLUS};
  static const int widths[CHAT_CHIPS] = {34, 58, 54, 76, 30};
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), 28);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 3, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < CHAT_CHIPS; i++) {
    lv_obj_t* b = lv_button_create(row);
    lv_obj_set_size(b, widths[i], 26);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(i == CHAT_CHIPS - 1 ? COL_ACCENT_D : COL_CARD), 0);
    if (i == CHAT_CHIPS - 1) lv_obj_add_event_cb(b, chip_add_cb, LV_EVENT_CLICKED, NULL);
    else lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)(intptr_t) i);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, names[i]);
    lv_obj_set_style_text_font(l, &rc_font_12, 0);
    lv_obj_center(l);
    chips[i] = b;
  }
}

// ---- share screen ----

static lv_obj_t* share_scr = NULL;

static void share_close_cb(lv_event_t* e) {
  if (share_scr == NULL) return;
  lv_obj_delete_async(share_scr);
  share_scr = NULL;
}

void channelShareOpen(int idx) {
  ChannelDetails ch;
  if (!the_mesh.getChannel(idx, ch) || ch.name[0] == 0) return;
  for (int i = CHANNEL_KEY_LEN; i < (int) sizeof(ch.channel.secret); i++) {
    if (ch.channel.secret[i] != 0) {   // the shared link format holds 16 bytes
      ui->showToast("This key is too long for a QR code");
      return;
    }
  }
  char url[140];
  if (!channelShareUrl(ch.name, ch.channel.secret, url, sizeof(url))) {
    ui->showToast("Name is too long for a QR code");
    return;
  }
  bool pub = channelIsPublic(ch);

  share_scr = lv_obj_create(lv_layer_top());
  lv_obj_set_size(share_scr, 320, 240);
  lv_obj_set_style_bg_color(share_scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(share_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(share_scr, 0, 0);
  lv_obj_set_style_radius(share_scr, 0, 0);
  lv_obj_set_style_pad_all(share_scr, 0, 0);
  lv_obj_remove_flag(share_scr, LV_OBJ_FLAG_SCROLLABLE);

  // white border is the quiet zone scanners need
  lv_obj_t* qr = lv_qrcode_create(share_scr);
  lv_qrcode_set_size(qr, 200);
  lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
  lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
  lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(qr, 10, 0);
  lv_obj_set_pos(qr, 0, 10);
  if (lv_qrcode_update(qr, url, strlen(url)) != LV_RESULT_OK) {
    lv_obj_delete(share_scr);
    share_scr = NULL;
    ui->showToast("QR code failed");
    return;
  }

  lv_obj_t* back = lv_button_create(share_scr);
  lv_obj_set_size(back, 40, 24);
  lv_obj_set_pos(back, 276, 2);
  lv_obj_add_event_cb(back, share_close_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_CLOSE);
  lv_obj_center(bl);

  lv_obj_t* col = lv_obj_create(share_scr);
  lv_obj_set_size(col, 98, 208);
  lv_obj_set_pos(col, 221, 30);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_style_pad_row(col, 4, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* t = lv_label_create(col);
  lv_label_set_text(t, ch.name);
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(t, 98);
  lv_obj_set_style_text_color(t, lv_color_hex(COL_ACCENT), 0);

  lv_obj_t* info = lv_label_create(col);
  lv_label_set_text(info, pub ? "Public. Anyone can join."
                              : "Private. Whoever scans this can read it.");
  lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(info, 98);
  lv_obj_set_style_text_font(info, &rc_font_12, 0);
  lv_obj_set_style_text_color(info, lv_color_hex(pub ? COL_MUTED : COL_WARN), 0);

  // this screen has no camera on the other side: another L2 joins by typing the key
  char keytxt[40];
  size_t o = 0;
  for (int i = 0; i < CHANNEL_KEY_LEN; i++) {
    o += snprintf(&keytxt[o], sizeof(keytxt) - o, "%02x%s", ch.channel.secret[i],
                  i == CHANNEL_KEY_LEN - 1 ? "" : (i % 4 == 3 ? "\n" : ""));
  }
  lv_obj_t* k = lv_label_create(col);
  lv_label_set_text(k, keytxt);
  lv_obj_set_style_text_font(k, &rc_font_14, 0);
}

// ---- new channel screen ----

#define ADDCH_PRIVATE 0
#define ADDCH_PUBLIC  1
#define ADDCH_JOIN    2

static lv_obj_t* addch_scr;
static lv_obj_t* addch_title;
static lv_obj_t* addch_name_ta;
static lv_obj_t* addch_psk_ta;
static lv_obj_t* addch_kb;
static lv_obj_t* addch_help;
static lv_obj_t* addch_seg[3];
static int addch_mode = ADDCH_PRIVATE;

#define ADDCH_NAME_Y 42
#define ADDCH_PSK_Y  72

static void addchLayoutForKey(bool typing_key) {
  // the keyboard covers the key field's usual place
  if (typing_key) {
    lv_obj_add_flag(addch_name_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(addch_psk_ta, ADDCH_NAME_Y);
  } else {
    lv_obj_remove_flag(addch_name_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(addch_psk_ta, ADDCH_PSK_Y);
  }
}

static void addchSetMode(int mode) {
  static const char* help[3] = {
    "Only people you give the QR code or key can read it.",
    "Anyone who types the same #name joins and can read it.",
    "Type the key someone shared: 32 letters and numbers.",
  };
  addch_mode = mode;
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_color(addch_seg[i], lv_color_hex(i == mode ? COL_ACCENT_D : COL_CARD), 0);
  }
  lv_label_set_text(addch_help, help[mode]);
  lv_textarea_set_placeholder_text(addch_name_ta, mode == ADDCH_PUBLIC ? "Name, e.g. dallas" : "Channel name");
  if (mode == ADDCH_JOIN) {
    lv_obj_remove_flag(addch_psk_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(addch_help, 102);
  } else {
    lv_obj_add_flag(addch_psk_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(addch_help, ADDCH_PSK_Y + 4);
  }
}

static void addch_seg_cb(lv_event_t* e) {
  addchSetMode((int)(intptr_t) lv_event_get_user_data(e));
}

void addchOpen() {
  int used = channelsUsed();
  lv_textarea_set_text(addch_name_ta, "");
  lv_textarea_set_text(addch_psk_ta, "");
  lv_label_set_text_fmt(addch_title, "New channel   %d of %d used", used, MAX_GROUP_CHANNELS);
  lv_obj_add_flag(addch_kb, LV_OBJ_FLAG_HIDDEN);
  addchLayoutForKey(false);
  addchSetMode(ADDCH_PRIVATE);
  lv_obj_remove_flag(addch_scr, LV_OBJ_FLAG_HIDDEN);
}

static void addchClose() {
  lv_keyboard_set_textarea(addch_kb, NULL);
  lv_obj_add_flag(addch_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(addch_scr, LV_OBJ_FLAG_HIDDEN);
}

static void addch_cancel_cb(lv_event_t* e) { addchClose(); }

static void addch_ta_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* ta = (lv_obj_t*) lv_event_get_target(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(addch_kb, ta);
    lv_obj_remove_flag(addch_kb, LV_OBJ_FLAG_HIDDEN);
    addchLayoutForKey(ta == addch_psk_ta);
  } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(addch_kb, LV_OBJ_FLAG_HIDDEN);
    addchLayoutForKey(false);
  }
}

static void addch_create_cb(lv_event_t* e) {
  const char* typed = lv_textarea_get_text(addch_name_ta);
  while (*typed == '#') typed++;
  if (typed[0] == 0) {
    ui->showToast("Channel needs a name");
    return;
  }

  ChannelDetails ch;
  memset(&ch, 0, sizeof(ch));
  int keylen = CHANNEL_KEY_LEN;
  if (addch_mode == ADDCH_PUBLIC) {
    // the key is the first 16 bytes of sha256("#name"), the same rule the app
    // uses. Case matters: #Test and #test are different channels.
    snprintf(ch.name, sizeof(ch.name), "#%s", typed);
    mesh::Utils::sha256(ch.channel.secret, keylen, (const uint8_t*) ch.name, strlen(ch.name));
  } else if (addch_mode == ADDCH_PRIVATE) {
    StrHelper::strncpy(ch.name, typed, sizeof(ch.name));
    channelNewKey(ch.channel.secret);
  } else {
    const char* psk = lv_textarea_get_text(addch_psk_ta);
    if (!channelHexKey(psk, ch.channel.secret)) {
      size_t len = strlen(psk);
      unsigned char decoded[48];
      keylen = len > 0 && len <= 44 ? (int) decode_base64((const unsigned char*) psk, len, decoded) : 0;
      if (keylen != 16 && keylen != 32) {
        ui->showToast("Key must be 32 hex characters");
        return;
      }
      memcpy(ch.channel.secret, decoded, keylen);
    }
    StrHelper::strncpy(ch.name, typed, sizeof(ch.name));
  }
  mesh::Utils::sha256(ch.channel.hash, sizeof(ch.channel.hash), ch.channel.secret, keylen);

  // NOTE: BaseChatMesh::addChannel writes at its own counter which ignores
  // flash-loaded channels (it would overwrite slot 0) - place the channel
  // into the first truly empty slot ourselves
  int slot = -1;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails t;
    if (!the_mesh.getChannel(i, t)) break;
    const char* existing = t.name[0] == '#' ? t.name + 1 : t.name;
    if (t.name[0] != 0 && strcmp(existing, typed) == 0) {
      ui->showToast("Channel already exists");
      return;
    }
    if (t.name[0] == 0 && slot < 0) slot = i;
  }
  if (slot < 0) {
    ui->showToast("All channels used, remove one first");
    return;
  }
  if (!the_mesh.setChannel(slot, ch)) {
    ui->showToast("Channel add failed");
    return;
  }
  the_mesh.saveChannels();
  bool joined = addch_mode == ADDCH_JOIN;
  addchClose();
  ui->refreshChatsTab();
  if (joined) ui->showToast("Channel joined");
  else channelShareOpen(slot);
}

void addchBuild() {
  addch_scr = lv_obj_create(lv_screen_active());
  lv_obj_set_size(addch_scr, 320, 240);
  lv_obj_align(addch_scr, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(addch_scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(addch_scr, 0, 0);
  lv_obj_set_style_radius(addch_scr, 0, 0);
  lv_obj_set_style_pad_all(addch_scr, 8, 0);
  lv_obj_add_flag(addch_scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(addch_scr, LV_OBJ_FLAG_SCROLLABLE);

  addch_title = lv_label_create(addch_scr);
  lv_label_set_text(addch_title, "New channel");
  lv_obj_set_style_text_color(addch_title, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_pos(addch_title, 0, -4);

  static const char* seg_names[3] = {"Private", "Public", "Join"};
  for (int i = 0; i < 3; i++) {
    addch_seg[i] = lv_button_create(addch_scr);
    lv_obj_set_size(addch_seg[i], 98, 26);
    lv_obj_set_pos(addch_seg[i], i * 103, 14);
    lv_obj_add_event_cb(addch_seg[i], addch_seg_cb, LV_EVENT_CLICKED, (void*)(intptr_t) i);
    lv_obj_t* l = lv_label_create(addch_seg[i]);
    lv_label_set_text(l, seg_names[i]);
    lv_obj_center(l);
  }

  addch_name_ta = lv_textarea_create(addch_scr);
  lv_textarea_set_one_line(addch_name_ta, true);
  lv_textarea_set_max_length(addch_name_ta, 30);
  lv_obj_set_width(addch_name_ta, 304);
  lv_obj_set_height(addch_name_ta, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_ver(addch_name_ta, 5, 0);
  lv_obj_set_scroll_dir(addch_name_ta, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(addch_name_ta, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_pos(addch_name_ta, 0, ADDCH_NAME_Y);
  lv_obj_add_event_cb(addch_name_ta, addch_ta_cb, LV_EVENT_ALL, NULL);

  addch_psk_ta = lv_textarea_create(addch_scr);
  lv_textarea_set_one_line(addch_psk_ta, true);
  lv_textarea_set_placeholder_text(addch_psk_ta, "Key");
  lv_textarea_set_max_length(addch_psk_ta, 44);
  lv_obj_set_width(addch_psk_ta, 304);
  lv_obj_set_height(addch_psk_ta, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_ver(addch_psk_ta, 5, 0);
  lv_obj_set_scroll_dir(addch_psk_ta, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(addch_psk_ta, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_pos(addch_psk_ta, 0, ADDCH_PSK_Y);
  lv_obj_add_event_cb(addch_psk_ta, addch_ta_cb, LV_EVENT_ALL, NULL);

  addch_help = lv_label_create(addch_scr);
  lv_obj_set_width(addch_help, 304);
  lv_label_set_long_mode(addch_help, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(addch_help, &rc_font_12, 0);
  lv_obj_set_style_text_color(addch_help, lv_color_hex(COL_MUTED), 0);
  lv_obj_set_x(addch_help, 0);

  lv_obj_t* create_btn = lv_button_create(addch_scr);
  lv_obj_set_size(create_btn, 148, 34);
  lv_obj_align(create_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(create_btn, addch_create_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* cl = lv_label_create(create_btn);
  lv_label_set_text(cl, LV_SYMBOL_OK " Create");
  lv_obj_center(cl);

  lv_obj_t* cancel_btn = lv_button_create(addch_scr);
  lv_obj_set_size(cancel_btn, 148, 34);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COL_CARD), 0);
  lv_obj_add_event_cb(cancel_btn, addch_cancel_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* xl = lv_label_create(cancel_btn);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE " Cancel");
  lv_obj_center(xl);

  addch_kb = lv_keyboard_create(addch_scr);
  lv_keyboard_set_textarea(addch_kb, NULL);
  kbAttachShiftBehavior(addch_kb);
  lv_obj_set_size(addch_kb, 320, KB_HEIGHT);
  lv_obj_align(addch_kb, LV_ALIGN_BOTTOM_MID, 0, 8);
  lv_obj_add_flag(addch_kb, LV_OBJ_FLAG_HIDDEN);
}
