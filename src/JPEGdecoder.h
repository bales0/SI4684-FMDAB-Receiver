#pragma once
#include <TFT_eSPI.h>

enum class JPEGPreflightResult : uint8_t {
  SupportedBaseline = 0,
  InvalidJpeg,
  UnsupportedProgressive,
  UnsupportedMultiscan,
  UnsupportedDimensions,
  UnsupportedComponents
};

struct JPEGImageInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t restartInterval = 0;
  uint16_t restartMarkers = 0;
  uint8_t components = 0;
  uint8_t maxHorizontalSampling = 0;
  uint8_t maxVerticalSampling = 0;
  uint8_t scans = 0;
  bool app0 = false;
  bool sof0 = false;
  bool sof2 = false;
  int16_t lastRenderedMcuRow = -1;
};

typedef void (*JPEGRowCallback)(void* context);

JPEGPreflightResult JPEGpreflight(const uint8_t* data, size_t size,
                                  int displayWidth, int displayHeight,
                                  JPEGImageInfo& info);
const char* JPEGpreflightName(JPEGPreflightResult result);

// Decode the complete entropy stream without touching TFT. The same fixed
// workspace is reused by the subsequent render pass.
bool JPEGvalidate(const uint8_t* data, size_t size,
                  int displayWidth, int displayHeight,
                  uint8_t* workspace, size_t workspaceSize,
                  JPEGImageInfo* info = nullptr,
                  JPEGRowCallback rowCallback = nullptr,
                  void* rowContext = nullptr);

// Decode a JPEG directly from RAM and render it. Baseline decoding uses safe
// end-of-scan handling to prevent bitstream read-ahead past the final MCU.
// Progressive streams are parsed separately and are not required by the DAB
// SlideShow Simple Profile.
bool JPEGdecoder(const uint8_t* data, size_t size, TFT_eSPI& tft,
                 int displayWidth = 320, int displayHeight = 240,
                 uint8_t* workspace = nullptr, size_t workspaceSize = 0,
                 JPEGImageInfo* info = nullptr,
                 JPEGRowCallback rowCallback = nullptr,
                 void* rowContext = nullptr);
