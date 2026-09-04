// Learned IR remote support for GPIO12.
// GPIO12 is initialized here only when Settings -> GPIO12 is explicitly IR.
// AUTO never tries to autodetect an IR receiver.

#define RAW_BUFFER_LENGTH 100
#define NO_LED_FEEDBACK_CODE
#define DECODE_DENON
#define DECODE_JVC
#define DECODE_KASEIKYO
#define DECODE_LG
#define DECODE_NEC
#define DECODE_SAMSUNG
#define DECODE_SONY
#define DECODE_RC5
#define DECODE_RC6
#define DECODE_HASH
#define EXCLUDE_EXOTIC_PROTOCOLS
#include <IRremote.hpp>

#include <EEPROM.h>
#include <cstring>
#include <cstdlib>
#include "ir_remote.h"
#include "constants.h"
#include "gui.h"

extern void KeyUp(void);
extern void KeyDown(void);
extern void ButtonPress(void);
extern void SlideShowButtonPress(void);
extern void doStandby(void);
extern void RemoteModeAction(void);
extern void RemoteVolumeStep(int8_t delta);
extern void MarkEepromDirty(void);
extern void FlushEeprom(void);

enum IrAction : uint8_t {
  IR_ACTION_TUNE_UP = 0,
  IR_ACTION_TUNE_DOWN,
  IR_ACTION_OK,
  IR_ACTION_VOL_UP,
  IR_ACTION_VOL_DOWN,
  IR_ACTION_MODE,
  IR_ACTION_SLIDESHOW,
  IR_ACTION_STANDBY,
  IR_ACTION_NONE = 0xFF
};

static const char* const kActionName[EE_IR_KEY_COUNT] = {
  "TUNE +", "TUNE -", "OK", "VOL +", "VOL -", "MODE", "SLIDESHOW", "STANDBY"
};

struct LearnedCode {
  uint8_t protocol;
  uint16_t address;
  uint16_t command;
  uint16_t extra;
  uint16_t bits;
  uint64_t raw;
};

// Keep learned-code working sets off .bss. The classic ESP32 has a tighter
// linker-visible DRAM segment than its total runtime heap. Both tables are
// allocated once during boot when GPIO12 is configured for IR and are then
// retained for the lifetime of the firmware. This avoids runtime malloc/free
// churn and makes the IR path heap-fragmentation neutral after setup().
static LearnedCode* learned = nullptr;
static LearnedCode* learnWork = nullptr;
static bool buffersPrepareAttempted = false;
static bool buffersReady = false;
static bool profileValid = false;
static bool profileLoaded = false;
static bool receiverStarted = false;
static IrAction lastRuntimeAction = IR_ACTION_NONE;
static uint32_t lastRuntimeFrameMs = 0;
static constexpr uint32_t IR_HELD_FRAME_GAP_MS = 220UL;
static constexpr uint32_t IR_LEARN_RELEASE_MS = 220UL;

static constexpr uint8_t IR_PROFILE_VERSION = 1;
static constexpr uint8_t kMagic[4] = {'I', 'R', '0', '1'};

enum UiState : uint8_t {
  UI_NONE,
  UI_MENU,
  UI_LEARN,
  UI_CLEAR_CONFIRM,
  UI_TEST,
  UI_NOTICE
};

static UiState uiState = UI_NONE;
static uint8_t uiSelection = 0;
static uint8_t learnIndex = 0;
static bool learnWaitingRelease = false;
static uint32_t learnLastFrameMs = 0;
static bool clearYes = false;

static constexpr size_t IR_CODE_TABLE_BYTES =
    sizeof(LearnedCode) * EE_IR_KEY_COUNT;

bool IrRemotePrepare(void) {
  if (buffersPrepareAttempted) return buffersReady;
  buffersPrepareAttempted = true;

  learned = static_cast<LearnedCode*>(calloc(EE_IR_KEY_COUNT, sizeof(LearnedCode)));
  learnWork = static_cast<LearnedCode*>(calloc(EE_IR_KEY_COUNT, sizeof(LearnedCode)));
  if (!learned || !learnWork) {
    Serial.printf("[IR] buffer allocation failed (2 x %u bytes)\n",
                  static_cast<unsigned>(IR_CODE_TABLE_BYTES));
    buffersReady = false;
    return false;
  }

  buffersReady = true;
  Serial.printf("[IR] persistent heap buffers allocated: 2 x %u bytes\n",
                static_cast<unsigned>(IR_CODE_TABLE_BYTES));
  return true;
}

