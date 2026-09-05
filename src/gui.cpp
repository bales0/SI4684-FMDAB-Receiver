// All TFT rendering. Each Show*() function updates one piece of the screen;
// they only redraw what has actually changed by comparing against a "*Old"
// shadow variable, so the loop can call them all every iteration cheaply.
// BuildDisplay() / BuildChannelList() / BuildMenu() do the full redraws
// when the active view switches.

#include "gui.h"
#include "ir_remote.h"

// Defined by the FM RDS decoder in si4684.cpp. PTY=0 is a real value, so
// this flag distinguishes "not received yet" from a valid PTY 0 (Unknown).
extern bool fmPtyValid;
extern uint8_t DabServiceLabelCharset(uint8_t serviceIndex);
extern uint8_t DabDynamicLabelCharsetValue(void);
extern uint8_t DabDynamicLabelLengthValue(void);
extern void DabDynamicLabelTextToBuffer(const char* input, char* output, size_t outputSize);

byte menuitem;                    // logical item index in Settings
static byte menuFirstItem = 0;      // first logical item shown in 9-row window
static constexpr byte MENU_ITEM_COUNT = 11;
static constexpr byte MENU_VISIBLE_ROWS = 9;

// Apply the user-selected colour theme to the global PrimaryColor/etc. used
// by every draw routine. Use the linked RGB565 picker to author new themes.
void doTheme(void) {  // Use this to put your own colors in: http://www.barth-dev.de/online/rgb565-color-picker/
  switch (CurrentTheme) {
    case 0:  // Elegant
      PrimaryColor = 0x2e65;
      PrimaryColorSmooth = 0x09a1;
      SecondaryColor = 0x3d7d;
      SecondaryColorSmooth = 0x08e5;
      GreyoutColor = 0x5b0d;
      BackgroundColor = 0x0063;
      BackgroundColor2 = 0x016a;
      BackgroundColor3 = 0x0107;
      BackgroundColor4 = 0x00c6;
      ActiveColor = 0xFFFF;
      ActiveColorSmooth = 0x18E3;
      SignificantColor = 0xF800;
      SignificantColorSmooth = 0x2000;
      InsignificantColor = 0x07E0;
      InsignificantColorSmooth = 0x00C0;
      BarSignificantColor = 0xF800;
      BarInsignificantColor = 0x051f;
      break;

    case 1: // GoldenDusk
      PrimaryColor = 0x8ff1;
      PrimaryColorSmooth = 0x10c2;
      SecondaryColor = 0xFFE0;
      SecondaryColorSmooth = 0x2120;
      GreyoutColor = 0x5b0d;
      BackgroundColor = 0x016b;
      BackgroundColor2 = 0x016a;
      BackgroundColor3 = 0x0107;
      BackgroundColor4 = 0x00c6;
      ActiveColor = 0xFFFF;
      ActiveColorSmooth = 0x18E3;
      SignificantColor = 0xF800;
      SignificantColorSmooth = 0x2000;
      InsignificantColor = 0x07E0;
      InsignificantColorSmooth = 0x00C0;
      BarSignificantColor = 0xF800;
      BarInsignificantColor = 0x07E0;
      break;

    case 2: // Vibrant
      PrimaryColor = 0xF00A;
      PrimaryColorSmooth = 0x3800;
      SecondaryColor = 0xFFE0;
      SecondaryColorSmooth = 0x2120;
      GreyoutColor = 0x5b0d;
      BackgroundColor = 0x016b;
      BackgroundColor2 = 0x016a;
      BackgroundColor3 = 0x0107;
      BackgroundColor4 = 0x00c6;
      ActiveColor = 0x051F;
      ActiveColorSmooth = 0x0106;
      SignificantColor = 0xF3D5;
      SignificantColorSmooth = 0x3008;
      InsignificantColor = 0x07E0;
      InsignificantColorSmooth = 0x00C0;
      BarSignificantColor = 0xF3D5;
      BarInsignificantColor = 0x07E0;
      break;

    case 3: // Serenity
      PrimaryColor = 0xF3D5;
      PrimaryColorSmooth = 0x3008;
      SecondaryColor = 0x9C96;
      SecondaryColorSmooth = 0x41C8;
      GreyoutColor = 0x5b0d;
      BackgroundColor = 0x016b;
      BackgroundColor2 = 0x016a;
      BackgroundColor3 = 0x0107;
      BackgroundColor4 = 0x00c6;
      ActiveColor = 0x9B8D;
      ActiveColorSmooth = 0x5207;
      SignificantColor = 0x748E;
      SignificantColorSmooth = 0x3206;
      InsignificantColor = 0x9B90;
      InsignificantColorSmooth = 0x3946;
      BarSignificantColor = 0x748E;
      BarInsignificantColor = 0xF3D5;
      break;

    case 4: // Luminous
      PrimaryColor = 0x051F;
      PrimaryColorSmooth = 0x0106;
      SecondaryColor = 0xFA8D;
      SecondaryColorSmooth = 0x3083;
      GreyoutColor = 0x5b0d;
      BackgroundColor = 0x016b;
      BackgroundColor2 = 0x016a;
      BackgroundColor3 = 0x0107;
      BackgroundColor4 = 0x00c6;
      ActiveColor = 0xFC00;
      ActiveColorSmooth = 0x3165;
      SignificantColor = 0xFFE0;
      SignificantColorSmooth = 0x2120;
      InsignificantColor = 0xF8C3;
      InsignificantColorSmooth = 0x3800;
      BarSignificantColor = 0xFFE0;
      BarInsignificantColor = 0x867D;
      break;

    case 5: // Radiant
      PrimaryColor = 0x051F;
      PrimaryColorSmooth = 0x0106;
      SecondaryColor = 0xFFE0;
      SecondaryColorSmooth = 0x2120;
      GreyoutColor = 0x5b0d;
      BackgroundColor = 0x016b;
      BackgroundColor2 = 0x016a;
      BackgroundColor3 = 0x0107;
      BackgroundColor4 = 0x00c6;
      ActiveColor = 0xFC00;
      ActiveColorSmooth = 0x3165;
      SignificantColor = 0xFFE0;
      SignificantColorSmooth = 0x2120;
      InsignificantColor = 0xF8C3;
      InsignificantColorSmooth = 0x3800;
      BarSignificantColor = 0xFFE0;
      BarInsignificantColor = 0x867D;
      break;

    case 6: // Sunset
      PrimaryColor = 0x051F;
      PrimaryColorSmooth = 0x0106;
      SecondaryColor = 0xFFE0;
      SecondaryColorSmooth = 0x2120;
      GreyoutColor = 0x5b0d;
      BackgroundColor = 0x016b;
      BackgroundColor2 = 0x016a;
      BackgroundColor3 = 0x0107;
      BackgroundColor4 = 0x00c6;
      ActiveColor = 0xED20;
      ActiveColorSmooth = 0x3940;
      SignificantColor = 0xF800;
      SignificantColorSmooth = 0x2000;
      InsignificantColor = 0x07E0;
      InsignificantColorSmooth = 0x00C0;
      BarSignificantColor = 0xF800;
      BarInsignificantColor = 0x07E0;
      break;
  }
}

