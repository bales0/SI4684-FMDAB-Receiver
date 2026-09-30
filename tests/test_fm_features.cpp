#include "../src/fm_features.h"
#include "../src/FmRegion.h"
#include <cassert>
#include <cstring>

struct Service { uint8_t ServiceType; };

int main() {
  assert(stepFmFrequency(10800, 10, static_cast<uint8_t>(FmRegion::Europe)) == 8750);
  assert(stepFmFrequency(10790, 20, static_cast<uint8_t>(FmRegion::NorthAmerica)) == 8790);
  assert(stepFmFrequency(9500, 10, static_cast<uint8_t>(FmRegion::Japan)) == 7600);
  fm_features::AfList af;
  af.clear(0x1234);
  assert(af.addCode(1) && af.frequency10kHz[0] == 8760);
  assert(af.addCode(204) && af.frequency10kHz[1] == 10790);
  assert(!af.addCode(204));
  assert(!af.addCode(205));
  assert(!af.addCode(0));
  assert(!af.addCode(226) && af.expected == 2);

  // MJD 60000 = 2023-02-25, 13:45 UTC, +1 hour local offset.
  fm_features::ClockTime ct;
  const uint32_t mjd = 60000;
  const uint16_t b = static_cast<uint16_t>((mjd >> 15) & 3U);
  const uint16_t c = static_cast<uint16_t>((mjd << 1) | (13U >> 4));
  const uint16_t d = static_cast<uint16_t>(((13U & 15U) << 12) |
                                           (45U << 6) | 2U);
  assert(fm_features::decodeClockTime(b, c, d, ct));
  assert(ct.year == 2023 && ct.month == 2 && ct.day == 25);
  assert(ct.hour == 13 && ct.minute == 45 && ct.localOffsetHalfHours == 2);
  assert(!fm_features::decodeClockTime(
      b, c, static_cast<uint16_t>(63U << 6), ct));
  assert(std::strcmp(fm_features::ptyName(4, false), "Sport") == 0);
  assert(std::strcmp(fm_features::ptyName(4, true), "Talk") == 0);

  const Service services[] = {{8}, {0}, {4}, {8}, {5}};
  assert(fm_features::adjacentAudioService(services, 5, 1, true) == 2);
  assert(fm_features::adjacentAudioService(services, 5, 4, true) == -1);
  assert(fm_features::adjacentAudioService(services, 5, 1, false) == -1);
  assert(fm_features::edgeAudioService(services, 5, true) == 1);
  assert(fm_features::edgeAudioService(services, 5, false) == 4);
  assert(fm_features::nextDabChannel(37, true) == 0);
  assert(fm_features::nextDabChannel(0, false) == 37);
  assert(fm_features::sameStation(0x1234, 9000, 0x1234, 9500));
  assert(!fm_features::sameStation(0x1234, 9000, 0x5678, 9000));
  assert(fm_features::sameStation(0, 9000, 0, 9000));

  using fm_features::Rotary2Target;
  assert(fm_features::rotary2Target(true, true, true, false) ==
         Rotary2Target::FmScanList);
  assert(fm_features::rotary2Target(true, true, false, false) ==
         Rotary2Target::None);
  assert(fm_features::rotary2Target(true, false, false, false) ==
         Rotary2Target::FmFineTune);
  assert(fm_features::rotary2Target(false, true, true, true) ==
         Rotary2Target::DabGlobalList);
  assert(fm_features::rotary2Target(false, false, false, true) ==
         Rotary2Target::DabCurrentMux);
  assert(fm_features::rotary2Target(false, false, false, false) ==
         Rotary2Target::None);

  fm_features::PressTracker press;
  assert(press.update(true, 100, 1000) == 0);
  assert(press.update(false, 900, 1000) == 1);
  assert(press.update(true, 2000, 1000) == 0);
  assert(press.update(true, 3000, 1000) == 2);
  assert(press.update(false, 3100, 1000) == 0);
  return 0;
}
