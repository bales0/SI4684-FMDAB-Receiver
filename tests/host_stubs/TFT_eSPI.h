#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Minimal host sink for compiling the production JPEG decoder unchanged.
class TFT_eSPI {
 public:
  bool valid = true;
  int pushedRows = 0;
  int maximumWidth = 0;
  uint32_t pixelHash = 2166136261U;

  void pushImage(int32_t, int32_t, int32_t width, int32_t height,
                 const uint16_t* pixels) {
    if (!pixels || width <= 0 || width > 320 || height != 1) {
      valid = false;
      return;
    }
    maximumWidth = std::max(maximumWidth, static_cast<int>(width));
    ++pushedRows;
    for (int32_t x = 0; x < width; ++x) {
      pixelHash ^= pixels[x];
      pixelHash *= 16777619U;
    }
  }
};