static uint16_t read16(int offset) {
  return static_cast<uint16_t>(EEPROM.readByte(offset)) |
         (static_cast<uint16_t>(EEPROM.readByte(offset + 1)) << 8);
}

static uint64_t read64(int offset) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(EEPROM.readByte(offset + i)) << (8U * i);
  return value;
}

static void write16(int offset, uint16_t value) {
  EEPROM.writeByte(offset, static_cast<uint8_t>(value));
  EEPROM.writeByte(offset + 1, static_cast<uint8_t>(value >> 8));
}

static void write64(int offset, uint64_t value) {
  for (uint8_t i = 0; i < 8; ++i)
    EEPROM.writeByte(offset + i, static_cast<uint8_t>(value >> (8U * i)));
}

static uint16_t crc16Update(uint16_t crc, uint8_t data) {
  crc ^= static_cast<uint16_t>(data) << 8;
  for (uint8_t i = 0; i < 8; ++i)
    crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1) ^ 0x1021U)
                          : static_cast<uint16_t>(crc << 1);
  return crc;
}

static uint16_t storedProfileCrc(void) {
  uint16_t crc = 0xFFFFU;
  for (int i = 0; i < EE_IR_CONFIG_SIZE - 2; ++i)
    crc = crc16Update(crc, EEPROM.readByte(EE_IR_CONFIG_START + i));
  return crc;
}

// Validate the stored profile without requiring the learned-code heap tables.
// Settings can therefore display/clear the IR profile while GPIO12 is AUTO or
// INTB without introducing a late runtime allocation.
static bool storedProfileValid(void) {
  for (uint8_t i = 0; i < 4; ++i)
    if (EEPROM.readByte(EE_IR_CONFIG_START + i) != kMagic[i]) return false;
  if (EEPROM.readByte(EE_IR_CONFIG_START + 4) != IR_PROFILE_VERSION) return false;
  if (EEPROM.readByte(EE_IR_CONFIG_START + 5) != EE_IR_KEY_COUNT) return false;
  return storedProfileCrc() ==
         read16(EE_IR_CONFIG_START + EE_IR_CONFIG_SIZE - 2);
}

static bool codesEqual(const LearnedCode& a, const LearnedCode& b) {
  return a.protocol == b.protocol && a.address == b.address &&
         a.command == b.command && a.extra == b.extra &&
         a.bits == b.bits && a.raw == b.raw;
}

static bool profilesEqual(const LearnedCode* a, const LearnedCode* b) {
  for (uint8_t i = 0; i < EE_IR_KEY_COUNT; ++i)
    if (!codesEqual(a[i], b[i])) return false;
  return true;
}

static void loadProfile(void) {
  profileLoaded = false;
  profileValid = false;
  if (!IrRemotePrepare()) return;
  profileLoaded = true;
  if (!storedProfileValid()) {
    Serial.println("[IR] EEPROM profile empty or invalid");
    return;
  }

  int p = EE_IR_CONFIG_START + 6;
  for (uint8_t i = 0; i < EE_IR_KEY_COUNT; ++i) {
    learned[i].protocol = EEPROM.readByte(p++);
    learned[i].address = read16(p); p += 2;
    learned[i].command = read16(p); p += 2;
    learned[i].extra = read16(p); p += 2;
    learned[i].bits = read16(p); p += 2;
    learned[i].raw = read64(p); p += 8;
  }
  profileValid = true;
  Serial.println("[IR] learned profile loaded");
}

