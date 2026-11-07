#pragma once
#include <vector>
#include <string.h>
#define MAX_ANON_CONTACTS 0
#define ADV_TYPE_CHAT 1
#define ADV_TYPE_REPEATER 2
#define OUT_PATH_UNKNOWN 0xFF
struct Ident { uint8_t pub_key[32]; };
struct ContactInfo { Ident id; char name[32]; uint8_t type; uint8_t out_path_len; uint32_t last_advert_timestamp; int32_t gps_lat, gps_lon; };
struct AdvertPath { uint8_t pubkey_prefix[7]; char name[32]; uint32_t recv_timestamp; uint8_t path_len; };
struct Mesh { std::vector<ContactInfo> c; std::vector<AdvertPath> h;
  int getTotalContactSlots() { return (int) c.size(); }
  bool getContactByIdx(int i, ContactInfo& o) { if (i < 0 || i >= (int) c.size()) return false; o = c[i]; return true; }
  int getRecentlyHeard(AdvertPath d[], int m) { int n = 0; for (auto& a : h) if (n < m) d[n++] = a; return n; } };
extern Mesh the_mesh;
struct NodePrefs { double node_lat, node_lon; };
struct RtcStub { uint32_t getCurrentTime() { return 1800000000u; } };
extern RtcStub rtc_clock;
