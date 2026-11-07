# PC bench

Runs the real `MapView.cpp`, `UIRepeaterList.cpp` and `UIPlaces.cpp` on a PC with
real LVGL, the firmware fonts and made-up tiles. A script taps and drags, checks
the results and saves screenshots as `sheet.png`. No board is needed.

Windows with MinGW g++ and Python with Pillow. Run a firmware build once first,
so that PlatformIO has fetched LVGL.

    python gen_tiles.py
    python build.py

The contacts, places and tiles are made up.
