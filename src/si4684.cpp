// SI4684 DAB chip driver. See si4684.h for the high-level summary.
//
// Implementation notes:
//   - The supplied Si468x library owns the common CTS/error state machine.
//     Legacy DAB parsers below still use SPIbuffer, but all their commands are
//     routed through that one common transport.
//   - Commands and reply layouts mirror the Si468x programming guide (AN649).
//   - Slideshow segments and the assembled current image stay in RAM.

#include "si4684.h"
#include "constants.h"
#include "esp_heap_caps.h"

extern void SlideshowReceptionState(bool active);
#include "Si468xROM.h"
// Arduino defines interrupts() as a function-like macro; the standalone
// driver also has a reply field named `interrupts`.
#ifdef interrupts
#undef interrupts
#endif
#include "vendor/si468x/Si468x.h"
// V15 A/B test: use the newer DAB 6.0.9 application image.
#include "vendor/si468x/dab_6_0_9.h"
#include "vendor/si468x/fmhd_5_3_3.h"

unsigned char SPIbuffer[4096];      // shared SPI tx/rx buffer (commands and replies)
bool once = false;

static uint32_t slideshowFirstSegmentMs = 0;

unsigned long DataUpdate = 0;       // millis() of last EnsembleInfo/ServiceInfo refresh (500 ms throttle)
bool EnsembleInfoSet;
uint8_t slaveSelectPin;

static si468x::Si468x chip;
// One 4 KiB workspace reduces the approximately 0.5 MB firmware image to
// 4092-byte HOST_LOAD payloads (4096 bytes including the three command args).
// Allocate it once from internal heap instead of static DRAM so the independent
// 40 KiB MOT slideshow buffer remains unchanged and the ESP32 DRAM linker
// segment is not overcommitted.
static constexpr size_t CHIP_WORKSPACE_BYTES = 4096U;
static constexpr size_t CHIP_HOST_LOAD_PAYLOAD =
    (CHIP_WORKSPACE_BYTES - 3U) & ~static_cast<size_t>(3U);
static uint8_t* chipWorkspace = nullptr;
static volatile uint8_t lastStatus0;
static volatile bool radioIntbPending = false;
static volatile uint32_t radioIntbEdgeCount = 0;
static RadioControlMode radioControlMode = RADIO_CTRL_DETECT;
enum class RadioIntbCapability : uint8_t {
  Unknown,
  Absent,
  Present
};
static RadioIntbCapability radioIntbCapability = RadioIntbCapability::Unknown;
static bool radioIntbInterruptAttached = false;
static uint32_t radioBootIrqEdgeCount = 0;
static bool commandAwaitingCts = false;
static bool commandStatusReadTriggeredByIrq = false;
static uint32_t commandStartedUs = 0;
static bool radioPowerUpCtsViaIrq = false;
static bool radioDabRuntimeActive = false;
static bool dabStcPending = false;
static bool dabDsrvPending = false;
static bool dabDeviceEventPending = false;

// AN649 PIN_CONFIG_ENABLE (0x0800) output-enable bits used by this board.
// Keep both audio paths enabled; expose INTB only when GPIO12 was detected.
static constexpr uint16_t RADIO_PIN_CONFIG_DACOUTEN = 1U << 0;
static constexpr uint16_t RADIO_PIN_CONFIG_I2SOUTEN = 1U << 1;
static constexpr uint16_t RADIO_PIN_CONFIG_INTBOUTEN = 1U << 15;
static constexpr uint16_t RADIO_PIN_CONFIG_AUDIO =
    RADIO_PIN_CONFIG_DACOUTEN | RADIO_PIN_CONFIG_I2SOUTEN;

// Keep INTB as the fast path for real events and retain short, bounded safety
// polls so a disconnected or faulty application IRQ cannot stall the UI.
static constexpr uint32_t RADIO_INTB_CTS_SAFETY_US = 2000UL;
static constexpr uint32_t RADIO_FM_DIAG_INTERVAL_MS = 5000UL;
static constexpr uint32_t RADIO_DAB_DIAG_INTERVAL_MS = 5000UL;
static constexpr uint32_t RADIO_DAB_TUNE_TIMEOUT_MS = 5000UL;
static constexpr uint8_t RADIO_DAB_MAX_DSRV_BURST = 4U;
static constexpr uint16_t RADIO_DAB_EVENT_SERVICE_LIST = 0x0001U;
static constexpr uint16_t RADIO_DAB_EVENT_RECONFIGURATION = 0x0080U;

// Runtime-only counters. They are reset after each image boot/reuse so the
// rate-limited FM line compares main-screen and menu behaviour directly.
static bool radioRuntimeDiagnosticsActive = false;
static uint32_t diagFmBusySkipCount = 0;
static uint32_t diagFmRsqStatusCount = 0;
static uint32_t diagFmRdsStatusCount = 0;
static uint32_t diagCtsIrqCompletionCount = 0;
static uint32_t diagCtsPollCompletionCount = 0;
static uint32_t diagFmLastReportMs = 0;
static uint32_t diagDabStcCount = 0;
static uint32_t diagDabDsrvCount = 0;
static uint32_t diagDabDsrvOverflowCount = 0;
static uint32_t diagDabDeviceEventCount = 0;
static uint32_t diagDabCommandErrorCount = 0;
static uint32_t diagDabBusySkipCount = 0;
static uint32_t diagDabLastReportMs = 0;

// Boot diagnostics. HOST_LOAD is intentionally not logged chunk-by-chunk;
// progress is reported roughly every 64 KiB to keep the UART readable.
static uint8_t diagLoadPhase = 0;
static uint32_t diagPhaseBytes[3] = {0, 0, 0};
static uint32_t diagNextLoadReport = 65536UL;
static volatile uint8_t diagLastCommand = 0;

static void IRAM_ATTR radioIntbIsr(void) {
  // ISR contract: record the edge only. SPI, logging and GUI work stay in the
  // cooperative foreground code.
  radioIntbPending = true;
  ++radioIntbEdgeCount;
}

static bool takeRadioIntb(void) {
  return __atomic_exchange_n(&radioIntbPending, false, __ATOMIC_ACQ_REL);
}

static uint32_t radioIntbEdges(void) {
  return __atomic_load_n(&radioIntbEdgeCount, __ATOMIC_ACQUIRE);
}

static uint32_t radioRuntimeIntbEdges(void) {
  return radioIntbEdges() - radioBootIrqEdgeCount;
}

static void setRadioIntbInterrupt(bool enabled) {
  if (enabled == radioIntbInterruptAttached) return;
  if (enabled)
    attachInterrupt(digitalPinToInterrupt(SI4684_INTB_PIN), radioIntbIsr, FALLING);
  else
    detachInterrupt(digitalPinToInterrupt(SI4684_INTB_PIN));
  radioIntbInterruptAttached = enabled;
}

static bool radioIntbActive(void) {
  // INTB is active-low and level sources can coalesce: if another enabled
  // source already holds the line low, a new CTS condition produces no second
  // falling edge. Foreground code must therefore honor both the latched edge
  // and the current pin level.
  const bool edge = takeRadioIntb();
  return edge || digitalRead(SI4684_INTB_PIN) == LOW;
}

static void usePollingFallback(const char* reason) {
  if (radioControlMode == RADIO_CTRL_POLL) return;
  setRadioIntbInterrupt(false);
  radioControlMode = RADIO_CTRL_POLL;
  chip.setCtsPollIntervalUs(1000);
  chip.setIdleStatusPollIntervalUs(20000);
  Serial.printf("[RADIO/IRQ] %s; polling fallback\n", reason ? reason : "INTB disabled");
}

// Runtime fallback also disables the physical INTB output. This helper is used
// only after BOOT, when application properties are available.
static void useRuntimePollingFallback(const char* reason) {
  usePollingFallback(reason);
  const si468x::Result pinConfigResult = chip.setProperty(
      si468x::Property::PIN_CONFIG_ENABLE, RADIO_PIN_CONFIG_AUDIO);
  Serial.printf("[RADIO/IRQ] runtime PIN_CONFIG=0x%04X fallback result=%d\n",
                static_cast<unsigned>(RADIO_PIN_CONFIG_AUDIO),
                static_cast<int>(pinConfigResult));
}

static bool hostWriteCommand(void*, uint8_t command, const uint8_t* args, uint16_t length) {
  diagLastCommand = command;
  commandAwaitingCts = true;
  commandStatusReadTriggeredByIrq = false;
  commandStartedUs = micros();
  if (command == 0x01) {
    Serial.printf("[RADIO/SPI] POWER_UP args=%u data=", length);
    for (uint16_t i = 0; i < length; ++i) Serial.printf("%02X%s", args[i], (i + 1U < length) ? " " : "");
    Serial.println();
  } else if (command == 0x06) {
    if (diagLoadPhase < 2) ++diagLoadPhase;
    diagPhaseBytes[diagLoadPhase] = 0;
    diagNextLoadReport = 65536UL;
    Serial.printf("[RADIO/SPI] LOAD_INIT phase=%u (%s)\n",
                  diagLoadPhase, diagLoadPhase == 1 ? "PATCH" : "FIRMWARE");
  } else if (command == 0x04) {
    if (diagLoadPhase <= 2) {
      const uint16_t payloadLength = length >= 3U ? length - 3U : 0U;
      diagPhaseBytes[diagLoadPhase] += payloadLength;
      if (diagPhaseBytes[diagLoadPhase] == static_cast<uint32_t>(payloadLength) ||
          diagPhaseBytes[diagLoadPhase] >= diagNextLoadReport) {
        Serial.printf("[RADIO/SPI] HOST_LOAD phase=%u bytes=%u\n",
                      diagLoadPhase, diagPhaseBytes[diagLoadPhase]);
        while (diagNextLoadReport <= diagPhaseBytes[diagLoadPhase])
          diagNextLoadReport += 65536UL;
      }
    }
  } else if (command == 0x07) {
    Serial.printf("[RADIO/SPI] BOOT patchBytes=%u fwBytes=%u\n",
                  diagPhaseBytes[1], diagPhaseBytes[2]);
  }

  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  digitalWrite(slaveSelectPin, LOW);
  SPI.transfer(command);
  for (uint16_t i = 0; i < length; ++i) SPI.transfer(args[i]);
  digitalWrite(slaveSelectPin, HIGH);
  SPI.endTransaction();
  return true;
}

static bool hostReadReply(void*, uint8_t* destination, uint16_t length) {
  if (!destination || !length) return false;
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  digitalWrite(slaveSelectPin, LOW);
  SPI.transfer(0);  // SPI framing byte; hidden from the common driver
  for (uint16_t i = 0; i < length; ++i) destination[i] = SPI.transfer(0);
  digitalWrite(slaveSelectPin, HIGH);
  SPI.endTransaction();

  // Print raw replies for the two boot-state commands and for every command error.
  // This exposes RESP4 (the AN649 command-error reason) without depending on
  // any higher-level library diagnostic API.
  if (diagLastCommand == 0x01 || diagLastCommand == 0x09 || (destination[0] & 0x40U)) {
    const uint16_t shown = length < 12 ? length : 12;
    Serial.printf("[RADIO/SPI] REPLY cmd=0x%02X len=%u data=",
                  static_cast<unsigned>(diagLastCommand), static_cast<unsigned>(length));
    for (uint16_t i = 0; i < shown; ++i)
      Serial.printf("%02X%s", destination[i], (i + 1U < shown) ? " " : "");
    if (length > shown) Serial.print(" ...");
    Serial.println();
    if ((destination[0] & 0x40U) && length >= 5)
      Serial.printf("[RADIO/SPI] ERR_CMD reason RESP4=0x%02X\n", destination[4]);
  }
  return true;
}

static uint32_t hostTimeUs(void*) { return micros(); }
static void hostIdle(void*) {
  if ((radioControlMode == RADIO_CTRL_INTB ||
       radioControlMode == RADIO_CTRL_DETECT) &&
      radioIntbActive()) {
    if (commandAwaitingCts) commandStatusReadTriggeredByIrq = true;
    chip.notifyInterrupt();
  }
  yield();
}

static void statusChanged(void*, const si468x::Status& status) {
  lastStatus0 = status.status0;
  const bool irqTriggered = commandStatusReadTriggeredByIrq;
  // One callback corresponds to the status read caused by the most recent
  // service decision. Do not carry its trigger into a later safety poll.
  commandStatusReadTriggeredByIrq = false;
  if (radioDabRuntimeActive) {
    if (status.stcInt() && !dabStcPending) {
      dabStcPending = true;
      ++diagDabStcCount;
    }
    if (status.dsrvInt() && !dabDsrvPending) {
      dabDsrvPending = true;
      ++diagDabDsrvCount;
    }
    if (status.deviceEventInt() && !dabDeviceEventPending) {
      dabDeviceEventPending = true;
      ++diagDabDeviceEventCount;
    }
  }

  if (!commandAwaitingCts || !status.cts()) return;

  if (diagLastCommand == static_cast<uint8_t>(si468x::Command::POWER_UP) &&
      radioControlMode == RADIO_CTRL_DETECT && irqTriggered)
    radioPowerUpCtsViaIrq = true;

  if (radioRuntimeDiagnosticsActive) {
    if ((radioControlMode == RADIO_CTRL_INTB ||
         radioControlMode == RADIO_CTRL_DETECT) && irqTriggered)
      ++diagCtsIrqCompletionCount;
    else
      ++diagCtsPollCompletionCount;
  }

  if ((radioControlMode == RADIO_CTRL_INTB ||
       radioControlMode == RADIO_CTRL_DETECT) && !irqTriggered) {
    // Keep a diagnostic for a genuinely long wait, but do not flood the UART
    // for the normal 2 ms hybrid safety poll.
    if (static_cast<uint32_t>(micros() - commandStartedUs) >= 200000UL)
      Serial.println("[RADIO/IRQ] long CTS wait resolved by status poll");
  }
  commandAwaitingCts = false;
}

static void finishCommandDiagnostics(si468x::Result result) {
  // Pending/Busy still belong to a live operation. Every other result is
  // terminal, including timeout/transport failures which have no CTS callback.
  if (result == si468x::Result::Pending || result == si468x::Result::Busy)
    return;
  commandAwaitingCts = false;
  commandStatusReadTriggeredByIrq = false;
}

static size_t progmemImageReader(void* context, uint32_t offset, uint8_t* destination, size_t length) {
  const uint8_t* source = static_cast<const uint8_t*>(context);
  for (size_t i = 0; i < length; ++i) destination[i] = pgm_read_byte(source + offset + i);
  return length;
}

static void Set_Property(uint16_t property, uint16_t value);
static String convertToUTF8(const wchar_t* input);
static String extractUTF8Substring(const String& utf8String, size_t start, size_t length);
static void charConverter(const char* input, wchar_t* output, size_t size);
static int compareCompID(const void* a, const void* b);

