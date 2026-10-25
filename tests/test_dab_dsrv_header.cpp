#include "../src/vendor/si468x/Si468x.h"
#include "../src/dab_spi_probe.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
  // Synthetic parser fixtures, not evidence of firmware UATYPE support.
  const uint16_t rawValues[] = {0, 2, 7, 0xABCD};
  const dab_probe::Endpoint audio(0x2FD9, 0x01FE002A, true);
  const dab_probe::Endpoint sls(0x12345678, 0x00F0E321, true);
  const dab_probe::Endpoint spi;
  for (unsigned source = 0; source < 3; ++source) {
    for (uint16_t raw : rawValues) {
      uint8_t reply[27] = {};
      reply[0] = 0x80;
      reply[4] = 1; reply[5] = 3; reply[6] = 3;
      reply[7] = static_cast<uint8_t>((source << 6) | (source == 2 ? 0 : 60));
      const uint32_t sid = source == 0 ? sls.sid : audio.sid;
      const uint32_t cid = source == 0 ? sls.cid : 0x2A;
      for (unsigned i = 0; i < 4; ++i) {
        reply[8+i] = static_cast<uint8_t>(sid >> (8*i));
        reply[12+i] = static_cast<uint8_t>(cid >> (8*i));
      }
      reply[16] = static_cast<uint8_t>(raw); reply[17] = raw >> 8;
      reply[18] = 3; reply[20] = 0x34; reply[21] = 0x12;
      reply[22] = 0x78; reply[23] = 0x56;
      reply[24] = 0xFF; reply[25] = 0xD8; reply[26] = 0x42;
      uint8_t saved[sizeof(reply)]; std::memcpy(saved, reply, sizeof(reply));
      si468x::DsrvHeader header;
      for (size_t length = 0; length < 24; ++length) {
        header.rawUatypeOrRfu = 0x9876;
        assert(si468x::Si468x::parseDsrvHeader(reply, length, header) ==
               si468x::Result::MalformedReply);
        assert(header.rawUatypeOrRfu == 0x9876);
      }
      assert(si468x::Si468x::parseDsrvHeader(nullptr, 24, header) ==
             si468x::Result::MalformedReply);
      assert(si468x::Si468x::parseDsrvHeader(reply, 24, header) == si468x::Result::Ok);
      assert(header.rawUatypeOrRfu == raw);
      assert(header.dataSource == source && header.dscType == (source == 2 ? 0 : 60));
      assert(header.interruptSource == 1 && header.buffersRemaining == 3);
      assert(header.serviceState == 3 && header.serviceId == sid && header.componentId == cid);
      assert(header.byteCount == 3 && header.segmentNumber == 0x1234);
      assert(header.numberOfSegments == 0x5678);
      assert(std::memcmp(saved, reply, sizeof(reply)) == 0);
      assert(reply[24] == 0xFF && reply[25] == 0xD8 && reply[26] == 0x42);
      const auto route = dab_probe::route(header.serviceId, header.componentId,
                                          header.dataSource, audio, sls, spi);
      assert(route == (source == 0 ? dab_probe::Route::Sls : dab_probe::Route::Audio));
      assert(dab_probe::feedLegacyDecoder(route, false, true));
    }
  }
  std::puts("DSRV raw field, short replies, offsets, payload and source routing: PASS");
}
