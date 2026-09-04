// RAM-only slideshow renderer.

#include "slideshow.h"

PNG png;

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
    bool ok = JPEGdecoder(image, fileSize, tft);
    tft.endWrite();

    Serial.printf("[SLS/JPEG] JPEGdecoder result=%s\n", ok ? "OK" : "FAIL");
    fadeUp();
    return ok;
  }

  if (isPNG) {
    fadeDown();
    int16_t rc = png.openRAM(const_cast<uint8_t*>(image), fileSize,
      +[](PNGDRAW *pDraw) {
        if (!pDraw || pDraw->iWidth <= 0 || pDraw->iWidth > 320) {
          return 0;
        }
        static uint32_t pngBkgd;
        pngBkgd = png.hasAlpha() ? 0x00FFFFFF : 0xFFFFFFFF;
        uint16_t lineBuffer[320];
        png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_LITTLE_ENDIAN, pngBkgd);
        tft.pushImage((320 - png.getWidth()) / 2,
                      ((240 - png.getHeight()) / 2) + pDraw->y,
                      pDraw->iWidth, 1, lineBuffer);
        return 1;
      });

    Serial.printf("[SLS/PNG] openRAM rc=%d\n", rc);
    if (rc != PNG_SUCCESS) {
      fadeUp();
      return false;
    }

    const int pngWidth = png.getWidth();
    const int pngHeight = png.getHeight();
    if (pngWidth <= 0 || pngWidth > 320 ||
        pngHeight <= 0 || pngHeight > 240) {
      Serial.printf("[SLS/PNG] unsupported dimensions=%dx%d\n",
                    pngWidth, pngHeight);
      png.close();
      fadeUp();
      return false;
    }

    Serial.printf("[SLS/PNG] dimensions=%dx%d alpha=%u\n",
                  pngWidth, pngHeight, png.hasAlpha());
    tft.fillScreen(png.hasAlpha() ? TFT_WHITE : TFT_BLACK);
    tft.startWrite();
    rc = png.decode(nullptr, 0);
    tft.endWrite();
    Serial.printf("[SLS/PNG] decode rc=%d\n", rc);
    png.close();
    fadeUp();
    return rc == PNG_SUCCESS;
  }

  Serial.println("[SLS] unsupported/invalid image signature");
  return false;
}