// Read back the chip identifier string (e.g. "Si4684").
char* DAB::getChipID(void) {
  return ChipType;
}

// Return the loaded firmware version string ("major.minor.build").
char* DAB::getFirmwareVersion(void) {
  return FirmwVersion;
}

// Sanity check: query the chip status. Returns true if it looks hung
// (caller responds by reinitialising the chip via doRecovery()).
bool DAB::panic(void) {
  si468x::SystemState state;
  const si468x::Result result = chip.getSystemState(state, 100000UL);
  finishCommandDiagnostics(result);
  if (result != si468x::Result::Ok) return true;
  return state.image != (isFm() ? si468x::Image::FMHD : si468x::Image::DAB);
}

int16_t DAB::getRSSI(void) {
  if (isFm()) return static_cast<int16_t>(fmRssi) * 10;
  return dabRssi10;
}

uint32_t DAB::getFreq(uint8_t freq) {
  if (isFm()) return static_cast<uint32_t>(fmFrequency10kHz) * 10UL;
  return DABfrequencyTable_DAB[freq].frequency;
}

const char* DAB::getChannel(uint8_t freq) {
  if (isFm()) return "FM";
  return DABfrequencyTable_DAB[freq].label;
}

// Set the chip's internal audio attenuator (the headphone amp does the
// fine volume; this is mostly a coarse control).
void DAB::vol(uint8_t vol) {
  Set_Property(0x0300, (vol & 0x3F));
}

// SET_PROPERTY (cmd 0x13): write one of the chip's internal properties such
// as sample rate, audio output config, FIC interrupt source. See AN649 §6.
static void Set_Property(uint16_t property, uint16_t value) {
  const si468x::Result result = chip.setProperty(property, value);
  finishCommandDiagnostics(result);
  if (result != si468x::Result::Ok)
    Serial.printf("[RADIO/PROP] set 0x%04X=0x%04X failed result=%d\n",
                  static_cast<unsigned>(property),
                  static_cast<unsigned>(value), static_cast<int>(result));
}

RadioControlMode DAB::controlMode(void) const {
  return radioControlMode;
}

const char* DAB::controlModeName(void) const {
  if (radioControlMode == RADIO_CTRL_DETECT) return "DETECT";
  return radioControlMode == RADIO_CTRL_INTB ? "INTB" : "POLL";
}

const char* DAB::intbHardwareName(void) const {
  if (radioIntbCapability == RadioIntbCapability::Present) return "INTB";
  if (radioIntbCapability == RadioIntbCapability::Absent) return "Polling";
  return "Unknown";
}

void DAB::applyFmRegionProperties(void) {
  const FmRegionProfile& profile = fmProfile();
  Set_Property(0x3100, profile.minFrequency10kHz);
  Set_Property(0x3101, profile.maxFrequency10kHz);
  Set_Property(0x3102, profile.seekSpacing10kHz);
  Set_Property(0x3900, profile.deEmphasis);
  Serial.printf("[FM/REGION] %s band=%u-%u spacing=%u de-emphasis=%u us data=%s\n",
                profile.menuName, profile.minFrequency10kHz,
                profile.maxFrequency10kHz, profile.seekSpacing10kHz,
                profile.deEmphasis == 0 ? 75U : 50U,
                profile.rbds ? "RBDS" : "RDS");
}

void DAB::setFmRegion(uint8_t region, bool applyNow) {
  activeFmRegion = sanitizeFmRegion(region);
  if (applyNow && isFm()) applyFmRegionProperties();
}

