#pragma once
#include <TFT_eSPI.h>

// Decode a JPEG (baseline or progressive) directly from RAM and render it.
// This is the original d64ecd4 decoder path, with one surgical baseline
// end-of-scan read-ahead fix to prevent the historical gray final MCU block.
// Progressive decoding is intentionally left on the original code path.
bool JPEGdecoder(const uint8_t* data, size_t size, TFT_eSPI& tft,
                 int displayWidth = 320, int displayHeight = 240);
