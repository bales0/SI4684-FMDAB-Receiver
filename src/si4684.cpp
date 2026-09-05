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
extern uint8_t gpio12Mode;
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
// FM PTY value 0 is a valid RDS value ("Unknown"), so validity must be
// tracked separately from fmPty itself. Cleared on every tune/seek and set
// only after FM_RDS_STATUS explicitly reports TP/PTY valid.
bool fmPtyValid = false;

// DAB labels carry independent charset identifiers. Keep one 4-bit charset
// value per *sorted* service row (32 bytes total). Si468x DLS service-data
// packets carry their own control/charset bytes before the text payload, so
// Dynamic Label decoding is independent from ServiceLabelCharset as well.
static uint8_t dabServiceCharsetValue[32] = {0};
static uint8_t dabDynamicLabelCharset = 0;
static uint8_t dabDynamicLabelLength = 0;

uint8_t DabServiceLabelCharset(uint8_t serviceIndex) {
  return serviceIndex < 32 ? dabServiceCharsetValue[serviceIndex] : 0;
}

uint8_t slaveSelectPin;

static si468x::Si468x chip;
// One 4 KiB workspace reduces the approximately 0.5 MB firmware image to
// 4092-byte HOST_LOAD payloads (4096 bytes including the three command args).
// Allocate it once from internal heap instead of static DRAM so it and the
// independent 50 KiB MOT slideshow buffer do not overcommit the ESP32 DRAM
// linker segment.
static constexpr size_t CHIP_WORKSPACE_BYTES = 4096U;
static constexpr size_t CHIP_HOST_LOAD_PAYLOAD =
    (CHIP_WORKSPACE_BYTES - 3U) & ~static_cast<size_t>(3U);
static uint8_t* chipWorkspace = nullptr;
static volatile uint8_t lastStatus0;
static volatile bool radioIntbPending = false;
// Diagnostic build: count every GPIO12 transition and its direction.
// radioIntbEdgeCount remains the total transition counter used by existing
// runtime telemetry.
static volatile uint32_t radioIntbEdgeCount = 0;
static volatile uint32_t radioIntbFallingCount = 0;
static volatile uint32_t radioIntbRisingCount = 0;
static RadioControlMode radioControlMode = RADIO_CTRL_DETECT;
enum class RadioIntbCapability : uint8_t {
  Unknown,
  Absent,
  Present
};
static RadioIntbCapability radioIntbCapability = RadioIntbCapability::Unknown;
static bool radioIntbInterruptAttached = false;
static int radioIntbInterruptMode = -1;
static uint32_t radioBootIrqEdgeCount = 0;
static bool commandAwaitingCts = false;
static bool commandStatusReadTriggeredByIrq = false;
static uint32_t commandStartedUs = 0;
static bool radioPowerUpCtsViaIrq = false;
static bool radioAppTestCtsViaIrq = false;
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
  // ISR contract: record the transition only. SPI, logging and GUI work stay
  // in cooperative foreground code. CHANGE is deliberate in this diagnostic
  // build so the real Si4684 INTB polarity/pulse shape is observable.
  const uint8_t level = digitalRead(SI4684_INTB_PIN) == HIGH ? HIGH : LOW;
  radioIntbPending = true;
  ++radioIntbEdgeCount;
  if (level == LOW)
    ++radioIntbFallingCount;
  else
    ++radioIntbRisingCount;
}

static bool takeRadioIntb(void) {
  return __atomic_exchange_n(&radioIntbPending, false, __ATOMIC_ACQ_REL);
}

static uint32_t radioIntbEdges(void) {
  return __atomic_load_n(&radioIntbEdgeCount, __ATOMIC_ACQUIRE);
}

static uint32_t radioIntbFallingEdges(void) {
  return __atomic_load_n(&radioIntbFallingCount, __ATOMIC_ACQUIRE);
}

static uint32_t radioIntbRisingEdges(void) {
  return __atomic_load_n(&radioIntbRisingCount, __ATOMIC_ACQUIRE);
}

static uint32_t radioRuntimeIntbEdges(void) {
  return radioIntbEdges() - radioBootIrqEdgeCount;
}

static void setRadioIntbInterrupt(bool enabled, int mode = FALLING) {
  if (!enabled) {
    if (radioIntbInterruptAttached)
      detachInterrupt(digitalPinToInterrupt(SI4684_INTB_PIN));
    radioIntbInterruptAttached = false;
    radioIntbInterruptMode = -1;
    return;
  }

  // DETECT listens to both directions so the first physical transition is
  // impossible to miss. Once INTB is proven, switch to the documented
  // active-low FALLING interrupt and keep the LOW-level safeguard in
  // radioIntbActive() for coalesced/sticky sources.
  if (radioIntbInterruptAttached && radioIntbInterruptMode == mode) return;
  if (radioIntbInterruptAttached)
    detachInterrupt(digitalPinToInterrupt(SI4684_INTB_PIN));

  attachInterrupt(digitalPinToInterrupt(SI4684_INTB_PIN), radioIntbIsr, mode);
  radioIntbInterruptAttached = true;
  radioIntbInterruptMode = mode;
}

static bool radioIntbActive(void) {
  const bool edge = takeRadioIntb();
  // During one-shot HW detection a static LOW level must never masquerade as a
  // real IRQ. Only a transition latched by the ISR is accepted as evidence.
  if (radioControlMode == RADIO_CTRL_DETECT) return edge;

  // Once INTB is proven the ISR uses FALLING. A level check is retained because
  // several Si4684 interrupt sources can coalesce while INTB is already LOW.
  return edge || digitalRead(SI4684_INTB_PIN) == LOW;
}