// Full-screen overlay listing every metadata field for the current service
// (chip, firmware, ensemble, ECC, PTY, audio mode, bitrate, sample rate).
void ShowServiceInfo(void) {
  setvolume = false;
  displayreset = true;
  tft.pushImage(0, 0, 320, 240, serviceinfobackground);
  tftPrintFixed(0, myLanguage[language][27], 155, 4,
                ActiveColor, ActiveColorSmooth, 28);

  char value[80];

  if (radio.isFm()) {
    const char* labels[] = {
        myLanguage[language][28], myLanguage[language][36], fmPsText[language],
        myLanguage[language][31], fmPiText[language], fmMultipathText[language],
        fmStereoBlendText[language], myLanguage[language][35],
        radio.isRbds() ? fmRbdsText[language] : fmRdsText[language]};
    for (uint8_t i = 0; i < 9; ++i)
      tftPrintFixed(-1, labels[i], 8, 36 + i * 20,
                    ActiveColor, ActiveColorSmooth, 16);

    snprintf(value, sizeof(value), "%u.%u MHz",
             static_cast<unsigned>(fmfreq / 100),
             static_cast<unsigned>((fmfreq % 100) / 10));
    tftPrintFixed(-1, value, 166, 36, PrimaryColor, PrimaryColorSmooth, 16);

    if (radio.fmAfcRail) {
      snprintf(value, sizeof(value), "%d/%d dB %s",
               static_cast<int>(radio.fmRssi), static_cast<int>(radio.fmSnr),
               fmAfcRailText[language]);
    } else {
      snprintf(value, sizeof(value), "%d/%d dB",
               static_cast<int>(radio.fmRssi), static_cast<int>(radio.fmSnr));
    }
    tftPrintFixed(-1, value, 166, 56, PrimaryColor, PrimaryColorSmooth, 16);

    const char* ps = PSold[0] != '\0' ? PSold : "--------";
    tftPrintFixed(-1, ps, 166, 76, PrimaryColor, PrimaryColorSmooth, 16);

    value[0] = '\0';
    if (!tuning && fmPtyValid && radio.fmPty > 0 && radio.fmPty <= 31) {
      snprintf(value, sizeof(value), "%u: %s",
               static_cast<unsigned>(radio.fmPty),
               myLanguage[language][37 + radio.fmPty]);
    }
    tftPrintFixed(-1, value, 166, 96, PrimaryColor, PrimaryColorSmooth, 16);

    if (radio.fmPi) {
      snprintf(value, sizeof(value), "%04X", static_cast<unsigned>(radio.fmPi));
      tftPrintFixed(-1, value, 166, 116, PrimaryColor, PrimaryColorSmooth, 16);
    } else {
      tftPrintFixed(-1, "-", 166, 116, PrimaryColor, PrimaryColorSmooth, 16);
    }

    snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(radio.fmMultipath));
    tftPrintFixed(-1, value, 166, 136, PrimaryColor, PrimaryColorSmooth, 16);

    snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(radio.fmStereoBlend));
    tftPrintFixed(-1, value, 166, 156, PrimaryColor, PrimaryColorSmooth, 16);

    tftPrintFixed(-1, radio.fmPilot ? fmStereoText[language] : fmMonoText[language],
                  166, 176, PrimaryColor, PrimaryColorSmooth, 16);
    tftPrintFixed(-1, radio.fmPi ? myLanguage[language][23] : "-",
                  166, 196, PrimaryColor, PrimaryColorSmooth, 16);
    return;
  }

  tftPrintFixed(-1, myLanguage[language][28], 8, 36, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][36], 8, 56, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][29], 8, 76, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][30], 8, 96, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][31], 8, 116, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][32], 8, 136, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][33], 8, 156, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][34], 8, 176, ActiveColor, ActiveColorSmooth, 16);
  tftPrintFixed(-1, myLanguage[language][35], 8, 196, ActiveColor, ActiveColorSmooth, 16);

  const uint32_t dabFrequency = radio.getFreq(dabfreq);
  snprintf(value, sizeof(value), "%s - %lu.%03lu MHz",
           radio.getChannel(dabfreq),
           static_cast<unsigned long>(dabFrequency / 1000UL),
           static_cast<unsigned long>(dabFrequency % 1000UL));
  tftPrintFixed(-1, value, 166, 36, PrimaryColor, PrimaryColorSmooth, 16);

  snprintf(value, sizeof(value), "%s  MER:", unitString[unit]);
  tftPrintFixed(-1, value, 193, 56, PrimaryColor, PrimaryColorSmooth, 16);
  tftPrintFixed(-1, "dB", 286, 56, PrimaryColor, PrimaryColorSmooth, 16);

  // Reuse the already converted/cached main-screen labels here. Opening the
  // information page must not create another set of temporary Arduino Strings.
  tftPrintFixed(-1, EnsembleNameOld, 166, 76,
                PrimaryColor, PrimaryColorSmooth, 16);
  tftPrintFixed(-1, PSold, 166, 96,
                PrimaryColor, PrimaryColorSmooth, 16);

  value[0] = '\0';
  if (radio.ServiceStart && radio.pty > 0 && radio.pty <= 29) {
    snprintf(value, sizeof(value), "%u: %s",
             static_cast<unsigned>(radio.pty),
             myLanguage[language][37 + radio.pty]);
  }
  tftPrintFixed(-1, value, 166, 116, PrimaryColor, PrimaryColorSmooth, 16);

  const char* protectionInfo =
      (radio.ServiceStart && radio.protectionlevel > 0 && radio.protectionlevel < 14)
          ? ProtectionText[radio.protectionlevel]
          : "";
  tftPrintFixed(-1, protectionInfo, 166, 136,
                PrimaryColor, PrimaryColorSmooth, 16);

  value[0] = '\0';
  if (radio.ServiceStart && radio.samplerate != 0) {
    char sampleRate[16];
    snprintf(sampleRate, sizeof(sampleRate), "%u",
             static_cast<unsigned>(radio.samplerate));
    const size_t sampleRateLen = strlen(sampleRate);
    if (sampleRateLen > 2 && sampleRateLen + 1 < sizeof(sampleRate)) {
      // Preserve the original display format exactly: insert a decimal point
      // after the first two digits (48000 -> 48.000).
      memmove(sampleRate + 3, sampleRate + 2, sampleRateLen - 1);
      sampleRate[2] = '.';
    }
    snprintf(value, sizeof(value), "%s Hz", sampleRate);
  }
  tftPrintFixed(-1, value, 166, 156, PrimaryColor, PrimaryColorSmooth, 16);

  value[0] = '\0';
  if (radio.ServiceStart && radio.bitrate != 0) {
    snprintf(value, sizeof(value), "%u kb/s", static_cast<unsigned>(radio.bitrate));
  }
  tftPrintFixed(-1, value, 166, 176, PrimaryColor, PrimaryColorSmooth, 16);

  value[0] = '\0';
  if (radio.ServiceStart && radio.servicetype < 9 && radio.audiomode <= 3) {
    snprintf(value, sizeof(value), "%s - %s",
             ServiceTypeText[radio.servicetype], AudioModeText[radio.audiomode]);
  }
  tftPrintFixed(-1, value, 166, 196, PrimaryColor, PrimaryColorSmooth, 16);
}

// Render the scrollable list of services in the current ensemble. Used as
// the alternative to "main display" when the user opens the channel list.
void BuildChannelList(void) {
  setvolume = false;
  tft.pushImage (0, 0, 320, 240, servicelistbackground);
  tftPrintFixed(0, myLanguage[language][11], 155, 4, ActiveColor, ActiveColorSmooth, 28);

  byte y = 0;
  if (radio.ServiceIndex > 8 && radio.ServiceIndex < 17) {
    y = 9;
  } else if (radio.ServiceIndex > 16 && radio.ServiceIndex < 25) {
    y = 17;
  } else if (radio.ServiceIndex > 24) {
    y = 25;
  }

  if (radio.numberofservices > 8) {
    byte z = 0;
    if (radio.numberofservices < 17) {
      z = 2;
    } else if (radio.numberofservices < 25) {
      z = 3;
    } else if (radio.numberofservices > 24) {
      z = 4;
    }
    uint8_t page = 1;
    if (y == 9) page = 2;
    else if (y == 17) page = 3;
    else if (y == 25) page = 4;

    char pageText[8];
    snprintf(pageText, sizeof(pageText), "%u/%u",
             static_cast<unsigned>(page), static_cast<unsigned>(z));
    tftPrintFixed(0, pageText, 290, 10,
                  SecondaryColor, SecondaryColorSmooth, 16);
  }

  for (byte i = y; i < radio.numberofservices; i++) {
    ShowOneLine(20 * (i - y), i, (radio.ServiceIndex - y == i - y ? true : false));
    if (i - y == 8) i = 254;
  }
}

