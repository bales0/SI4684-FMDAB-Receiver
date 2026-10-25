"""Compile actual radio configuration against a fault-injecting transport.

Usage: python tests/test_i2s_integration.py [path-to-g++]
No ESP32 hardware is accessed. Generated files live under ignored .pio/.
"""
from pathlib import Path
import subprocess
import sys
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/si4684.cpp").read_text(encoding="utf-8")
start = source.index("static I2sAudioMode radioI2sAudioMode")
end = source.index("// Keep INTB", start)
actual = source[start:end]
# Compile the enum from its GUI owner without importing TFT/Arduino on the host.
gui_header = (ROOT / "src/gui.h").read_text(encoding="utf-8")
menu_enum = re.search(r"enum SettingsMenuItem : uint8_t \{.*?\};", gui_header, re.S).group(0)
menu_assertions = """
static_assert(MENU_LANGUAGE == 0 && MENU_BRIGHTNESS == 1 && MENU_THEME == 2 &&
              MENU_AUTO_SLIDESHOW == 3 && MENU_SIGNAL_UNIT == 4 &&
              MENU_SLEEP_TIMER == 5 && MENU_RADIO_MODE == 6 && MENU_FM_REGION == 7 &&
              MENU_GPIO12 == 8 && MENU_I2S_AUDIO == 9 && MENU_IR_REMOTE == 10 &&
              MENU_FM_SEEK == 11 && MENU_FM_AF == 12 && MENU_ABOUT == 13 &&
              MENU_ITEM_COUNT == 14, "existing menu order");
"""
prefix = r'''
#include "i2s_audio.h"
#include <cassert>
#include <vector>
#include <utility>
#include <string>
#include <cstdio>
#include <cstdarg>
static std::string logText;
static void logLine(const char* format, ...) {
  char buffer[512];
  va_list args; va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args); logText += buffer;
}
#define DIAG_PRINTF(...) logLine(__VA_ARGS__)
#define DIAG_PRINTLN(text) logLine("%s\n", text)
namespace si468x {
enum class Property : uint16_t {
  DIGITAL_IO_OUTPUT_SELECT = 0x0200,
  DIGITAL_IO_OUTPUT_SAMPLE_RATE = 0x0201,
  DIGITAL_IO_OUTPUT_FORMAT = 0x0202
};
enum class Result { Ok, Error };
}
struct Chip {
  std::vector<std::pair<uint16_t, uint16_t> > writes;
  std::vector<uint16_t> reads;
  uint16_t values[3] = {};
  int failAt = -1, readFailAt = -1, mismatchAt = -1;
  si468x::Result setProperty(si468x::Property p, uint16_t value) {
    const uint16_t id = static_cast<uint16_t>(p);
    writes.push_back({id, value});
    if (static_cast<int>(writes.size()) == failAt) return si468x::Result::Error;
    values[id - 0x0200] = value;
    return si468x::Result::Ok;
  }
  si468x::Result getProperty(si468x::Property p, uint16_t& value, uint32_t timeout) {
    assert(timeout == 250000UL);
    const uint16_t id = static_cast<uint16_t>(p);
    reads.push_back(id);
    value = values[id - 0x0200];
    if (static_cast<int>(reads.size()) == mismatchAt) value ^= 0x10;
    // Deliberately leave the expected value even on failure: Result is mandatory.
    return static_cast<int>(reads.size()) == readFailAt ? si468x::Result::Error : si468x::Result::Ok;
  }
} chip;
class DAB { public: void setI2sAudioMode(I2sAudioMode); };
'''
suffix = r'''
int main() {
  DAB radio;
  const uint16_t ids[] = {0x0200, 0x0202, 0x0201};
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    for (unsigned value = 0; value < I2S_AUDIO_MODE_COUNT; ++value) {
      const I2sAudioMode mode = sanitizeI2sAudioMode(value);
      radio.setI2sAudioMode(mode);
      const uint16_t expected[] = {0x8000, 0x1870, i2sAudioSampleRate(mode)};
      for (bool reuse : {false, true}) {
        for (unsigned fault = 0; fault < 10; ++fault) {
          chip = Chip{}; logText.clear();
          chip.failAt = fault >= 1 && fault <= 3 ? fault : -1;
          chip.readFailAt = fault >= 4 && fault <= 6 ? fault - 3 : -1;
          chip.mismatchAt = fault >= 7 ? fault - 6 : -1;
          if (reuse) { // An already tuned image: only verify, no writes allowed.
            chip.values[0] = 0x8000;
            chip.values[1] = i2sAudioSampleRate(mode);
            chip.values[2] = 0x1870;
          }
          radioI2sOutputReady = true; // stale success must not survive any failure
          configureI2sAudio(reuse);
          const bool writeFailed = !reuse && chip.failAt != -1;
          const bool enabled = mode != I2S_AUDIO_OFF && !writeFailed &&
                               chip.readFailAt == -1 && chip.mismatchAt == -1;
          assert(radioI2sOutputReady == enabled);
          if (mode == I2S_AUDIO_OFF) {
            assert(chip.writes.empty() && chip.reads.empty());
          } else {
            const unsigned written = reuse ? 0U : writeFailed ? chip.failAt : 3U;
            assert(chip.writes.size() == written);
            for (unsigned i = 0; i < written; ++i) {
              assert(chip.writes[i].first == ids[i]);
              assert(chip.writes[i].second == expected[i]);
            }
            const unsigned read = writeFailed ? 0U : chip.readFailAt != -1 ?
                chip.readFailAt : chip.mismatchAt != -1 ? chip.mismatchAt : 3U;
            assert(chip.reads.size() == read);
            for (unsigned i = 0; i < read; ++i) assert(chip.reads[i] == ids[i]);
            assert(logText.find(enabled ? "digital output ready" : "analog DAC retained") != std::string::npos);
            if (read >= 2) assert(logText.find("expected=0x1870") != std::string::npos);
          }
          assert(audioPinConfig(false) == (enabled ? 0x0003 : 0x0001));
          assert(audioPinConfig(true) == (enabled ? 0x8003 : 0x8001));
          assert(radioI2sAudioMode == mode);
        }
      }
    }
  }
  // ESP32-only restart over an older receiver configuration: do not repair
  // a potentially tuned chip in place, and do not claim readiness.
  radio.setI2sAudioMode(I2S_AUDIO_44K1);
  for (uint16_t oldFormat : {uint16_t(0x1600), uint16_t(0x1800), uint16_t(0x1870)}) {
    chip = Chip{};
    chip.values[0] = 0x8000; chip.values[1] = 48000; chip.values[2] = oldFormat;
    configureI2sAudio(true);
    assert(chip.writes.empty() && !radioI2sOutputReady);
    assert(audioPinConfig(true) == 0x8001);
  }
  radio.setI2sAudioMode(static_cast<I2sAudioMode>(255));
  assert(radioI2sAudioMode == I2S_AUDIO_48K);
}
'''
output = ROOT / ".pio/tests_i2s"
output.mkdir(parents=True, exist_ok=True)
cpp = output / "radio_configuration.cpp"
cpp.write_text(prefix + menu_enum + menu_assertions + actual + suffix, encoding="utf-8")
exe = output / "radio_configuration.exe"
compiler = sys.argv[1] if len(sys.argv) > 1 else "g++"
subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(ROOT / "src"), str(cpp), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)