static void saveProfile(const LearnedCode* codes) {
  if (!IrRemotePrepare()) return;
  if (profileValid && profilesEqual(codes, learned)) {
    Serial.println("[IR] learned profile unchanged; EEPROM not written");
    return;
  }

  int p = EE_IR_CONFIG_START;
  for (uint8_t i = 0; i < 4; ++i) EEPROM.writeByte(p++, kMagic[i]);
  EEPROM.writeByte(p++, IR_PROFILE_VERSION);
  EEPROM.writeByte(p++, EE_IR_KEY_COUNT);

  for (uint8_t i = 0; i < EE_IR_KEY_COUNT; ++i) {
    EEPROM.writeByte(p++, codes[i].protocol);
    write16(p, codes[i].address); p += 2;
    write16(p, codes[i].command); p += 2;
    write16(p, codes[i].extra); p += 2;
    write16(p, codes[i].bits); p += 2;
    write64(p, codes[i].raw); p += 8;
  }

  // Two bytes at the end are CRC; bytes between records and CRC are reserved.
  while (p < EE_IR_CONFIG_START + EE_IR_CONFIG_SIZE - 2)
    EEPROM.writeByte(p++, 0);

  uint16_t crc = 0xFFFFU;
  for (int i = 0; i < EE_IR_CONFIG_SIZE - 2; ++i)
    crc = crc16Update(crc, EEPROM.readByte(EE_IR_CONFIG_START + i));
  write16(EE_IR_CONFIG_START + EE_IR_CONFIG_SIZE - 2, crc);

  memcpy(learned, codes, IR_CODE_TABLE_BYTES);
  profileValid = true;
  MarkEepromDirty();
  FlushEeprom();
  Serial.println("[IR] learned profile saved");
}

static void clearProfile(void) {
  if (!profileValid) {
    Serial.println("[IR] clear requested but profile already empty");
    return;
  }
  for (int i = 0; i < EE_IR_CONFIG_SIZE; ++i)
    EEPROM.writeByte(EE_IR_CONFIG_START + i, 0);
  if (learned) memset(learned, 0, IR_CODE_TABLE_BYTES);
  profileValid = false;
  lastRuntimeAction = IR_ACTION_NONE;
  lastRuntimeFrameMs = 0;
  MarkEepromDirty();
  FlushEeprom();
  Serial.println("[IR] learned profile cleared");
}

static LearnedCode fromFrame(const IRData& data) {
  LearnedCode result{};
  result.protocol = static_cast<uint8_t>(data.protocol);
  result.address = data.address;
  result.command = data.command;
  result.extra = data.extra;
  result.bits = data.numberOfBits;
  result.raw = static_cast<uint64_t>(data.decodedRawData);
  return result;
}

static bool frameMatches(const LearnedCode& code, const IRData& data) {
  if (code.protocol != static_cast<uint8_t>(data.protocol)) return false;
  if (data.protocol == UNKNOWN)
    return code.raw == static_cast<uint64_t>(data.decodedRawData);
  return code.address == data.address && code.command == data.command &&
         code.extra == data.extra && code.bits == data.numberOfBits;
}

static IrAction findAction(const IRData& data) {
  if (!profileValid) return IR_ACTION_NONE;
  for (uint8_t i = 0; i < EE_IR_KEY_COUNT; ++i)
    if (frameMatches(learned[i], data)) return static_cast<IrAction>(i);
  return IR_ACTION_NONE;
}

static int8_t findLearningDuplicate(const IRData& data) {
  if (!learnWork) return -1;
  for (uint8_t i = 0; i < learnIndex; ++i)
    if (frameMatches(learnWork[i], data)) return static_cast<int8_t>(i);
  return -1;
}

static bool actionRepeats(IrAction action) {
  return action == IR_ACTION_TUNE_UP || action == IR_ACTION_TUNE_DOWN ||
         action == IR_ACTION_VOL_UP || action == IR_ACTION_VOL_DOWN;
}