// Draw one row of a list/menu. `position` is the vertical line slot
// (ITEM1..ITEM10) and `item` is the data row to read from. `selected`
// switches between highlighted vs normal styling.
void ShowOneLine(byte position, byte item, bool selected) {
  char value[80];

  if (ChannelListView) {
    FullLineSprite.pushImage (-8, -position - 35, 320, 240, servicelistbackground);
    if (selected) FullLineSprite.pushImage(0, 0, 304, 20, selector);

    if (radio.isFm()) {
      const uint16_t stationFrequency = static_cast<uint16_t>(radio.service[item].CompID);
      FullLineSprite.setTextDatum(TL_DATUM);
      FullLineSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
      snprintf(value, sizeof(value), "%u.%u",
               static_cast<unsigned>(stationFrequency / 100),
               static_cast<unsigned>((stationFrequency % 100) / 10));
      FullLineSprite.drawString(value, 8, 3);

      FullLineSprite.setTextDatum(TC_DATUM);
      snprintf(value, sizeof(value), "%04X",
               static_cast<unsigned>(radio.service[item].ServiceID & 0xFFFF));
      FullLineSprite.drawString(value, 92, 3);

      FullLineSprite.setTextDatum(TL_DATUM);
      FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
      radio.ASCIIToBuffer(radio.service[item].Label, 0, value, sizeof(value));
      FullLineSprite.drawString(value, 122, 3);

      FullLineSprite.setTextDatum(TR_DATUM);
      FullLineSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
      FullLineSprite.drawString(fmModeText[language], 300, 3);
      FullLineSprite.pushSprite(8, 35 + position);
      return;
    }

    FullLineSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
    FullLineSprite.setTextDatum(TL_DATUM);
    snprintf(value, sizeof(value), "%u",
             static_cast<unsigned>(radio.service[item].CompID & 0xFF));
    FullLineSprite.drawString(value, 12, 3);

    FullLineSprite.setTextDatum(TC_DATUM);
    snprintf(value, sizeof(value), "%04X",
             static_cast<unsigned>(radio.service[item].ServiceID & 0xFFFF));
    FullLineSprite.drawString(value, 56, 3);

    FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
    FullLineSprite.setTextDatum(TL_DATUM);
    radio.ASCIIToBuffer(radio.service[item].Label,
                        DabServiceLabelCharset(item), value, sizeof(value));
    FullLineSprite.drawString(value, 84, 3);

    FullLineSprite.setTextDatum(TC_DATUM);
    FullLineSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
    FullLineSprite.drawString(ServiceTypeText[radio.service[item].ServiceType], 282, 3);
    FullLineSprite.pushSprite(8, 35 + position);
  } else if (menu) {
    FullLineSprite.pushImage (-8, -position - 32, 320, 240, configurationbackground);
    if (selected) FullLineSprite.pushImage(0, 0, 304, 20, selector);

    FullLineSprite.setTextDatum(TL_DATUM);
    FullLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
    switch (item) {
      case 0:
        FullLineSprite.drawString(myLanguage[language][12], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.drawString(myLanguage[language][0], 300, 3);
        break;

      case 1:
        FullLineSprite.drawString(myLanguage[language][13], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.drawString("%", 300, 3);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        snprintf(value, sizeof(value), "%u", static_cast<unsigned>(ContrastSet));
        FullLineSprite.drawString(value, 270, 3);
        break;

      case 2:
        FullLineSprite.drawString(myLanguage[language][14], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.drawString(Theme[CurrentTheme], 300, 3);
        break;

      case 3:
        FullLineSprite.drawString(myLanguage[language][15], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.drawString((autoslideshow ? myLanguage[language][23] : myLanguage[language][24]), 300, 3);
        break;

      case 4:
        FullLineSprite.drawString(myLanguage[language][16], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.drawString(unitString[unit], 300, 3);
        break;

      case 5:
        FullLineSprite.drawString(myLanguage[language][25], 6, 3);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.setTextDatum(TR_DATUM);
        if (tot != 0) {
          snprintf(value, sizeof(value), "%u", static_cast<unsigned>(tot));
          FullLineSprite.drawString(value, 270, 3);
        }
        if (tot != 0) FullLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
        FullLineSprite.drawString((tot != 0 ? myLanguage[language][26] : myLanguage[language][24]), 300, 3);
        break;

      case 6:
        FullLineSprite.drawString(myLanguage[language][17], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.drawString(radioModeValueText[requestedRadioMode], 300, 3);
        break;

      case 7:
        FullLineSprite.drawString(fmRegionMenuText[language], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.drawString(
            fmRegionValueText[language][sanitizeFmRegion(requestedFmRegion)],
            300, 3);
        break;

      case 8:
        FullLineSprite.drawString("GPIO12", 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        FullLineSprite.drawString(
            Gpio12ModeText[sanitizeGpio12Mode(requestedGpio12Mode)], 300, 3);
        break;

      case 9:
        FullLineSprite.drawString(irRemoteText[language], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
        if (IrRemoteHasProfile()) {
          snprintf(value, sizeof(value), "%s >", irLearnedShortText[language]);
        } else {
          snprintf(value, sizeof(value), ">");
        }
        FullLineSprite.drawString(value, 300, 3);
        break;

      case 10:
        FullLineSprite.drawString(myLanguage[language][81], 6, 3);
        FullLineSprite.setTextDatum(TR_DATUM);
        break;
    }
    FullLineSprite.pushSprite(8, 32 + position);
  }
}

static void DrawMenuRows(void) {
  for (byte row = 0; row < MENU_VISIBLE_ROWS; row++) {
    const byte logicalItem = menuFirstItem + row;
    if (logicalItem >= MENU_ITEM_COUNT) break;
    ShowOneLine(ITEM1 + row * ITEM_GAP, logicalItem, logicalItem == menuitem);
  }
}

static void RedrawMenuSelection(byte oldItem) {
  // If the visible window did not move, only two 20-pixel rows need updating:
  // remove the selector from the old row and draw it on the new row. This
  // avoids the distracting full-menu text repaint on every encoder detent.
  if (oldItem >= menuFirstItem && oldItem < menuFirstItem + MENU_VISIBLE_ROWS)
    ShowOneLine(ITEM1 + (oldItem - menuFirstItem) * ITEM_GAP, oldItem, false);

  if (menuitem >= menuFirstItem && menuitem < menuFirstItem + MENU_VISIBLE_ROWS)
    ShowOneLine(ITEM1 + (menuitem - menuFirstItem) * ITEM_GAP, menuitem, true);

  menuoption = ITEM1 + (menuitem - menuFirstItem) * ITEM_GAP;
}

// Full redraw of the settings menu (entered by long-pressing MODE).
void BuildMenu(void) {
  // Settings has 11 logical entries while the original layout has nine rows.
  // Scroll the existing 9-row window instead of shrinking the proven UI.
  menuopen = false;
  IrRemoteUiAbort();
  SlideShowView = false;
  slsWaitingView = false;
  ShowServiceInformation = false;
  ChannelListView = false;

  if (menuitem >= MENU_ITEM_COUNT) menuitem = 0;
  if (menuitem < menuFirstItem) menuFirstItem = menuitem;
  if (menuitem >= menuFirstItem + MENU_VISIBLE_ROWS)
    menuFirstItem = menuitem - (MENU_VISIBLE_ROWS - 1);
  if (menuFirstItem > MENU_ITEM_COUNT - MENU_VISIBLE_ROWS)
    menuFirstItem = MENU_ITEM_COUNT - MENU_VISIBLE_ROWS;

  menuoption = ITEM1 + (menuitem - menuFirstItem) * ITEM_GAP;

  tft.pushImage (0, 0, 320, 240, configurationbackground);
  tftPrintFixed(0, myLanguage[language][20], 155, 4,
                PrimaryColor, PrimaryColorSmooth, 28);
  tftPrintFixed(0, myLanguage[language][19], 155, 222,
                SecondaryColor, SecondaryColorSmooth, 16);
  DrawMenuRows();
}

// Full redraw of the main radio screen (frequency, PS, RT, signal bars,
// memory slot, clock). Called when switching back from another view or
// after a theme/display-orientation change.
void BuildDisplay(void) {
  SlideShowView = false;
  slsWaitingView = false;
  ShowServiceInformation = false;
  ChannelListView = false;
  displayreset = true;
  setvolume = false;
  rotary = 0;
  rotary2 = 0;

  tft.pushImage (0, 0, 320, 240, Background);
  tftPrint(1, "PR:", 84, 65, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(-1, radio.isFm() ? fmPiText[language] : "EID", 10, 105, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(-1, radio.isFm() ? fmPtyText[language] : "SID", 10, 120, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(1, "MHz", 310, 55, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(-1, "SIG:", 123, 109, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(-1, unitString[unit], 183, 109, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(-1, radio.isFm() ? String(fmSnrText[language]) + ":" : "MER:", 237, 109, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(1, "dB", 309, 109, ActiveColor, ActiveColorSmooth, 16);
  // The compact meter uses a single-letter M. A two-letter "MP" reaches the
  // bar border and its clipped P looks like a stray white pixel.
  tftPrint(-1, radio.isFm() ? "M" : "Q", 122, 90,
           ActiveColor, ActiveColorSmooth, 16);
  tftPrint(-1, "S", 122, 127, SecondaryColor, SecondaryColorSmooth, 16);
  if (!radio.isFm()) tftPrint(1, "ECC", 110, 90, ActiveColor, ActiveColorSmooth, 16);

  for (byte segments = 0; segments < 13; segments++) tft.fillRect(134 + (segments * 14), 135, 2, 3, (segments < 8 ? BarInsignificantColor : BarSignificantColor));
  tft.drawLine(134, 138, 302, 138, ActiveColor);
  tftPrint(-1, "1   3   5   7   9  +10 +30", 134, 140, ActiveColor, ActiveColorSmooth, 16);
  tft.drawRect(134, 90, 141, 12, GreyoutColor);

  ShowFreq();
  ShowTuneMode();
  ShowMemoryPos();
}

// Menu navigation: handle the rotary-up event while the settings menu is
// open. Either steps a value of the current row or moves to the next row.
void MenuUp(void) {
  if (!menuopen) {
    const byte oldItem = menuitem;
    const byte oldFirstItem = menuFirstItem;

    menuitem++;
    if (menuitem >= MENU_ITEM_COUNT) {
      menuitem = 0;
      menuFirstItem = 0;
    } else if (menuitem >= menuFirstItem + MENU_VISIBLE_ROWS) {
      menuFirstItem = menuitem - (MENU_VISIBLE_ROWS - 1);
    }

    if (menuFirstItem != oldFirstItem) {
      // Only the scroll-window boundary needs all menu rows repainted. The
      // header/footer/background remain untouched.
      menuoption = ITEM1 + (menuitem - menuFirstItem) * ITEM_GAP;
      DrawMenuRows();
    } else {
      RedrawMenuSelection(oldItem);
    }
    return;
  }

  if (menuitem == 9) {
    IrRemoteUiRotate(+1);
    return;
  }
  if (menuitem == 10) return;  // About is read-only.

  OneBigLineSprite.pushImage(-11, -88, 292, 170, popupbackground);
  OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
  OneBigLineSprite.setTextDatum(TC_DATUM);
  char value[8];

  switch (menuitem) {
    case 0:
      language ++;
      if (language == (sizeof (myLanguage) / sizeof (myLanguage[0]))) language = 0;
      OneBigLineSprite.drawString(myLanguage[language][0], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 1:
      ContrastSet ++;
      if (ContrastSet > 100) ContrastSet = 1;
      OneBigLineSprite.setTextDatum(TL_DATUM);
      OneBigLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
      OneBigLineSprite.drawString("%", 155, 2);
      OneBigLineSprite.setTextDatum(TR_DATUM);
      OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
      snprintf(value, sizeof(value), "%u", static_cast<unsigned>(ContrastSet));
      OneBigLineSprite.drawString(value, 135, 2);
      analogWrite(CONTRASTPIN, ContrastSet * 2 + 27);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 2:
      CurrentTheme ++;
      if (CurrentTheme > sizeof(Theme) / sizeof(Theme[0]) - 1) CurrentTheme = 0;
      doTheme();
      tft.pushImage (13, 30, 292, 170, popupbackground);
      Infoboxprint(myLanguage[language][14]);
      OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
      OneBigLineSprite.drawString(Theme[CurrentTheme], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 3:
      autoslideshow = !autoslideshow;
      OneBigLineSprite.drawString(
          autoslideshow ? myLanguage[language][23] : myLanguage[language][24],
          135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 4:
      unit ++;
      if (unit > sizeof(unitString) / sizeof(unitString[0]) - 1) unit = 0;
      OneBigLineSprite.drawString(unitString[unit], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 5:
      switch (tot) {
        case 0: tot = 15; break;
        case 15: tot = 30; break;
        case 30: tot = 60; break;
        case 60: tot = 90; break;
        default: tot = 0; break;
      }
      if (tot != 0) {
        OneBigLineSprite.setTextDatum(TR_DATUM);
        snprintf(value, sizeof(value), "%u", static_cast<unsigned>(tot));
        OneBigLineSprite.drawString(value, 135, 2);
        OneBigLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
        OneBigLineSprite.setTextDatum(TL_DATUM);
        OneBigLineSprite.drawString(myLanguage[language][26], 155, 2);
      } else {
        OneBigLineSprite.drawString(myLanguage[language][24], 135, 2);
      }
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 6:
      requestedRadioMode = requestedRadioMode == RADIO_MODE_DAB
                               ? RADIO_MODE_FM : RADIO_MODE_DAB;
      OneBigLineSprite.drawString(radioModeValueText[requestedRadioMode], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 7:
      requestedFmRegion = static_cast<uint8_t>(
          (sanitizeFmRegion(requestedFmRegion) + 1U) % FM_REGION_COUNT);
      OneBigLineSprite.drawString(
          fmRegionValueText[language][requestedFmRegion], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 8:
      requestedGpio12Mode = static_cast<uint8_t>(
          (sanitizeGpio12Mode(requestedGpio12Mode) + 1U) % 3U);
      OneBigLineSprite.drawString(
          Gpio12ModeText[requestedGpio12Mode], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;
  }
}

// Menu navigation: rotary-down counterpart of MenuUp().
void MenuDown(void) {
  if (!menuopen) {
    const byte oldItem = menuitem;
    const byte oldFirstItem = menuFirstItem;

    if (menuitem == 0) {
      menuitem = MENU_ITEM_COUNT - 1;
      menuFirstItem = MENU_ITEM_COUNT - MENU_VISIBLE_ROWS;
    } else {
      menuitem--;
      if (menuitem < menuFirstItem) menuFirstItem = menuitem;
    }

    if (menuFirstItem != oldFirstItem) {
      menuoption = ITEM1 + (menuitem - menuFirstItem) * ITEM_GAP;
      DrawMenuRows();
    } else {
      RedrawMenuSelection(oldItem);
    }
    return;
  }

  if (menuitem == 9) {
    IrRemoteUiRotate(-1);
    return;
  }
  if (menuitem == 10) return;  // About is read-only.

  OneBigLineSprite.pushImage(-11, -88, 292, 170, popupbackground);
  OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
  OneBigLineSprite.setTextDatum(TC_DATUM);
  char value[8];

  switch (menuitem) {
    case 0:
      language --;
      if (language > (sizeof (myLanguage) / sizeof (myLanguage[0])))
        language = (sizeof (myLanguage) / sizeof (myLanguage[0])) - 1;
      OneBigLineSprite.drawString(myLanguage[language][0], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 1:
      ContrastSet --;
      if (ContrastSet < 1) ContrastSet = 100;
      OneBigLineSprite.setTextDatum(TL_DATUM);
      OneBigLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
      OneBigLineSprite.drawString("%", 155, 2);
      OneBigLineSprite.setTextDatum(TR_DATUM);
      OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
      snprintf(value, sizeof(value), "%u", static_cast<unsigned>(ContrastSet));
      OneBigLineSprite.drawString(value, 135, 2);
      analogWrite(CONTRASTPIN, ContrastSet * 2 + 27);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 2:
      CurrentTheme --;
      if (CurrentTheme > sizeof(Theme) / sizeof(Theme[0]) - 1)
        CurrentTheme = sizeof(Theme) / sizeof(Theme[0]) - 1;
      doTheme();
      tft.pushImage (13, 30, 292, 170, popupbackground);
      Infoboxprint(myLanguage[language][14]);
      OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
      OneBigLineSprite.drawString(Theme[CurrentTheme], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 3:
      autoslideshow = !autoslideshow;
      OneBigLineSprite.drawString(
          autoslideshow ? myLanguage[language][23] : myLanguage[language][24],
          135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 4:
      unit --;
      if (unit > sizeof(unitString) / sizeof(unitString[0]) - 1)
        unit = sizeof(unitString) / sizeof(unitString[0]) - 1;
      OneBigLineSprite.drawString(unitString[unit], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 5:
      switch (tot) {
        case 15: tot = 0; break;
        case 30: tot = 15; break;
        case 60: tot = 30; break;
        case 90: tot = 60; break;
        default: tot = 90; break;
      }
      if (tot != 0) {
        OneBigLineSprite.setTextDatum(TR_DATUM);
        snprintf(value, sizeof(value), "%u", static_cast<unsigned>(tot));
        OneBigLineSprite.drawString(value, 135, 2);
        OneBigLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
        OneBigLineSprite.setTextDatum(TL_DATUM);
        OneBigLineSprite.drawString(myLanguage[language][26], 155, 2);
      } else {
        OneBigLineSprite.drawString(myLanguage[language][24], 135, 2);
      }
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 6:
      requestedRadioMode = requestedRadioMode == RADIO_MODE_DAB
                               ? RADIO_MODE_FM : RADIO_MODE_DAB;
      OneBigLineSprite.drawString(radioModeValueText[requestedRadioMode], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 7:
      requestedFmRegion = sanitizeFmRegion(requestedFmRegion) == 0
                              ? FM_REGION_COUNT - 1
                              : sanitizeFmRegion(requestedFmRegion) - 1;
      OneBigLineSprite.drawString(
          fmRegionValueText[language][requestedFmRegion], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 8:
      requestedGpio12Mode = sanitizeGpio12Mode(requestedGpio12Mode) == GPIO12_AUTO
                                ? GPIO12_IR
                                : requestedGpio12Mode - 1;
      OneBigLineSprite.drawString(
          Gpio12ModeText[requestedGpio12Mode], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;
  }
}

// Menu confirm: rotary-button click while in the menu. Either enters a sub-
// menu, applies the current change, or commits the value to EEPROM.
void DoMenu(void) {
  if (menuopen) {
    if (menuitem == 9) {
      if (IrRemoteUiPress()) {
        menuopen = false;
        BuildMenu();
      }
      return;
    }
    menuopen = false;
    BuildMenu();
    return;
  }

  if (menuitem == 9) {
    menuopen = true;
    IrRemoteUiOpen();
    return;
  }

  tft.pushImage (13, 30, 292, 170, popupbackground);
  OneBigLineSprite.pushImage(-11, -88, 292, 170, popupbackground);
  OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
  OneBigLineSprite.setTextDatum(TC_DATUM);
  menuopen = true;
  char value[80];

  switch (menuitem) {
    case 0:
      Infoboxprint(myLanguage[language][12]);
      OneBigLineSprite.drawString(myLanguage[language][0], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 1:
      Infoboxprint(myLanguage[language][13]);
      OneBigLineSprite.setTextDatum(TL_DATUM);
      OneBigLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
      OneBigLineSprite.drawString("%", 155, 2);
      OneBigLineSprite.setTextDatum(TR_DATUM);
      OneBigLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
      snprintf(value, sizeof(value), "%u", static_cast<unsigned>(ContrastSet));
      OneBigLineSprite.drawString(value, 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 2:
      Infoboxprint(myLanguage[language][14]);
      OneBigLineSprite.drawString(Theme[CurrentTheme], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 3:
      Infoboxprint(myLanguage[language][15]);
      OneBigLineSprite.drawString(
          autoslideshow ? myLanguage[language][23] : myLanguage[language][24],
          135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 4:
      Infoboxprint(myLanguage[language][16]);
      OneBigLineSprite.drawString(unitString[unit], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 5:
      Infoboxprint(myLanguage[language][25]);
      if (tot != 0) {
        OneBigLineSprite.setTextDatum(TR_DATUM);
        snprintf(value, sizeof(value), "%u", static_cast<unsigned>(tot));
        OneBigLineSprite.drawString(value, 135, 2);
        OneBigLineSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
        OneBigLineSprite.setTextDatum(TL_DATUM);
        OneBigLineSprite.drawString(myLanguage[language][26], 155, 2);
      } else {
        OneBigLineSprite.drawString(myLanguage[language][24], 135, 2);
      }
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 6:
      Infoboxprint(myLanguage[language][17]);
      OneBigLineSprite.drawString(radioModeValueText[requestedRadioMode], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 7:
      Infoboxprint(fmRegionMenuText[language]);
      OneBigLineSprite.drawString(
          fmRegionValueText[language][sanitizeFmRegion(requestedFmRegion)],
          135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 8:
      Infoboxprint("GPIO12");
      OneBigLineSprite.drawString(
          Gpio12ModeText[sanitizeGpio12Mode(requestedGpio12Mode)], 135, 2);
      OneBigLineSprite.pushSprite(24, 118);
      break;

    case 10:
      tftPrintFixed(0, myLanguage[language][79], 155, 40,
                    ActiveColor, ActiveColorSmooth, 28);
      tftPrintFixed(0, "PE5PVB, bales", 155, 72,
                    PrimaryColor, PrimaryColorSmooth, 28);
      tftPrintFixed(0, myLanguage[language][80], 155, 104,
                    ActiveColor, ActiveColorSmooth, 28);
      tftPrintFixed(0, "mcelliotg", 155, 132,
                    PrimaryColor, PrimaryColorSmooth, 28);
      tftPrintFixed(0, "github.com/PE5PVB/SI4684-DAB-Receiver", 155, 161,
                    SecondaryColor, SecondaryColorSmooth, 16);
      snprintf(value, sizeof(value), "GPIO12: %s / %s",
               Gpio12ModeText[sanitizeGpio12Mode(gpio12Mode)],
               radio.controlModeName());
      tftPrintFixed(0, value, 155, 182,
                    SecondaryColor, SecondaryColorSmooth, 16);
      break;
  }
}

// Render a centered text line in the small "info" box at the top of the
// screen (used for transient messages like "Saved" / "No signal").
void Infoboxprint(const char* input) {
  int length = strlen(input);
  int newlineIndex = -1;

  for (int i = 0; i < length; i++) {
    if (input[i] == '\n') {
      newlineIndex = i;
      break;
    }
  }

  if (newlineIndex != -1) {
    char* line1 = (char*)malloc((newlineIndex + 1) * sizeof(char));
    strncpy(line1, input, newlineIndex);
    line1[newlineIndex] = '\0';

    char* line2 = (char*)malloc((length - newlineIndex) * sizeof(char));
    strcpy(line2, input + newlineIndex + 1);

    tftPrint(0, line1, 155, 48, ActiveColor, ActiveColorSmooth, 28);
    tftPrint(0, line2, 155, 78, ActiveColor, ActiveColorSmooth, 28);
    free(line1);
    free(line2);
  } else {
    tftPrint(0, input, 155, 78, ActiveColor, ActiveColorSmooth, 28);
  }
}

// The Show*() routines below are individually called from ProcessDAB() every
// loop. Each one diffs the current radio.* / live state against a `*Old` shadow
// variable and only pushes its sprite/region to the TFT when something changed.

void ShowFreq(void) {
  char value[12];

  if (radio.isFm() || radioMode == RADIO_MODE_FM) {
    snprintf(value, sizeof(value), "%u.%u",
             static_cast<unsigned>(fmfreq / 100),
             static_cast<unsigned>((fmfreq % 100) / 10));
    tftReplaceFixed(0, fmModeText[language], fmModeText[language],
                    145, 45, PrimaryColor, PrimaryColorSmooth,
                    BackgroundColor2, 28);
    tftReplaceFixed(-1, dabfreqStringOld, value,
                    184, 43, SecondaryColor, SecondaryColorSmooth,
                    BackgroundColor2, 52);
    snprintf(dabfreqStringOld, sizeof(dabfreqStringOld), "%s", value);
    return;
  }

  tftReplaceFixed(0, radio.getChannel(dabfreqold), radio.getChannel(dabfreq),
                  145, 45, PrimaryColor, PrimaryColorSmooth,
                  BackgroundColor2, 28);

  const uint32_t frequency = radio.getFreq(dabfreq);
  const uint32_t fraction = frequency % 1000U;
  // Preserve the original display formatting exactly: it added one leading
  // zero only when the kHz remainder was below 100.
  if (fraction < 100U) {
    snprintf(value, sizeof(value), "%lu.0%lu",
             static_cast<unsigned long>(frequency / 1000U),
             static_cast<unsigned long>(fraction));
  } else {
    snprintf(value, sizeof(value), "%lu.%lu",
             static_cast<unsigned long>(frequency / 1000U),
             static_cast<unsigned long>(fraction));
  }

  tftReplaceFixed(-1, dabfreqStringOld, value,
                  184, 43, SecondaryColor, SecondaryColorSmooth,
                  BackgroundColor2, 52);
  snprintf(dabfreqStringOld, sizeof(dabfreqStringOld), "%s", value);
  dabfreqold = dabfreq;
}

void ShowPTY(void) {
  const uint8_t ptyValue = radio.isFm() ? radio.fmPty : radio.pty;
  const bool ptyVisible =
      radio.isFm()
          ? (!tuning && fmPtyValid && ptyValue > 0 && ptyValue <= 31)
          : (radio.ServiceStart && ptyValue > 0 && ptyValue <= 29);
  const uint8_t displayPty = ptyVisible ? ptyValue : 0xFF;

  // PTY is checked every UI pass. Point directly at the existing language
  // string instead of constructing a temporary Arduino String each time.
  // PTY 0 means no/undefined programme type and remains visually blank.
  const char* value = ptyVisible ? myLanguage[language][37 + ptyValue] : "";

  if (displayPty != ptyold || displayreset) {
    LongSprite.pushImage(-8, -162, 320, 240, Background);
    LongSprite.setTextDatum(TC_DATUM);
    LongSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
    LongSprite.drawString(value, 75, 0);
    LongSprite.pushSprite(8, 162);
    ptyold = displayPty;
  }
}

void ShowRT(void) {
  // The 20-pixel radiotext interior is y=219..238; row 239 is the black edge
  // of every background. Always sample and push that same region so clearing
  // an empty label cannot leave a stale row behind.
  constexpr int16_t rtStripY = 219;
  // FONT16's basic ascent is 13 pixels, while accented capitals such as Zcaron
  // and Dcaron extend three pixels above it. Local y=3 keeps those accents at
  // row 0 and keeps j/p/g descenders on the final valid row (19).
  constexpr int16_t rtTextY = 3;
  if (ShowServiceInformation)  {
    FullLineSprite.pushImage(-6, -rtStripY, 320, 240, serviceinfobackground);
  } else if (ChannelListView) {
    FullLineSprite.pushImage(-6, -rtStripY, 320, 240, servicelistbackground);
  } else {
    FullLineSprite.pushImage(-6, -rtStripY, 320, 240, Background);
  }

  // Radiotext is queried from the GUI on every fast main-loop pass, while the
  // broadcast text itself changes only occasionally. Keep the decoded UTF-8
  // text in the fixed RTold[] buffer and rebuild it only when the actual source
  // bytes (or DLS charset/length / FM tuning state) change. No radio, scheduler,
  // slideshow or TFT update timing is changed here.
  static char rtSourceSnapshot[sizeof(radio.ServiceData)] = {0};
  static bool rtSourceValid = false;
  static bool rtFmSnapshot = false;
  static bool rtTuningSnapshot = false;
  static uint8_t rtDabCharsetSnapshot = 0xFF;
  static uint8_t rtDabLengthSnapshot = 0xFF;

  const bool fm = radio.isFm();
  const uint8_t dabCharset = fm ? 0U : DabDynamicLabelCharsetValue();
  const uint8_t dabLength = fm ? 0U : DabDynamicLabelLengthValue();
  const bool sourceChanged =
      !rtSourceValid ||
      fm != rtFmSnapshot ||
      (fm && tuning != rtTuningSnapshot) ||
      (!fm && (dabCharset != rtDabCharsetSnapshot ||
               dabLength != rtDabLengthSnapshot)) ||
      memcmp(rtSourceSnapshot, radio.ServiceData,
             sizeof(rtSourceSnapshot)) != 0;

  if (sourceChanged) {
    char value[RT_TEXT_BUFFER_SIZE];
    if (fm) {
      radio.ASCIIToBuffer(radio.ServiceData, 0, value, sizeof(value));
      if (tuning) value[0] = '\0';
    } else {
      DabDynamicLabelTextToBuffer(radio.ServiceData, value, sizeof(value));
    }

    // A new string must start from x=0 before its first frame is drawn.
    if (strcmp(RTold, value) != 0) xPos = 0;
    memcpy(RTold, value, strlen(value) + 1U);

    memcpy(rtSourceSnapshot, radio.ServiceData, sizeof(rtSourceSnapshot));
    rtSourceValid = true;
    rtFmSnapshot = fm;
    rtTuningSnapshot = tuning;
    rtDabCharsetSnapshot = dabCharset;
    rtDabLengthSnapshot = dabLength;
  }

  const char* value = RTold;
  FullLineSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
  if (value[0] != '\0') {
    // Measure with the exact sprite/font that renders the text. tft.textWidth()
    // could use whichever font the preceding main-screen field left loaded.
    RTWidth = FullLineSprite.textWidth(value);
    if (RTWidth < 300) {
      xPos = 0;
      FullLineSprite.setTextDatum(TC_DATUM);
      FullLineSprite.drawString(value, 154, rtTextY);
      FullLineSprite.pushSprite(6, rtStripY);
    } else if (millis() - rtticker >= 20) {
      --xPos;
      rttickerhold = millis();

      if (xPos < -RTWidth - 50) xPos = 0;
      FullLineSprite.setTextDatum(TL_DATUM);
      FullLineSprite.drawString(value, xPos, rtTextY);
      FullLineSprite.drawString(value, xPos + RTWidth + 50, rtTextY);
      FullLineSprite.pushSprite(6, rtStripY);
      rtticker = millis();
    }
  } else {
    FullLineSprite.pushSprite(6, rtStripY);
  }
}

void ShowSID(void) {
  char value[5] = "";

  if (radio.isFm()) {
    if (!tuning && fmPtyValid && radio.fmPty > 0 && radio.fmPty <= 31)
      snprintf(value, sizeof(value), "%u", static_cast<unsigned>(radio.fmPty));
  } else {
    if (!radio.ServiceStart) radio.SID[0] = '\0';
    snprintf(value, sizeof(value), "%s", radio.SID);
  }

  if (strcmp(value, SIDold) != 0 || displayreset) {
    ShortSprite.pushImage(-38, -120, 320, 240, Background);
    ShortSprite.setTextDatum(TL_DATUM);
    ShortSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
    ShortSprite.drawString(value, 0, 0);
    ShortSprite.pushSprite(38, 120);
    snprintf(SIDold, sizeof(SIDold), "%s", value);
  }
}

void ShowEID(void) {
  char value[5] = "";

  if (radio.isFm()) {
    if (!tuning && radio.fmPi)
      snprintf(value, sizeof(value), "%04X", static_cast<unsigned>(radio.fmPi));
  } else {
    if (tuning) radio.EID[0] = '\0';
    snprintf(value, sizeof(value), "%s", radio.EID);
  }

  if (strcmp(value, EIDold) != 0 || displayreset) {
    ShortSprite.pushImage(-38, -106, 320, 240, Background);
    ShortSprite.setTextDatum(TL_DATUM);
    ShortSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
    ShortSprite.drawString(value, 0, 0);
    ShortSprite.pushSprite(38, 106);
    snprintf(EIDold, sizeof(EIDold), "%s", value);
  }
}

static void trimFixed(char* text) {
  if (!text || !text[0]) return;

  char* first = text;
  while (*first && static_cast<uint8_t>(*first) <= 0x20U) ++first;

  if (first != text) memmove(text, first, strlen(first) + 1U);

  size_t len = strlen(text);
  while (len > 0 && static_cast<uint8_t>(text[len - 1]) <= 0x20U)
    text[--len] = '\0';
}

void ShowPS(void) {
  char value[65] = "";

  if (radio.isFm()) {
    if (!tuning) radio.ASCIIToBuffer(radio.fmPs, 0, value, sizeof(value));
    trimFixed(value);

    // The station-name field has only two visible states: placeholder while
    // PS is unknown/acquiring, then the decoder-confirmed eight-character PS.
    if (value[0] == '\0') snprintf(value, sizeof(value), "--------");

    if (strcmp(value, PSold) != 0 || displayreset) {
      OneBigLineSprite.pushImage(-44, -185, 320, 240, Background);
      OneBigLineSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
      OneBigLineSprite.setTextDatum(TC_DATUM);
      OneBigLineSprite.drawString(value, 130, 4);
      OneBigLineSprite.pushSprite(44, 185);
      snprintf(PSold, sizeof(PSold), "%s", value);
    }
    return;
  }

  if (radio.ServiceStart) {
    radio.ASCIIToBuffer(radio.PStext, radio.ServiceLabelCharset,
                        value, sizeof(value));

    // CurrentServiceInfo can arrive shortly after START_DIGITAL_SERVICE.
    // Until then keep the already selected service-list label visible.
    if (value[0] == '\0' &&
        radio.numberofservices > 0 &&
        radio.ServiceIndex < radio.numberofservices) {
      radio.ASCIIToBuffer(
          radio.service[radio.ServiceIndex].Label,
          DabServiceLabelCharset(radio.ServiceIndex),
          value, sizeof(value));
    }
  } else if (tuning || seek) {
    value[0] = '\0';
  } else if (radio.signallock) {
    if (radio.numberofservices == 0) {
      snprintf(value, sizeof(value), "%s", myLanguage[language][73]);  // Waiting for list
    } else {
      // During asynchronous service selection/restoration _serviceName equals
      // the target service-list label. Otherwise no service has been selected.
      uint8_t pendingCharset = radio.ServiceLabelCharset;
      if (radio.ServiceIndex < radio.numberofservices)
        pendingCharset = DabServiceLabelCharset(radio.ServiceIndex);

      char pendingName[65] = "";
      char indexedName[65] = "";
      radio.ASCIIToBuffer(_serviceName, pendingCharset,
                          pendingName, sizeof(pendingName));
      if (radio.ServiceIndex < radio.numberofservices) {
        radio.ASCIIToBuffer(radio.service[radio.ServiceIndex].Label,
                            pendingCharset, indexedName, sizeof(indexedName));
      }

      if (pendingName[0] != '\0' && strcmp(pendingName, indexedName) == 0)
        snprintf(value, sizeof(value), "%s", pendingName);
      else
        snprintf(value, sizeof(value), "%s", myLanguage[language][74]);  // Select service
    }
  } else if (trysetservice) {
    // The EEPROM name belongs to the previously stored service. Do not paint it
    // until the current multiplex/service list confirms that service ID; this
    // avoids a brief stale/incorrect station-name flash after restart.
    value[0] = '\0';
  } else {
    value[0] = '\0';
  }

  if (strcmp(value, PSold) != 0 || displayreset) {
    if (tunemode != TUNE_MEM || value[0] != '\0') {
      OneBigLineSprite.pushImage(-44, -185, 320, 240, Background);
      OneBigLineSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
      OneBigLineSprite.setTextDatum(TC_DATUM);
      OneBigLineSprite.drawString(value, 130, 4);
      OneBigLineSprite.pushSprite(44, 185);
    }
    snprintf(PSold, sizeof(PSold), "%s", value);
  }
}

void ShowEN(void) {
  char value[65] = "";

  if (radio.isFm()) {
    const char* source = tuning || radio.isTunePending() ? myLanguage[language][75]
                         : (!radio.signallock ? myLanguage[language][76]
                            : (radio.fmPi ? (radio.isRbds() ? fmRbdsText[language]
                                                           : fmRdsText[language])
                                          : (radio.fmPilot ? fmStereoText[language]
                                                           : fmMonoText[language])));
    snprintf(value, sizeof(value), "%s", source);

    if (strcmp(value, EnsembleNameOld) != 0 || displayreset) {
      tft.fillRect(167, 162, 145, 16, BackgroundColor4);
      tftPrintFixed(0, value, 238, 162,
                    SecondaryColor, SecondaryColorSmooth, 16);
      snprintf(EnsembleNameOld, sizeof(EnsembleNameOld), "%s", value);
    }
    return;
  }

  if (tuning) {
    snprintf(value, sizeof(value), "%s", myLanguage[language][75]);
  } else if (!radio.signallock) {
    snprintf(value, sizeof(value), "%s", myLanguage[language][76]);
  } else {
    radio.ASCIIToBuffer(radio.EnsembleLabel, radio.EnsembleLabelCharset,
                        value, sizeof(value));
  }

  if (strcmp(value, EnsembleNameOld) != 0 || displayreset) {
    tft.fillRect(167, 162, 145, 16, BackgroundColor4);
    tftPrintFixed(0, value, 238, 162,
                  SecondaryColor, SecondaryColorSmooth, 16);
    snprintf(EnsembleNameOld, sizeof(EnsembleNameOld), "%s", value);
  }
  // Preserve the original state semantics: tuning/no-lock never leaves a
  // placeholder string in the actual ensemble-label receive buffer.
  if (!radio.signallock || tuning) radio.EnsembleLabel[0] = '\0';
}

void ShowProtectionlevel(void) {
  char value[9] = "";

  if (radio.isFm()) {
    snprintf(value, sizeof(value), "ST %u%%",
             static_cast<unsigned>(radio.fmStereoBlend));
  } else {
    if (!radio.ServiceStart) radio.protectionlevel = 0;
    const char* protection =
        radio.protectionlevel < 14 ? ProtectionText[radio.protectionlevel] : "";
    snprintf(value, sizeof(value), "%s", protection);
  }

  if (strcmp(value, PLold) != 0 || displayreset) {
    MediumSprite.pushImage(-9, -90, 320, 240, Background);
    MediumSprite.setTextDatum(TC_DATUM);
    MediumSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
    MediumSprite.drawString(value, 30, 0);
    MediumSprite.pushSprite(9, 90);
    snprintf(PLold, sizeof(PLold), "%s", value);
  }
}

void ShowAudioMode(void) {
  if (radio.isFm()) {
    if (displayreset || audiomodeold != radio.audiomode) {
      tftPrint(-1, fmModeText[language], 70, 33, SecondaryColor, SecondaryColorSmooth, 16);
      tft.pushImage(10, 4, 28, 19, radio.fmPilot ? stereoon : mono);
      audiomodeold = radio.audiomode;
    }
    return;
  }
  if (!radio.ServiceStart) radio.servicetype = 9;
  if (servicetypeold != radio.servicetype || displayreset) {
    tftPrint(-1, ServiceTypeText[4], 70, 33, GreyoutColor, BackgroundColor, 16);
    if (radio.servicetype == 4 || radio.servicetype == 5) tftPrint(-1, ServiceTypeText[radio.servicetype], 70, 33, SecondaryColor, SecondaryColorSmooth, 16);
    servicetypeold = radio.servicetype;
  }

  if (!radio.ServiceStart) radio.audiomode = 4;
  if (audiomodeold != radio.audiomode || displayreset) {
    switch (radio.audiomode) {
      case 0: tft.pushImage(10, 4, 28, 19, mono); break;
      case 1: tft.pushImage(10, 4, 28, 19, mono); break;
      case 2:
      case 3: tft.pushImage(10, 4, 28, 19, stereoon); break;
      case 4: tft.pushImage(10, 4, 28, 19, stereooff); break;
    }
    audiomodeold = radio.audiomode;
  }
}

static void ClearDabFlagArea(void) {
  // An empty flag slot is intentionally quieter than the old '???' bitmap.
  tft.fillRect(80, 110, 36, 23, BackgroundColor3);
}

void ShowECC(void) {
  if (radio.isFm()) return;
  if (eccold != radio.ecc || displayreset) {
    const char* ITU = "";
    switch (radio.serviceHasOwnEcc ? radio.SID[0] : radio.EID[0]) {
      case '1':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, de); ITU = "D"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, gr); ITU = "GRC"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, ma); ITU = "MRC"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, me); ITU = "MNE"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, md); ITU = "MDA"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '2':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, dz); ITU = "ALG"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, cy); ITU = "CYP"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, cz); ITU = "CZE"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, ie); ITU = "IRL"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, ee); ITU = "EST"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '3':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, ad); ITU = "AND"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, sm); ITU = "SM"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, pl); ITU = "POL"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, tr); ITU = "TUR"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, mk); ITU = "MKD"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '4':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, il); ITU = "ISR"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, ch); ITU = "SUI"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, va); ITU = "CVA"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '5':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, it); ITU = "I"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, jo); ITU = "JOR"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, sk); ITU = "SVK"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, tj); ITU = "TJK"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '6':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, be); ITU = "BEL"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, fi); ITU = "FNL"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, sy); ITU = "SYR"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, ua); ITU = "UKR"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '7':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, ru); ITU = "RUS"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, lu); ITU = "LUX"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, tn); ITU = "TUN"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, kz); ITU = "XXK"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '8':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, ra); ITU = "AZR"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, bg); ITU = "BUL"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, m1); ITU = "MDR"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, nl); ITU = "HOL"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, pt); ITU = "POR"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case '9':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, al); ITU = "ALB"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, dk); ITU = "DNK"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, li); ITU = "LIE"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, lv); ITU = "LVA"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, si); ITU = "SVN"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case 'A':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, at); ITU = "AUT"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, gi); ITU = "GIB"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, is); ITU = "ISL"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, lb); ITU = "LBN"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, am); ITU = "ARM"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case 'B':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, hu); ITU = "HNG"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, iq); ITU = "IRQ"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, mc); ITU = "MCO"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, az); ITU = "AZE"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, uz); ITU = "UZB"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case 'C':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, mt); ITU = "MLT"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, gb); ITU = "G"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, lt); ITU = "LTU"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, hr); ITU = "HRV"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, ge); ITU = "GEO"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case 'D':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, de); ITU = "D"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, ly); ITU = "LBY"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, rs); ITU = "SRB"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, kz); ITU = "KAZ"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case 'E':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, c1); ITU = "CNR"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, ro); ITU = "ROU"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, es); ITU = "E"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, se); ITU = "S"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, tm); ITU = "TKM"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;

      case 'F':
        switch (radio.ecc) {
          case 0xe0: tft.pushImage(80, 110, 36, 23, eg); ITU = "EGY"; break;
          case 0xe1: tft.pushImage(80, 110, 36, 23, fr); ITU = "F"; break;
          case 0xe2: tft.pushImage(80, 110, 36, 23, no); ITU = "NOR"; break;
          case 0xe3: tft.pushImage(80, 110, 36, 23, by); ITU = "BLR"; break;
          case 0xe4: tft.pushImage(80, 110, 36, 23, ba); ITU = "BIH"; break;
          default: ClearDabFlagArea(); ITU = ""; break;
        }
        break;
      default: ClearDabFlagArea(); ITU = ""; break;
    }
    tftReplaceFixed(0, ITUold, ITU, 97, 140,
                    SecondaryColor, SecondaryColorSmooth,
                    BackgroundColor3, 16);
    eccold = radio.ecc;
    snprintf(ITUold, sizeof(ITUold), "%s", ITU);
  }
}

