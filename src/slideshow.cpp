// RAM-only slideshow renderer.

#include "slideshow.h"

#include <esp_heap_caps.h>
#include <new>

// DAB SlideShow Simple Profile limits (ETSI TS 101 499): one JPEG/PNG image
// up to 50 KiB and display support through 320 x 240.  JPEG baseline support
// is mandatory; progressive/multiscan is optional and is deliberately not
// accepted by this resource-constrained receiver.
static constexpr size_t SLS_PROFILE_MAX_FILE_BYTES = 50U * 1024U;
static constexpr uint16_t SLS_PROFILE_MAX_WIDTH = 320U;
static constexpr uint16_t SLS_PROFILE_MAX_HEIGHT = 240U;

// ISO/IEC 10918 sequential interleaved scans are limited to 10 DCT blocks per
// MCU.  The largest row workspace for a supported 320-pixel baseline image is
// therefore 40 minimum-width MCUs * 10 blocks * 64 samples *
// (int16 coefficient + uint8 pixel) = 76,800 bytes.  PNGdec stores its zlib
// and line workspace inside the PNG object.  JPEG and PNG never decode at the
// same time, so a single early persistent arena can be shared by both.
static constexpr size_t JPEG_MAX_BLOCKS_PER_MCU = 10U;
static constexpr size_t JPEG_MIN_MCU_WIDTH = 8U;
static constexpr size_t JPEG_MAX_MCUS_PER_ROW =
    (SLS_PROFILE_MAX_WIDTH + JPEG_MIN_MCU_WIDTH - 1U) / JPEG_MIN_MCU_WIDTH;
static constexpr size_t JPEG_BYTES_PER_BLOCK =
    64U * (sizeof(int16_t) + sizeof(uint8_t));
static constexpr size_t JPEG_BASELINE_ROW_WORKSPACE =
    JPEG_MAX_MCUS_PER_ROW * JPEG_MAX_BLOCKS_PER_MCU * JPEG_BYTES_PER_BLOCK;
static constexpr size_t SLS_DECODER_WORKSPACE_BYTES =
    JPEG_BASELINE_ROW_WORKSPACE > sizeof(PNG)
        ? JPEG_BASELINE_ROW_WORKSPACE
        : sizeof(PNG);

static_assert(JPEG_BASELINE_ROW_WORKSPACE == 76800U,
              "Unexpected JPEG baseline workspace calculation");
static uint8_t* decoderWorkspace = nullptr;
static bool decoderWorkspaceAttempted = false;

