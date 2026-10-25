#include "../src/dab_component_inventory.h"
#include "../src/dab_spi_discovery.h"
#include "../src/vendor/si468x/Si468x.h"
#include <cassert>
#include <cstdio>
#include <vector>

using dab_spi::Identity;
using dab_spi::Status;
using Bytes = std::vector<uint8_t>;

static void append32(Bytes& b, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(value >> (8*i)));
}
static void addService(Bytes& list, uint32_t sid, bool data,
                       std::initializer_list<uint32_t> components) {
  ++list[4];
  append32(list, sid);
  list.push_back(data ? 1 : 0);
  list.push_back(static_cast<uint8_t>(components.size()));
  list.push_back(0); list.push_back(0);
  for (unsigned i = 0; i < 16; ++i) list.push_back('A');
  for (uint32_t cid : components) append32(list, cid);
  const uint16_t size = static_cast<uint16_t>(list.size() - 2);
  list[0] = static_cast<uint8_t>(size); list[1] = static_cast<uint8_t>(size >> 8);
}
static Bytes audioMux() {
  Bytes list = {6, 0, 7, 0, 0, 0, 0, 0};
  for (unsigned i = 0; i < 10; ++i) addService(list, 0x2100+i, false, {0x01FE0008+i});
  return list;
}
static dab_spi::Result classify(const Bytes& list, uint32_t confirmedCid = 0,
                                uint32_t failedCid = 0) {
  dab_inventory::Header header;
  assert(dab_inventory::validate(list.data(), list.size(), 32, 448, header));
  dab_spi::Selector selector(0x2100);
  size_t offset = 8;
  for (unsigned i = 0; i < header.services; ++i) {
    const uint32_t sid = dab_inventory::le32(list.data() + offset);
    const unsigned count = list[offset + 5] & 15;
    offset += 24;
    for (unsigned j = 0; j < count; ++j, offset += 4) {
      const uint32_t cid = dab_inventory::le32(list.data() + offset);
      const dab_inventory::Component component{cid};
      const Identity identity = cid == failedCid ? Identity::Failed :
          (cid == confirmedCid ? Identity::Spi :
           (component.tmid() == 0 ? Identity::Slideshow : Identity::NotSpi));
      selector.consider(sid, cid, identity, component.packetMot());
    }
  }
  return selector.finish();
}
static void testFixtures() {
  auto list = audioMux();
  auto result = classify(list);
  assert(result.status == Status::NotSignalled && !result.confirmedPresent());
  assert(result.packetMotUnverified == 0 && !result.available());
  // Separate data service, exact full SID/CID, UA-valid=0 is not a negative proof.
  const uint32_t packetCid = 0x00F2E321;
  addService(list, 0xE1234567, true, {packetCid});
  result = classify(list);
  assert(result.status == Status::PacketMotUnverified && result.packetMotUnverified == 1);
  assert(!result.confirmedPresent() && !result.available());
  result = classify(list, packetCid);
  assert(result.confirmedPresent() && result.available());
  assert(result.serviceId == 0xE1234567 && result.componentId == packetCid);
  assert(result.packetMotUnverified == 0);
  result = classify(list, 0, packetCid);
  assert(result.status == Status::Incomplete && !result.available());
  // Positive SPI is on the last component, never replaced by unknown MOT.
  list = {6, 0, 7, 0, 0, 0, 0, 0};
  addService(list, 0x2100, false, {0x01FE0008, 0x00F0E111, 0x01F2E222});
  for (unsigned i = 1; i < 10; ++i)
    addService(list, 0x2100+i, false, {0x01FE0008+i});
  addService(list, 0xE1234567, true, {packetCid});
  result = classify(list, 0x01F2E222);
  assert(result.confirmed == 1 && result.packetMotUnverified == 2);
  assert(result.status == Status::SelectedService && result.componentId == 0x01F2E222);
  dab_spi::Selector ambiguous(0x2100);
  ambiguous.consider(11, packetCid, Identity::Spi, true);
  ambiguous.consider(12, 0x01F0E222, Identity::SpiSlideshow, true);
  result = ambiguous.finish();
  assert(result.status == Status::Ambiguous && result.confirmedPresent());
  assert(!result.available() && result.serviceId == 0 && result.componentId == 0);
  dab_spi::Selector incomplete(0x2100);
  incomplete.consider(11, packetCid, Identity::Spi, true);
  incomplete.consider(12, 0x01F0E222, Identity::Failed, true);
  result = incomplete.finish();
  assert(result.status == Status::Incomplete && result.confirmedPresent() && !result.available());
}
static void testBounds() {
  const auto list = audioMux();
  dab_inventory::Header old;
  old.version = 55; old.services = 22; old.components = 333;
  for (size_t bytes = 0; bytes < list.size(); ++bytes) {
    assert(!dab_inventory::validate(list.data(), bytes, 32, 448, old));
    assert(old.version == 55 && old.services == 22 && old.components == 333);
  }
  assert(!dab_inventory::validate(nullptr, list.size(), 32, 448, old));
  assert(!dab_inventory::validate(list.data(), list.size(), 9, 448, old));
  assert(!dab_inventory::validate(list.data(), list.size(), 32, 9, old));
  auto bad = list;
  bad[8+5] = 15;
  assert(!dab_inventory::validate(bad.data(), bad.size(), 32, 448, old));
  bad = list; bad[4] = 32;
  assert(!dab_inventory::validate(bad.data(), bad.size(), 32, 448, old));
  bad = list; ++bad[0];
  assert(!dab_inventory::validate(bad.data(), bad.size(), 32, 448, old));
  bad = list; bad.push_back(0);
  assert(!dab_inventory::validate(bad.data(), bad.size(), 32, 448, old));
  bad = list; bad.resize(8); bad[0] = 6; bad[1] = 0;
  assert(!dab_inventory::validate(bad.data(), bad.size(), 32, 448, old));
  Bytes empty = {6, 0, 9, 1, 0, 0, 0, 0};
  assert(dab_inventory::validate(empty.data(), empty.size(), 32, 448, old));
  assert(old.services == 0 && old.components == 0 && old.version == 265);
}
static void testVendorAgreement() {
  // All 65,536 raw TMID/reference fields, plus every info/valid byte value.
  for (uint32_t raw = 0; raw <= 0xFFFF; ++raw) {
    const uint32_t cid = raw | (((raw * 73) & 255) << 16) | ((raw & 255) << 24);
    Bytes list = {6, 0, 1, 0, 0, 0, 0, 0};
    addService(list, 0xE1234567, true, {cid});
    struct Capture { si468x::DabComponentEntry component; bool program; } capture{};
    si468x::DabServiceListSink sink;
    sink.context = &capture;
    sink.onService = [](void* ctx, const si468x::DabServiceEntry& s) {
      static_cast<Capture*>(ctx)->program = s.programService;
    };
    sink.onComponent = [](void* ctx, const si468x::DabComponentEntry& c) {
      static_cast<Capture*>(ctx)->component = c;
    };
    si468x::DabServiceListParser vendor;
    vendor.setSink(sink);
    assert(vendor.feed(list.data(), list.size()) == si468x::Result::Ok && vendor.complete());
    const auto& expected = capture.component;
    const dab_inventory::Component actual{cid};
    assert(!capture.program);
    assert(expected.componentId == actual.cid && expected.transportModeId == actual.tmid());
    assert(expected.componentType == actual.type() && expected.dataGroupFlag == actual.dataGroups());
    assert(actual.primary() == ((cid & 0x00020000UL) != 0));
    assert(expected.primary == actual.primary() && expected.conditionalAccess == actual.ca());
    assert(expected.userApplicationInfoValid == actual.uaValid());
    assert(expected.componentReference == actual.reference());
    assert(actual.packetMot() == (actual.tmid() == 3 && actual.type() == 60));
  }
}
static void testPrimaryPolarity() {
  const dab_inventory::Component primary{0x01FE0002};
  const dab_inventory::Component secondary{0x01FC0002};
  assert(primary.primary() && !secondary.primary());
  assert(primary.tmid()==secondary.tmid() && primary.type()==secondary.type());
  assert(primary.ca()==secondary.ca() && primary.dataGroups()==secondary.dataGroups());
  assert(primary.uaValid()==secondary.uaValid() && primary.reference()==secondary.reference());
  for (const uint32_t cid : {primary.cid, secondary.cid}) {
    Bytes list{6,0,0,0,0,0,0,0}; addService(list,0x2F23,false,{cid});
    si468x::DabComponentEntry parsed;
    si468x::DabServiceListSink sink;
    sink.context=&parsed;
    sink.onComponent=[](void* ctx,const si468x::DabComponentEntry& c) {
      *static_cast<si468x::DabComponentEntry*>(ctx)=c;
    };
    si468x::DabServiceListParser parser; parser.setSink(sink);
    assert(parser.feed(list.data(),list.size())==si468x::Result::Ok && parser.complete());
    assert(parsed.componentId==cid && parsed.primary==(cid==primary.cid));
    assert(parsed.transportModeId==0 && parsed.componentType==63);
    assert(!parsed.conditionalAccess && !parsed.dataGroupFlag);
    assert(parsed.userApplicationInfoValid && parsed.componentReference==2);
  }
}
static void testSlideshowIsolation() {
  const uint32_t audio = 0x01FE0008, packet = 0x01F0E321;
  for (Identity identity : {Identity::Unqueried, Identity::Failed, Identity::NotSpi,
                            Identity::Spi, Identity::SpiSlideshow}) {
    dab_spi::SlideshowSelector sls(10, audio);
    sls.consider(10, packet, identity);
    assert(!sls.selected());
  }
  dab_spi::SlideshowSelector explicitSls(10, audio);
  explicitSls.consider(10, packet, Identity::Slideshow);
  assert(explicitSls.selected() && explicitSls.componentId() == packet);
  explicitSls.consider(10, audio, Identity::SpiSlideshow);
  assert(!explicitSls.selected()); // X-PAD already carries slideshow
  dab_spi::SlideshowSelector foreign(10, audio);
  foreign.consider(11, packet, Identity::Slideshow);
  foreign.consider(10, packet | 0x10000, Identity::Slideshow); // conditional access
  assert(!foreign.selected());
  dab_spi::SlideshowSelector multiple(10, audio);
  multiple.consider(10, packet, Identity::Slideshow);
  multiple.consider(10, packet + 1, Identity::Slideshow);
  assert(!multiple.selected());
}
int main() {
  testFixtures(); testBounds(); testVendorAgreement(); testPrimaryPolarity(); testSlideshowIsolation();
  std::puts("DAB packet inventory fixtures, bounds, vendor agreement and SLS isolation: PASS");
}
