#pragma once

// Channel links in the format the MeshCore app reads (docs/qr_codes.md). Free
// of LVGL and mesh types so it can be tested on a PC.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHANNEL_KEY_LEN 16

// meshcore://channel/add?name=<url-encoded>&secret=<32 hex>. Returns false if
// it does not fit, so a cut-off link can never be shown.
static inline bool channelShareUrl(const char* name, const uint8_t* secret, char* out, size_t sz) {
  static const char hex[] = "0123456789abcdef";
  size_t o = (size_t) snprintf(out, sz, "meshcore://channel/add?name=");
  if (o >= sz) return false;
  for (const unsigned char* p = (const unsigned char*) name; *p != 0; p++) {
    bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')
              || *p == '-' || *p == '_' || *p == '.' || *p == '~';
    if (o + 4 >= sz) return false;
    if (plain) {
      out[o++] = (char) *p;
    } else {
      out[o++] = '%';
      out[o++] = hex[*p >> 4];
      out[o++] = hex[*p & 15];
    }
  }
  int n = snprintf(&out[o], sz - o, "&secret=");
  if (n < 0 || (size_t) n >= sz - o) return false;
  o += (size_t) n;
  if (o + CHANNEL_KEY_LEN * 2 + 1 > sz) return false;
  for (int i = 0; i < CHANNEL_KEY_LEN; i++) {
    out[o++] = hex[secret[i] >> 4];
    out[o++] = hex[secret[i] & 15];
  }
  out[o] = 0;
  return true;
}

static inline int channelHexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// reads a key typed as 32 hex characters; spaces between groups are allowed
static inline bool channelHexKey(const char* text, uint8_t* out) {
  int digits = 0, high = 0;
  for (const char* p = text; *p != 0; p++) {
    if (*p == ' ') continue;
    int v = channelHexVal(*p);
    if (v < 0 || digits >= CHANNEL_KEY_LEN * 2) return false;
    if (digits % 2 == 0) high = v;
    else out[digits / 2] = (uint8_t)(high << 4 | v);
    digits++;
  }
  return digits == CHANNEL_KEY_LEN * 2;
}