bool SlideshowPrepareWorkspace(void) {
  if (decoderWorkspace) return true;
  if (decoderWorkspaceAttempted) return false;
  decoderWorkspaceAttempted = true;

  decoderWorkspace = static_cast<uint8_t*>(heap_caps_malloc(
      SLS_DECODER_WORKSPACE_BYTES,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  if (!decoderWorkspace) {
    DIAG_PRINTF("[SLS/WS] shared decoder workspace allocation FAILED (%u bytes)\n",
                  static_cast<unsigned>(SLS_DECODER_WORKSPACE_BYTES));
    return false;
  }

  DIAG_PRINTF("[SLS/WS] shared decoder workspace=%u bytes PNG-object=%u addr=%p "
                "free=%u largest=%u\n",
                static_cast<unsigned>(SLS_DECODER_WORKSPACE_BYTES),
                static_cast<unsigned>(sizeof(PNG)),
                decoderWorkspace,
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return true;
}

static PNG* acquirePngDecoder(bool& inSharedWorkspace) {
  if (!decoderWorkspace || sizeof(PNG) > SLS_DECODER_WORKSPACE_BYTES) {
    inSharedWorkspace = false;
    DIAG_PRINTLN("[SLS/PNG] shared decoder workspace unavailable");
    return nullptr;
  }
  inSharedWorkspace = true;
  return new (decoderWorkspace) PNG;
}

static void releasePngDecoder(PNG* decoder, bool inSharedWorkspace) {
  if (decoder && inSharedWorkspace) decoder->~PNG();
}

static void fadeDown(void) {
  for (int x = ContrastSet; x > 0; --x) {
    analogWrite(CONTRASTPIN, x * 2);
    delay(5);
  }
  analogWrite(CONTRASTPIN, 0);
}

static void fadeUp(void) {
  for (int x = 0; x <= ContrastSet; ++x) {
    analogWrite(CONTRASTPIN, x * 2 + 27);
    delay(5);
  }
}


bool ShowSlideShow(void) {
  if (diagnosticDebug) DIAG_PRINTLN("[SLS] ShowSlideShow() called");

  // The decoder arena is mandatory and is reserved once during setup().
  if (!decoderWorkspace) {
    DIAG_PRINTLN("[SLS] display aborted: decoder workspace unavailable");
    return false;
  }

  const uint8_t* image = radio.slideshowData();
  const uint32_t fileSize = radio.slideshowSize();
  if (diagnosticDebug)
    DIAG_PRINTF("[SLS] image ptr=%p size=%u available=%u update=%u\n",
                  image, fileSize, radio.SlideShowAvailable, radio.SlideShowUpdate);

  if (!image || fileSize < 8) {
    DIAG_PRINTLN("[SLS] display aborted: no complete image");
    return false;
  }
  if (fileSize > SLS_PROFILE_MAX_FILE_BYTES) {
    DIAG_PRINTF("[SLS] image exceeds Simple Profile limit size=%u max=%u\n",
                  static_cast<unsigned>(fileSize),
                  static_cast<unsigned>(SLS_PROFILE_MAX_FILE_BYTES));
    return false;
  }

  bool isJPG = image[0] == 0xFF && image[1] == 0xD8 && image[2] == 0xFF;
  bool isPNG = image[0] == 0x89 && image[1] == 0x50 && image[2] == 0x4E &&
               image[3] == 0x47 && image[4] == 0x0D && image[5] == 0x0A &&
               image[6] == 0x1A && image[7] == 0x0A;

  if (diagnosticDebug)
    DIAG_PRINTF("[SLS] Display: size=%u isJPG=%u isPNG=%u hdr=%02X %02X %02X %02X tail=%02X %02X\n",
                  fileSize, isJPG, isPNG,
                  image[0], image[1], image[2], image[3],
                  image[fileSize - 2], image[fileSize - 1]);

  if (isJPG) {
    fadeDown();
    tft.fillScreen(TFT_BLACK);
    tft.startWrite();
    bool ok = JPEGdecoder(image, fileSize, tft, SLS_PROFILE_MAX_WIDTH,
                          SLS_PROFILE_MAX_HEIGHT,
                          decoderWorkspace,
                          decoderWorkspace ? SLS_DECODER_WORKSPACE_BYTES : 0);
    tft.endWrite();

    DIAG_PRINTF("[SLS/JPEG] render=%s size=%u\n",
                  ok ? "OK" : "FAIL", static_cast<unsigned>(fileSize));
    fadeUp();
    return ok;
  }

  if (isPNG) {
    // PNG IHDR starts at byte 8 and stores width/height as big-endian uint32
    // values at offsets 16 and 20.  Reject unsupported dimensions before the
    // decoder touches the image.  Simple Profile permits larger slides to be
    // ignored by a 320 x 240 receiver.
    if (fileSize < 24U) {
      DIAG_PRINTLN("[SLS/PNG] truncated IHDR");
      return false;
    }
    const uint32_t pngHeaderWidth =
        (static_cast<uint32_t>(image[16]) << 24) |
        (static_cast<uint32_t>(image[17]) << 16) |
        (static_cast<uint32_t>(image[18]) << 8) |
        static_cast<uint32_t>(image[19]);
    const uint32_t pngHeaderHeight =
        (static_cast<uint32_t>(image[20]) << 24) |
        (static_cast<uint32_t>(image[21]) << 16) |
        (static_cast<uint32_t>(image[22]) << 8) |
        static_cast<uint32_t>(image[23]);
    if (pngHeaderWidth == 0U || pngHeaderHeight == 0U ||
        pngHeaderWidth > SLS_PROFILE_MAX_WIDTH ||
        pngHeaderHeight > SLS_PROFILE_MAX_HEIGHT) {
      DIAG_PRINTF("[SLS/PNG] unsupported dimensions=%ux%u max=%ux%u\n",
                    static_cast<unsigned>(pngHeaderWidth),
                    static_cast<unsigned>(pngHeaderHeight),
                    static_cast<unsigned>(SLS_PROFILE_MAX_WIDTH),
                    static_cast<unsigned>(SLS_PROFILE_MAX_HEIGHT));
      return false;
    }

    bool pngInSharedWorkspace = false;
    PNG* png = acquirePngDecoder(pngInSharedWorkspace);
    if (!png) return false;

    fadeDown();
    int16_t rc = png->openRAM(const_cast<uint8_t*>(image), fileSize,
      +[](PNGDRAW *pDraw) {
        if (!pDraw || !pDraw->pUser ||
            pDraw->iWidth <= 0 || pDraw->iWidth > SLS_PROFILE_MAX_WIDTH) {
          return 0;
        }

        PNG* decoder = static_cast<PNG*>(pDraw->pUser);
        uint32_t pngBkgd = decoder->hasAlpha() ? 0x00FFFFFF : 0xFFFFFFFF;
        uint16_t lineBuffer[SLS_PROFILE_MAX_WIDTH];
        decoder->getLineAsRGB565(pDraw, lineBuffer,
                                 PNG_RGB565_LITTLE_ENDIAN, pngBkgd);
        tft.pushImage((SLS_PROFILE_MAX_WIDTH - decoder->getWidth()) / 2,
                      ((SLS_PROFILE_MAX_HEIGHT - decoder->getHeight()) / 2) + pDraw->y,
                      pDraw->iWidth, 1, lineBuffer);
        return 1;
      });

    if (diagnosticDebug) DIAG_PRINTF("[SLS/PNG] openRAM rc=%d\n", rc);
    if (rc != PNG_SUCCESS) {
      releasePngDecoder(png, pngInSharedWorkspace);
      fadeUp();
      return false;
    }

    const int pngWidth = png->getWidth();
    const int pngHeight = png->getHeight();
    if (pngWidth <= 0 || pngWidth > SLS_PROFILE_MAX_WIDTH ||
        pngHeight <= 0 || pngHeight > SLS_PROFILE_MAX_HEIGHT) {
      DIAG_PRINTF("[SLS/PNG] unsupported dimensions=%dx%d\n",
                    pngWidth, pngHeight);
      png->close();
      releasePngDecoder(png, pngInSharedWorkspace);
      fadeUp();
      return false;
    }

    if (diagnosticDebug)
      DIAG_PRINTF("[SLS/PNG] dimensions=%dx%d alpha=%u\n",
                    pngWidth, pngHeight, png->hasAlpha());
    tft.fillScreen(png->hasAlpha() ? TFT_WHITE : TFT_BLACK);
    tft.startWrite();
    rc = png->decode(png, 0);
    tft.endWrite();
    DIAG_PRINTF("[SLS/PNG] render=%s size=%u\n",
                  rc == PNG_SUCCESS ? "OK" : "FAIL",
                  static_cast<unsigned>(fileSize));
    png->close();
    releasePngDecoder(png, pngInSharedWorkspace);
    fadeUp();
    return rc == PNG_SUCCESS;
  }

  DIAG_PRINTLN("[SLS] unsupported/invalid image signature");
  return false;
}
