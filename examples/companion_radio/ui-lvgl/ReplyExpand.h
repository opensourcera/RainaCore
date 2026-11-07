#pragma once

// Fills {variables} in a quick reply. Free of LVGL and mesh types so it can be
// tested on a PC.

#include <stddef.h>
#include <string.h>

struct ReplyVars {
  char name[33];    // who sent the packet
  char hops[8];     // repeaters it came through
  char path[48];    // those repeaters, e.g. a1>3f
  char snr[10];     // dB, last hop
  char rssi[10];    // dBm, last hop
  char time[8];     // local time it arrived
  char dist[14];    // distance to the sender
  char loc[26];     // my position
  char batt[8];     // my battery
  char me[33];      // my node name
};

struct ReplyVarDef {
  const char* key;
  size_t offset;
};

static const ReplyVarDef REPLY_VARS[] = {
  {"hops", offsetof(ReplyVars, hops)},
  {"snr",  offsetof(ReplyVars, snr)},
  {"rssi", offsetof(ReplyVars, rssi)},
  {"path", offsetof(ReplyVars, path)},
  {"name", offsetof(ReplyVars, name)},
  {"time", offsetof(ReplyVars, time)},
  {"dist", offsetof(ReplyVars, dist)},
  {"loc",  offsetof(ReplyVars, loc)},
  {"batt", offsetof(ReplyVars, batt)},
  {"me",   offsetof(ReplyVars, me)},
};
#define REPLY_VAR_COUNT (sizeof(REPLY_VARS) / sizeof(REPLY_VARS[0]))

// length of src that fits in room bytes without splitting a UTF-8 character
static inline size_t replyFit(const char* src, size_t room) {
  size_t n = strlen(src);
  if (n <= room) return n;
  n = room;
  while (n > 0 && ((unsigned char) src[n] & 0xC0) == 0x80) n--;
  return n;
}

// Unknown names stay as typed, so a plain "{" in a message is safe. Output is
// always terminated and never ends in part of a UTF-8 character.
static inline size_t replyExpand(const char* tmpl, const ReplyVars& v, char* out, size_t out_sz) {
  if (out_sz == 0) return 0;
  size_t used = 0, room = out_sz - 1;
  const char* p = tmpl;
  while (*p != 0 && used < room) {
    const char* val = NULL;
    size_t skip = 0;
    if (*p == '{') {
      const char* end = strchr(p, '}');
      if (end != NULL) {
        size_t klen = (size_t)(end - p - 1);
        for (size_t i = 0; i < REPLY_VAR_COUNT; i++) {
          if (strlen(REPLY_VARS[i].key) == klen && memcmp(REPLY_VARS[i].key, p + 1, klen) == 0) {
            val = (const char*) &v + REPLY_VARS[i].offset;
            skip = klen + 2;
            break;
          }
        }
      }
    }
    if (val != NULL) {
      size_t n = replyFit(val, room - used);
      memcpy(out + used, val, n);
      used += n;
      p += skip;
      if (val[n] != 0) break;   // value was cut, nothing after it fits either
    } else {
      size_t clen = 1;
      while ((((unsigned char) p[clen]) & 0xC0) == 0x80) clen++;
      if (used + clen > room) break;
      memcpy(out + used, p, clen);
      used += clen;
      p += clen;
    }
  }
  out[used] = 0;
  return used;
}