// Cold-start sequence per AN649:
//   1. POWER_UP - configure clock + crystal
//   2. LOAD_INIT + HOST_LOAD - upload the patch + firmware blobs from flash
//   3. BOOT - jump to firmware
//   4. Configure DAB-specific properties (sample rate, audio output, FIC etc.)
// Returns true once the chip reports the DAB image is running.
bool DAB::begin(uint8_t SSpin, RadioMode requestedMode) {
  const uint32_t radioBeginMs = millis();
  if (!chipWorkspace) {
    chipWorkspace = static_cast<uint8_t*>(heap_caps_malloc(
        CHIP_WORKSPACE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!chipWorkspace) {
      Serial.println("[RADIO/BOOT] ERROR: cannot allocate 4096-byte workspace");
      return false;
    }
  }
  if (slideshowSlotSize == 0) slideshowSlotSize = SLS_BASE_SEG_SIZE;
  Serial.printf("[RAM/SLS] single MOT buffer=%u address=%p free=%u largest=%u\n",
                (unsigned)sizeof(slideshowSegBuf), slideshowSegBuf,
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  Serial.println();
  Serial.println("[RADIO] ================================================");
  Serial.printf("[RADIO] begin SS=%u requestedMode=%s\n",
                SSpin, requestedMode == RADIO_MODE_FM ? "FM" : "DAB");
  Serial.printf("[RADIO] heap before begin=%u min=%u\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap());

  diagLoadPhase = 0;
  diagPhaseBytes[0] = diagPhaseBytes[1] = diagPhaseBytes[2] = 0;
  diagNextLoadReport = 65536UL;

  memset(SPIbuffer, 0, sizeof(SPIbuffer));
  slaveSelectPin = SSpin;
  tunePending = false;
  seekPending = false;
  ServiceStart = false;
  ServiceIndex = 0;
  numberofservices = 0;
  dabRssi10 = 0;
  dabSignalTimer = 0;
  dabCommand = DabCommand::None;
  dabCommandRequestId = 0;
  dabTuneRequestId = 0;
  dabWaitingTuneRequestId = 0;
  dabTuneRequestPending = false;
  dabWaitingForStc = false;
  dabRequestedServiceId = 0;
  dabRequestedComponentId = 0;
  dabActiveServiceId = 0;
  dabActiveComponentId = 0;
  dabCommandServiceId = 0;
  dabCommandComponentId = 0;
  dabServiceRequestPending = false;
  dabActiveServiceValid = false;
  dabSignalRefreshPending = false;
  dabServiceListRefreshPending = false;
  dabEnsembleRefreshPending = false;
  dabTimeRefreshPending = false;
  dabAudioRefreshPending = false;
  dabCurrentSubchannelRefreshPending = false;
  dabCurrentServiceRefreshPending = false;
  dabServiceTypeScanIndex = 0;
  dabDataServicePending = false;
  dabDsrvBurstCount = 0;
  lastStatus0 = 0;
  radioDabRuntimeActive = false;
  dabStcPending = false;
  dabDsrvPending = false;
  dabDeviceEventPending = false;

  // GPIO12 capability is detected exactly once per ESP32 run. A later
  // FM/DAB switch or recovery resets the tuner, but restores this immutable HW
  // result instead of probing the physical connection again.
  setRadioIntbInterrupt(false);
  if (radioIntbCapability == RadioIntbCapability::Unknown)
    radioControlMode = RADIO_CTRL_DETECT;
  else if (radioIntbCapability == RadioIntbCapability::Present)
    radioControlMode = RADIO_CTRL_INTB;
  else
    radioControlMode = RADIO_CTRL_POLL;
  __atomic_store_n(&radioIntbPending, false, __ATOMIC_RELEASE);
  __atomic_store_n(&radioIntbEdgeCount, 0U, __ATOMIC_RELEASE);
  radioBootIrqEdgeCount = 0;
  radioPowerUpCtsViaIrq = false;
  radioRuntimeDiagnosticsActive = false;
  diagFmBusySkipCount = 0;
  diagFmRsqStatusCount = 0;
  diagFmRdsStatusCount = 0;
  diagCtsIrqCompletionCount = 0;
  diagCtsPollCompletionCount = 0;
  diagFmLastReportMs = 0;
  diagDabStcCount = 0;
  diagDabDsrvCount = 0;
  diagDabDsrvOverflowCount = 0;
  diagDabDeviceEventCount = 0;
  diagDabCommandErrorCount = 0;
  diagDabBusySkipCount = 0;
  diagDabLastReportMs = 0;
  // GPIO12 is MTDI on classic ESP32. Hardware using it for INTB must have
  // VDD_SDIO fixed safely at 3.3 V; firmware never reads or writes eFuse.
  pinMode(SI4684_INTB_PIN, INPUT_PULLUP);
  if (radioControlMode != RADIO_CTRL_POLL) setRadioIntbInterrupt(true);
  if (radioControlMode == RADIO_CTRL_DETECT)
    Serial.println("[RADIO/IRQ] detecting INTB on GPIO12");
  else
    Serial.printf("[RADIO/IRQ] reusing startup HW capability=%s\n",
                  radioIntbCapability == RadioIntbCapability::Present
                      ? "INTB"
                      : "POLL");

  pinMode(slaveSelectPin, OUTPUT);
  digitalWrite(slaveSelectPin, HIGH);
  // V16: the Arduino SPI object is initialised once in setup() BEFORE the
  // shared GPIO17 reset and before TFT_eSPI init.  Do not call SPI.begin()
  // again here; all radio traffic is wrapped in beginTransaction/endTransaction.
  Serial.println("[RADIO] V16 shared SPI already initialised in setup; SPI.begin SKIPPED");
  digitalWrite(slaveSelectPin, HIGH);
#ifdef TFT_CS
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  Serial.printf("[RADIO] V16 TFT_CS=%d forced HIGH before radio traffic\n", TFT_CS);
#endif
  Serial.println("[RADIO] SPI ready");

  si468x::HostInterface host;
  host.writeCommand = hostWriteCommand;
  host.readReply = hostReadReply;
  host.timeUs = hostTimeUs;
  host.idle = hostIdle;
  chip.setHost(host);
  chip.setWorkspace(chipWorkspace, CHIP_WORKSPACE_BYTES);
  chip.setStatusCallback(statusChanged);
  chip.setCtsPollIntervalUs(radioControlMode == RADIO_CTRL_INTB
                                ? RADIO_INTB_CTS_SAFETY_US
                                : 1000UL);
  chip.setIdleStatusPollIntervalUs(radioControlMode == RADIO_CTRL_INTB
                                       ? 50000UL
                                       : 20000UL);
  Serial.printf("[RADIO/BOOT] workspace=%u chunk=%u CTS bootstrap=%s\n",
                static_cast<unsigned>(CHIP_WORKSPACE_BYTES),
                static_cast<unsigned>(CHIP_HOST_LOAD_PAYLOAD),
                controlModeName());

  // Check which image is already running before POWER_UP. This is important on
  // this PCB because resetting/reflashing the ESP32 does not necessarily reset
  // the Si4684. POWER_UP is a bootloader/startup command and is rejected by an
  // already running DAB/FM application image.
  delay(5);
  si468x::SystemState preState;
  Serial.println("[RADIO] PRECHECK GET_SYS_STATE before POWER_UP");
  si468x::Result preResult = chip.getSystemState(preState, 100000UL);
  Serial.printf("[RADIO] PRECHECK result=%d image=%u status0=0x%02X status3=0x%02X\n",
                static_cast<int>(preResult),
                preResult == si468x::Result::Ok ? static_cast<unsigned>(preState.image) : 255U,
                preResult == si468x::Result::Ok ? static_cast<unsigned>(preState.status.status0) : static_cast<unsigned>(lastStatus0),
                preResult == si468x::Result::Ok ? static_cast<unsigned>(preState.status.status3) : 0U);

  const si468x::Image expected = requestedMode == RADIO_MODE_FM ? si468x::Image::FMHD : si468x::Image::DAB;
  const bool reuseRunningImage =
      preResult == si468x::Result::Ok && preState.image == expected;

  // V9: do not touch the shared GPIO17 reset when the requested application is
  // already running.  This preserves the TFT controller state.  A fresh/cold
  // Si4684 STARTUP state (for example the diagnostic image value seen as 9) is
  // still allowed to continue through POWER_UP + HOST_LOAD + BOOT.  If a real
  // opposite application image is active, refuse here; mode switching is kept
  // separate from this startup test because GPIO17 also resets the TFT.
  if (reuseRunningImage) {
    if (radioIntbCapability == RadioIntbCapability::Unknown) {
      Serial.println("[RADIO/IRQ] running image without startup POWER_UP; INTB detection inconclusive");
      return false;
    }
    Serial.printf("[RADIO] V9 active image already matches requested=%u - REUSE, no POWER_UP/upload\n",
                  static_cast<unsigned>(expected));
  } else if (preResult == si468x::Result::Ok &&
             (preState.image == si468x::Image::DAB || preState.image == si468x::Image::FMHD)) {
    Serial.printf("[RADIO] V9 opposite active application image=%u requested=%u - refusing shared reset\n",
                  static_cast<unsigned>(preState.image), static_cast<unsigned>(expected));
    return false;
  } else {
    Serial.printf("[RADIO] V9 startup/bootloader state image=%u - boot sequence required\n",
                  preResult == si468x::Result::Ok ? static_cast<unsigned>(preState.image) : 255U);
  }

  si468x::PowerUpConfig power;
  // During the first ESP32 boot DETECT is a real hybrid IRQ+poll mode. The
  // FALLING edge both proves the physical connection and wakes the same
  // generic command engine that completes POWER_UP.
  power.ctsInterruptEnable = true;
  power.clockMode = 1;
  power.trSize = 7;
  power.iBias = 0x48;
  power.crystalFrequencyHz = 19200000UL;
  power.cTune = 0x1F;
  power.iBiasRun = 0x18;

  // V15 clean A/B test:
  //   FM  -> FMHD 5.3.3 (unchanged)
  //   DAB -> DAB 6.0.9 (only firmware-image change versus V14.1)
  // Reset handling, TFT recovery, patch and SPI timing remain unchanged.
  const uint8_t* image =
      requestedMode == RADIO_MODE_FM ? si468x_fmhd_5_3_3 : si468x_dab_6_0_9;
  const uint32_t imageSize =
      requestedMode == RADIO_MODE_FM ? si468x_fmhd_5_3_3_size : si468x_dab_6_0_9_size;

  si468x::Result result = si468x::Result::Ok;
  uint32_t patchUploadMs = 0;
  uint32_t firmwareUploadMs = 0;
  if (!reuseRunningImage) {
    Serial.printf("[RADIO] bootHostImage start: patch=%u B image=%s %u B\n",
                  static_cast<unsigned>(sizeof(rom_patch_016)),
                  requestedMode == RADIO_MODE_FM ? "FMHD 5.3.3" : "DAB 6.0.9",
                  static_cast<unsigned>(imageSize));
    const uint32_t bootStartMs = millis();

    if (radioIntbCapability == RadioIntbCapability::Unknown) {
      // The proof must belong to this POWER_UP, not to the pre-flight status
      // query or to a stale level left by an earlier device state.
      __atomic_store_n(&radioIntbPending, false, __ATOMIC_RELEASE);
      __atomic_store_n(&radioIntbEdgeCount, 0U, __ATOMIC_RELEASE);
      radioPowerUpCtsViaIrq = false;
    }
    result = chip.powerUp(power, 1000000UL);
    if (radioIntbCapability == RadioIntbCapability::Unknown) {
      if (result != si468x::Result::Ok) {
        Serial.println("[RADIO/IRQ] POWER_UP failed; INTB detection inconclusive");
      } else if (radioIntbEdges() > 0U) {
        radioIntbCapability = RadioIntbCapability::Present;
        radioControlMode = RADIO_CTRL_INTB;
        chip.setCtsPollIntervalUs(RADIO_INTB_CTS_SAFETY_US);
        chip.setIdleStatusPollIntervalUs(50000UL);
        Serial.printf("[RADIO/IRQ] POWER_UP CTS via %s\n",
                      radioPowerUpCtsViaIrq ? "INTB" : "safety poll");
        Serial.println("[RADIO/IRQ] INTB connected; IRQ mode");
      } else {
        radioIntbCapability = RadioIntbCapability::Absent;
        Serial.println("[RADIO/IRQ] POWER_UP completed by polling");
        usePollingFallback("no INTB edge");
      }
    }

    const uint32_t patchStartMs = millis();
    if (result == si468x::Result::Ok) result = chip.loadInit(1000000UL);
    if (result == si468x::Result::Ok)
      result = chip.hostLoadImage(
          progmemImageReader, const_cast<uint8_t*>(rom_patch_016),
          sizeof(rom_patch_016), 1000000UL);
    if (result == si468x::Result::Ok) delay(4);
    patchUploadMs = millis() - patchStartMs;

    const uint32_t firmwareStartMs = millis();
    if (result == si468x::Result::Ok) result = chip.loadInit(1000000UL);
    if (result == si468x::Result::Ok)
      result = chip.hostLoadImage(
          progmemImageReader, const_cast<uint8_t*>(image), imageSize,
          1000000UL);
    firmwareUploadMs = millis() - firmwareStartMs;
    if (result == si468x::Result::Ok) result = chip.boot(1000000UL);

    Serial.printf("[RADIO] bootHostImage result=%d elapsed=%u ms status0=0x%02X patchBytes=%u fwBytes=%u\n",
                  static_cast<int>(result),
                  static_cast<unsigned>(millis() - bootStartMs),
                  static_cast<unsigned>(lastStatus0),
                  static_cast<unsigned>(diagPhaseBytes[1]),
                  static_cast<unsigned>(diagPhaseBytes[2]));
    if (result != si468x::Result::Ok) {
      Serial.println("[RADIO] ERROR: bootHostImage failed");
      return false;
    }

  } else {
    Serial.println("[RADIO] V9 bootHostImage skipped; preserving running image and TFT state");
  }

  // Everything through BOOT belongs to the bootloader diagnostic phase. The
  // edge delta below is runtime telemetry only; it never reclassifies HW.
  radioBootIrqEdgeCount = radioIntbEdges();
  Serial.printf("[RADIO/IRQ] boot INTB edges=%u\n",
                static_cast<unsigned>(radioBootIrqEdgeCount));

  // BOOT resets application properties. Configure the physical INTB output
  // before enabling its CTS source. While doing so, force the common transport
  // into polling: the commands which create runtime IRQ signalling cannot
  // depend on that signalling for their own completion.
  const bool runtimeIntbRequested =
      radioIntbCapability == RadioIntbCapability::Present;
  const uint16_t pinConfig = RADIO_PIN_CONFIG_AUDIO |
      (runtimeIntbRequested ? RADIO_PIN_CONFIG_INTBOUTEN : 0U);
  if (runtimeIntbRequested) radioControlMode = RADIO_CTRL_POLL;
  chip.setCtsPollIntervalUs(1000);
  chip.setIdleStatusPollIntervalUs(20000);

  const si468x::Result pinConfigResult =
      chip.setProperty(si468x::Property::PIN_CONFIG_ENABLE, pinConfig);
  Serial.printf("[RADIO/IRQ] runtime PIN_CONFIG=0x%04X result=%d\n",
                static_cast<unsigned>(pinConfig),
                static_cast<int>(pinConfigResult));

  si468x::Result runtimeCtsResult = si468x::Result::Ok;
  si468x::Result pendingStatusResult = si468x::Result::Ok;
  si468x::Status pendingStatus;
  if (runtimeIntbRequested && pinConfigResult == si468x::Result::Ok) {
    runtimeCtsResult = chip.setInterruptEnable(si468x::INTERRUPT_CTS);
    Serial.printf("[RADIO/IRQ] runtime sources=0x%04X stage=bootstrap result=%d\n",
                  static_cast<unsigned>(si468x::INTERRUPT_CTS),
                  static_cast<int>(runtimeCtsResult));
    if (runtimeCtsResult == si468x::Result::Ok) {
      // Observe the status accumulated while IRQ assistance was off. The
      // host-side edge latch is cleared below; mode handlers acknowledge their
      // own sticky STC/RSQ/ACF/RDS sources with the documented ACK arguments.
      pendingStatusResult = chip.readStatus(pendingStatus);
      Serial.printf("[RADIO/IRQ] runtime pending status=0x%02X readResult=%d\n",
                    static_cast<unsigned>(pendingStatus.status0),
                    static_cast<int>(pendingStatusResult));
    }
  }

  if (runtimeIntbRequested) {
    radioControlMode = RADIO_CTRL_INTB;
    if (pinConfigResult == si468x::Result::Ok &&
        runtimeCtsResult == si468x::Result::Ok &&
        pendingStatusResult == si468x::Result::Ok) {
      takeRadioIntb();
      chip.setCtsPollIntervalUs(RADIO_INTB_CTS_SAFETY_US);
      chip.setIdleStatusPollIntervalUs(50000);
      Serial.println("[RADIO/IRQ] runtime INTB armed");
    } else {
      useRuntimePollingFallback("runtime INTB bootstrap failed");
    }
  }

  if (pinConfigResult != si468x::Result::Ok) {
    Serial.println("[RADIO] ERROR: PIN_CONFIG_ENABLE failed");
    return false;
  }

  si468x::PartInfo part;
  si468x::SystemState state;

  Serial.println("[RADIO] GET_PART_INFO");
  result = chip.getPartInfo(part);
  Serial.printf("[RADIO] GET_PART_INFO result=%d part=%u status0=0x%02X\n",
                static_cast<int>(result),
                result == si468x::Result::Ok ? static_cast<unsigned>(part.partNumber) : 0U,
                static_cast<unsigned>(lastStatus0));
  if (result != si468x::Result::Ok || part.partNumber != 4684) {
    Serial.println("[RADIO] ERROR: GET_PART_INFO failed or part != 4684");
    return false;
  }
  snprintf(ChipType, sizeof(ChipType), "SI%u", part.partNumber);

  Serial.println("[RADIO] GET_SYS_STATE");
  result = chip.getSystemState(state);
  Serial.printf("[RADIO] GET_SYS_STATE result=%d image=%u status0=0x%02X\n",
                static_cast<int>(result),
                result == si468x::Result::Ok ? static_cast<unsigned>(state.image) : 0xFFU,
                static_cast<unsigned>(lastStatus0));
  if (result != si468x::Result::Ok) {
    Serial.println("[RADIO] ERROR: GET_SYS_STATE failed");
    return false;
  }
  if (state.image != expected) {
    Serial.printf("[RADIO] ERROR: wrong active image expected=%u actual=%u\n",
                  static_cast<unsigned>(expected), static_cast<unsigned>(state.image));
    return false;
  }
  activeMode = requestedMode;

  si468x::FunctionInfo functionInfo;
  Serial.println("[RADIO] GET_FUNC_INFO");
  result = chip.getFunctionInfo(functionInfo);
  Serial.printf("[RADIO] GET_FUNC_INFO result=%d status0=0x%02X\n",
                static_cast<int>(result), static_cast<unsigned>(lastStatus0));
  if (result != si468x::Result::Ok) {
    Serial.println("[RADIO] ERROR: GET_FUNC_INFO failed");
    return false;
  }
  snprintf(FirmwVersion, sizeof(FirmwVersion), "%u.%u.%u",
           functionInfo.major, functionInfo.minor, functionInfo.build);
  Serial.printf("[RADIO] Si%u image=%s firmware=%s\n", part.partNumber,
                requestedMode == RADIO_MODE_FM ? "FMHD" : "DAB", FirmwVersion);

  Serial.println("[RADIO] shared properties begin");
  // Shared audio/front-end setup retained from the proven DAB configuration.
  Set_Property(0x0200, 0x8000);
  Set_Property(0x0202, 0x1600);
  Set_Property(0x1710, 0xFC4A);
  Set_Property(0x1711, 0x00F8);
  Serial.println("[RADIO] shared properties done");

  if (requestedMode == RADIO_MODE_DAB) {
    Serial.println("[RADIO] DAB frequency list + generic properties");
    uint32_t frequencies[38];
    for (uint8_t i = 0; i < 38; ++i)
      frequencies[i] = DABfrequencyTable_DAB[i].frequency;
    const si468x::Result frequencyListResult =
        chip.dabSetFrequencyList(frequencies, 38);

    const si468x::Result dsrvSourceResult =
        chip.setDigitalServiceInterruptSource(
            si468x::DSRV_INTERRUPT_PACKET_READY |
            si468x::DSRV_INTERRUPT_OVERFLOW);
    const si468x::Result dsrvRepeatResult =
        chip.setInterruptRepeat(si468x::INTERRUPT_DSRV);
    const si468x::Result dabEventSourceResult = chip.setProperty(
        si468x::Property::DAB_EVENT_INTERRUPT_SOURCE,
        RADIO_DAB_EVENT_SERVICE_LIST |
            RADIO_DAB_EVENT_RECONFIGURATION);
    Set_Property(static_cast<uint16_t>(si468x::Property::DIGITAL_SERVICE_RESTART_DELAY), 0x0064);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_VALID_RSSI_TIME), 0x0000);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_VALID_RSSI_THRESHOLD), 0x0080);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_EVENT_MIN_SVRLIST_PERIOD), 0x0000);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_EVENT_MIN_SVRLIST_PERIOD_RECONFIG), 0x0000);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_EVENT_MIN_FREQINFO_PERIOD), 0x0000);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_XPAD_ENABLE), 0x0097);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_DRC_OPTION), 0x0002);
    Set_Property(static_cast<uint16_t>(si468x::Property::DAB_CTRL_DAB_MUTE_ENABLE), 0x0000);
    const uint16_t interruptSources =
        si468x::INTERRUPT_CTS | si468x::INTERRUPT_STC |
        si468x::INTERRUPT_DSRV | si468x::INTERRUPT_DEVICE_EVENT;
    const si468x::Result irqResult = chip.setInterruptEnable(interruptSources);
    si468x::Status irqStatus;
    const si468x::Result irqStatusResult =
        irqResult == si468x::Result::Ok
            ? chip.readStatus(irqStatus)
            : irqResult;
    takeRadioIntb();
    Serial.printf("[RADIO/IRQ] DAB freq=%d sources=0x%04X repeat=0x%04X dsrv=0x%04X event=0x%04X irq=%d read=%d\n",
                  static_cast<int>(frequencyListResult),
                  static_cast<unsigned>(interruptSources),
                  static_cast<unsigned>(si468x::INTERRUPT_DSRV),
                  static_cast<unsigned>(si468x::DSRV_INTERRUPT_PACKET_READY |
                                        si468x::DSRV_INTERRUPT_OVERFLOW),
                  static_cast<unsigned>(RADIO_DAB_EVENT_SERVICE_LIST |
                                        RADIO_DAB_EVENT_RECONFIGURATION),
                  static_cast<int>(irqResult),
                  static_cast<int>(irqStatusResult));
    if (frequencyListResult != si468x::Result::Ok ||
        dsrvSourceResult != si468x::Result::Ok ||
        dsrvRepeatResult != si468x::Result::Ok ||
        dabEventSourceResult != si468x::Result::Ok ||
        irqResult != si468x::Result::Ok ||
        irqStatusResult != si468x::Result::Ok)
      useRuntimePollingFallback("DAB interrupt-source configuration failed");
  } else {
    Serial.println("[RADIO] FM band/RDS properties");
    applyFmRegionProperties();
    Set_Property(0x3200, 20);    // max tune error
    Set_Property(0x3202, 18);    // seek RSSI threshold (dBuV)
    Set_Property(0x3204, 4);     // seek SNR threshold (dB)
    Set_Property(0x3C00, 0x001B);
    Set_Property(0x3C01, 1);
    Set_Property(0x3C02, 0x0051); // RDS enabled, conservative BLE thresholds
    const uint16_t interruptSources =
        si468x::INTERRUPT_CTS | si468x::INTERRUPT_STC |
        si468x::INTERRUPT_ACF | si468x::INTERRUPT_RDS |
        si468x::INTERRUPT_RSQ | si468x::INTERRUPT_DEVICE_EVENT;
    const si468x::Result irqResult = chip.setInterruptEnable(interruptSources);
    si468x::Status irqStatus;
    const si468x::Result irqStatusResult =
        irqResult == si468x::Result::Ok
            ? chip.readStatus(irqStatus)
            : irqResult;
    takeRadioIntb();
    Serial.printf("[RADIO/IRQ] runtime sources=0x%04X mode=FM result=%d read=%d\n",
                  static_cast<unsigned>(interruptSources),
                  static_cast<int>(irqResult),
                  static_cast<int>(irqStatusResult));
    if (irqResult != si468x::Result::Ok ||
        irqStatusResult != si468x::Result::Ok)
      useRuntimePollingFallback("FM interrupt-source configuration failed");
    clearFmData();
  }

  clearData();
  // No boot/configuration command may leak diagnostic state into FM runtime.
  commandAwaitingCts = false;
  commandStatusReadTriggeredByIrq = false;
  radioRuntimeDiagnosticsActive = true;
  diagFmLastReportMs = millis();
  diagDabLastReportMs = millis();
  radioDabRuntimeActive = requestedMode == RADIO_MODE_DAB;
  Serial.printf("[RADIO/BOOT] patch=%u ms firmware=%u ms total=%u ms mode=%s irqEdges=%u workspace=%u chunk=%u\n",
                static_cast<unsigned>(patchUploadMs),
                static_cast<unsigned>(firmwareUploadMs),
                static_cast<unsigned>(millis() - radioBeginMs),
                controlModeName(), static_cast<unsigned>(radioIntbEdges()),
                static_cast<unsigned>(CHIP_WORKSPACE_BYTES),
                static_cast<unsigned>(CHIP_HOST_LOAD_PAYLOAD));
  Serial.printf("[RADIO] begin SUCCESS mode=%s firmware=%s heap=%u min=%u\n",
                requestedMode == RADIO_MODE_FM ? "FM" : "DAB",
                FirmwVersion, ESP.getFreeHeap(), ESP.getMinFreeHeap());
  return true;
}

