#include "../src/dab_diagnostics.h"
#include "../src/dab_spi_discovery.h"
#include "../src/dab_spi_probe.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>

int main() {
  dab_diag::XpadReadback xpad;
  assert(!xpad.valid && !xpad.matches());
  xpad.setResult=0; xpad.getResult=0; xpad.value=0x0097; xpad.valid=true;
  assert(xpad.matches());
  xpad.value=0x0095; assert(!xpad.matches());
  xpad.value=0x0097; xpad.getResult=-4; assert(!xpad.matches());
  xpad=dab_diag::XpadReadback{}; assert(!xpad.valid && !xpad.matches());

  assert(std::strcmp(dab_diag::rawMeaning(1,2),"SLS-candidate")==0);
  assert(std::strcmp(dab_diag::rawMeaning(1,7),"SPI-candidate")==0);
  assert(std::strcmp(dab_diag::rawMeaning(0,7),"SPI-candidate")==0);
  assert(std::strcmp(dab_diag::rawMeaning(1,0),"unknown")==0);
  assert(std::strcmp(dab_diag::rawMeaning(1,0x1234),"unknown")==0);
  for (uint16_t raw : {uint16_t(0),uint16_t(2),uint16_t(7)}) {
    assert(std::strcmp(dab_diag::rawMeaning(2,raw),"DLS-raw")==0);
    assert(std::strcmp(dab_diag::rawMeaning(3,raw),"reserved")==0);
  }
  dab_diag::RawLog log;
  assert(!log.ready(100,1,false,512) && log.suppressed==0);
  assert(!log.ready(100,1,true,0) && log.suppressed==1);
  assert(log.ready(100,1,true,512)); log.emitted();
  assert(!log.ready(5099,1,true,512) && log.suppressed==1);
  assert(log.ready(5100,1,true,512) && log.suppressed==1); log.emitted();
  assert(!log.ready(10100,1,true,255));
  assert(log.ready(10100,1,true,256) && log.suppressed==1); log.emitted();
  assert(log.ready(10101,2,true,512) && log.suppressed==0); // retune
  assert(!log.ready(10102,2,false,512) && log.suppressed==0);
  assert(log.ready(0xFFFFFFF0U,3,true,512)); log.emitted();
  assert(!log.ready(0x1377,3,true,512)); // 4999 ms over millis wrap
  assert(log.ready(0x1378,3,true,512)); log.emitted();
  log.suppressed=UINT32_MAX;
  assert(!log.ready(0x1379,3,true,0) && log.suppressed==UINT32_MAX);

  // Raw 0x007 is diagnostic only. Confirmation remains a separate UA scan.
  const dab_probe::Endpoint audio(0x2FB9,0x01FE0005,true), none;
  for (uint16_t raw : {uint16_t(0),uint16_t(2),uint16_t(7)}) {
    (void)dab_diag::rawMeaning(1,raw);
    const auto route=dab_probe::route(0x2FB9,5,1,audio,none,none);
    assert(route==dab_probe::Route::Audio);
    assert(dab_probe::feedLegacyDecoder(route,false,true));
    assert(dab_probe::route(0x2FB9,5,2,audio,none,none)==dab_probe::Route::Audio);
    assert(dab_probe::route(0x2FB8,5,1,audio,none,none)==dab_probe::Route::Ignored);
    assert(dab_probe::route(0x2FB9,6,1,audio,none,none)==dab_probe::Route::Ignored);
    dab_spi::Selector selector(0x2FB9);
    selector.consider(0x2FB9,0x01FE0005,dab_spi::Identity::Slideshow);
    assert(!selector.finish().confirmedPresent());
  }
  dab_spi::Selector packet(0x2FB9);
  packet.consider(12,0x00F0E321,dab_spi::Identity::NotSpi,true);
  assert(packet.finish().status==dab_spi::Status::PacketMotUnverified);
  assert(!packet.finish().confirmedPresent());
  assert(sizeof(dab_diag::RawLog)<=16 && sizeof(dab_diag::XpadReadback)<=6);
  std::puts("XPAD diagnostic state, raw meaning, pacing, wrap, contexts and SPI isolation: PASS");
}