# Wiring checks complement the behavior test; the full target build checks types.
boot = source[source.index("bool DAB::begin("):source.index('DIAG_PRINTLN("[RADIO] shared properties done")')]
assert boot.count("configureI2sAudio(reuseRunningImage);") == 1
assert boot.index("configureI2sAudio(reuseRunningImage);") < boot.index("si468x::Property::PIN_CONFIG_ENABLE")
assert "Set_Property(0x0202" not in source
assert source.count("si468x::Property::DIGITAL_IO_OUTPUT_FORMAT") == 1
assert 'Set_Property(0x1710, 0xFC4A);' in boot
assert 'Set_Property(0x1711, 0x00F8);' in boot
assert "audioPinConfig(intbOutputEnabled)" in boot
assert "audioPinConfig(false)" in boot  # failed AUTO probe
fallback = source[source.index("static void useRuntimePollingFallback"):]
assert "audioPinConfig(false)" in fallback
gui = (ROOT / "src/gui.cpp").read_text(encoding="utf-8")
assert gui.count("case MENU_I2S_AUDIO:") == 4
assert gui.count("menuitem == MENU_IR_REMOTE") == 4
assert gui.count("menuitem == MENU_ABOUT) return;") == 2
for name in ("MenuUp", "MenuDown"):
    start = gui.index("void " + name + "(void)")
    end = gui.index("\nvoid ", start + 1)
    body = gui[start:end]
    assert "cycleI2sAudioMode(requestedI2sAudioMode," in body
    assert "setI2sAudioMode" not in body and "SwitchRadioMode" not in body
ino = (ROOT / "SI4684-DAB-Receiver.ino").read_text(encoding="utf-8")
assert "Storage.setI2sAudioMode(I2S_AUDIO_48K);" in ino
assert "SwitchRadioMode(requestedRadioMode, i2sChanged)" in ino
assert "trysetserviceComponentValid = restoreLiveDab;" in ino
assert "else Storage.get(EE_UINT32_SERVICEID, _serviceID);" in ino
exit_body = ino[ino.index("void ExitSettingsMenu(void) {"):ino.index("void CycleTuneMode(void) {")]
assert exit_body.index("i2sChanged && !FlushEeprom()") < exit_body.index("radio.setI2sAudioMode")
assert exit_body.index("radio.setI2sAudioMode") < exit_body.index("SwitchRadioMode")
print("I2S production configuration: all rates, OFF, cold/reuse init, all write/read failures and mismatches: PASS")
print("I2S boot/fallback/menu/apply/factory-default wiring: PASS")

# The existing binary fonts must contain the superscript and every rate glyph.
fonts = (ROOT / "src/font.h").read_text(encoding="utf-8")
for name in ("FONT16", "FONT28"):
    array = fonts.split("const uint8_t " + name + "[] PROGMEM = {", 1)[1].split("};", 1)[0]
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", array))
    count = struct.unpack_from(">I", data)[0]
    glyphs = {struct.unpack_from(">7i", data, 24 + i * 28)[0]:
              struct.unpack_from(">7i", data, 24 + i * 28) for i in range(count)}
    for char in "I\u00b2S AudioOFF324.18kHz":
        assert char == " " or ord(char) in glyphs, (name, char)
    for label in ("OFF", "32 kHz", "44.1 kHz", "48 kHz"):
        width = sum(glyphs[ord(char)][3] if char != " " else 10 for char in label)
        assert width < 270, (name, label, width)
print("I2S existing font glyphs and popup value widths: PASS")
