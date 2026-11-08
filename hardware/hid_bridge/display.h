// Optional status screen for boards with a built-in LCD (M5 StickS3: flash.sh --sticks3 sets -DHAS_LCD=1).
// Everything lives in display.cpp and only reads the other modules' public getters, so upstream files need
// just one call in setup(). Without HAS_LCD this header is a no-op and display.cpp compiles to nothing.
#pragma once

#if HAS_LCD
void display_begin();  // starts its own task: panel init, KEY1 = next page, KEY2 = show/hide the Wi-Fi password
#else
inline void display_begin() {}
#endif