static void dispatch(IrAction action) {
  switch (action) {
    case IR_ACTION_TUNE_UP:    KeyUp(); break;
    case IR_ACTION_TUNE_DOWN:  KeyDown(); break;
    case IR_ACTION_OK:         ButtonPress(); break;
    case IR_ACTION_VOL_UP:     if (!menu) RemoteVolumeStep(+2); break;
    case IR_ACTION_VOL_DOWN:   if (!menu) RemoteVolumeStep(-2); break;
    case IR_ACTION_MODE:       RemoteModeAction(); break;
    case IR_ACTION_SLIDESHOW:  if (!menu) SlideShowButtonPress(); break;
    case IR_ACTION_STANDBY:    if (!menu) doStandby(); break;
    default: break;
  }
}

static void drawUiBase(const char* title) {
  tft.pushImage(0, 0, 320, 240, configurationbackground);
  tftPrint(0, title, 155, 5, PrimaryColor, PrimaryColorSmooth, 28);
}

static void drawMenu(void) {
  drawUiBase(irRemoteText[language]);
  const char* items[] = {
    irLearnText[language],
    irClearText[language],
    irTestText[language],
    irBackText[language]
  };
  for (uint8_t i = 0; i < 4; ++i) {
    const int color = i == uiSelection ? ActiveColor : PrimaryColor;
    const int smooth = i == uiSelection ? ActiveColorSmooth : PrimaryColorSmooth;
    tftPrint(-1, String(i == uiSelection ? "> " : "  ") + items[i],
             70, 55 + i * 32, color, smooth, 28);
  }
  tftPrint(0,
           profileValid ? irProfileLearnedText[language]
                        : irProfileEmptyText[language],
           155, 204, SecondaryColor, SecondaryColorSmooth, 16);
}

static void drawNeedIr(void) {
  drawUiBase(irRemoteText[language]);
  tftPrint(0, irSetGpio12Text[language], 155, 102,
           ActiveColor, ActiveColorSmooth, 28);
  tftPrint(0, irPressOkText[language], 155, 145,
           SecondaryColor, SecondaryColorSmooth, 16);
}

static void drawLearn(void) {
  drawUiBase(irLearningText[language]);
  tftPrint(0, irPressText[language], 155, 72,
           SecondaryColor, SecondaryColorSmooth, 16);
  tftPrint(0, kActionName[learnIndex], 155, 101,
           ActiveColor, ActiveColorSmooth, 28);
  tftPrint(0, String(learnIndex + 1) + "/" + String(EE_IR_KEY_COUNT),
           155, 145, PrimaryColor, PrimaryColorSmooth, 16);
  tftPrint(0, irPhysicalOkCancelsText[language], 155, 199,
           SecondaryColor, SecondaryColorSmooth, 16);
}

static void drawLearnRelease(void) {
  drawUiBase(irLearningText[language]);
  tftPrint(0, irReleaseKeyText[language], 155, 96,
           ActiveColor, ActiveColorSmooth, 28);
  tftPrint(0, String(learnIndex) + "/" + String(EE_IR_KEY_COUNT),
           155, 145, PrimaryColor, PrimaryColorSmooth, 16);
  tftPrint(0, irPhysicalOkCancelsText[language], 155, 199,
           SecondaryColor, SecondaryColorSmooth, 16);
}

static void drawClear(void) {
  drawUiBase(irRemoteText[language]);
  tftPrint(0, irClearLearnedRemoteText[language], 155, 82,
           ActiveColor, ActiveColorSmooth, 28);

  String choices;
  if (clearYes)
    choices = String(irNoText[language]) + "     > " + irYesText[language];
  else
    choices = String("> ") + irNoText[language] + "     " + irYesText[language];

  tftPrint(0, choices, 155, 132,
           PrimaryColor, PrimaryColorSmooth, 28);
}

static String hex16(uint16_t value) {
  String s(value, HEX);
  s.toUpperCase();
  while (s.length() < 4) s = "0" + s;
  return s;
}