void ShowMemoryPos(void) {
  int memposcolor = 0;
  int memposcolorsmooth = 0;
  switch (memoryposstatus) {
    case MEM_DARK:
      memposcolor = InsignificantColor;
      memposcolorsmooth = InsignificantColorSmooth;
      break;

    case MEM_NORMAL:
      memposcolor = PrimaryColor;
      memposcolorsmooth = PrimaryColorSmooth;
      break;

    case MEM_EXIST:
      memposcolor = SignificantColor;
      memposcolorsmooth = SignificantColorSmooth;
      break;
  }

  char oldValue[5];
  char value[5];
  snprintf(oldValue, sizeof(oldValue), "%u",
           static_cast<unsigned>(memoryposold + 1U));
  snprintf(value, sizeof(value), "%u",
           static_cast<unsigned>(memorypos + 1U));
  tftReplaceFixed(-1, oldValue, value, 93, 65,
                  memposcolor, memposcolorsmooth, BackgroundColor2, 16);
  memoryposold = memorypos;
}

void ShowVolume(void) {
  uint8_t segments = map(volume, 0, 63, 0, 100);
  tft.pushImage(25, 46, 270, 50, volumebackground);
  OneBigLineSprite.pushImage(0, 0, 270, 50, volumebackground);
  if (segments > 100) segments = 100;
  OneBigLineSprite.fillRect(60, 9, 2 * segments, 9, BarInsignificantColor);
  OneBigLineSprite.pushSprite(25, 46);

  char value[5];
  snprintf(value, sizeof(value), "%ld",
           static_cast<long>(map(volume, 0, 62, 0, 100)));
  tftPrintFixed(0, value, 190, 68,
                ActiveColor, ActiveColorSmooth, 28);
  Headphones.SetVolume(volume);
  VolumeTimer = millis();
}


