#pragma once
#include <TFT_eSPI.h>

// Decode a JPEG directly from RAM and render it. Baseline decoding uses safe
// end-of-scan handling to prevent bitstream read-ahead past the final MCU.
// Progressive streams are parsed separately and are not required by the DAB
// SlideShow Simple Profile.
bool JPEGdecoder(const uint8_t* data, size_t size, TFT_eSPI& tft,
                 int displayWidth = 320, int displayHeight = 240,
                 uint8_t* workspace = nullptr, size_t workspaceSize = 0);