static void drawTest(const IRData* data = nullptr) {
  drawUiBase(irTestTitleText[language]);
  if (!data) {
    tftPrint(0, irPressRemoteKeyText[language], 155, 90,
             ActiveColor, ActiveColorSmooth, 28);
    tftPrint(0, irPhysicalOkExitsText[language], 155, 195,
             SecondaryColor, SecondaryColorSmooth, 16);
    return;
  }

  const IrAction action = findAction(*data);
  tftPrint(-1,
           String(irProtocolText[language]) + ": " +
               getProtocolString(data->protocol),
           28, 55, PrimaryColor, PrimaryColorSmooth, 16);
  tftPrint(-1,
           String(irAddressText[language]) + ":  0x" +
               hex16(data->address),
           28, 80, PrimaryColor, PrimaryColorSmooth, 16);
  tftPrint(-1,
           String(irCommandText[language]) + ":  0x" +
               hex16(data->command),
           28, 105, PrimaryColor, PrimaryColorSmooth, 16);
  tftPrint(-1,
           String(irActionText[language]) + ":   " +
               (action == IR_ACTION_NONE
                    ? String(irNotAssignedText[language])
                    : String(kActionName[action])),
           28, 130, ActiveColor, ActiveColorSmooth, 16);
  tftPrint(-1,
           String(irRepeatText[language]) + ":   " +
               ((data->flags &
                 (IRDATA_FLAGS_IS_REPEAT | IRDATA_FLAGS_IS_AUTO_REPEAT))
                    ? String(irYesText[language])
                    : String(irNoText[language])),
           28, 155, SecondaryColor, SecondaryColorSmooth, 16);
  tftPrint(0, irPhysicalOkExitsText[language], 155, 200,
           SecondaryColor, SecondaryColorSmooth, 16);
}

void IrRemoteBegin(void) {
  if (receiverStarted) return;
  if (!IrRemotePrepare()) {
    Serial.println("[IR] receiver not started: persistent buffers unavailable");
    return;
  }
  if (!profileLoaded) loadProfile();
  IrReceiver.begin(SI4684_INTB_PIN, false);
  receiverStarted = true;
  lastRuntimeAction = IR_ACTION_NONE;
  lastRuntimeFrameMs = 0;
  Serial.printf("[IR] receiver started GPIO%u profile=%s\n",
                SI4684_INTB_PIN, profileValid ? "learned" : "empty");
}

void IrRemoteStop(void) {
  if (!receiverStarted) return;
  IrReceiver.stop();
  receiverStarted = false;
  lastRuntimeAction = IR_ACTION_NONE;
  lastRuntimeFrameMs = 0;
  Serial.println("[IR] receiver stopped");
}

bool IrRemoteHasProfile(void) {
  return profileLoaded ? profileValid : storedProfileValid();
}
bool IrRemoteUiActive(void) { return uiState != UI_NONE; }

void IrRemoteUiOpen(void) {
  if (receiverStarted) {
    if (!profileLoaded) loadProfile();
  } else {
    // In AUTO/INTB mode the submenu remains discoverable, but simply reading
    // its status must not allocate IR buffers after boot.
    profileValid = storedProfileValid();
  }
  uiState = UI_MENU;
  uiSelection = 0;
  drawMenu();
}

void IrRemoteUiAbort(void) {
  uiState = UI_NONE;
  learnIndex = 0;
  learnWaitingRelease = false;
  learnLastFrameMs = 0;
  clearYes = false;
}

void IrRemoteUiRotate(int8_t direction) {
  if (uiState == UI_MENU) {
    if (direction > 0) uiSelection = (uiSelection + 1U) % 4U;
    else uiSelection = uiSelection == 0 ? 3 : uiSelection - 1;
    drawMenu();
  } else if (uiState == UI_CLEAR_CONFIRM) {
    clearYes = !clearYes;
    drawClear();
  }
}