// Signal meters are refreshed at 10 Hz. Keep raw driver values separate from
// the displayed IIR average; feeding an already averaged CNR back into the
// filter on every fast loop pass made the two modes drift differently.
void ShowSignalLevel(void) {
  if (!displayreset && millis() - rssiTimer < 100UL) return;
  rssiTimer = millis();

  const int16_t rawSignal = radio.getRSSI();
  const int16_t rawCnr = radio.cnr;
  if (displayreset) {
    // A mode/tune redraw must not inherit the previous mode's filter history.
    SAvg = rawSignal;
    SAvg2 = rawCnr;
  } else {
    SAvg = static_cast<int16_t>((static_cast<int32_t>(SAvg) * 7 +
                                static_cast<int32_t>(rawSignal) * 3) / 10);
    SAvg2 = static_cast<int16_t>((static_cast<int32_t>(SAvg2) * 7 +
                                 static_cast<int32_t>(rawCnr) * 3) / 10);
  }
  SignalLevel = SAvg;
  CNR = static_cast<int8_t>(SAvg2 < 0 ? 0 : (SAvg2 > 127 ? 127 : SAvg2));

  int SignalLevelprint = 0;
  if (unit == 0) SignalLevelprint = SignalLevel;
  if (unit == 1) SignalLevelprint = ((SignalLevel * 100) + 10875) / 100;
  if (unit == 2)
    SignalLevelprint = round((float(SignalLevel) / 10.0 -
                              10.0 * log10(75) - 90.0) * 10.0);

  char signalText[16];
  snprintf(signalText, sizeof(signalText), "%d.%d",
           SignalLevelprint / 10, abs(SignalLevelprint % 10));

  if (!ShowServiceInformation) {
    if (SignalLevelprint > SignalLevelold + 3 ||
        SignalLevelprint < SignalLevelold - 3 || displayreset) {
      ShortSprite.fillSprite(BackgroundColor3);
      ShortSprite.setTextDatum(TR_DATUM);
      ShortSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
      ShortSprite.drawString(signalText, 35, 0);
      ShortSprite.pushSprite(146, 109);

      int segments = 0;
      if (SignalLevel > 120)
        segments = map(SignalLevel, 100, 700, 0, 85);
      if (segments < 0) segments = 0;
      if (segments > 85) segments = 85;

      // Always clear the complete bar first and draw only positive widths.
      // The old unsigned-byte subtraction produced a negative fillRect width
      // below segment 56, which appeared as a full-scale flash/artifact.
      tft.fillRect(134, 129, 170, 6, GreyoutColor);
      const int insignificantSegments = segments < 56 ? segments : 56;
      const int significantSegments = segments > 56 ? segments - 56 : 0;
      if (insignificantSegments > 0)
        tft.fillRect(134, 129, 2 * insignificantSegments, 6,
                     BarInsignificantColor);
      if (significantSegments > 0)
        tft.fillRect(134 + 2 * 56, 129, 2 * significantSegments, 6,
                     BarSignificantColor);

      SignalLevelold = SignalLevelprint;
    }

    if (CNRold != CNR || displayreset) {
      char cnrOldText[8];
      char cnrText[8];
      snprintf(cnrOldText, sizeof(cnrOldText), "%d", static_cast<int>(CNRold));
      snprintf(cnrText, sizeof(cnrText), "%d", static_cast<int>(CNR));

      if (radio.signallock) {
        // If the previous state was unlocked, first erase the "--" placeholder,
        // then replace the previous numeric CNR exactly as the original path did.
        tftPrintFixed(1, "--", 289, 109,
                      BackgroundColor, BackgroundColor3, 16);
        tftReplaceFixed(1, cnrOldText, cnrText, 289, 109,
                        PrimaryColor, PrimaryColorSmooth, BackgroundColor3, 16);
      } else {
        tftReplaceFixed(1, cnrOldText, "--", 289, 109,
                        PrimaryColor, PrimaryColorSmooth, BackgroundColor3, 16);
      }
      CNRold = CNR;
    }

    uint8_t rawQuality = radio.fic > 100U ? 100U : radio.fic;
    if (!radio.isFm()) {
      // FIC quality is commonly pinned at 100 whenever all FIBs are error-free,
      // even while reception margin changes considerably. Q is therefore the
      // conservative combination of FIC integrity and CNR margin. Preserve
      // radio.fic itself for serial/API diagnostics.
      const uint8_t cnrQuality =
          radio.cnr >= 20U ? 100U : static_cast<uint8_t>(radio.cnr * 5U);
      if (cnrQuality < rawQuality) rawQuality = cnrQuality;
      if (!radio.signallock) rawQuality = 0;
    }

    // Smooth Q/M independently from RSSI/CNR. This also suppresses display
    // flashes if one RF-quality sample is anomalously full scale.
    static uint16_t qualityAverage10 = 0;
    static bool qualityAverageValid = false;
    static bool qualityWasFm = false;
    const bool qualityIsFm = radio.isFm();
    if (displayreset || !qualityAverageValid || qualityWasFm != qualityIsFm) {
      qualityAverage10 = static_cast<uint16_t>(rawQuality) * 10U;
      qualityAverageValid = true;
      qualityWasFm = qualityIsFm;
    } else {
      qualityAverage10 = static_cast<uint16_t>(
          (static_cast<uint32_t>(qualityAverage10) * 7U +
           static_cast<uint32_t>(rawQuality) * 30U + 5U) / 10U);
    }
    const uint8_t qualityValue = static_cast<uint8_t>(
        (qualityAverage10 + 5U) / 10U);
    if (ficold != qualityValue || displayreset) {
      const int filledWidth = map(qualityValue, 0, 100, 0, 139);
      // Build the final frame in RAM. Drawing the full gradient directly to TFT
      // and clearing its unused tail afterwards exposed a brief 100% frame.
      for (byte x = 0; x < 10; x++)
        QualityBarSprite.pushImage(0, x, 139, 1, QualLine);
      if (filledWidth < 139)
        QualityBarSprite.fillRect(filledWidth, 0, 139 - filledWidth, 10,
                                  BackgroundColor3);
      QualityBarSprite.pushSprite(135, 91);

      char ficOldText[8];
      char qualityText[8];
      snprintf(ficOldText, sizeof(ficOldText), "%u%%",
               static_cast<unsigned>(ficold));
      snprintf(qualityText, sizeof(qualityText), "%u%%",
               static_cast<unsigned>(qualityValue));
      tftReplaceFixed(1, ficOldText, qualityText, 315, 90,
                      PrimaryColor, PrimaryColorSmooth,
                      BackgroundColor3, 16);
      ficold = qualityValue;
    }
  } else {
    static char signalLevelOldText[16] = "";

    if (((strcmp(signalText, signalLevelOldText) != 0) && !setvolume) ||
        displayreset) {
      tftReplaceFixed(1, signalLevelOldText, signalText, 191, 56,
                      PrimaryColor, PrimaryColorSmooth, BackgroundColor3, 16);
      snprintf(signalLevelOldText, sizeof(signalLevelOldText), "%s", signalText);
    }

    if (((CNRold != CNR) && !setvolume) || displayreset) {
      char cnrOldText[8];
      char cnrText[8];
      snprintf(cnrOldText, sizeof(cnrOldText), "%d", static_cast<int>(CNRold));
      snprintf(cnrText, sizeof(cnrText), "%d", static_cast<int>(CNR));
      tftReplaceFixed(1, cnrOldText, cnrText, 284, 56,
                      PrimaryColor, PrimaryColorSmooth, BackgroundColor3, 16);
      CNRold = CNR;
    }
  }
}

