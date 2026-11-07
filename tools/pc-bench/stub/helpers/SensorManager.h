#pragma once
struct LocationProvider { bool isValid() { return true; } long getLatitude() { return 37771000; } long getLongitude() { return -122417000; } };
struct SensorManager { LocationProvider loc; LocationProvider* getLocationProvider() { return &loc; } };