// Queue a cooperative DAB metadata refresh. The scheduler starts the actual
// commands one by one after the generic transport becomes idle.
void DAB::EnsembleInfo(void) {
  dabSignalRefreshPending = true;
}

void DAB::parseDabServiceListReply(uint16_t replyLength) {
  if (replyLength < 9U) return;
  const uint16_t listSize = static_cast<uint16_t>(SPIbuffer[5]) |
                            (static_cast<uint16_t>(SPIbuffer[6]) << 8);
  if (static_cast<uint32_t>(listSize) + 6U > replyLength) return;

  const uint8_t parsedServices = SPIbuffer[9];
  if (parsedServices > sizeof(service) / sizeof(DABService)) {
    clearData();
    numberofservices = 0;
    return;
  }

  numberofservices = parsedServices;
  uint16_t offset = 13U;
  for (uint8_t i = 0; i < numberofservices; ++i) {
    if (static_cast<uint32_t>(offset) + 24U > replyLength + 1U) {
      clearData();
      numberofservices = 0;
      return;
    }

    serviceID = static_cast<uint32_t>(SPIbuffer[offset]) |
                (static_cast<uint32_t>(SPIbuffer[offset + 1]) << 8) |
                (static_cast<uint32_t>(SPIbuffer[offset + 2]) << 16) |
                (static_cast<uint32_t>(SPIbuffer[offset + 3]) << 24);
    const uint8_t numberOfComponents = SPIbuffer[offset + 5] & 0x0FU;
    memcpy(service[i].Label, &SPIbuffer[offset + 8], 16);
    service[i].Label[16] = '\0';
    for (int8_t j = 15; j >= 0 && service[i].Label[j] == ' '; --j)
      service[i].Label[j] = '\0';
    offset = static_cast<uint16_t>(offset + 24U);

    componentID = 0;
    for (uint8_t j = 0; j < numberOfComponents; ++j) {
      if (static_cast<uint32_t>(offset) + 4U > replyLength + 1U) {
        clearData();
        numberofservices = 0;
        return;
      }
      if (j == 0) {
        componentID = static_cast<uint32_t>(SPIbuffer[offset]) |
                      (static_cast<uint32_t>(SPIbuffer[offset + 1]) << 8) |
                      (static_cast<uint32_t>(SPIbuffer[offset + 2]) << 16) |
                      (static_cast<uint32_t>(SPIbuffer[offset + 3]) << 24);
      }
      offset = static_cast<uint16_t>(offset + 4U);
    }
    service[i].ServiceID = serviceID;
    service[i].CompID = componentID;
    service[i].ServiceType = 0;
  }

  if (numberofservices == 0) return;
  qsort(service, numberofservices, sizeof(DABService), compareCompID);
  if (ServiceIndex >= numberofservices) ServiceIndex = 0;
  if (CurrentServiceID != service[ServiceIndex].ServiceID) {
    for (uint8_t i = 0; i < numberofservices; ++i) {
      if (CurrentServiceID == service[i].ServiceID) {
        ServiceIndex = i;
        break;
      }
    }
  }
}

// Pull one chunk of service-data from the chip. Each chunk is either:
//   - Dynamic Label / Radiotext (group flag 0x02) → copy into ServiceData
//   - MOT slideshow header     (0x80 0x00 0x12 ...) → record total length, TID
//   - MOT slideshow segment    (0x00/0x80 ...   ) → memcpy into slideshowSegBuf
// Called on every Update() while the receiver has signal lock.
void DAB::getServiceData(void) {
  uint32_t byte_count = 0;
  uint32_t byte_number = 0;

  // finishDabCommand() has already fetched and parsed the fixed 24-byte DSRV
  // header into SPIbuffer[1..24]. Re-read the preserved reply at full length
  // only when its payload fits the shared 4 KiB buffer.
  byte_count = SPIbuffer[19] + (static_cast<uint32_t>(SPIbuffer[20]) << 8);
  if (byte_count + 24U < sizeof(SPIbuffer)) {
    if (byte_count > 0) {
      const si468x::Result readResult = chip.readCurrentReply(
          SPIbuffer + 1, static_cast<uint16_t>(byte_count + 24U));
      finishCommandDiagnostics(readResult);
      if (readResult == si468x::Result::Ok) {
        byte_count = SPIbuffer[19] + (SPIbuffer[20] << 8);

        // Read Radiotext
        if (((SPIbuffer[8] >> 6) & 0x03) == 0x02 && !((SPIbuffer[25] & 0x10) == 0x10)) {
          const uint32_t textLength = byte_count < sizeof(ServiceData) ? byte_count : sizeof(ServiceData) - 1;
          for (byte_number = 0; byte_number < textLength; byte_number++) ServiceData[byte_number] = (char)SPIbuffer[27 + byte_number];
          ServiceData[byte_number] = '\0';

          // Read Slideshow header - extract total length
        } else if (((SPIbuffer[8] >> 6) & 0x03) == 0x01 &&
                   SPIbuffer[27] == 0x80 && SPIbuffer[28] == 0x00 &&
                   SPIbuffer[29] == 0x12 && byte_count < 200) {
          uint16_t transportID = (SPIbuffer[30] << 8) | SPIbuffer[31];
          uint32_t newLength = (((uint16_t)SPIbuffer[35] << 12) | ((uint16_t)SPIbuffer[36] << 4) | ((uint16_t)SPIbuffer[37] >> 4)) & 0x00FFFF;

          if (newLength > 0) {
            if (SlideShowLength == 0) {
              // A header identifies the object but carries no image payload.
              // Keep the UI idle until the first valid data segment is stored.
              SlideShowLength = newLength;

              // If segments were collected with a different TID, discard them
              if (SlideShowTransportID != 0 && transportID != SlideShowTransportID) {
if (SlideShowDebug) Serial.printf("[SLS] Header TID=%u != segments TID=%u, discarding old segments\n", transportID, SlideShowTransportID);
                clearSegmentBuffer();
                SlideShowByteCounter = 0;
                SlideShowHighestSegment = 0;
                SlideShowTotalSegments = 0;
                SlideShowInit = false;
              }

              SlideShowTransportID = transportID;
              SlideShowInit = true;
              SlideShowLastActivity = millis();
              if (SlideShowDebug) Serial.printf("[SLS] Header received, length=%u, bytes so far=%u, TID=%u\n", SlideShowLength, SlideShowByteCounter, transportID);

              if (SlideShowByteCounter >= SlideShowLength && allSegmentsReceived()) {
                SlideShowTotalSegments = SlideShowHighestSegment + 1;
                if (SlideShowDebug) Serial.printf("[SLS] All segments ready after header, assembling %u segments\n", SlideShowTotalSegments);
                assembleSlideshow();
              }
            } else if (SlideShowLength == newLength) {
              // Same image, new carousel cycle - update TID to accept segments again
              SlideShowTransportID = transportID;
              if (SlideShowDebug) Serial.printf("[SLS] Header confirmed, length=%u, bytes so far=%u, TID=%u\n", SlideShowLength, SlideShowByteCounter, transportID);

              if (SlideShowByteCounter >= SlideShowLength && allSegmentsReceived()) {
                SlideShowTotalSegments = SlideShowHighestSegment + 1;
                if (SlideShowDebug) Serial.printf("[SLS] All segments ready after header, assembling %u segments\n", SlideShowTotalSegments);
                assembleSlideshow();
              }
            } else {
              // A new carousel object replaced an incomplete one.
              if (SlideShowDebug) Serial.printf("[SLS] New header length=%u replaces %u, TID=%u\n", newLength, SlideShowLength, transportID);
              clearSegmentBuffer();
              SlideShowLength = newLength;
              SlideShowTransportID = transportID;
              SlideShowByteCounter = 0;
              SlideShowHighestSegment = 0;
              SlideShowTotalSegments = 0;
              SlideShowInit = true;
              SlideShowLastActivity = millis();
            }
          }

          // Read Slideshow packets - store each segment (works with or without header)
        } else if (((SPIbuffer[8] >> 6) & 0x03) == 0x01 &&
                   (SPIbuffer[27] == 0x00 || SPIbuffer[27] == 0x80) &&
                   SPIbuffer[29] == 0x12) {
          uint16_t transportID = (SPIbuffer[30] << 8) | SPIbuffer[31];
          uint8_t segmentNumber = SPIbuffer[28];

          // Check Transport ID
          if (SlideShowTransportID == 0) {
            clearSegmentBuffer();
            SlideShowTransportID = transportID;
            if (SlideShowDebug) Serial.printf("[SLS] Transport ID set to %u\n", transportID);
          } else if (transportID != SlideShowTransportID) {
            // Different carousel object - skip this segment, don't reset
            if (SlideShowDebug) Serial.printf("[SLS] Skipping segment %u, TID=%u (collecting TID=%u)\n", segmentNumber, transportID, SlideShowTransportID);
          }

          if (transportID == SlideShowTransportID) {
            uint8_t byteIndex = segmentNumber / 8;
            uint8_t bitIndex = segmentNumber % 8;

            // Check if we already have this segment
            if (!(SlideShowSegmentBitmap[byteIndex] & (1 << bitIndex))) {
              const uint16_t dataLen = byte_count > 11U
                                           ? static_cast<uint16_t>(byte_count - 11U)
                                           : 0U;
              const bool slotReady = ensureSlideshowSlotSize(dataLen);
              const uint16_t slotCapacity = static_cast<uint16_t>(
                  (sizeof(slideshowSegBuf) + slideshowSlotSize - 1U) /
                  slideshowSlotSize);
              const size_t slotOffset =
                  static_cast<size_t>(segmentNumber) * slideshowSlotSize;
              const bool segmentFits = slotReady &&
                                       slotOffset < sizeof(slideshowSegBuf) &&
                                       dataLen <= sizeof(slideshowSegBuf) - slotOffset;

              // Check the exact range as the last slot may be shorter than the
              // active stride while still fitting inside the 40960-byte buffer.
              if (dataLen > 0 && segmentFits) {
                if (SlideShowByteCounter == 0) {
                  // A Transport ID alone is not enough to claim active MOT
                  // reception. Publish IN PROGRESS only after the first
                  // segment has passed its length and buffer-range checks.
                  beginSlideshowReception();
                  slideshowFirstSegmentMs = millis();
                }
                memcpy(&slideshowSegBuf[slotOffset],
                       &SPIbuffer[34], dataLen);
                slideshowSegLen[segmentNumber] = dataLen;

                // Mark segment as received and update highest seen
                SlideShowSegmentBitmap[byteIndex] |= (1 << bitIndex);
                SlideShowByteCounter += dataLen;
                if (segmentNumber > SlideShowHighestSegment) {
                  SlideShowHighestSegment = segmentNumber;
                }
                SlideShowInit = true;
                SlideShowLastActivity = millis();
                if (SlideShowDebug) Serial.printf("[SLS] Segment %u saved, %u bytes (total %u/%u) TID=%u\n", segmentNumber, dataLen, SlideShowByteCounter, SlideShowLength, transportID);

                // Check if complete - using byte count + all segments when we have header length
                if (SlideShowLength > 0 && SlideShowByteCounter >= SlideShowLength && allSegmentsReceived()) {
                  SlideShowTotalSegments = SlideShowHighestSegment + 1;
                  if (SlideShowDebug) Serial.printf("[SLS] Complete by byte count, assembling %u segments\n", SlideShowTotalSegments);
                  assembleSlideshow();
                }
              } else {
                if (SlideShowDebug) {
                  Serial.printf("[SLS] Drop seg %u (dataLen=%u slot=%u capacity=%u)\n",
                                segmentNumber, dataLen, slideshowSlotSize,
                                slotCapacity);
                }
              }
            } else if (segmentNumber == 0 && SlideShowLength == 0 && SlideShowHighestSegment > 0) {
              // Segment 0 received again (duplicate) - a full broadcast cycle has completed
              if (SlideShowDebug) Serial.printf("[SLS] Segment 0 repeated, highest=%u\n", SlideShowHighestSegment);
              if (allSegmentsReceived()) {
                SlideShowTotalSegments = SlideShowHighestSegment + 1;
                if (SlideShowDebug) Serial.printf("[SLS] Complete by cycle detection, assembling %u segments\n", SlideShowTotalSegments);
                assembleSlideshow();
              }
            }
          }
        }
      }
    }
  }
}

// The only MOT buffer stops representing a complete image as soon as a new
// object starts. The previous picture, if currently visible, remains untouched
// in the TFT controller's GRAM until a new complete image is decoded.
void DAB::beginSlideshowReception(void) {
  SlideShowAvailable = false;
  SlideShowUpdate = false;
  SlideShowUpdate2 = false;
  slideshowRamSize = 0;
  SlideshowReceptionState(true);
}

// Select an adaptive fixed stride without changing the 40960-byte reservation.
// Common small blocks use 512 or 1024 bytes; larger blocks use their exact size
// up to 2048 bytes. Existing out-of-order slots are moved backwards as needed.
bool DAB::ensureSlideshowSlotSize(uint16_t dataLength) {
  if (slideshowSlotSize == 0) slideshowSlotSize = SLS_BASE_SEG_SIZE;
  if (dataLength <= slideshowSlotSize) return true;
  if (dataLength > SLS_MAX_SEG_SIZE) return false;

  const uint16_t newSlotSize = dataLength <= 1024U ? 1024U : dataLength;
  const uint16_t newCapacity = static_cast<uint16_t>(
      (sizeof(slideshowSegBuf) + newSlotSize - 1U) / newSlotSize);

  for (uint8_t i = 0; i <= SlideShowHighestSegment; ++i) {
    if (slideshowSegLen[i] == 0) continue;
    const size_t newOffset = static_cast<size_t>(i) * newSlotSize;
    if (newOffset >= sizeof(slideshowSegBuf) ||
        slideshowSegLen[i] > sizeof(slideshowSegBuf) - newOffset) {
      return false;
    }
  }

  for (int16_t i = SlideShowHighestSegment; i >= 0; --i) {
    if (slideshowSegLen[i] == 0) continue;
    memmove(slideshowSegBuf + static_cast<size_t>(i) * newSlotSize,
            slideshowSegBuf + static_cast<size_t>(i) * slideshowSlotSize,
            slideshowSegLen[i]);
  }

  Serial.printf("[SLS] slot layout %u -> %u bytes, capacity=%u segments\n",
                slideshowSlotSize, newSlotSize, newCapacity);
  slideshowSlotSize = newSlotSize;
  return true;
}

// Forget every buffered slideshow segment. Clearing the metadata is enough;
// fixed slots in slideshowSegBuf are overwritten by the next received object.
void DAB::clearSegmentBuffer(void) {
  SlideshowReceptionState(false);
  memset(slideshowSegLen, 0, sizeof(slideshowSegLen));
  memset(SlideShowSegmentBitmap, 0, sizeof(SlideShowSegmentBitmap));
  slideshowSlotSize = SLS_BASE_SEG_SIZE;
}

