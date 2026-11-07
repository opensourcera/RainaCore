#pragma once

#include <stddef.h>

#define PLACE_NAME_MAX 48

// Reads a Nominatim jsonv2 reply into "Dallas, TX" or "Paris, Ile-de-France".
bool placeParseReply(const char* json, char* out, size_t out_sz);
void placeAdd(double lat, double lon, const char* name);
bool placeName(double lat, double lon, char* out, size_t out_sz);   // false when none is near