void ShowBitrate(void) {
  if (radio.isFm()) return;
  if (tuning) radio.bitrate = 0;
  if (radio.bitrate != BitrateOld || displayreset) {
    char value[16] = "";
    if (radio.bitrate != 0 && radio.ServiceStart && !tuning) {
      snprintf(value, sizeof(value), "%u kbit/s",
               static_cast<unsigned>(radio.bitrate));
    }

    MediumSprite.pushImage(-9, -140, 320, 240, Background);
    MediumSprite.setTextDatum(TC_DATUM);
    MediumSprite.setTextColor(PrimaryColor, PrimaryColorSmooth, false);
    MediumSprite.drawString(value, 30, 0);
    MediumSprite.pushSprite(9, 140);
    BitrateOld = radio.bitrate;
  }
}

void ShowClock(void) {
  if (!radio.isFm() && radio.signallock) setTime(radio.Hours, radio.Minutes, radio.Seconds, radio.Days, radio.Months, radio.Year);

  // This function runs in the normal UI loop. Keep the two displayed strings in
  // fixed buffers so checking an unchanged clock/date never allocates on heap.
  char clockstring[6];
  char datestring[11];
  snprintf(clockstring, sizeof(clockstring), "%02d:%02d", hour(), minute());
  snprintf(datestring, sizeof(datestring), "%02d-%02d-%04d", day(), month(), year());

  if (strcmp(clockstringOld, clockstring) != 0 || displayreset) {
    ShortSprite.pushImage(-105, -7, 320, 240, Background);
    ShortSprite.setTextDatum(TL_DATUM);
    ShortSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
    ShortSprite.drawString(clockstring, 0, 0);
    ShortSprite.pushSprite(105, 7);
    snprintf(clockstringOld, sizeof(clockstringOld), "%s", clockstring);
  }

  if (strcmp(datestringOld, datestring) != 0 || displayreset) {
    MediumSprite.pushImage(-177, -7, 320, 240, Background);
    MediumSprite.setTextDatum(TL_DATUM);
    MediumSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
    MediumSprite.drawString(datestring, 0, 0);
    MediumSprite.pushSprite(177, 7);
    snprintf(datestringOld, sizeof(datestringOld), "%s", datestring);
  }
}