// Check if we have every segment from 0..N where N is either:
//   - SlideShowTotalSegments (set from the MOT header when known), or
//   - SlideShowHighestSegment + 1 (best guess until we see segment 0 wrap).
bool DAB::allSegmentsReceived(void) {
  // Determine how many segments to check
  uint8_t segmentsToCheck = SlideShowTotalSegments;
  if (segmentsToCheck == 0) {
    // No header received, use highest segment seen + 1
    segmentsToCheck = SlideShowHighestSegment + 1;
  }

  if (segmentsToCheck == 0) return false;

  for (uint8_t i = 0; i < segmentsToCheck; i++) {
    uint8_t byteIndex = i / 8;
    uint8_t bitIndex = i % 8;
    if (!(SlideShowSegmentBitmap[byteIndex] & (1 << bitIndex))) {
      return false;
    }
  }
  return true;
}

// Compact the fixed-stride segment slots in place, validate the image header,
// and publish the resulting contiguous RAM buffer. memmove is safe here
// because every destination begins at or below its source slot.
void DAB::assembleSlideshow(void) {
  if (SlideShowDebug) {
    Serial.printf("[SLS] Assembling: %u segments, %u bytes received, %u bytes expected\n",
                  SlideShowTotalSegments, SlideShowByteCounter, SlideShowLength);
  }

  // MOT segments live in adaptive fixed-stride slots in the one buffer.
  // Compact them towards the beginning in place. memmove is required because
  // later source slots can overlap the growing contiguous destination.
  uint32_t actualSize = 0;
  for (uint8_t i = 0; i < SlideShowTotalSegments && i < SLS_MAX_SEGMENTS; i++) {
    if (slideshowSegLen[i] > 0) {
      const size_t src = static_cast<size_t>(i) * slideshowSlotSize;
      memmove(slideshowSegBuf + actualSize,
              slideshowSegBuf + src,
              slideshowSegLen[i]);
      actualSize += slideshowSegLen[i];
    } else if (SlideShowDebug) {
      Serial.printf("[SLS] WARNING: segment %u missing!\n", i);
    }
  }

  const bool sizeValid =
      actualSize > 8 &&
      actualSize <= sizeof(slideshowSegBuf) &&
      (SlideShowLength == 0 || actualSize == SlideShowLength);

  const bool validJPEG =
      sizeValid &&
      slideshowSegBuf[0] == 0xFF &&
      slideshowSegBuf[1] == 0xD8 &&
      slideshowSegBuf[2] == 0xFF &&
      slideshowSegBuf[actualSize - 2] == 0xFF &&
      slideshowSegBuf[actualSize - 1] == 0xD9;

  const bool validPNG =
      sizeValid &&
      slideshowSegBuf[0] == 0x89 &&
      slideshowSegBuf[1] == 0x50 &&
      slideshowSegBuf[2] == 0x4E &&
      slideshowSegBuf[3] == 0x47 &&
      slideshowSegBuf[4] == 0x0D &&
      slideshowSegBuf[5] == 0x0A &&
      slideshowSegBuf[6] == 0x1A &&
      slideshowSegBuf[7] == 0x0A;

  if (!validJPEG && !validPNG) {
    if (SlideShowDebug) {
      Serial.printf("[SLS] REJECTED new image: size=%u expected=%u hdr=%02X %02X %02X %02X\n",
                    actualSize, SlideShowLength,
                    actualSize > 0 ? slideshowSegBuf[0] : 0,
                    actualSize > 1 ? slideshowSegBuf[1] : 0,
                    actualSize > 2 ? slideshowSegBuf[2] : 0,
                    actualSize > 3 ? slideshowSegBuf[3] : 0);
    }
  } else {
    uint32_t incomingHash = 2166136261u;
    for (uint32_t n = 0; n < actualSize; ++n) {
      incomingHash ^= slideshowSegBuf[n];
      incomingHash *= 16777619u;
    }

    uint32_t displayHash = 2166136261u;
    for (uint32_t n = 0; n < actualSize; ++n) {
      displayHash ^= slideshowSegBuf[n];
      displayHash *= 16777619u;
    }

    Serial.printf("[SLS/INPLACE] size=%u inHash=%08X outHash=%08X hdr=%02X %02X %02X %02X tail=%02X %02X\n",
                  actualSize,
                  incomingHash,
                  displayHash,
                  slideshowSegBuf[0], slideshowSegBuf[1],
                  slideshowSegBuf[2], slideshowSegBuf[3],
                  slideshowSegBuf[actualSize - 2],
                  slideshowSegBuf[actualSize - 1]);
    slideshowRamSize = actualSize;
    SlideShowUpdate = true;
    SlideShowUpdate2 = true;
    SlideShowAvailable = true;

    if (SlideShowDebug) {
      Serial.printf("[SLS] RAM image ready: %s, %u bytes\n",
                    validJPEG ? "JPEG" : "PNG", actualSize);
    }
    Serial.printf("[SLS/UI] READY: icon/button enabled, %u bytes\n", actualSize);
    SlideshowReceptionState(false);
    Serial.printf("[SLS/TIME] first-segment-to-ready=%u ms\n", slideshowFirstSegmentMs ? (millis() - slideshowFirstSegmentMs) : 0U);
    Serial.printf("[RAM/SLS] single MOT buffer=%u image=%u\n",
                  (unsigned)sizeof(slideshowSegBuf), actualSize);
  }

  // Reset collection metadata. On success the same buffer now contains the
  // complete image; on failure it remains unavailable until the next object.
  SlideShowInit = false;
  SlideShowTransportID = 0;
  SlideShowByteCounter = 0;
  SlideShowHighestSegment = 0;
  SlideShowTotalSegments = 0;
  SlideShowLength = 0;
  slideshowFirstSegmentMs = 0;
  clearSegmentBuffer();
}

// Populate per-service metadata (PTY, ECC, bitrate, audio mode, sample rate,
// time/date, protection level) for the currently selected service. The
// service list itself is filled in EnsembleInfo().
void DAB::ServiceInfo(void) {
  if (!dabActiveServiceValid) return;
  dabAudioRefreshPending = true;
  dabCurrentSubchannelRefreshPending = true;
  dabCurrentServiceRefreshPending = true;
}

// Wipe every cached metadata field. Called when re-tuning so stale labels,
// PTY etc. from the previous channel don't briefly show up on the display.
void DAB::clearData(void) {
  for (byte x = 0; x < 32; x++) {
    service[x].ServiceID = 0;
    service[x].CompID = 0;
    service[x].ServiceType = 0;
    for (byte y = 0; y < 16; y++) service[x].Label[y] = '\0';
  }
  for (byte x = 0; x < 128; x++) ServiceData[x] = '\0';
}

// Start a DAB tune. Completion is handled cooperatively by Update(); there is
// no multi-second polling loop here.
void DAB::setFreq(uint8_t freq) {
  if (isFm()) return;
  DataUpdate -= 1000;
  numberofservices = 0;
  clearData();

  for (byte x = 0; x < 16; x++) {
    EnsembleLabel[x] = '\0';
    PStext[x] = '\0';
  }

  EID[0] = '\0';
  SID[0] = '\0';
  pty = 36;
  ecc = 0;
  ensembleEcc = 0;
  serviceHasOwnEcc = false;
  protectionlevel = 0;
  bitrate = 0;
  dataServiceCheck = 0;
  ServiceStart = false;
  SlideShowInit = false;
  SlideShowAvailable = false;
  SlideShowLength = 0;
  slideshowRamSize = 0;
  SlideShowTransportID = 0;
  SlideShowByteCounter = 0;
  SlideShowHighestSegment = 0;
  SlideShowTotalSegments = 0;
  clearSegmentBuffer();

  signallock = false;
  fic = 0;
  cnr = 0;
  lastStatus0 = 0;

  ++dabTuneRequestId;
  if (dabTuneRequestId == 0) ++dabTuneRequestId;
  dabRequestedFrequency = freq;
  dabTuneRequestPending = true;
  dabWaitingForStc = false;
  dabStcPending = false;
  dabServiceRequestPending = false;
  dabActiveServiceValid = false;
  dabServiceListRefreshPending = false;
  dabServiceTypeScanIndex = 0;
  dabDataServicePending = false;
  tunePending = true;
  DataUpdate = millis() - 500U;
  Serial.printf("[DAB/ASYNC] queued tune index=%u request=%u\n",
                freq, static_cast<unsigned>(dabTuneRequestId));
}

void DAB::clearFmData(void) {
  fmValid = false;
  fmAfcRail = false;
  fmPilot = false;
  fmStereoBlend = 0;
  fmRssi = -100;
  fmSnr = 0;
  fmMultipath = 0;
  fmPi = 0;
  fmPty = 0;
  fmPsMask = 0;
  fmRtMask = 0;
  fmRtSeenMask = 0;
  fmRtAb = false;
  fmRtVersionB = false;
  fmRtVersionKnown = false;
  memset(fmPs, 0, sizeof(fmPs));
  memset(fmRadioText, 0, sizeof(fmRadioText));
  memset(fmPsWork, ' ', 8); fmPsWork[8] = '\0';
  memset(fmPsCandidate, 0, sizeof(fmPsCandidate));
  memset(fmRtWork, ' ', 64); fmRtWork[64] = '\0';
  memset(PStext, 0, sizeof(PStext));
  memset(ServiceData, 0, sizeof(ServiceData));
  ServiceLabelCharset = 0;
  EnsembleLabelCharset = 0;
  SlideShowAvailable = false;
  SlideShowUpdate = false;
  slideshowRamSize = 0;
  signallock = false;
}

void DAB::setFmFrequency(uint16_t frequency10kHz) {
  if (!isFm()) return;
  frequency10kHz = normalizeFmFrequency(frequency10kHz, activeFmRegion);
  clearFmData();
  fmFrequency10kHz = frequency10kHz;
  lastStatus0 = 0;
  const si468x::Result result = chip.startFmTune(frequency10kHz);
  finishCommandDiagnostics(result);
  tunePending = result == si468x::Result::Pending || result == si468x::Result::Ok;
  seekPending = false;
  tuneDeadline = millis() + 3000UL;
  fmRsqTimer = fmAcfTimer = fmRdsTimer = 0;
}

bool DAB::startFmSeek(bool up) {
  if (!isFm() || tunePending || chip.busy()) return false;
  clearFmData();
  lastStatus0 = 0;
  const si468x::Result result = chip.startFmSeek(up, true);
  finishCommandDiagnostics(result);
  tunePending = result == si468x::Result::Pending || result == si468x::Result::Ok;
  seekPending = tunePending;
  tuneDeadline = millis() + 12000UL;
  fmRsqTimer = fmAcfTimer = fmRdsTimer = 0;
  return tunePending;
}

void DAB::processFmRds(void) {
  si468x::FmRdsGroup group;
  ++diagFmRdsStatusCount;
  const si468x::Result statusResult =
      chip.fmRdsStatus(group, false, false, true, 100000UL);
  finishCommandDiagnostics(statusResult);
  if (statusResult != si468x::Result::Ok) return;
  // Loss of instantaneous RDS sync must not erase a text that was already
  // assembled correctly. A FIFO overrun invalidates only the in-progress
  // assembly; the last published PS/RT remains on screen until replaced.
  if (group.fifoLost) {
    fmPsMask = 0;
    fmRtMask = 0;
    fmRtSeenMask = 0;
    fmRtVersionKnown = false;
    memset(fmPsCandidate, 0, sizeof(fmPsCandidate));
    memset(fmPsWork, ' ', 8); fmPsWork[8] = '\0';
    memset(fmRtWork, ' ', 64); fmRtWork[64] = '\0';
    ++diagFmRdsStatusCount;
    const si468x::Result clearResult =
        chip.fmRdsStatus(group, true, true, true, 100000UL);
    finishCommandDiagnostics(clearResult);
    return;
  }
  if (!group.sync) return;

  // FM_RDS_STATUS can legitimately report sync with an empty FIFO. Its block
  // fields do not contain a new group in that case, so publishing them would
  // corrupt candidate state and could replace an already stable PS.
  if (group.fifoUsed == 0U) return;

  if (group.piValid) fmPi = group.pi;
  if (group.tpPtyValid) {
    fmPty = group.pty;
    pty = fmPty;
  }
  // BLE 0 and 1 are clean or corrected by at most two bits. Do not use BLE 2
  // for text: a 3-5 bit correction can otherwise become a visible character.
  if (group.ble[1] > 1U) return;

  const uint16_t blockB = group.block[1];
  const uint8_t groupType = static_cast<uint8_t>((blockB >> 12) & 0x0F);
  const bool versionB = (blockB & 0x0800U) != 0;

  if (groupType == 0 && group.ble[3] <= 1U) {
    const uint8_t segment = blockB & 0x03U;
    fmPsWork[segment * 2] = static_cast<char>(group.block[3] >> 8);
    fmPsWork[segment * 2 + 1] = static_cast<char>(group.block[3] & 0xFF);
    fmPsMask |= static_cast<uint8_t>(1U << segment);
    if (fmPsMask == 0x0F) {
      bool validPs = false;
      for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t c = static_cast<uint8_t>(fmPsWork[i]);
        if (c < 0x20U) {
          validPs = false;
          break;
        }
        if (c != ' ') validPs = true;
      }
      if (!validPs) {
        fmPsMask = 0;
        memset(fmPsCandidate, 0, sizeof(fmPsCandidate));
        return;
      }
      if (memcmp(fmPsCandidate, fmPsWork, 8) == 0) {
        memcpy(fmPs, fmPsWork, 8); fmPs[8] = '\0';
        memset(PStext, 0, sizeof(PStext));
        memcpy(PStext, fmPs, 8);
        Serial.printf("[FM/RDS] PS confirmed='%s'\n", fmPs);
      } else {
        memcpy(fmPsCandidate, fmPsWork, 8);
        fmPsCandidate[8] = '\0';
        Serial.printf("[FM/RDS] PS candidate='%s'\n", fmPsCandidate);
      }
      fmPsMask = 0;
    }
  } else if (groupType == 2) {
    const bool ab = (blockB & 0x0010U) != 0;
    if (!fmRtVersionKnown || versionB != fmRtVersionB || ab != fmRtAb) {
      fmRtAb = ab;
      fmRtVersionB = versionB;
      fmRtVersionKnown = true;
      fmRtMask = 0;
      fmRtSeenMask = 0;
      memset(fmRtWork, ' ', 64); fmRtWork[64] = '\0';
    }

    const uint8_t segment = blockB & 0x0FU;
    const uint8_t charsPerSegment = versionB ? 2U : 4U;
    const uint8_t pos = segment * charsPerSegment;
    char segmentData[4];

    if (!versionB && group.ble[2] <= 1U && group.ble[3] <= 1U) {
      segmentData[0] = static_cast<char>(group.block[2] >> 8);
      segmentData[1] = static_cast<char>(group.block[2] & 0xFF);
      segmentData[2] = static_cast<char>(group.block[3] >> 8);
      segmentData[3] = static_cast<char>(group.block[3] & 0xFF);
    } else if (versionB && group.ble[3] <= 1U) {
      segmentData[0] = static_cast<char>(group.block[3] >> 8);
      segmentData[1] = static_cast<char>(group.block[3] & 0xFF);
    } else {
      return;
    }

    // RDS RadioText uses 0x0D as its end marker. Other C0 control bytes are
    // not displayable text and indicate that this segment must be reacquired.
    for (uint8_t i = 0; i < charsPerSegment; ++i) {
      const uint8_t c = static_cast<uint8_t>(segmentData[i]);
      if (c < 0x20U && c != 0x0DU && c != 0x0AU) return;
    }

    const uint16_t segmentBit = static_cast<uint16_t>(1U << segment);
    if ((fmRtSeenMask & segmentBit) == 0) {
      memcpy(fmRtWork + pos, segmentData, charsPerSegment);
      fmRtSeenMask |= segmentBit;
      fmRtMask &= static_cast<uint16_t>(~segmentBit);
    } else if (memcmp(fmRtWork + pos, segmentData, charsPerSegment) == 0) {
      // A segment becomes publishable only after an identical repeat.
      fmRtMask |= segmentBit;
    } else {
      // A changed segment without an A/B toggle can be a marginal reception
      // or a non-conforming dynamic update. Reacquire the whole message so old
      // and new segments cannot be combined on screen.
      memset(fmRtWork, ' ', 64); fmRtWork[64] = '\0';
      memcpy(fmRtWork + pos, segmentData, charsPerSegment);
      fmRtSeenMask = segmentBit;
      fmRtMask = 0;
    }

    // Publish only after every required segment has been seen identically at
    // least twice. If no end marker is present, all 16 segments are required.
    const uint8_t maxChars = versionB ? 32U : 64U;
    int16_t endPos = -1;
    for (uint8_t i = 0; i < maxChars; ++i) {
      const uint8_t c = static_cast<uint8_t>(fmRtWork[i]);
      if (c == 0x0D || c == '\n') {
        endPos = i;
        break;
      }
    }

    uint8_t requiredLastSegment = 15U;
    if (endPos >= 0) {
      requiredLastSegment = static_cast<uint8_t>(endPos / charsPerSegment);
    }

    const uint16_t requiredMask =
        requiredLastSegment == 15U
            ? 0xFFFFU
            : static_cast<uint16_t>((1UL << (requiredLastSegment + 1U)) - 1UL);

    if ((fmRtMask & requiredMask) == requiredMask) {
      memcpy(fmRadioText, fmRtWork, maxChars);
      fmRadioText[maxChars] = '\0';

      if (endPos >= 0 && endPos < maxChars) fmRadioText[endPos] = '\0';

      for (int16_t i = static_cast<int16_t>(maxChars) - 1;
           i >= 0 && fmRadioText[i] == ' '; --i) {
        fmRadioText[i] = '\0';
      }

      memset(ServiceData, 0, sizeof(ServiceData));
      strncpy(ServiceData, fmRadioText, sizeof(ServiceData) - 1);
      Serial.printf("[FM/RDS] RT complete mask=0x%04X text='%s'\n",
                    fmRtMask, fmRadioText);
    }
  }
}

