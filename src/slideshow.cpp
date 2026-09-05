// RAM-only slideshow renderer.

#include "slideshow.h"
#include <esp_heap_caps.h>
#include <new>

// PNGdec keeps its complete zlib/line workspace inside the PNG object.
// JPEG and PNG are never decoded at the same time, so both decoders share one
// persistent 64 kB arena instead of repeatedly allocating JPEG scratch blocks
// beside a permanently resident PNG object.
static constexpr size_t SLS_DECODER_WORKSPACE_BYTES = 64U * 1024U;
static uint8_t* decoderWorkspace = nullptr;
static bool decoderWorkspaceAttempted = false;

// Only used if the shared 64 kB reservation cannot be made. It is allocated
// once and retained, so even the fallback PNG path does not malloc/free for
// every image.
static PNG* fallbackPng = nullptr;

bool SlideshowPrepareWorkspace(void) {
  if (decoderWorkspace) return true;
  if (decoderWorkspaceAttempted) return false;
  decoderWorkspaceAttempted = true;

  decoderWorkspace = static_cast<uint8_t*>(heap_caps_malloc(
      SLS_DECODER_WORKSPACE_BYTES,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  if (!decoderWorkspace) {
    Serial.printf("[SLS/WS] shared decoder workspace allocation FAILED (%u bytes); "
                  "compatibility fallbacks remain enabled\n",
                  static_cast<unsigned>(SLS_DECODER_WORKSPACE_BYTES));
    return false;
  }

  Serial.printf("[SLS/WS] shared decoder workspace=%u bytes PNG-object=%u addr=%p "
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
  if (decoderWorkspace && sizeof(PNG) <= SLS_DECODER_WORKSPACE_BYTES) {
    inSharedWorkspace = true;
    return new (decoderWorkspace) PNG;
  }

  inSharedWorkspace = false;
  if (!fallbackPng) {
    fallbackPng = new (std::nothrow) PNG;
    if (fallbackPng) {
      Serial.printf("[SLS/PNG] using persistent fallback PNG object=%u bytes\n",
                    static_cast<unsigned>(sizeof(PNG)));
    } else {
      Serial.println("[SLS/PNG] fallback PNG object allocation FAILED");
    }
  }
  return fallbackPng;
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
  Serial.println("[SLS] ShowSlideShow() called");

  // Normally reserved once from setup(), after the other major permanent
  // allocations. Keep lazy preparation as a safety net for alternate callers.
  if (!decoderWorkspace && !decoderWorkspaceAttempted) {
    SlideshowPrepareWorkspace();
  }

  const uint8_t* image = radio.slideshowData();
  const uint32_t fileSize = radio.slideshowSize();
  Serial.printf("[SLS] image ptr=%p size=%u available=%u update=%u\n",
                image, fileSize, radio.SlideShowAvailable, radio.SlideShowUpdate);

  if (!image || fileSize < 8) {
    Serial.println("[SLS] display aborted: no complete image");
    return false;
  }

  bool isJPG = image[0] == 0xFF && image[1] == 0xD8 && image[2] == 0xFF;
  bool isPNG = image[0] == 0x89 && image[1] == 0x50 && image[2] == 0x4E &&
               image[3] == 0x47 && image[4] == 0x0D && image[5] == 0x0A &&
               image[6] == 0x1A && image[7] == 0x0A;

  Serial.printf("[SLS] Display: size=%u isJPG=%u isPNG=%u hdr=%02X %02X %02X %02X tail=%02X %02X\n",
                fileSize, isJPG, isPNG,
                image[0], image[1], image[2], image[3],
                image[fileSize - 2], image[fileSize - 1]);

  if (isJPG) {
    fadeDown();
    tft.fillScreen(TFT_BLACK);
    tft.startWrite();
    bool ok = JPEGdecoder(image, fileSize, tft, 320, 240,
                          decoderWorkspace,
                          decoderWorkspace ? SLS_DECODER_WORKSPACE_BYTES : 0);
    tft.endWrite();

    Serial.printf("[SLS/JPEG] JPEGdecoder result=%s\n", ok ? "OK" : "FAIL");
    fadeUp();
    return ok;
  }

  if (isPNG) {
    bool pngInSharedWorkspace = false;
    PNG* png = acquirePngDecoder(pngInSharedWorkspace);
    if (!png) return false;

    fadeDown();
    int16_t rc = png->openRAM(const_cast<uint8_t*>(image), fileSize,
      +[](PNGDRAW *pDraw) {
        if (!pDraw || !pDraw->pUser ||
            pDraw->iWidth <= 0 || pDraw->iWidth > 320) {
          return 0;
        }

        PNG* decoder = static_cast<PNG*>(pDraw->pUser);
        uint32_t pngBkgd = decoder->hasAlpha() ? 0x00FFFFFF : 0xFFFFFFFF;
        uint16_t lineBuffer[320];
        decoder->getLineAsRGB565(pDraw, lineBuffer,
                                 PNG_RGB565_LITTLE_ENDIAN, pngBkgd);
        tft.pushImage((320 - decoder->getWidth()) / 2,
                      ((240 - decoder->getHeight()) / 2) + pDraw->y,
                      pDraw->iWidth, 1, lineBuffer);
        return 1;
      });

    Serial.printf("[SLS/PNG] openRAM rc=%d\n", rc);
    if (rc != PNG_SUCCESS) {
      releasePngDecoder(png, pngInSharedWorkspace);
      fadeUp();
      return false;
    }

    const int pngWidth = png->getWidth();
    const int pngHeight = png->getHeight();
    if (pngWidth <= 0 || pngWidth > 320 ||
        pngHeight <= 0 || pngHeight > 240) {
      Serial.printf("[SLS/PNG] unsupported dimensions=%dx%d\n",
                    pngWidth, pngHeight);
      png->close();
      releasePngDecoder(png, pngInSharedWorkspace);
      fadeUp();
      return false;
    }

    Serial.printf("[SLS/PNG] dimensions=%dx%d alpha=%u\n",
                  pngWidth, pngHeight, png->hasAlpha());
    tft.fillScreen(png->hasAlpha() ? TFT_WHITE : TFT_BLACK);
    tft.startWrite();
    rc = png->decode(png, 0);
    tft.endWrite();
    Serial.printf("[SLS/PNG] decode rc=%d\n", rc);
    png->close();
    releasePngDecoder(png, pngInSharedWorkspace);
    fadeUp();
    return rc == PNG_SUCCESS;
  }

  Serial.println("[SLS] unsupported/invalid image signature");
  return false;
}