static void promoteDetectedIntb(const char* stage) {
  if (radioControlMode != RADIO_CTRL_DETECT ||
      radioIntbCapability != RadioIntbCapability::Unknown)
    return;

  // This executes in foreground context, never in the ISR. The transition that
  // caused entry into hostIdle() is the physical proof. Promote immediately so
  // the *same* bootloader/application command and every following command use
  // the normal IRQ-first CTS path. Safety polling remains bounded as backup.
  radioIntbCapability = RadioIntbCapability::Present;
  radioControlMode = RADIO_CTRL_INTB;
  setRadioIntbInterrupt(true, FALLING);
  chip.setCtsPollIntervalUs(RADIO_INTB_CTS_SAFETY_US);
  chip.setIdleStatusPollIntervalUs(50000UL);

  Serial.printf(
      "[RADIO/IRQ] first INTB transition detected during %s cmd=0x%02X phase=%u; IRQ mode ACTIVE\n",
      stage ? stage : "command",
      static_cast<unsigned>(diagLastCommand),
      static_cast<unsigned>(diagLoadPhase));
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
  const bool detecting = radioControlMode == RADIO_CTRL_DETECT;
  if ((radioControlMode == RADIO_CTRL_INTB || detecting) &&
      radioIntbActive()) {
    // The first real GPIO12 transition is enough to prove the connection.
    // Promote before servicing it, so this very first event is already used as
    // a normal IRQ completion and all remaining LOAD_INIT/HOST_LOAD/BOOT
    // commands run IRQ-first instead of staying in DETECT until BOOT finishes.
    if (detecting) promoteDetectedIntb("command");

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

  if (irqTriggered) {
    // hostIdle() may promote DETECT -> INTB before this callback executes, so
    // completion-source diagnostics must not depend on the *current* mode.
    if (diagLastCommand == static_cast<uint8_t>(si468x::Command::POWER_UP))
      radioPowerUpCtsViaIrq = true;
    if (diagLastCommand == static_cast<uint8_t>(si468x::Command::GET_PART_INFO))
      radioAppTestCtsViaIrq = true;
  }

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
  if (!slideshowSegBuf) {
    slideshowSegBuf = static_cast<uint8_t*>(heap_caps_malloc(
        SLS_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!slideshowSegBuf) {
      Serial.println("[RADIO/BOOT] ERROR: cannot allocate 51200-byte MOT buffer");
      return false;
    }
  }
  Serial.printf("[RAM/SLS] single MOT buffer=%u address=%p free=%u largest=%u\n",
                (unsigned)SLS_BUFFER_BYTES, slideshowSegBuf,
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
  SlideShowAvailable = false;
  SlideShowUpdate = false;
  SlideShowUpdate2 = false;
  slideshowRamSize = 0;
  slideshowPublishedPending = false;
  lastCompletedTransportId = 0;
  lastCompletedTransportIdValid = false;
  resetSlideshowCollector();
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

  // GPIO12 has an explicit boot-time role. INTB bypasses all detection and
  // attaches the proven FALLING IRQ path immediately; IR never lets the radio
  // driver own GPIO12. AUTO preserves the existing one-shot HW detection and
  // reuses its result across later FM/DAB switches in the same ESP32 run.
  setRadioIntbInterrupt(false);
  const uint8_t configuredGpio12Mode = sanitizeGpio12Mode(gpio12Mode);
  if (configuredGpio12Mode == GPIO12_INTB) {
    radioIntbCapability = RadioIntbCapability::Present;
    radioControlMode = RADIO_CTRL_INTB;
  } else if (configuredGpio12Mode == GPIO12_IR) {
    radioIntbCapability = RadioIntbCapability::Absent;
    radioControlMode = RADIO_CTRL_POLL;
  } else if (radioIntbCapability == RadioIntbCapability::Unknown) {
    radioControlMode = RADIO_CTRL_DETECT;
  } else if (radioIntbCapability == RadioIntbCapability::Present) {
    radioControlMode = RADIO_CTRL_INTB;
  } else {
    radioControlMode = RADIO_CTRL_POLL;
  }
  __atomic_store_n(&radioIntbPending, false, __ATOMIC_RELEASE);
  __atomic_store_n(&radioIntbEdgeCount, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&radioIntbFallingCount, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&radioIntbRisingCount, 0U, __ATOMIC_RELEASE);
  radioBootIrqEdgeCount = 0;
  radioPowerUpCtsViaIrq = false;
  radioAppTestCtsViaIrq = false;
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
  // In IR mode leave the pin completely to Arduino-IRremote.
  if (configuredGpio12Mode != GPIO12_IR) {
    pinMode(SI4684_INTB_PIN, INPUT_PULLUP);
    if (radioControlMode == RADIO_CTRL_DETECT)
      setRadioIntbInterrupt(true, CHANGE);
    else if (radioControlMode == RADIO_CTRL_INTB)
      setRadioIntbInterrupt(true, FALLING);
  }
  if (configuredGpio12Mode == GPIO12_INTB)
    Serial.println("[RADIO/IRQ] GPIO12 setting INTB; FALLING IRQ active before radio commands");
  else if (configuredGpio12Mode == GPIO12_IR)
    Serial.println("[RADIO/IRQ] GPIO12 setting IR; radio forced to polling");
  else if (radioControlMode == RADIO_CTRL_DETECT)
    Serial.println("[RADIO/IRQ] GPIO12 setting AUTO; detecting INTB");
  else
    Serial.printf("[RADIO/IRQ] AUTO reusing startup HW capability=%s\n",
                  radioIntbCapability == RadioIntbCapability::Present ? "INTB" : "POLL");

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
    if (configuredGpio12Mode == GPIO12_AUTO &&
        radioIntbCapability == RadioIntbCapability::Unknown) {
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

    int intbBeforePowerUp = digitalRead(SI4684_INTB_PIN);
    uint32_t powerUpStartChanges = radioIntbEdges();
    uint32_t powerUpStartFall = radioIntbFallingEdges();
    uint32_t powerUpStartRise = radioIntbRisingEdges();
    if (configuredGpio12Mode == GPIO12_AUTO &&
        radioIntbCapability == RadioIntbCapability::Unknown) {
      // The proof must belong to this POWER_UP, not to the pre-flight status
      // query or to a stale level left by an earlier device state.
      __atomic_store_n(&radioIntbPending, false, __ATOMIC_RELEASE);
      __atomic_store_n(&radioIntbEdgeCount, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&radioIntbFallingCount, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&radioIntbRisingCount, 0U, __ATOMIC_RELEASE);
      radioPowerUpCtsViaIrq = false;
      intbBeforePowerUp = digitalRead(SI4684_INTB_PIN);
      powerUpStartChanges = 0;
      powerUpStartFall = 0;
      powerUpStartRise = 0;
      Serial.printf("[RADIO/IRQTEST] before POWER_UP INTB=%d changes=0 fall=0 rise=0\n",
                    intbBeforePowerUp);
    }
    result = chip.powerUp(power, 1000000UL);
    if (configuredGpio12Mode == GPIO12_AUTO &&
        radioIntbCapability == RadioIntbCapability::Unknown) {
      const int intbAfterPowerUp = digitalRead(SI4684_INTB_PIN);
      const uint32_t powerUpChanges = radioIntbEdges() - powerUpStartChanges;
      const uint32_t powerUpFall = radioIntbFallingEdges() - powerUpStartFall;
      const uint32_t powerUpRise = radioIntbRisingEdges() - powerUpStartRise;
      Serial.printf("[RADIO/IRQTEST] after POWER_UP result=%d status0=0x%02X INTB=%d changes=%u fall=%u rise=%u viaIrq=%u\n",
                    static_cast<int>(result), static_cast<unsigned>(lastStatus0),
                    intbAfterPowerUp, static_cast<unsigned>(powerUpChanges),
                    static_cast<unsigned>(powerUpFall),
                    static_cast<unsigned>(powerUpRise),
                    radioPowerUpCtsViaIrq ? 1U : 0U);

      if (result != si468x::Result::Ok) {
        Serial.println("[RADIO/IRQ] POWER_UP failed; INTB detection inconclusive");
      } else if (powerUpChanges > 0U) {
        radioIntbCapability = RadioIntbCapability::Present;
        radioControlMode = RADIO_CTRL_INTB;
        chip.setCtsPollIntervalUs(RADIO_INTB_CTS_SAFETY_US);
        chip.setIdleStatusPollIntervalUs(50000UL);
        Serial.printf("[RADIO/IRQTEST] POWER_UP transition observed before=%d after=%d changes=%u fall=%u rise=%u\n",
                      intbBeforePowerUp, intbAfterPowerUp,
                      static_cast<unsigned>(powerUpChanges),
                      static_cast<unsigned>(powerUpFall),
                      static_cast<unsigned>(powerUpRise));
        Serial.printf("[RADIO/IRQ] POWER_UP CTS via %s\n",
                      radioPowerUpCtsViaIrq ? "INTB" : "safety poll");
        Serial.println("[RADIO/IRQ] INTB connected; IRQ mode");
      } else {
        // Do NOT classify Absent here. Keep DETECT active into LOAD_INIT /
        // HOST_LOAD / BOOT. The first later transition promotes immediately to
        // INTB mode inside hostIdle(); only if the whole boot produces no
        // transition do we need the explicit application probe after BOOT.
        Serial.println("[RADIO/IRQTEST] POWER_UP produced no GPIO12 transition");
        Serial.println("[RADIO/IRQTEST] continuing DETECT into LOAD_INIT/HOST_LOAD/BOOT; first toggle will activate IRQ immediately");
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

  // If POWER_UP itself was silent, DETECT stayed armed through the complete
  // bootloader transfer. hostIdle() promotes to INTB on the first transition,
  // so by this point capability is already Present whenever LOAD_INIT,
  // HOST_LOAD or BOOT generated a usable INTB event. Only a completely silent
  // boot reaches the deterministic application-firmware fallback probe below.
  radioBootIrqEdgeCount = radioIntbEdges();
  Serial.printf("[RADIO/IRQ] boot INTB edges=%u\n",
                static_cast<unsigned>(radioBootIrqEdgeCount));

  const bool applicationProbeNeeded =
      configuredGpio12Mode == GPIO12_AUTO &&
      radioIntbCapability == RadioIntbCapability::Unknown;
  const bool runtimeIntbRequested =
      configuredGpio12Mode != GPIO12_IR &&
      radioIntbCapability != RadioIntbCapability::Absent;
  // In the IR hardware variant INTB is not physically connected to GPIO12, so
  // do not unnecessarily disable the Si4684 INTB output itself. We simply do
  // not route/service it in the ESP32 and keep the radio in polling mode.
  const bool intbOutputEnabled =
      configuredGpio12Mode == GPIO12_IR || runtimeIntbRequested;
  const uint16_t pinConfig = RADIO_PIN_CONFIG_AUDIO |
      (intbOutputEnabled ? RADIO_PIN_CONFIG_INTBOUTEN : 0U);

  // This block is reached in DETECT only if POWER_UP *and the entire
  // LOAD_INIT/HOST_LOAD/BOOT sequence* produced no transition. It is the final
  // fallback probe. The first application-side transition is again promoted
  // immediately by hostIdle(); old hardware with GPIO12 NC still cannot stall
  // because the 1 ms safety poll remains enabled.
  uint32_t appStartChanges = 0;
  uint32_t appStartFall = 0;
  uint32_t appStartRise = 0;
  int appIntbBefore = digitalRead(SI4684_INTB_PIN);
  if (applicationProbeNeeded) {
    radioControlMode = RADIO_CTRL_DETECT;
    setRadioIntbInterrupt(true, CHANGE);
    takeRadioIntb();
    appStartChanges = radioIntbEdges();
    appStartFall = radioIntbFallingEdges();
    appStartRise = radioIntbRisingEdges();
    appIntbBefore = digitalRead(SI4684_INTB_PIN);
    Serial.printf("[RADIO/IRQTEST] before APP bootstrap INTB=%d changes=%u fall=%u rise=%u\n",
                  appIntbBefore,
                  static_cast<unsigned>(appStartChanges),
                  static_cast<unsigned>(appStartFall),
                  static_cast<unsigned>(appStartRise));
  } else if (runtimeIntbRequested) {
    radioControlMode = RADIO_CTRL_POLL;
  }

  // PIN_CONFIG_ENABLE and INT_CTL_ENABLE are always protected by bounded
  // polling. In DETECT mode an actual IRQ can still win and complete the same
  // command through hostIdle()/notifyInterrupt().
  chip.setCtsPollIntervalUs(1000);
  chip.setIdleStatusPollIntervalUs(20000);

  si468x::Result pinConfigResult = chip.setProperty(
      si468x::Property::PIN_CONFIG_ENABLE, pinConfig);
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
      pendingStatusResult = chip.readStatus(pendingStatus);
      Serial.printf("[RADIO/IRQ] runtime pending status=0x%02X readResult=%d\n",
                    static_cast<unsigned>(pendingStatus.status0),
                    static_cast<int>(pendingStatusResult));
    }
  }

  if (pinConfigResult != si468x::Result::Ok) {
    Serial.println("[RADIO] ERROR: PIN_CONFIG_ENABLE failed");
    return false;
  }

  si468x::PartInfo part;
  si468x::SystemState state;
  bool partInfoAlreadyRead = false;

  if (applicationProbeNeeded) {
    if (runtimeCtsResult != si468x::Result::Ok ||
        pendingStatusResult != si468x::Result::Ok) {
      Serial.println("[RADIO/IRQTEST] application INTB bootstrap failed; detection inconclusive");
      return false;
    }

    // The bootstrap itself is part of the physical test: enabling INTBOUTEN or
    // CTSIEN may already cause a transition, and that is valid proof that the
    // Si4684 output reaches GPIO12. Clear only the pending host latch here; keep
    // the counters so all application-side transitions remain in the delta.
    takeRadioIntb();
    radioAppTestCtsViaIrq = false;
    const int appBeforeGetPart = digitalRead(SI4684_INTB_PIN);
    Serial.printf("[RADIO/IRQTEST] before APP GET_PART_INFO INTB=%d appChanges=%u fall=%u rise=%u\n",
                  appBeforeGetPart,
                  static_cast<unsigned>(radioIntbEdges() - appStartChanges),
                  static_cast<unsigned>(radioIntbFallingEdges() - appStartFall),
                  static_cast<unsigned>(radioIntbRisingEdges() - appStartRise));

    const si468x::Result appProbeResult = chip.getPartInfo(part);
    partInfoAlreadyRead = appProbeResult == si468x::Result::Ok;
    const int appIntbAfter = digitalRead(SI4684_INTB_PIN);
    const uint32_t appChanges = radioIntbEdges() - appStartChanges;
    const uint32_t appFall = radioIntbFallingEdges() - appStartFall;
    const uint32_t appRise = radioIntbRisingEdges() - appStartRise;

    Serial.printf("[RADIO/IRQTEST] after APP GET_PART_INFO result=%d status0=0x%02X INTB=%d changes=%u fall=%u rise=%u viaIrq=%u part=%u\n",
                  static_cast<int>(appProbeResult),
                  static_cast<unsigned>(lastStatus0), appIntbAfter,
                  static_cast<unsigned>(appChanges),
                  static_cast<unsigned>(appFall),
                  static_cast<unsigned>(appRise),
                  radioAppTestCtsViaIrq ? 1U : 0U,
                  partInfoAlreadyRead ? static_cast<unsigned>(part.partNumber) : 0U);

    if (appProbeResult != si468x::Result::Ok) {
      Serial.println("[RADIO/IRQTEST] application probe command failed; INTB detection inconclusive");
      return false;
    }

    if (appChanges > 0U) {
      radioIntbCapability = RadioIntbCapability::Present;
      radioControlMode = RADIO_CTRL_INTB;
      chip.setCtsPollIntervalUs(RADIO_INTB_CTS_SAFETY_US);
      chip.setIdleStatusPollIntervalUs(50000UL);
      Serial.printf("[RADIO/IRQTEST] application INTB transition observed before=%d after=%d changes=%u fall=%u rise=%u\n",
                    appIntbBefore, appIntbAfter,
                    static_cast<unsigned>(appChanges),
                    static_cast<unsigned>(appFall),
                    static_cast<unsigned>(appRise));
      Serial.printf("[RADIO/IRQTEST] APP GET_PART_INFO CTS serviced via %s\n",
                    radioAppTestCtsViaIrq ? "INTB" : "safety polling");
      Serial.println("[RADIO/IRQ] INTB connected; IRQ mode confirmed by application firmware");
    } else {
      radioIntbCapability = RadioIntbCapability::Absent;
      Serial.printf("[RADIO/IRQTEST] application firmware produced no GPIO12 transition INTB=%d->%d\n",
                    appIntbBefore, appIntbAfter);
      usePollingFallback("no application INTB transition");
      pinConfigResult = chip.setProperty(
          si468x::Property::PIN_CONFIG_ENABLE, RADIO_PIN_CONFIG_AUDIO);
      Serial.printf("[RADIO/IRQ] runtime PIN_CONFIG=0x%04X after failed probe result=%d\n",
                    static_cast<unsigned>(RADIO_PIN_CONFIG_AUDIO),
                    static_cast<int>(pinConfigResult));
      if (pinConfigResult != si468x::Result::Ok) return false;
    }
  } else if (runtimeIntbRequested) {
    radioControlMode = RADIO_CTRL_INTB;
    if (runtimeCtsResult == si468x::Result::Ok &&
        pendingStatusResult == si468x::Result::Ok) {
      takeRadioIntb();
      chip.setCtsPollIntervalUs(RADIO_INTB_CTS_SAFETY_US);
      chip.setIdleStatusPollIntervalUs(50000UL);
      Serial.println("[RADIO/IRQ] runtime INTB armed");
    } else {
      useRuntimePollingFallback("runtime INTB bootstrap failed");
    }
  }

  // Start runtime edge telemetry after the one-shot application probe so its
  // diagnostic transitions are not counted as ordinary FM/DAB runtime IRQs.
  // Discard any diagnostic release-edge still latched in the host-side flag.
  takeRadioIntb();
  radioBootIrqEdgeCount = radioIntbEdges();

  if (!partInfoAlreadyRead) {
    Serial.println("[RADIO] GET_PART_INFO");
    result = chip.getPartInfo(part);
    Serial.printf("[RADIO] GET_PART_INFO result=%d part=%u status0=0x%02X\n",
                  static_cast<int>(result),
                  result == si468x::Result::Ok ? static_cast<unsigned>(part.partNumber) : 0U,
                  static_cast<unsigned>(lastStatus0));
  } else {
    result = si468x::Result::Ok;
    Serial.printf("[RADIO] GET_PART_INFO reused application probe part=%u status0=0x%02X\n",
                  static_cast<unsigned>(part.partNumber),
                  static_cast<unsigned>(lastStatus0));
  }
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
  memset(dabServiceCharsetValue, 0, sizeof(dabServiceCharsetValue));
  uint32_t parsedServiceId[32] = {0};
  uint8_t parsedServiceCharset[32] = {0};
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
    const uint8_t serviceCharset = SPIbuffer[offset + 6] & 0x0FU;
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
    // Service Info 3 low nibble is SlCharset for this specific service.
    // Remember the pre-sort ServiceID so the charset can be realigned after
    // qsort() without growing the DABService structure or static DRAM.
    parsedServiceId[i] = serviceID;
    parsedServiceCharset[i] = serviceCharset;
  }

  if (numberofservices == 0) return;
  qsort(service, numberofservices, sizeof(DABService), compareCompID);
  for (uint8_t sorted = 0; sorted < numberofservices; ++sorted) {
    for (uint8_t original = 0; original < numberofservices; ++original) {
      if (service[sorted].ServiceID == parsedServiceId[original]) {
        dabServiceCharsetValue[sorted] = parsedServiceCharset[original];
        break;
      }
    }
  }
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

        // Read DAB Dynamic Label (DLS). Si468x exposes the two DLS
        // prefix bytes at PAYLOAD[0..1] and the complete character field from
        // PAYLOAD+2. BYTE_COUNT covers the complete DSRV payload, hence only
        // BYTE_COUNT-2 bytes belong to the displayed text. This follows the
        // Si468x DLS handling used by independent working drivers and avoids
        // the previous regression that tried to reassemble X-PAD segments here.
        if (((SPIbuffer[8] >> 6) & 0x03) == 0x02 && byte_count >= 2U) {
          const uint8_t dlsPrefix0 = SPIbuffer[25];

          if ((dlsPrefix0 & 0x10U) == 0U) {
            // For a normal Dynamic Label, the high nibble of prefix byte 1 is
            // the character-set identifier. Some Si468x software reports 0x04
            // for UCS-2; normalize it to the registered 0x06 decoder path.
            uint8_t charset = static_cast<uint8_t>((SPIbuffer[26] >> 4) & 0x0FU);
            if (charset == 0x04U) charset = 0x06U;
            dabDynamicLabelCharset = charset;

            size_t copyLength = static_cast<size_t>(byte_count - 2U);
            if (copyLength > sizeof(ServiceData)) copyLength = sizeof(ServiceData);
            memcpy(ServiceData, SPIbuffer + 27, copyLength);

            // Keep an explicit byte length for UCS-2/UTF-16BE but discard only
            // trailing NUL padding. The old working parser copied BYTE_COUNT
            // bytes starting at PAYLOAD+2, i.e. two bytes past the payload;
            // those foreign bytes could appear as random residual glyphs.
            while (copyLength > 0U && ServiceData[copyLength - 1U] == '\0')
              --copyLength;
            dabDynamicLabelLength = static_cast<uint8_t>(copyLength);
            if (copyLength < sizeof(ServiceData)) ServiceData[copyLength] = '\0';
          } else if ((dlsPrefix0 & 0x0FU) == 0x01U) {
            // DLS command 1 removes the currently displayed Dynamic Label.
            memset(ServiceData, 0, sizeof(ServiceData));
            dabDynamicLabelLength = 0;
          }

          // MOT header. The collector identity is TransportId; BodySize is
          // only a completeness/integrity constraint.
        } else if (((SPIbuffer[8] >> 6) & 0x03) == 0x01 &&
                   byte_count >= 13U && byte_count < 200U &&
                   SPIbuffer[27] == 0x80 && SPIbuffer[28] == 0x00 &&
                   SPIbuffer[29] == 0x12) {
          const uint16_t transportID =
              (static_cast<uint16_t>(SPIbuffer[30]) << 8) | SPIbuffer[31];
          const uint32_t newLength =
              (static_cast<uint32_t>(SPIbuffer[34]) << 20) |
              (static_cast<uint32_t>(SPIbuffer[35]) << 12) |
               (static_cast<uint32_t>(SPIbuffer[36]) << 4) |
               (static_cast<uint32_t>(SPIbuffer[37]) >> 4);

          if (newLength == 0U || newLength > SLS_BUFFER_BYTES) {
            if (SlideShowDebug)
              Serial.printf("[SLS] Reject header TID=%u length=%u\n",
                            transportID, newLength);
          } else if (lastCompletedTransportIdValid &&
                     transportID == lastCompletedTransportId) {
            if (SlideShowDebug)
              Serial.printf("[SLS] Ignore repeated completed header TID=%u\n",
                            transportID);
          } else if (slideshowPublishedPending || SlideShowUpdate) {
            // The only image-sized buffer is still owned by the UI. DSRV is
            // deliberately drained, but no new object may claim the buffer.
            if (SlideShowDebug)
              Serial.printf("[SLS] Hold published image; ignore header TID=%u\n",
                            transportID);
          } else {
            if (SlideShowTransportIDValid &&
                transportID != SlideShowTransportID) {
              if (SlideShowDebug)
                Serial.printf("[SLS] Partial TID=%u abandoned; new header TID=%u\n",
                              SlideShowTransportID, transportID);
              lockSlideshowTransport(transportID);
            } else if (!SlideShowTransportIDValid) {
              lockSlideshowTransport(transportID);
            }

            bool metadataChanged = SlideShowLength == 0U;
            if (SlideShowLength != 0U && SlideShowLength != newLength) {
              // Conflicting metadata for the same TID is corruption. Start the
              // same TID afresh using the newest complete header.
              if (SlideShowDebug)
                Serial.printf("[SLS] TID=%u BodySize changed %u -> %u; reset\n",
                              transportID, SlideShowLength, newLength);
              lockSlideshowTransport(transportID);
              metadataChanged = true;
            }

            SlideShowLength = newLength;
            SlideShowInit = true;
            if (metadataChanged || SlideShowLastActivity == 0U)
              SlideShowLastActivity = millis();
            if (SlideShowDebug)
              Serial.printf("[SLS] Header TID=%u length=%u bytes=%u\n",
                            transportID, SlideShowLength,
                            SlideShowByteCounter);

            if (SlideShowByteCounter == SlideShowLength &&
                allSegmentsReceived()) {
              if (SlideShowTotalSegments == 0U)
                SlideShowTotalSegments =
                    static_cast<uint16_t>(SlideShowHighestSegment) + 1U;
              assembleSlideshow();
            } else if (SlideShowByteCounter > SlideShowLength) {
              if (SlideShowDebug)
                Serial.println("[SLS] Buffered bytes exceed BodySize; reset");
              resetSlideshowCollector();
            }
          }

          // MOT body segment. SPIbuffer[27] bit 7 is the last-segment flag in
          // the DSRV data-group prefix already used by this parser.
        } else if (((SPIbuffer[8] >> 6) & 0x03) == 0x01 &&
                   byte_count > 11U &&
                   (SPIbuffer[27] == 0x00 || SPIbuffer[27] == 0x80) &&
                   SPIbuffer[29] == 0x12) {
          const uint16_t transportID =
              (static_cast<uint16_t>(SPIbuffer[30]) << 8) | SPIbuffer[31];
          const uint8_t segmentNumber = SPIbuffer[28];
          const bool lastSegment = (SPIbuffer[27] & 0x80U) != 0U;
          const uint16_t dataLen = static_cast<uint16_t>(byte_count - 11U);

          if (lastCompletedTransportIdValid &&
              transportID == lastCompletedTransportId) {
            if (SlideShowDebug && segmentNumber == 0U)
              Serial.printf("[SLS] Ignore repeated completed TID=%u\n",
                            transportID);
          } else if (slideshowPublishedPending || SlideShowUpdate) {
            // Preserve the complete image until acknowledgeSlideshow().
            if (SlideShowDebug && segmentNumber == 0U)
              Serial.printf("[SLS] Hold published image; ignore seg=0 TID=%u\n",
                            transportID);
          } else {
            if (SlideShowTransportIDValid &&
                transportID != SlideShowTransportID && segmentNumber == 0U) {
              if (SlideShowDebug)
                Serial.printf("[SLS] Partial TID=%u abandoned; new seg=0 TID=%u\n",
                              SlideShowTransportID, transportID);
              lockSlideshowTransport(transportID);
            } else if (!SlideShowTransportIDValid) {
              lockSlideshowTransport(transportID);
              if (SlideShowDebug)
                Serial.printf("[SLS] Collector locked to TID=%u\n", transportID);
            }

            if (transportID != SlideShowTransportID) {
              if (SlideShowDebug)
                Serial.printf("[SLS] Ignore seg=%u TID=%u (collecting TID=%u)\n",
                              segmentNumber, transportID,
                              SlideShowTransportID);
            } else {
              const uint8_t byteIndex = segmentNumber / 8U;
              const uint8_t bitIndex = segmentNumber % 8U;
              const uint8_t bitMask = static_cast<uint8_t>(1U << bitIndex);

              if ((SlideShowSegmentBitmap[byteIndex] & bitMask) == 0U) {
                const bool lastConflict =
                    lastSegment && SlideShowLastSegmentValid &&
                    segmentNumber != SlideShowLastSegment;
                const bool pastKnownEnd =
                    SlideShowLastSegmentValid &&
                    segmentNumber > SlideShowLastSegment;
                const bool lastBeforeHighest =
                    lastSegment && SlideShowByteCounter != 0U &&
                    segmentNumber < SlideShowHighestSegment;

                if (lastConflict || pastKnownEnd || lastBeforeHighest ||
                    dataLen > SLS_MAX_SEG_SIZE ||
                    SlideShowByteCounter > SLS_BUFFER_BYTES - dataLen) {
                  if (SlideShowDebug)
                    Serial.printf("[SLS] Invalid seg=%u len=%u last=%u; reset TID=%u\n",
                                  segmentNumber, dataLen,
                                  lastSegment ? 1U : 0U, transportID);
                  resetSlideshowCollector();
                } else {
                  // Invalidate the previous published view before the first
                  // write, not afterwards. slideshowPublishedPending above
                  // guarantees the UI has already finished using its bytes.
                  if (SlideShowByteCounter == 0U) {
                    beginSlideshowReception();
                    slideshowFirstSegmentMs = millis();
                  }

                  if (!storeSlideshowSegment(segmentNumber,
                                              SPIbuffer + 34, dataLen)) {
                    if (SlideShowDebug)
                      Serial.printf("[SLS] Seg=%u len=%u does not fit packed buffer\n",
                                    segmentNumber, dataLen);
                    resetSlideshowCollector();
                  } else {
                    SlideShowSegmentBitmap[byteIndex] |= bitMask;
                    SlideShowByteCounter += dataLen;
                    if (segmentNumber > SlideShowHighestSegment)
                      SlideShowHighestSegment = segmentNumber;

                    if (lastSegment) {
                      SlideShowLastSegmentValid = true;
                      SlideShowLastSegment = segmentNumber;
                      SlideShowTotalSegments =
                          static_cast<uint16_t>(segmentNumber) + 1U;
                    }

                    SlideShowInit = true;
                    SlideShowLastActivity = millis();
                    if (SlideShowDebug)
                      Serial.printf("[SLS] Saved TID=%u seg=%u len=%u bytes=%u/%u%s\n",
                                    transportID, segmentNumber, dataLen,
                                    SlideShowByteCounter, SlideShowLength,
                                    lastSegment ? " LAST" : "");

                    if (SlideShowLength != 0U &&
                        SlideShowByteCounter > SlideShowLength) {
                      if (SlideShowDebug)
                        Serial.println("[SLS] Segment data exceed BodySize; reset");
                      resetSlideshowCollector();
                    } else if (allSegmentsReceived() &&
                               ((SlideShowLength != 0U &&
                                 SlideShowByteCounter == SlideShowLength) ||
                                (SlideShowLength == 0U &&
                                 SlideShowLastSegmentValid))) {
                      if (SlideShowTotalSegments == 0U)
                        SlideShowTotalSegments =
                            static_cast<uint16_t>(SlideShowHighestSegment) + 1U;
                      assembleSlideshow();
                    }
                  }
                }
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

void DAB::acknowledgeSlideshow(void) {
  SlideShowUpdate = false;
  slideshowPublishedPending = false;
}

// Insert one new segment into the single buffer while keeping already received
// data packed in SegmentNumber order. Later arrival of a lower segment shifts
// only the collected tail and therefore needs no second image-sized buffer.
bool DAB::storeSlideshowSegment(uint8_t segmentNumber,
                                const uint8_t* data,
                                uint16_t dataLength) {
  if (!slideshowSegBuf || !data || dataLength == 0U ||
      dataLength > SLS_MAX_SEG_SIZE ||
      SlideShowByteCounter > SLS_BUFFER_BYTES - dataLength) return false;

  size_t insertOffset = 0U;
  for (uint16_t i = 0; i < segmentNumber; ++i) {
    insertOffset += slideshowSegLen[i];
  }
  if (insertOffset > SlideShowByteCounter) return false;

  const size_t tailLength = SlideShowByteCounter - insertOffset;
  memmove(slideshowSegBuf + insertOffset + dataLength,
          slideshowSegBuf + insertOffset, tailLength);
  memcpy(slideshowSegBuf + insertOffset, data, dataLength);
  slideshowSegLen[segmentNumber] = dataLength;
  return true;
}

// Forget every buffered slideshow segment. Clearing the metadata is enough;
// packed bytes in slideshowSegBuf are overwritten by the next received object.
void DAB::clearSegmentBuffer(void) {
  SlideshowReceptionState(false);
  memset(slideshowSegLen, 0, sizeof(slideshowSegLen));
  memset(SlideShowSegmentBitmap, 0, sizeof(SlideShowSegmentBitmap));
  SlideShowLastSegmentValid = false;
  SlideShowLastSegment = 0;
}

void DAB::resetSlideshowCollector(void) {
  clearSegmentBuffer();
  SlideShowTransportIDValid = false;
  SlideShowTransportID = 0;
  SlideShowByteCounter = 0;
  SlideShowHighestSegment = 0;
  SlideShowTotalSegments = 0;
  SlideShowLength = 0;
  SlideShowInit = false;
  SlideShowLastActivity = 0;
  slideshowFirstSegmentMs = 0;
}

void DAB::lockSlideshowTransport(uint16_t transportId) {
  resetSlideshowCollector();
  SlideShowTransportID = transportId;
  SlideShowTransportIDValid = true;
}

// Check if we have every segment from 0..N where N is either:
//   - SlideShowTotalSegments (set from an explicit last-segment flag), or
//   - SlideShowHighestSegment + 1 (used with an exact BodySize match).
bool DAB::allSegmentsReceived(void) {
  uint16_t segmentsToCheck = SlideShowTotalSegments;
  if (segmentsToCheck == 0U && SlideShowByteCounter != 0U)
    segmentsToCheck = static_cast<uint16_t>(SlideShowHighestSegment) + 1U;
  if (segmentsToCheck == 0) return false;

  for (uint16_t i = 0; i < segmentsToCheck; ++i) {
    const uint8_t byteIndex = static_cast<uint8_t>(i / 8U);
    const uint8_t bitIndex = static_cast<uint8_t>(i % 8U);
    if (!(SlideShowSegmentBitmap[byteIndex] & (1 << bitIndex))) {
      return false;
    }
  }
  return true;
}

// Validate the already packed segments and publish the contiguous RAM buffer.
void DAB::assembleSlideshow(void) {
  if (SlideShowDebug) {
    Serial.printf("[SLS] Assembling: %u segments, %u bytes received, %u bytes expected\n",
                  SlideShowTotalSegments, SlideShowByteCounter, SlideShowLength);
  }

  uint32_t actualSize = 0;
  for (uint16_t i = 0;
       i < SlideShowTotalSegments && i < SLS_MAX_SEGMENTS; ++i) {
    if (slideshowSegLen[i] > 0) {
      actualSize += slideshowSegLen[i];
    } else if (SlideShowDebug) {
      Serial.printf("[SLS] WARNING: segment %u missing!\n", i);
    }
  }

  const bool sizeValid =
      actualSize > 8 &&
      actualSize <= SLS_BUFFER_BYTES &&
      actualSize == SlideShowByteCounter &&
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
    slideshowRamSize = actualSize;
    SlideShowUpdate = true;
    SlideShowUpdate2 = true;
    SlideShowAvailable = true;
    slideshowPublishedPending = true;
    lastCompletedTransportId = SlideShowTransportID;
    lastCompletedTransportIdValid = SlideShowTransportIDValid;

    if (SlideShowDebug) {
      Serial.printf("[SLS] RAM image ready: %s, %u bytes\n",
                    validJPEG ? "JPEG" : "PNG", actualSize);
    }
    Serial.printf("[SLS/UI] READY: icon/button enabled, %u bytes\n", actualSize);
    SlideshowReceptionState(false);
    Serial.printf("[SLS/TIME] first-segment-to-ready=%u ms\n", slideshowFirstSegmentMs ? (millis() - slideshowFirstSegmentMs) : 0U);
    Serial.printf("[RAM/SLS] single MOT buffer=%u image=%u\n",
                  (unsigned)SLS_BUFFER_BYTES, actualSize);
  }

  // Reset only collector metadata. The compacted image bytes remain in this
  // single buffer and are published through slideshowRamSize on success.
  resetSlideshowCollector();
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
    dabServiceCharsetValue[x] = 0;
    for (byte y = 0; y < 16; y++) service[x].Label[y] = '\0';
  }
  dabDynamicLabelCharset = 0;
  dabDynamicLabelLength = 0;
  for (byte x = 0; x < 128; x++) ServiceData[x] = '\0';
  ServiceLabelCharset = 0;
  EnsembleLabelCharset = 0;
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
  samplerate = 0;
  servicetype = 9;
  audiomode = 4;
  dataServiceCheck = 0;
  ServiceStart = false;
  SlideShowAvailable = false;
  SlideShowUpdate = false;
  SlideShowUpdate2 = false;
  slideshowRamSize = 0;
  slideshowPublishedPending = false;
  lastCompletedTransportIdValid = false;
  resetSlideshowCollector();

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
  fmPtyValid = false;
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
  dabDynamicLabelCharset = 0;
  dabDynamicLabelLength = 0;
  ServiceLabelCharset = 0;
  EnsembleLabelCharset = 0;
  SlideShowAvailable = false;
  SlideShowUpdate = false;
  SlideShowUpdate2 = false;
  slideshowRamSize = 0;
  slideshowPublishedPending = false;
  lastCompletedTransportIdValid = false;
  resetSlideshowCollector();
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
    fmPtyValid = true;
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
        // Current EBU/RDS charset 0000 assigns printable letters to several
        // byte values below 0x20 (for example 0x1B = U+011A / E-caron).
        // Only the explicitly non-displayable control codes are invalid here.
        if (c == 0x00U || c == 0x0AU || c == 0x0BU || c == 0x0DU) {
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

    // RDS RadioText uses 0x0D as its end marker and accepts 0x0A as a
    // line-break control. Other low byte values can be real EBU letters, so
    // reject only NUL and the explicitly non-displayable 0x0B control here.
    for (uint8_t i = 0; i < charsPerSegment; ++i) {
      const uint8_t c = static_cast<uint8_t>(segmentData[i]);
      if (c == 0x00U || c == 0x0BU) return;
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
  samplerate = 0;
  servicetype = 9;
  audiomode = 4;
  protectionlevel = 0;
  for (byte x = 0; x < 128; x++) ServiceData[x] = '\0';
  memset(PStext, 0, sizeof(PStext));
  slideshowRamSize = 0;
  SlideShowAvailable = false;
  SlideShowUpdate = false;
  SlideShowUpdate2 = false;
  slideshowPublishedPending = false;
  lastCompletedTransportIdValid = false;
  ServiceStart = false;
  ServiceIndex = _index;
  ecc = 0;  // Reset so ServiceInfo() picks up the new service's ECC
  serviceHasOwnEcc = false;
  // Reset segment tracking (RAM-only).
  resetSlideshowCollector();

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
    // The buffer contains an incomplete object and therefore remains
    // unavailable. A picture already decoded to TFT GRAM is not erased here.
    resetSlideshowCollector();
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

  // DAB registered charsets used by labels and Dynamic Label are EBU Latin
  // (0000), UTF-16BE (0110) and UTF-8 (1111). Fixed DAB labels are 16 bytes,
  // so UTF-16BE must be decoded by length rather than as a C string.
  if (charset == 0x0F) return String(input);
  if (charset == 0x06) {
    wchar_t temp[9];
    size_t out = 0;
    for (size_t i = 0; i + 1 < 16 && out < 8; i += 2) {
      const uint16_t code =
          (static_cast<uint16_t>(static_cast<uint8_t>(input[i])) << 8) |
          static_cast<uint8_t>(input[i + 1]);
      if (code == 0) break;
      temp[out++] = static_cast<wchar_t>(code);
    }
    temp[out] = L'\0';
    return convertToUTF8(temp);
  }
  if (charset != 0x00) return String(input);

  wchar_t temp[128];
  charConverter(input, temp, sizeof(temp) / sizeof(wchar_t));
  return convertToUTF8(temp);
}


// qsort() comparator: order services by component ID low byte ascending.
static int compareCompID(const void* a, const void* b) {
  uint32_t compID_a = (*((DABService*)a)).CompID & 0xFF;
  uint32_t compID_b = (*((DABService*)b)).CompID & 0xFF;

  if (compID_a < compID_b) return -1;
  if (compID_a > compID_b) return 1;
  return 0;
}

// ETSI TS 101 756 V1.7.1, charset 0000 (Complete EBU Latin repertoire).
// This is also the Basic RDS character set. Numeric Unicode code points are
// used deliberately so source-file encoding cannot alter the mapping.
static const uint16_t EBU_LATIN_TO_UNICODE[256] = {
  0x0000, 0x0118, 0x012E, 0x0172, 0x0102, 0x0116, 0x010E, 0x0218, 0x021A, 0x010A, 0x0000, 0x0000, 0x0120, 0x0000, 0x017B, 0x0143, // 0x00..0x0F
  0x0105, 0x0119, 0x012F, 0x0173, 0x0103, 0x0117, 0x010F, 0x0219, 0x021B, 0x010B, 0x0147, 0x011A, 0x0121, 0x0139, 0x017C, 0x002D, // 0x10..0x1F
  0x0020, 0x0021, 0x0022, 0x0023, 0x013A, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F, // 0x20..0x2F
  0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, // 0x30..0x3F
  0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F, // 0x40..0x4F
  0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B, 0x016E, 0x005D, 0x0141, 0x0142, // 0x50..0x5F
  0x0104, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, // 0x60..0x6F
  0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x00AB, 0x016F, 0x00BB, 0x013D, 0x0126, // 0x70..0x7F
  0x00E1, 0x00E0, 0x00E9, 0x00E8, 0x00ED, 0x00EC, 0x00F3, 0x00F2, 0x00FA, 0x00F9, 0x00D1, 0x00C7, 0x015E, 0x00DF, 0x00A1, 0x0178, // 0x80..0x8F
  0x00E2, 0x00E4, 0x00EA, 0x00EB, 0x00EE, 0x00EF, 0x00F4, 0x00F6, 0x00FB, 0x00FC, 0x00F1, 0x00E7, 0x015F, 0x011F, 0x0131, 0x00FF, // 0x90..0x9F
  0x0136, 0x0145, 0x00A9, 0x0122, 0x011E, 0x011B, 0x0148, 0x0151, 0x0150, 0x20AC, 0x00A3, 0x0024, 0x0100, 0x0112, 0x012A, 0x016A, // 0xA0..0xAF
  0x0137, 0x0146, 0x013B, 0x0123, 0x013C, 0x0130, 0x0144, 0x0171, 0x0170, 0x00BF, 0x013E, 0x00B0, 0x0101, 0x0113, 0x012B, 0x016B, // 0xB0..0xBF
  0x00C1, 0x00C0, 0x00C9, 0x00C8, 0x00CD, 0x00CC, 0x00D3, 0x00D2, 0x00DA, 0x00D9, 0x0158, 0x010C, 0x0160, 0x017D, 0x00D0, 0x013F, // 0xC0..0xCF
  0x00C2, 0x00C4, 0x00CA, 0x00CB, 0x00CE, 0x00CF, 0x00D4, 0x00D6, 0x00DB, 0x00DC, 0x0159, 0x010D, 0x0161, 0x017E, 0x0111, 0x0140, // 0xD0..0xDF
  0x00C3, 0x00C5, 0x00C6, 0x0152, 0x0177, 0x00DD, 0x00D5, 0x00D8, 0x00DE, 0x014A, 0x0154, 0x0106, 0x015A, 0x0179, 0x0164, 0x00F0, // 0xE0..0xEF
  0x00E3, 0x00E5, 0x00E6, 0x0153, 0x0175, 0x00FD, 0x00F5, 0x00F8, 0x00FE, 0x014B, 0x0155, 0x0107, 0x015B, 0x017A, 0x0165, 0x0127, // 0xF0..0xFF
};

// Translate one charset-0000 EBU/RDS byte string into Unicode code points.
// Output is a wchar_t buffer that convertToUTF8() later serialises as UTF-8.
static void charConverter(const char* input, wchar_t* output, size_t outSize) {
  if (!input || !output || outSize == 0) return;

  size_t outIndex = 0;
  for (size_t inIndex = 0;
       input[inIndex] != '\0' && outIndex < outSize - 1;
       ++inIndex) {
    const uint8_t code = static_cast<uint8_t>(input[inIndex]);
    const uint16_t unicode = EBU_LATIN_TO_UNICODE[code];

    // 0x0A/0x0B/0x0D are explicitly non-displayable in the EBU table.
    if (unicode != 0)
      output[outIndex++] = static_cast<wchar_t>(unicode);
  }
  output[outIndex] = L'\0';
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

// Decode the currently assembled DAB Dynamic Label using its own charset and
// explicit byte length. The explicit length is essential for UTF-16BE because
// ordinary ASCII-range UTF-16 characters contain zero bytes.
String DabDynamicLabelText(const char* input) {
  if (!input || dabDynamicLabelLength == 0) return String();

  if (dabDynamicLabelCharset == 0x06) {
    wchar_t temp[65];
    size_t out = 0;
    for (size_t i = 0;
         i + 1 < dabDynamicLabelLength && out < (sizeof(temp) / sizeof(temp[0])) - 1;
         i += 2) {
      const uint16_t code =
          (static_cast<uint16_t>(static_cast<uint8_t>(input[i])) << 8) |
          static_cast<uint8_t>(input[i + 1]);
      if (code == 0) continue;
      // DLS control codes: flatten preferred line/headline breaks for the
      // one-line TFT strip; retain the standard preferred word break as '-'.
      if (code == 0x000AU || code == 0x000BU)
        temp[out++] = L' ';
      else if (code == 0x001FU)
        temp[out++] = L'-';
      else
        temp[out++] = static_cast<wchar_t>(code);
    }
    temp[out] = L'\0';
    return convertToUTF8(temp);
  }

  char temp[129];
  const size_t len = min(static_cast<size_t>(dabDynamicLabelLength),
                         sizeof(temp) - 1U);
  memcpy(temp, input, len);
  temp[len] = '\0';

  if (dabDynamicLabelCharset == 0x0F) {
    String output(temp);
    output.replace("\n", " ");
    String headlineBreak; headlineBreak += static_cast<char>(0x0B);
    String wordBreak; wordBreak += static_cast<char>(0x1F);
    output.replace(headlineBreak, " ");
    output.replace(wordBreak, "-");
    return output;
  }
  if (dabDynamicLabelCharset == 0x00) {
    // DLS defines these three bytes as layout hints independently of the
    // selected charset. Flatten them for our one-line ticker before EBU decode.
    for (size_t i = 0; i < len; ++i) {
      const uint8_t code = static_cast<uint8_t>(temp[i]);
      if (code == 0x0AU || code == 0x0BU) temp[i] = ' ';
      else if (code == 0x1FU) temp[i] = '-';
    }
    wchar_t wide[129];
    charConverter(temp, wide, sizeof(wide) / sizeof(wide[0]));
    return convertToUTF8(wide);
  }
  return String(temp);
}