void DAB::updateFm(void) {
  const uint32_t now = millis();
  const bool reportDiagnostics =
      static_cast<uint32_t>(now - diagFmLastReportMs) >= RADIO_FM_DIAG_INTERVAL_MS;
  if (chip.busy()) {
    ++diagFmBusySkipCount;
    if (reportDiagnostics) {
      diagFmLastReportMs = now;
      Serial.printf("[FM/IRQ] edges=%u busySkip=%u rsq=%u rds=%u ctsIrq=%u ctsPoll=%u\n",
                    static_cast<unsigned>(radioRuntimeIntbEdges()),
                    static_cast<unsigned>(diagFmBusySkipCount),
                    static_cast<unsigned>(diagFmRsqStatusCount),
                    static_cast<unsigned>(diagFmRdsStatusCount),
                    static_cast<unsigned>(diagCtsIrqCompletionCount),
                    static_cast<unsigned>(diagCtsPollCompletionCount));
    }
    return;
  }

  const bool stc = (lastStatus0 & si468x::INTERRUPT_STC) != 0;
  const bool timedOut = tunePending && static_cast<int32_t>(now - tuneDeadline) >= 0;
  if ((tunePending && (stc || now - fmRsqTimer >= 100U)) ||
      (!tunePending && now - fmRsqTimer >= 250U)) {
    si468x::FmRsqStatus rsq;
    ++diagFmRsqStatusCount;
    const si468x::Result rsqResult =
        chip.fmRsqStatus(rsq, true, false, timedOut, stc || timedOut, 100000UL);
    finishCommandDiagnostics(rsqResult);
    if (rsqResult == si468x::Result::Ok) {
      if (isFmFrequencyValid(rsq.frequency10kHz, activeFmRegion))
        fmFrequency10kHz = rsq.frequency10kHz;
      // During tune acquisition the command can return a syntactically valid
      // reply whose RSQ VALID bit is still clear. Do not publish its transient
      // metrics (multipath commonly reads 100 and flashes the bar full-scale).
      if (rsq.valid) {
        fmRssi = rsq.rssi;
        fmSnr = rsq.snr;
        const uint8_t rawMultipath =
            rsq.multipath > 100U ? 100U : rsq.multipath;
        // FM_RSQ occasionally returns a single full-scale multipath sample.
        // Filter at the RSQ sample rate so one raw reading cannot flash the UI
        // to 100%; a sustained change still converges normally.
        if (!fmValid)
          fmMultipath = rawMultipath;
        else
          fmMultipath = static_cast<uint8_t>(
              (static_cast<uint16_t>(fmMultipath) * 3U + rawMultipath + 2U) / 4U);
        cnr = rsq.snr < 0 ? 0 : static_cast<uint8_t>(rsq.snr);
        fic = fmMultipath;
      }
      fmValid = rsq.valid;
      fmAfcRail = rsq.afcRail;
      signallock = rsq.valid;
      if (stc || timedOut) {
        tunePending = false;
        seekPending = false;
      }
      lastStatus0 &= static_cast<uint8_t>(~si468x::INTERRUPT_STC);
    }
    fmRsqTimer = now;
  }

  if (!tunePending && now - fmAcfTimer >= 500U) {
    si468x::FmAcfStatus acf;
    const si468x::Result acfResult = chip.fmAcfStatus(acf, true, 100000UL);
    finishCommandDiagnostics(acfResult);
    if (acfResult == si468x::Result::Ok) {
      fmPilot = acf.pilot;
      fmStereoBlend = acf.stereoBlendPercent;
      audiomode = acf.pilot ? 2 : 1;
    }
    fmAcfTimer = now;
  }

  if (!tunePending && (lastStatus0 & si468x::INTERRUPT_RDS || now - fmRdsTimer >= 80U)) {
    processFmRds();
    lastStatus0 &= static_cast<uint8_t>(~si468x::INTERRUPT_RDS);
    fmRdsTimer = now;
  }

  if (reportDiagnostics) {
    diagFmLastReportMs = now;
    Serial.printf("[FM/IRQ] edges=%u busySkip=%u rsq=%u rds=%u ctsIrq=%u ctsPoll=%u\n",
                  static_cast<unsigned>(radioRuntimeIntbEdges()),
                  static_cast<unsigned>(diagFmBusySkipCount),
                  static_cast<unsigned>(diagFmRsqStatusCount),
                  static_cast<unsigned>(diagFmRdsStatusCount),
                  static_cast<unsigned>(diagCtsIrqCompletionCount),
                  static_cast<unsigned>(diagCtsPollCompletionCount));
  }
}

// Start audio for service[_index] in the current ensemble. Resets the
// slideshow segment buffer because the new service has its own MOT stream.
void DAB::setService(uint8_t _index) {
  union {
    uint32_t combine;
    uint8_t monoctet[4];
  } u;

  if (isFm() || _index >= numberofservices) return;

  pty = 36;
  bitrate = 0;
  protectionlevel = 0;
  for (byte x = 0; x < 128; x++) ServiceData[x] = '\0';
  SlideShowByteCounter = 0;
  SlideShowLength = 0;
  slideshowRamSize = 0;
  SlideShowAvailable = false;
  SlideShowInit = false;
  ServiceStart = false;
  ServiceIndex = _index;
  ecc = 0;  // Reset so ServiceInfo() picks up the new service's ECC
  serviceHasOwnEcc = false;
  // Reset segment tracking (RAM-only)
  clearSegmentBuffer();
  SlideShowTotalSegments = 0;
  SlideShowHighestSegment = 0;
  SlideShowTransportID = 0;
  SlideShowLastActivity = 0;

  u.combine = service[ServiceIndex].ServiceID;

  SID[3] = u.monoctet[0] & 0xF;
  SID[2] = (u.monoctet[0] & 0xF0) >> 4;
  SID[1] = u.monoctet[1] & 0xF;
  SID[0] = (u.monoctet[1] & 0xF0) >> 4;

  for (int i = 0; i < 4; i++) {
    if (SID[i] < 10) {
      SID[i] += '0';
    } else {
      SID[i] += 'A' - 10;
    }
  }
  CurrentServiceID = service[ServiceIndex].ServiceID;
  dabRequestedServiceId = service[ServiceIndex].ServiceID;
  dabRequestedComponentId = service[ServiceIndex].CompID;
  dabServiceRequestPending = true;
  dabDataServicePending = false;
  Serial.printf("[DAB/ASYNC] queued service SID=%08X CID=%08X\n",
                static_cast<unsigned>(dabRequestedServiceId),
                static_cast<unsigned>(dabRequestedComponentId));
}

bool DAB::startDabCommand(DabCommand operation, uint8_t command,
                          const uint8_t* args, uint16_t argLength,
                          uint16_t replyLength, uint32_t timeoutUs) {
  if (chip.busy() || dabCommand != DabCommand::None) return false;
  if (replyLength > sizeof(SPIbuffer) - 1U) {
    ++diagDabCommandErrorCount;
    return false;
  }
  if (replyLength > 0) memset(SPIbuffer + 1, 0, replyLength);
  const si468x::Result result = chip.startCommand(
      static_cast<si468x::Command>(command), args, argLength,
      replyLength > 0 ? SPIbuffer + 1 : nullptr, replyLength, timeoutUs);
  if (result == si468x::Result::Pending) {
    dabCommand = operation;
    return true;
  }
  finishCommandDiagnostics(result);
  ++diagDabCommandErrorCount;
  Serial.printf("[DAB/ASYNC] command 0x%02X start failed result=%d\n",
                command, static_cast<int>(result));
  return false;
}