bool IrRemoteUiPress(void) {
  if (uiState == UI_LEARN || uiState == UI_TEST || uiState == UI_NOTICE) {
    uiState = UI_MENU;
    drawMenu();
    return false;
  }
  if (uiState == UI_CLEAR_CONFIRM) {
    if (clearYes) clearProfile();
    uiState = UI_MENU;
    clearYes = false;
    drawMenu();
    return false;
  }
  if (uiState != UI_MENU) return true;

  switch (uiSelection) {
    case 0: // Learn
      if (!receiverStarted) {
        uiState = UI_NOTICE;
        drawNeedIr();
        return false;
      }
      if (!IrRemotePrepare()) {
        uiState = UI_NOTICE;
        drawUiBase(irRemoteText[language]);
        tftPrint(0, irMemoryErrorText[language], 155, 102,
                 ActiveColor, ActiveColorSmooth, 28);
        tftPrint(0, irPressOkText[language], 155, 145,
                 SecondaryColor, SecondaryColorSmooth, 16);
        return false;
      }
      memcpy(learnWork, learned, IR_CODE_TABLE_BYTES);
      learnIndex = 0;
      learnWaitingRelease = false;
      learnLastFrameMs = 0;
      uiState = UI_LEARN;
      drawLearn();
      return false;

    case 1: // Clear
      if (!profileValid) {
        drawMenu();
        return false;
      }
      clearYes = false;
      uiState = UI_CLEAR_CONFIRM;
      drawClear();
      return false;

    case 2: // Test
      if (!receiverStarted) {
        uiState = UI_NOTICE;
        drawNeedIr();
        return false;
      }
      uiState = UI_TEST;
      drawTest();
      return false;

    default: // Back
      uiState = UI_NONE;
      return true;
  }
}

void IrRemoteProcess(void) {
  if (!receiverStarted) return;

  // Between learning steps require a real release (a short quiet gap). This
  // prevents remotes that resend complete frames, not only explicit repeat
  // frames, from teaching one held key into multiple actions.
  if (uiState == UI_LEARN && learnWaitingRelease &&
      millis() - learnLastFrameMs >= IR_LEARN_RELEASE_MS) {
    learnWaitingRelease = false;
    drawLearn();
  }

  if (!IrReceiver.decode()) return;

  const IRData data = IrReceiver.decodedIRData;
  IrReceiver.resume();

  if (data.flags & (IRDATA_FLAGS_WAS_OVERFLOW | IRDATA_FLAGS_PARITY_FAILED))
    return;

  const bool repeat = data.flags &
      (IRDATA_FLAGS_IS_REPEAT | IRDATA_FLAGS_IS_AUTO_REPEAT);
  const uint32_t now = millis();

  if (uiState == UI_LEARN) {
    learnLastFrameMs = now;
    if (learnWaitingRelease || repeat) return;

    const int8_t duplicate = findLearningDuplicate(data);
    if (duplicate >= 0) {
      Serial.printf("[IR/LEARN] duplicate of %s ignored while waiting for %s\n",
                    kActionName[duplicate], kActionName[learnIndex]);
      learnWaitingRelease = true;
      drawLearnRelease();
      return;
    }

    learnWork[learnIndex] = fromFrame(data);
    Serial.printf("[IR/LEARN] %s protocol=%s address=0x%04X command=0x%04X bits=%u\n",
                  kActionName[learnIndex], getProtocolString(data.protocol),
                  data.address, data.command, data.numberOfBits);
    ++learnIndex;
    if (learnIndex >= EE_IR_KEY_COUNT) {
      saveProfile(learnWork);
      uiState = UI_MENU;
      learnIndex = 0;
      learnWaitingRelease = false;
      drawMenu();
    } else {
      learnWaitingRelease = true;
      drawLearnRelease();
    }
    return;
  }

  if (uiState == UI_TEST) {
    drawTest(&data);
    return;
  }

  if (uiState != UI_NONE) return;

  IrAction action = IR_ACTION_NONE;
  bool heldFrame = false;
  if (repeat) {
    action = lastRuntimeAction;
    heldFrame = action != IR_ACTION_NONE;
  } else {
    action = findAction(data);
    heldFrame = action != IR_ACTION_NONE && action == lastRuntimeAction &&
                now - lastRuntimeFrameMs < IR_HELD_FRAME_GAP_MS;
  }

  if (action == IR_ACTION_NONE) {
    lastRuntimeAction = IR_ACTION_NONE;
    lastRuntimeFrameMs = now;
    return;
  }

  // Update the receive timestamp even for a suppressed one-shot held frame.
  // Therefore a continuously held key can never retrigger every N ms.
  lastRuntimeAction = action;
  lastRuntimeFrameMs = now;
  if (heldFrame && !actionRepeats(action)) return;

  dispatch(action);
}
