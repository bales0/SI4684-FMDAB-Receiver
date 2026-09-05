// GUI module: all TFT rendering for the radio.
// Renders directly to the 320x240 ILI9341 panel via TFT_eSPI, plus uses
// TFT_eSprite objects (defined in the .ino) for flicker-free partial updates.

#ifndef GUI_H
#define GUI_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <TimeLib.h>
#include "si4684.h"
#include "TPA6130A2.h"
#include "language.h"
#include "constants.h"
#include "graphics.h"
#include <EEPROM.h>
#include <cstring>

extern bool autoslideshow;
extern bool change;
extern bool ChannelListView;
extern bool displayreset;
extern bool highz;
extern bool memorystore;
extern bool menu;
extern bool menuopen;
extern bool seek;
extern bool setvolume;
extern bool ShowServiceInformation;
extern bool SlideShowAvailableOld;
extern bool SlideShowView;
extern bool slsWaitingView;
extern bool trysetservice;
extern bool tuning;
extern byte audiomodeold;
extern byte ContrastSet;
extern byte CurrentTheme;
extern byte dabfreq;
extern byte dabfreqold;
extern byte eccold;
extern byte ficold;
extern byte language;
extern byte memorypos;
extern byte memoryposold;
extern byte memoryposstatus;
extern byte menuitem;
extern byte ptyold;
extern byte servicetypeold;
extern byte tot;
extern byte tunemode;
extern byte unit;
extern byte volume;
extern RadioMode radioMode;
extern RadioMode requestedRadioMode;
extern uint8_t fmRegion;
extern uint8_t requestedFmRegion;
extern uint8_t gpio12Mode;
extern uint8_t requestedGpio12Mode;
extern uint16_t fmfreq;
extern char _serviceName[17];
extern int ActiveColor;
extern int ActiveColorSmooth;
extern int BackgroundColor;
extern int BackgroundColor2;
extern int BackgroundColor3;
extern int BackgroundColor4;
extern int BarInsignificantColor;
extern int BarSignificantColor;
extern int BitrateAutoColor;
extern int BitrateAutoColorSmooth;
extern int GreyoutColor;
extern int InsignificantColor;
extern int InsignificantColorSmooth;
extern int menuoption;
extern int PrimaryColor;
extern int PrimaryColorSmooth;
extern volatile int rotary;
extern volatile int rotary2;
extern int rssi;
extern int rssiold;
extern int RTWidth;
extern int SecondaryColor;
extern int SecondaryColorSmooth;
extern int SignalLevelold;
extern int SignificantColor;
extern int SignificantColorSmooth;
extern int xPos;
extern int16_t SignalLevel;
extern int16_t SAvg;
extern int16_t SAvg2;
extern int8_t CNR;
extern int8_t CNRold;
extern char clockstringOld[6];
extern char dabfreqStringOld[12];
extern char datestringOld[11];
extern char EnsembleNameOld[65];
extern char EIDold[5];
extern char ITUold[4];
extern char PLold[9];
extern char PSold[65];
static constexpr size_t RT_TEXT_BUFFER_SIZE = 385;  // 128 EBU bytes * max 3 UTF-8 bytes + NUL
extern char RTold[RT_TEXT_BUFFER_SIZE];
extern char SIDold[5];
extern uint16_t BitrateOld;
extern unsigned long rssiTimer;
extern unsigned long rtticker;
extern unsigned long rttickerhold;
extern unsigned long VolumeTimer;

extern TFT_eSPI tft;
extern TFT_eSprite FullLineSprite;
extern TFT_eSprite OneBigLineSprite;
extern TFT_eSprite LongSprite;
extern TFT_eSprite MediumSprite;
extern TFT_eSprite ModeSprite;
extern TFT_eSprite QualityBarSprite;
extern TFT_eSprite ShortSprite;
extern DAB radio;
extern TPA6130A2 Headphones;

void BuildChannelList(void);
void BuildMenu(void);
void BuildDisplay(void);
void MenuUp(void);
void MenuDown(void);
void DoMenu(void);
void doTheme(void);
void Infoboxprint(const char* input);
void ShowServiceInfo(void);
void ShowFreq(void);
void ShowPTY(void);
void ShowRT(void);
void ShowSID(void);
void ShowEID(void);
void ShowPS(void);
void ShowEN(void);
void ShowProtectionlevel(void);
void ShowAudioMode(void);
void ShowECC(void);
void ShowMemoryPos(void);
void ShowVolume(void);
void ShowSignalLevel(void);
void ShowBitrate(void);
void ShowClock(void);
void ShowSlideShowIcon(void);
void ShowTuneMode(void);
void ShowRSSI(void);
void ShowOneLine(byte position, byte item, bool selected);

extern void tftPrint(int8_t offset, const String & text, int16_t x, int16_t y,
                     int color, int smoothcolor, uint8_t fontsize);
extern void tftReplace(int8_t offset, const String & textold, const String & text,
                       int16_t x, int16_t y, int color, int smoothcolor,
                       int backcolor, uint8_t fontsize);
extern void tftReplaceFixed(int8_t offset, const char *textold, const char *text,
                            int16_t x, int16_t y, int color, int smoothcolor,
                            int backcolor, uint8_t fontsize);
extern void tftPrintFixed(int8_t offset, const char *text, int16_t x, int16_t y,
                          int color, int smoothcolor, uint8_t fontsize);
extern void loadFonts(bool option);
extern bool IsStationEmpty(void);
extern void MarkEepromDirty(void);

#endif
