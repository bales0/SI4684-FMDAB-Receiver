#include "../src/i2s_audio.h"
#include "../src/nvs_storage.h"
#include <cassert>
#include <iostream>

namespace nvs_mock {
std::map<std::string, std::vector<uint8_t> > records;
std::vector<std::string> writes;
bool failWrites = false;
}

int main() {
  static_assert(I2S_AUDIO_FORMAT_STANDARD == 0x1870U, "24-bit I2S / 32-bit slots");
  static_assert(sizeof(I2sAudioMode) == 1, "compact mode");
  static_assert(I2S_AUDIO_OFF == 0 && I2S_AUDIO_32K == 1 &&
                I2S_AUDIO_44K1 == 2 && I2S_AUDIO_48K == 3 &&
                I2S_AUDIO_MODE_COUNT == 4, "four contiguous modes without migration");
  const uint16_t rates[] = {0, 32000, 44100, 48000};
  const char* labels[] = {"OFF", "32 kHz", "44.1 kHz", "48 kHz"};
  for (unsigned i = 0; i < 256; ++i) {
    const I2sAudioMode mode = sanitizeI2sAudioMode(i);
    assert(mode == (i < 4 ? i : 3));
    assert(i2sAudioSampleRate(mode) == rates[mode]);
    assert(std::strcmp(i2sAudioModeText(mode), labels[mode]) == 0);
    for (bool intb : {false, true}) {
      const uint16_t pins = i2sAudioPinConfig(mode, intb);
      assert((pins & 1) == 1);
      assert((pins & 2) == (mode == I2S_AUDIO_OFF ? 0 : 2));
      assert((pins & 0x8000) == (intb ? 0x8000 : 0));
      assert((pins & ~0x8003) == 0);
    }
    assert(cycleI2sAudioMode(cycleI2sAudioMode(mode, true), false) == mode);
    assert(cycleI2sAudioMode(mode, true) == (mode + 1) % 4);
    assert(cycleI2sAudioMode(mode, false) == (mode + 3) % 4);
  }
  using namespace nvs_mock;
  // Seed a complete legacy installation, including unrelated scan records.
  records["settings"] = std::vector<uint8_t>(EE_PRESETS_FREQ_START - 1, 0x42);
  records["dab_presets"] = std::vector<uint8_t>(EE_PRESETS_CNT * 22, 0x23);
  records["fm_presets"] = std::vector<uint8_t>(EE_PRESETS_CNT * (4 + EE_FM_PRESET_NAME_LENGTH), 0x31);
  records["ir_profile"] = std::vector<uint8_t>(EE_IR_CONFIG_SIZE, 0x17);
  records["dab_scan"] = {7, 8, 9};
  records["fm_scan"] = {1, 2, 3};
  const auto legacy = records;
  NvsStorage oldInstallation;
  assert(oldInstallation.begin());
  assert(oldInstallation.getI2sAudioMode() == I2S_AUDIO_48K);
  assert(records == legacy && writes.empty());
  for (unsigned mode = 0; mode < I2S_AUDIO_MODE_COUNT; ++mode) {
    oldInstallation.setI2sAudioMode(mode);
    assert(oldInstallation.commit());
    assert(writes.back() == "i2s_audio");
    for (const auto& record : legacy) assert(records[record.first] == record.second);
    NvsStorage reboot;
    assert(reboot.begin());
    assert(reboot.getI2sAudioMode() == mode);
  }
  // Invalid persisted values are repaired independently of the old image.
  records["i2s_audio"] = {255};
  NvsStorage corrupted;
  assert(corrupted.begin());
  assert(corrupted.getI2sAudioMode() == I2S_AUDIO_48K);
  assert(records["i2s_audio"][0] == I2S_AUDIO_48K);
  corrupted.setI2sAudioMode(I2S_AUDIO_OFF);
  failWrites = true;
  assert(!corrupted.commit());
  assert(records["i2s_audio"][0] == I2S_AUDIO_48K);
  failWrites = false;
  assert(corrupted.commit()); // dirty flag survives a write failure
  assert(records["i2s_audio"][0] == I2S_AUDIO_OFF);
  corrupted.setI2sAudioMode(I2S_AUDIO_48K); // factory-default operation
  assert(corrupted.commit());
  for (const auto& record : legacy) assert(records[record.first] == record.second);
  writes.clear();
  assert(corrupted.commit() && writes.empty());
  std::cout << "I2S mapping, pins, cycling, menu indices and production NVS: PASS\n";
}