void ShowSlideShowIcon(void) {
  if (radio.isFm()) {
    if (SlideShowAvailableOld || displayreset) tft.pushImage (10, 187, 30, 22, slideshowoff);
    SlideShowAvailableOld = false;
    return;
  }
  if (SlideShowAvailableOld != radio.SlideShowAvailable || displayreset) {
    if (radio.SlideShowAvailable) {
      tft.pushImage (10, 187, 30, 22, slideshowon);
    } else {
      tft.pushImage (10, 187, 30, 22, slideshowoff);
    }
    SlideShowAvailableOld = radio.SlideShowAvailable;
  }
}

void ShowTuneMode(void) {
  ModeSprite.pushImage(-6, -33, 320, 240, Background);

  switch (tunemode) {
    case TUNE_MAN:
      ModeSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
      ModeSprite.drawString("MAN", 23, 0);
      ModeSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
      ModeSprite.drawString("AUTO", 23, 16);
      ModeSprite.drawString("MEM", 23, 32);
      break;

    case TUNE_AUTO:
      ModeSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
      ModeSprite.drawString("AUTO", 23, 16);
      ModeSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
      ModeSprite.drawString("MAN", 23, 0);
      ModeSprite.drawString("MEM", 23, 32);
      break;

    case TUNE_MEM:
      ModeSprite.setTextColor(SecondaryColor, SecondaryColorSmooth, false);
      ModeSprite.drawString("MAN", 23, 0);
      ModeSprite.drawString("AUTO", 23, 16);

      if (memorystore) {
        ModeSprite.setTextColor(SignificantColor, SignificantColorSmooth, false);
      } else {
        ModeSprite.setTextColor(ActiveColor, ActiveColorSmooth, false);
      }
      ModeSprite.drawString("MEM", 23, 32);
      break;
  }
  ModeSprite.pushSprite(6, 33);
}