void DAB::finishDabCommand(void) {
  const DabCommand completed = dabCommand;
  const si468x::Result result = chip.lastResult();
  dabCommand = DabCommand::None;
  finishCommandDiagnostics(result);

  if (result != si468x::Result::Ok) {
    ++diagDabCommandErrorCount;
    Serial.printf("[DAB/ASYNC] command failed op=%u result=%d reason=0x%02X\n",
                  static_cast<unsigned>(completed), static_cast<int>(result),
                  static_cast<unsigned>(chip.lastDeviceError()));
    if ((completed == DabCommand::Tune || completed == DabCommand::TuneStatus) &&
        dabCommandRequestId == dabTuneRequestId) {
      dabTuneRequestPending = false;
      dabWaitingForStc = false;
      tunePending = false;
    }
    if (completed == DabCommand::StopService || completed == DabCommand::StartService) {
      dabServiceRequestPending = false;
      if (completed == DabCommand::StartService) ServiceStart = false;
    }
    if (completed == DabCommand::StartDataService) dabDataServicePending = false;
    return;
  }

  const auto parseOk = [](si468x::Result parseResult) {
    return parseResult == si468x::Result::Ok;
  };

  switch (completed) {
    case DabCommand::Tune:
      if (dabCommandRequestId == dabTuneRequestId && !dabTuneRequestPending) {
        dabWaitingTuneRequestId = dabCommandRequestId;
        dabWaitingForStc = true;
        dabTuneDeadlineMs = millis() + RADIO_DAB_TUNE_TIMEOUT_MS;
        Serial.printf("[DAB/ASYNC] tune command accepted request=%u\n",
                      static_cast<unsigned>(dabCommandRequestId));
      }
      break;

    case DabCommand::TuneStatus:
    case DabCommand::SignalStatus: {
      si468x::DabDigradStatus status;
      const si468x::Result parseResult = si468x::Si468x::parseDabDigradStatus(
          SPIbuffer + 1, 23, status);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
        break;
      }
      const bool wasLocked = signallock;
      dabRssi10 = static_cast<int16_t>(status.rssi) * 10;
      fic = status.ficQuality > 100U ? 100U : status.ficQuality;
      cnr = status.cnr;
      signallock = status.valid && status.acquired;

      if (completed == DabCommand::TuneStatus &&
          dabCommandRequestId == dabTuneRequestId) {
        dabWaitingForStc = false;
        tunePending = false;
        if (signallock) {
          dabServiceListRefreshPending = true;
          dabEnsembleRefreshPending = true;
          dabTimeRefreshPending = true;
        }
        Serial.printf("[DAB/ASYNC] tune complete request=%u lock=%u index=%u\n",
                      static_cast<unsigned>(dabCommandRequestId), signallock,
                      status.tuneIndex);
      } else if (completed == DabCommand::SignalStatus && signallock) {
        if (!wasLocked) dabServiceListRefreshPending = true;
        dabEnsembleRefreshPending = true;
        dabTimeRefreshPending = true;
        ServiceInfo();
      }
      break;
    }

    case DabCommand::DsrvHeader: {
      si468x::DsrvHeader header;
      const si468x::Result parseResult = si468x::Si468x::parseDsrvHeader(
          SPIbuffer + 1, 24, header);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
        dabDsrvPending = false;
        break;
      }
      if (header.interruptSource & 0x02U) ++diagDabDsrvOverflowCount;
      if (header.byteCount > 0) getServiceData();
      dabDsrvPending = header.buffersRemaining > 0;
      break;
    }

    case DabCommand::EventStatus: {
      si468x::DabEventStatus event;
      const si468x::Result parseResult = si468x::Si468x::parseDabEventStatus(
          SPIbuffer + 1, 8, event);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
        break;
      }
      dabDeviceEventPending = false;
      if (event.reconfiguration || event.serviceListInterrupt ||
          event.serviceListAvailable)
        dabServiceListRefreshPending = true;
      break;
    }

    case DabCommand::ServiceListHeader: {
      const uint16_t listSize = static_cast<uint16_t>(SPIbuffer[5]) |
                                (static_cast<uint16_t>(SPIbuffer[6]) << 8);
      const uint32_t fullLength = static_cast<uint32_t>(listSize) + 6U;
      if (fullLength < 9U || fullLength > sizeof(SPIbuffer) - 1U) {
        ++diagDabCommandErrorCount;
        Serial.printf("[DAB/ASYNC] invalid service-list length=%u\n",
                      static_cast<unsigned>(fullLength));
        break;
      }
      const si468x::Result readResult = chip.readCurrentReply(
          SPIbuffer + 1, static_cast<uint16_t>(fullLength));
      finishCommandDiagnostics(readResult);
      if (readResult != si468x::Result::Ok) {
        ++diagDabCommandErrorCount;
        break;
      }
      parseDabServiceListReply(static_cast<uint16_t>(fullLength));
      dabServiceTypeScanIndex = 0;
      dabEnsembleRefreshPending = true;
      queueDabDataService();
      break;
    }

    case DabCommand::EnsembleInfo: {
      si468x::DabEnsembleInfo info;
      const si468x::Result parseResult = si468x::Si468x::parseDabEnsembleInfo(
          SPIbuffer + 1, 26, info);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
        break;
      }
      if (info.ensembleId == 0 || info.ensembleId == 0xFFFFU) {
        EnsembleInfoSet = false;
        break;
      }
      EnsembleInfoSet = true;
      if (EID[0] == '\0' || EnsembleLabel[0] == '\0') {
        static const char hex[] = "0123456789ABCDEF";
        EID[0] = hex[(info.ensembleId >> 12) & 0x0F];
        EID[1] = hex[(info.ensembleId >> 8) & 0x0F];
        EID[2] = hex[(info.ensembleId >> 4) & 0x0F];
        EID[3] = hex[info.ensembleId & 0x0F];
        EID[4] = '\0';
        memcpy(EnsembleLabel, info.label, 16);
        EnsembleLabel[16] = '\0';
        for (int8_t i = 15; i >= 0 && EnsembleLabel[i] == ' '; --i)
          EnsembleLabel[i] = '\0';
        // AN649 keeps the ensemble label charset in the byte between ECC and
        // the abbreviation mask; the typed helper intentionally leaves it raw.
        EnsembleLabelCharset = SPIbuffer[24];
      }
      if (ensembleEcc == 0 && info.ecc != 0) ensembleEcc = info.ecc;
      break;
    }

    case DabCommand::Time: {
      si468x::DabTimeInfo time;
      const si468x::Result parseResult = si468x::Si468x::parseDabTime(
          SPIbuffer + 1, 11, time);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
        break;
      }
      Year = time.year;
      Months = time.month;
      Days = time.day;
      Hours = time.hour;
      Minutes = time.minute;
      Seconds = time.second;
      break;
    }

    case DabCommand::ServiceType: {
      const uint8_t index = static_cast<uint8_t>(dabCommandRequestId);
      si468x::DabSubchannelInfo info;
      const si468x::Result parseResult = si468x::Si468x::parseDabSubchannelInfo(
          SPIbuffer + 1, 12, info);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
      } else if (index < numberofservices) {
        service[index].ServiceType = info.serviceMode;
      }
      if (dabServiceTypeScanIndex == index)
        dabServiceTypeScanIndex = static_cast<uint8_t>(index + 1U);
      queueDabDataService();
      break;
    }

    case DabCommand::AudioInfo: {
      si468x::DabAudioInfo info;
      const si468x::Result parseResult = si468x::Si468x::parseDabAudioInfo(
          SPIbuffer + 1, 10, info);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
      } else if (dabActiveServiceValid &&
                 dabCommandServiceId == dabActiveServiceId &&
                 dabCommandComponentId == dabActiveComponentId) {
        bitrate = info.bitRateKbps;
        samplerate = info.sampleRateHz;
        audiomode = info.audioMode;
      }
      break;
    }

    case DabCommand::CurrentSubchannelInfo: {
      si468x::DabSubchannelInfo info;
      const si468x::Result parseResult = si468x::Si468x::parseDabSubchannelInfo(
          SPIbuffer + 1, 12, info);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
      } else if (dabActiveServiceValid &&
                 dabCommandServiceId == dabActiveServiceId &&
                 dabCommandComponentId == dabActiveComponentId) {
        servicetype = info.serviceMode;
        protectionlevel = info.protectionInfo;
      }
      break;
    }

    case DabCommand::CurrentServiceInfo: {
      si468x::DabServiceInfo info;
      const si468x::Result parseResult = si468x::Si468x::parseDabServiceInfo(
          SPIbuffer + 1, 26, info);
      if (!parseOk(parseResult)) {
        ++diagDabCommandErrorCount;
      } else if (dabActiveServiceValid &&
                 dabCommandServiceId == dabActiveServiceId) {
        memcpy(PStext, info.label, 16);
        PStext[16] = '\0';
        for (int8_t i = 15; i >= 0 && PStext[i] == ' '; --i) PStext[i] = '\0';
        pty = info.pty;
        ServiceLabelCharset = info.charset;
        if (ecc == 0) {
          serviceHasOwnEcc = info.ecc != 0;
          ecc = serviceHasOwnEcc ? info.ecc : ensembleEcc;
        }
      }
      break;
    }

    case DabCommand::StopService:
      if (dabActiveServiceValid && dabCommandServiceId == dabActiveServiceId &&
          dabCommandComponentId == dabActiveComponentId) {
        dabActiveServiceValid = false;
        ServiceStart = false;
      }
      break;

    case DabCommand::StartService:
      dabActiveServiceId = dabCommandServiceId;
      dabActiveComponentId = dabCommandComponentId;
      dabActiveServiceValid = true;
      if (dabCommandServiceId == dabRequestedServiceId &&
          dabCommandComponentId == dabRequestedComponentId) {
        dabServiceRequestPending = false;
        ServiceStart = true;
        ServiceInfo();
        queueDabDataService();
      }
      break;

    case DabCommand::StartDataService:
      dataServiceCheck = dabCommandComponentId;
      dabDataServicePending = false;
      break;

    case DabCommand::None:
      break;
  }
}

void DAB::queueDabDataService(void) {
  if (!dabActiveServiceValid || dabServiceTypeScanIndex < numberofservices ||
      dabDataServicePending)
    return;
  for (uint8_t i = 0; i < numberofservices; ++i) {
    if (service[i].ServiceType == 3 &&
        strstr(service[i].Label, "tpeg") == nullptr &&
        strstr(service[i].Label, "TPEG") == nullptr &&
        service[i].CompID != dataServiceCheck) {
      dabDataServiceId = service[i].ServiceID;
      dabDataComponentId = service[i].CompID;
      dabDataServicePending = true;
      break;
    }
  }
}

void DAB::scheduleNextDabCommand(void) {
  if (chip.busy() || dabCommand != DabCommand::None) return;

  const uint8_t zero = 0;
  if (dabDsrvPending && dabDsrvBurstCount < RADIO_DAB_MAX_DSRV_BURST) {
    const uint8_t args[1] = {0x01};
    if (startDabCommand(DabCommand::DsrvHeader,
                        static_cast<uint8_t>(si468x::Command::GET_DIGITAL_SERVICE_DATA),
                        args, sizeof(args), 24)) {
      ++dabDsrvBurstCount;
      dabDsrvPending = false;
    }
    return;
  }
  if (dabDsrvBurstCount >= RADIO_DAB_MAX_DSRV_BURST) dabDsrvBurstCount = 0;

  if (dabTuneRequestPending) {
    const uint32_t requestId = dabTuneRequestId;
    dabStcPending = false;
    const si468x::Result result = chip.startDabTune(dabRequestedFrequency);
    if (result == si468x::Result::Pending) {
      dabTuneRequestPending = false;
      dabCommandRequestId = requestId;
      dabCommand = DabCommand::Tune;
    } else {
      finishCommandDiagnostics(result);
      ++diagDabCommandErrorCount;
      tunePending = false;
      dabTuneRequestPending = false;
      Serial.printf("[DAB/ASYNC] tune start failed result=%d\n",
                    static_cast<int>(result));
    }
    return;
  }

  if (dabWaitingForStc &&
      (dabStcPending || static_cast<int32_t>(millis() - dabTuneDeadlineMs) >= 0)) {
    if (!dabStcPending) Serial.println("[DAB/ASYNC] STC timeout; reading final status");
    const uint8_t args[1] = {0x01};
    dabStcPending = false;
    dabCommandRequestId = dabWaitingTuneRequestId;
    startDabCommand(DabCommand::TuneStatus,
                    static_cast<uint8_t>(si468x::Command::DAB_DIGRAD_STATUS),
                    args, sizeof(args), 23);
    return;
  }

  if (dabDeviceEventPending) {
    const uint8_t args[1] = {0x01};
    if (startDabCommand(DabCommand::EventStatus,
                        static_cast<uint8_t>(si468x::Command::DAB_GET_EVENT_STATUS),
                        args, sizeof(args), 8))
      dabDeviceEventPending = false;
    return;
  }

  if (dabServiceRequestPending) {
    uint8_t args[11] = {0};
    if (dabActiveServiceValid) {
      si468x::writeLe32(args + 3, dabActiveServiceId);
      si468x::writeLe32(args + 7, dabActiveComponentId);
      dabCommandServiceId = dabActiveServiceId;
      dabCommandComponentId = dabActiveComponentId;
      startDabCommand(DabCommand::StopService,
                      static_cast<uint8_t>(si468x::Command::STOP_DIGITAL_SERVICE),
                      args, sizeof(args));
    } else {
      si468x::writeLe32(args + 3, dabRequestedServiceId);
      si468x::writeLe32(args + 7, dabRequestedComponentId);
      dabCommandServiceId = dabRequestedServiceId;
      dabCommandComponentId = dabRequestedComponentId;
      startDabCommand(DabCommand::StartService,
                      static_cast<uint8_t>(si468x::Command::START_DIGITAL_SERVICE),
                      args, sizeof(args));
    }
    return;
  }

  if (dabServiceListRefreshPending && signallock) {
    if (startDabCommand(DabCommand::ServiceListHeader,
                        static_cast<uint8_t>(si468x::Command::GET_DIGITAL_SERVICE_LIST),
                        &zero, 1, 8))
      dabServiceListRefreshPending = false;
    return;
  }

  if (dabServiceTypeScanIndex < numberofservices) {
    const uint8_t index = dabServiceTypeScanIndex;
    uint8_t args[11] = {0};
    si468x::writeLe32(args + 3, service[index].ServiceID);
    si468x::writeLe32(args + 7, service[index].CompID);
    dabCommandRequestId = index;
    startDabCommand(DabCommand::ServiceType,
                    static_cast<uint8_t>(si468x::Command::DAB_GET_SUBCHAN_INFO),
                    args, sizeof(args), 12);
    return;
  }

  if (dabDataServicePending) {
    uint8_t args[11] = {0};
    si468x::writeLe32(args + 3, dabDataServiceId);
    si468x::writeLe32(args + 7, dabDataComponentId);
    dabCommandServiceId = dabDataServiceId;
    dabCommandComponentId = dabDataComponentId;
    startDabCommand(DabCommand::StartDataService,
                    static_cast<uint8_t>(si468x::Command::START_DIGITAL_SERVICE),
                    args, sizeof(args));
    return;
  }

  if (dabSignalRefreshPending) {
    const uint8_t args[1] = {0x08};
    if (startDabCommand(DabCommand::SignalStatus,
                        static_cast<uint8_t>(si468x::Command::DAB_DIGRAD_STATUS),
                        args, sizeof(args), 23))
      dabSignalRefreshPending = false;
    return;
  }

  if (dabEnsembleRefreshPending && signallock) {
    if (startDabCommand(DabCommand::EnsembleInfo,
                        static_cast<uint8_t>(si468x::Command::DAB_GET_ENSEMBLE_INFO),
                        &zero, 1, 26))
      dabEnsembleRefreshPending = false;
    return;
  }

  if (dabTimeRefreshPending && signallock) {
    if (startDabCommand(DabCommand::Time,
                        static_cast<uint8_t>(si468x::Command::DAB_GET_TIME),
                        &zero, 1, 11))
      dabTimeRefreshPending = false;
    return;
  }

  if (dabAudioRefreshPending && dabActiveServiceValid) {
    dabCommandServiceId = dabActiveServiceId;
    dabCommandComponentId = dabActiveComponentId;
    if (startDabCommand(DabCommand::AudioInfo,
                        static_cast<uint8_t>(si468x::Command::DAB_GET_AUDIO_INFO),
                        &zero, 1, 10))
      dabAudioRefreshPending = false;
    return;
  }

  if (dabCurrentSubchannelRefreshPending && dabActiveServiceValid) {
    uint8_t args[11] = {0};
    si468x::writeLe32(args + 3, dabActiveServiceId);
    si468x::writeLe32(args + 7, dabActiveComponentId);
    dabCommandServiceId = dabActiveServiceId;
    dabCommandComponentId = dabActiveComponentId;
    if (startDabCommand(DabCommand::CurrentSubchannelInfo,
                        static_cast<uint8_t>(si468x::Command::DAB_GET_SUBCHAN_INFO),
                        args, sizeof(args), 12))
      dabCurrentSubchannelRefreshPending = false;
    return;
  }

  if (dabCurrentServiceRefreshPending && dabActiveServiceValid) {
    uint8_t args[7] = {0};
    si468x::writeLe32(args + 3, dabActiveServiceId);
    dabCommandServiceId = dabActiveServiceId;
    dabCommandComponentId = dabActiveComponentId;
    if (startDabCommand(DabCommand::CurrentServiceInfo,
                        static_cast<uint8_t>(si468x::Command::DAB_GET_SERVICE_INFO),
                        args, sizeof(args), 26))
      dabCurrentServiceRefreshPending = false;
    return;
  }

  // If DSRV consumed its fairness budget but no control/metadata operation was
  // ready, resume draining immediately.
  if (dabDsrvPending) {
    const uint8_t args[1] = {0x01};
    if (startDabCommand(DabCommand::DsrvHeader,
                        static_cast<uint8_t>(si468x::Command::GET_DIGITAL_SERVICE_DATA),
                        args, sizeof(args), 24)) {
      ++dabDsrvBurstCount;
      dabDsrvPending = false;
    }
  }
}

// Periodic cooperative driver pump. No DAB command waits for CTS or STC here;
// every call advances at most one in-flight command and starts at most one more.
void DAB::Update(void) {
  if (isFm()) {
    if (radioControlMode == RADIO_CTRL_INTB && radioIntbActive()) {
      // Non-blocking tune/seek commands are completed here rather than through
      // hostIdle(), so record that this CTS service was IRQ-triggered as well.
      if (commandAwaitingCts) commandStatusReadTriggeredByIrq = true;
      chip.notifyInterrupt();
    }
    finishCommandDiagnostics(chip.service());
    updateFm();
    return;
  }

  // Current project keeps slideshow data in RAM; do not pull old LittleFS code back in.
  const uint32_t now = millis();
  if (SlideShowInit && SlideShowLastActivity > 0 && now - SlideShowLastActivity > 30000) {
    if (SlideShowDebug) Serial.println("[SLS] Collection timeout, resetting");
    clearSegmentBuffer();
    SlideShowTransportID = 0;
    SlideShowByteCounter = 0;
    SlideShowHighestSegment = 0;
    SlideShowTotalSegments = 0;
    SlideShowLength = 0;
    // The buffer contains an incomplete object and therefore remains
    // unavailable. A picture already decoded to TFT GRAM is not erased here.
    SlideShowInit = false;
    SlideShowLastActivity = 0;
  }

  if (radioControlMode == RADIO_CTRL_INTB && radioIntbActive()) {
    if (commandAwaitingCts) commandStatusReadTriggeredByIrq = true;
    chip.notifyInterrupt();
  }
  const si468x::Result serviceResult = chip.service();
  finishCommandDiagnostics(serviceResult);
  if (dabCommand != DabCommand::None && !chip.busy()) finishDabCommand();

  if (now - DataUpdate >= 500U) {
    dabSignalRefreshPending = true;
    DataUpdate = now;
  }

  if (chip.busy()) {
    ++diagDabBusySkipCount;
  } else {
    scheduleNextDabCommand();
  }

  if (now - diagDabLastReportMs >= RADIO_DAB_DIAG_INTERVAL_MS) {
    diagDabLastReportMs = now;
    Serial.printf("[DAB/IRQ] hw=%s mode=%s edges=%u ctsIrq=%u ctsPoll=%u STC=%u DSRV=%u overflow=%u DEVNT=%u busy=%u errors=%u pending=%02X\n",
                  intbHardwareName(), controlModeName(),
                  static_cast<unsigned>(radioRuntimeIntbEdges()),
                  static_cast<unsigned>(diagCtsIrqCompletionCount),
                  static_cast<unsigned>(diagCtsPollCompletionCount),
                  static_cast<unsigned>(diagDabStcCount),
                  static_cast<unsigned>(diagDabDsrvCount),
                  static_cast<unsigned>(diagDabDsrvOverflowCount),
                  static_cast<unsigned>(diagDabDeviceEventCount),
                  static_cast<unsigned>(diagDabBusySkipCount),
                  static_cast<unsigned>(diagDabCommandErrorCount),
                  static_cast<unsigned>((dabStcPending ? 0x01U : 0U) |
                                        (dabDsrvPending ? 0x10U : 0U) |
                                        (dabDeviceEventPending ? 0x80U : 0U)));
  }
}

// Convert a label/text from the DAB-side character set to UTF-8 for the TFT.
String DAB::ASCII(const char* input, uint8_t charset) {
  if (!input) return String();
  String result;
  if (charset != 0) return String(input);

  // FM RDS text is always the single-byte EBU repertoire. Applying the DAB
  // UTF-8 heuristic to it can mistake two adjacent extended RDS characters for
  // a UTF-8 sequence and pass invalid bytes directly to the TFT.
  if (!isFm()) {
    bool looksLikeUTF8 = false;
    for (size_t i = 0; input[i] != '\0'; i++) {
      uint8_t c = (uint8_t)input[i];

      if ((c & 0xE0) == 0xC0) {
        uint8_t c2 = (uint8_t)input[i + 1];
        if ((c2 & 0xC0) == 0x80) {
          looksLikeUTF8 = true;
          break;
        }
      } else if ((c & 0xF0) == 0xE0) {
        uint8_t c2 = (uint8_t)input[i + 1];
        uint8_t c3 = (uint8_t)input[i + 2];
        if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
          looksLikeUTF8 = true;
          break;
        }
      }
    }

    if (looksLikeUTF8) return String(input);
  }

  wchar_t temp[128];
  charConverter(input, temp, sizeof(temp) / sizeof(wchar_t));
  result = convertToUTF8(temp);

  return result;
}


// qsort() comparator: order services by component ID low byte ascending.
static int compareCompID(const void* a, const void* b) {
  uint32_t compID_a = (*((DABService*)a)).CompID & 0xFF;
  uint32_t compID_b = (*((DABService*)b)).CompID & 0xFF;

  if (compID_a < compID_b) return -1;
  if (compID_a > compID_b) return 1;
  return 0;
}

// Translate an EBU-Latin (ETSI EN 300 401) byte string into Unicode code
// points, handling the DAB-specific shift/escape encodings. Output is a
// wchar_t buffer that convertToUTF8() later serialises as UTF-8.
static void charConverter(const char* input, wchar_t* output, size_t outSize) {
    if (!input || !output || outSize == 0) return;

    size_t i = 0;

    for (size_t dbi = 0; input[dbi] != '\0' && i < outSize - 1; dbi++) {
        uint8_t currentChar = (uint8_t)input[dbi];

        // ----- 2-byte UTF-8 decoding -----
        if ((currentChar & 0xE0) == 0xC0) {
            uint8_t nextChar = (uint8_t)input[dbi + 1];

            if ((nextChar & 0xC0) == 0x80) {
                // decode the Unicode code point
                uint16_t codepoint = ((currentChar & 0x1F) << 6) | (nextChar & 0x3F);
                output[i++] = (wchar_t)codepoint;
                dbi++; // skip the second byte
                continue;
            }
        }

        // ----- Single-byte / fallback -----
        switch (currentChar) {
            case 0x20: output[i] = L' '; break;
            case 0x21 ... 0x5D: output[i] = (wchar_t)currentChar; break;
            case 0x5E: output[i] = L'―'; break;
            case 0x5F: output[i] = L'_'; break;
            case 0x60: output[i] = L'`'; break;
            case 0x61 ... 0x7D: output[i] = (wchar_t)currentChar; break;
            case 0x7E: output[i] = L'¯'; break;
            case 0x7F: output[i] = L' '; break;
            case 0x80: output[i] = L'á'; break;
            case 0x81: output[i] = L'à'; break;
            case 0x82: output[i] = L'é'; break;
            case 0x83: output[i] = L'è'; break;
            case 0x84: output[i] = L'í'; break;
            case 0x85: output[i] = L'ì'; break;
            case 0x86: output[i] = L'ó'; break;
            case 0x87: output[i] = L'ò'; break;
            case 0x88: output[i] = L'ú'; break;
            case 0x89: output[i] = L'ù'; break;
            case 0x8A: output[i] = L'Ñ'; break;
            case 0x8B: output[i] = L'Ç'; break;
            case 0x8C: output[i] = L'Ş'; break;
            case 0x8D: output[i] = L'β'; break;
            case 0x8E: output[i] = L'¡'; break;
            case 0x8F: output[i] = L'Ĳ'; break;
            case 0x90: output[i] = L'â'; break;
            case 0x91: output[i] = L'ä'; break;
            case 0x92: output[i] = L'ê'; break;
            case 0x93: output[i] = L'ë'; break;
            case 0x94: output[i] = L'î'; break;
            case 0x95: output[i] = L'ï'; break;
            case 0x96: output[i] = L'ô'; break;
            case 0x97: output[i] = L'ö'; break;
            case 0x98: output[i] = L'û'; break;
            case 0x99: output[i] = L'ü'; break;
            case 0x9A: output[i] = L'ñ'; break;
            case 0x9B: output[i] = L'ç'; break;
            case 0x9C: output[i] = L'ş'; break;
            case 0x9D: output[i] = L'ǧ'; break;
            case 0x9E: output[i] = L'ı'; break;
            case 0x9F: output[i] = L'ĳ'; break;
            case 0xA0: output[i] = L'Ķ'; break;
            case 0xA1: output[i] = L'Ņ'; break;
            case 0xA2: output[i] = L'©'; break;
            case 0xA3: output[i] = L'Ģ'; break;
            case 0xA4: output[i] = L'Ğ'; break;
            case 0xA5: output[i] = L'ě'; break;
            case 0xA6: output[i] = L'ň'; break;
            case 0xA7: output[i] = L'ő'; break;
            case 0xA8: output[i] = L'Ő'; break;
            case 0xA9: output[i] = L'€'; break;
            case 0xAA: output[i] = L'£'; break;
            case 0xAB: output[i] = L'$'; break;
            case 0xAC: output[i] = L'Ā'; break;
            case 0xAD: output[i] = L'Ē'; break;
            case 0xAE: output[i] = L'Ī'; break;
            case 0xAF: output[i] = L'Ū'; break;
            case 0xB0: output[i] = L'ķ'; break;
            case 0xB1: output[i] = L'ņ'; break;
            case 0xB2: output[i] = L'ļ'; break;
            case 0xB3: output[i] = L'ģ'; break;
            case 0xB4: output[i] = L'ľ'; break;
            case 0xB5: output[i] = L'İ'; break;
            case 0xB6: output[i] = L'ń'; break;
            case 0xB7: output[i] = L'ű'; break;
            case 0xB8: output[i] = L'Ű'; break;
            case 0xB9: output[i] = L'¿'; break;
            case 0xBA: output[i] = L'ľ'; break;
            case 0xBB: output[i] = L'°'; break;
            case 0xBC: output[i] = L'ā'; break;
            case 0xBD: output[i] = L'ē'; break;
            case 0xBE: output[i] = L'ī'; break;
            case 0xBF: output[i] = L'ū'; break;
            case 0xC0: output[i] = L'Á'; break;
            case 0xC1: output[i] = L'À'; break;
            case 0xC2: output[i] = L'É'; break;
            case 0xC3: output[i] = L'È'; break;
            case 0xC4: output[i] = L'Í'; break;
            case 0xC5: output[i] = L'Ì'; break;
            case 0xC6: output[i] = L'Ó'; break;
            case 0xC7: output[i] = L'Ò'; break;
            case 0xC8: output[i] = L'Ú'; break;
            case 0xC9: output[i] = L'Ù'; break;
            case 0xCA: output[i] = L'Ř'; break;
            case 0xCB: output[i] = L'Č'; break;
            case 0xCC: output[i] = L'Š'; break;
            case 0xCD: output[i] = L'Ž'; break;
            case 0xCE: output[i] = L'Ð'; break;
            case 0xCF: output[i] = L'Ŀ'; break;
            case 0xD0: output[i] = L'Â'; break;
            case 0xD1: output[i] = L'Ä'; break;
            case 0xD2: output[i] = L'Ê'; break;
            case 0xD3: output[i] = L'Ë'; break;
            case 0xD4: output[i] = L'Î'; break;
            case 0xD5: output[i] = L'Ï'; break;
            case 0xD6: output[i] = L'Ô'; break;
            case 0xD7: output[i] = L'Ö'; break;
            case 0xD8: output[i] = L'Û'; break;
            case 0xD9: output[i] = L'Ü'; break;
            case 0xDA: output[i] = L'ř'; break;
            case 0xDB: output[i] = L'č'; break;
            case 0xDC: output[i] = L'š'; break;
            case 0xDD: output[i] = L'ž'; break;
            case 0xDE: output[i] = L'đ'; break;
            case 0xDF: output[i] = L'ŀ'; break;
            case 0xE0: output[i] = L'Ã'; break;
            case 0xE1: output[i] = L'Å'; break;
            case 0xE2: output[i] = L'Æ'; break;
            case 0xE3: output[i] = L'Œ'; break;
            case 0xE4: output[i] = L'ŷ'; break;
            case 0xE5: output[i] = L'Ý'; break;
            case 0xE6: output[i] = L'Õ'; break;
            case 0xE7: output[i] = L'Ø'; break;
            case 0xE8: output[i] = L'Þ'; break;
            case 0xE9: output[i] = L'Ŋ'; break;
            case 0xEA: output[i] = L'Ŕ'; break;
            case 0xEB: output[i] = L'Ć'; break;
            case 0xEC: output[i] = L'Ś'; break;
            case 0xED: output[i] = L'Ź'; break;
            case 0xEE: output[i] = L'Ť'; break;
            case 0xEF: output[i] = L'ð'; break;
            case 0xF0: output[i] = L'ã'; break;
            case 0xF1: output[i] = L'å'; break;
            case 0xF2: output[i] = L'æ'; break;
            case 0xF3: output[i] = L'œ'; break;
            case 0xF4: output[i] = L'ŵ'; break;
            case 0xF5: output[i] = L'ý'; break;
            case 0xF6: output[i] = L'õ'; break;
            case 0xF7: output[i] = L'ø'; break;
            case 0xF8: output[i] = L'þ'; break;
            case 0xF9: output[i] = L'ŋ'; break;
            case 0xFA: output[i] = L'ŕ'; break;
            case 0xFB: output[i] = L'ć'; break;
            case 0xFC: output[i] = L'ś'; break;
            case 0xFD: output[i] = L'ź'; break;
            case 0xFE: output[i] = L'ť'; break;
            case 0xFF: output[i] = L' '; break;
            default: output[i] = L'?'; break;
        }
        i++;
    }
    output[i] = L'\0';
}

// Substring helper that operates on code-points (not bytes) so cutting a
// UTF-8 string at index N doesn't slice a multi-byte sequence in half.
static String extractUTF8Substring(const String& utf8String, size_t start, size_t length) {
  String substring;
  size_t utf8Length = utf8String.length();
  size_t utf8Index = 0;
  size_t charIndex = 0;

  while (utf8Index < utf8Length && charIndex < start + length) {
    uint8_t currentByte = utf8String.charAt(utf8Index);
    uint8_t numBytes = 0;

    if (currentByte < 0x80) {
      numBytes = 1;
    } else if ((currentByte >> 5) == 0x6) {
      numBytes = 2;
    } else if ((currentByte >> 4) == 0xE) {
      numBytes = 3;
    } else if ((currentByte >> 3) == 0x1E) {
      numBytes = 4;
    }

    if (charIndex >= start) {
      substring += utf8String.substring(utf8Index, utf8Index + numBytes);
    }

    utf8Index += numBytes;
    charIndex++;
  }

  return substring;
}

// Encode the wchar_t code points produced by charConverter() into a UTF-8
// String suitable for the TFT and the serial protocol.
static String convertToUTF8(const wchar_t* input) {
  String output;
  while (*input) {
    uint32_t unicode = *input;
    if (unicode < 0x80) {
      output += (char)unicode;
    } else if (unicode < 0x800) {
      output += (char)(0xC0 | (unicode >> 6));
      output += (char)(0x80 | (unicode & 0x3F));
    } else if (unicode < 0x10000) {
      output += (char)(0xE0 | (unicode >> 12));
      output += (char)(0x80 | ((unicode >> 6) & 0x3F));
      output += (char)(0x80 | (unicode & 0x3F));
    } else {
      output += (char)(0xF0 | (unicode >> 18));
      output += (char)(0x80 | ((unicode >> 12) & 0x3F));
      output += (char)(0x80 | ((unicode >> 6) & 0x3F));
      output += (char)(0x80 | (unicode & 0x3F));
    }
    input++;
  }
  return output;

}
