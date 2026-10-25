#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/dab_user_application.h"
#include "../src/vendor/si468x/Si468x.h"

static void testKnownAndUnknown() {
  // Even data lengths (including zero) require a pad; odd lengths do not.
  const uint8_t block[] = {
      2, 0, 0, 0,
      4, 0, 2, 0xAA, 0xBB, 0,
      7, 0, 3, 0x11, 0x22, 0x33,
      0x34, 0x12, 1, 0xFF};
  assert(dab_ua::valid(block, sizeof(block), 4));
  dab_ua::Reader reader(block, sizeof(block), 4);
  dab_ua::Application app;
  assert(reader.next(app) && app.type == dab_ua::SLIDESHOW && app.length == 0);
  assert(strcmp(dab_ua::name(app.type), "Slideshow") == 0);
  assert(reader.next(app) && app.type == dab_ua::TPEG && app.length == 2);
  assert(app.data[0] == 0xAA && app.data[1] == 0xBB);
  assert(strcmp(dab_ua::name(app.type), "TPEG") == 0);
  assert(reader.next(app) && app.type == dab_ua::SPI && app.length == 3);
  assert(app.data[2] == 0x33 && strcmp(dab_ua::name(app.type), "SPI") == 0);
  assert(reader.next(app) && app.type == 0x1234 && app.data[0] == 0xFF);
  assert(strcmp(dab_ua::name(app.type), "unknown") == 0);
  assert(reader.complete());
  assert(!reader.next(app) && app.data == nullptr && app.length == 0);

  for (size_t n = 0; n < sizeof(block); ++n)
    assert(!dab_ua::valid(block, n, 4));
  assert(!dab_ua::valid(block, sizeof(block), 3));
  assert(!dab_ua::valid(block, sizeof(block), 5));
}

static void testLengthsAndCounts() {
  assert(dab_ua::valid(nullptr, 0, 0));
  assert(!dab_ua::valid(nullptr, 1, 0));
  assert(!dab_ua::valid(nullptr, 0, 1));
  const uint8_t zero[] = {7, 0, 0, 0};
  assert(dab_ua::valid(zero, sizeof(zero), 1));
  assert(!dab_ua::valid(zero, 3, 1));  // missing mandatory padding
  assert(!dab_ua::valid(zero, 4, 0));  // trailing bytes
  assert(!dab_ua::valid(zero, 4, 255));

  uint8_t largest[254] = {7, 0, 251};
  memset(largest + 3, 0x5A, sizeof(largest) - 3);
  assert(dab_ua::valid(largest, sizeof(largest), 1));
  for (size_t n = 0; n < sizeof(largest); ++n)
    assert(!dab_ua::valid(largest, n, 1));
  largest[2] = 252;  // data plus pad exceed LENUA capacity
  assert(!dab_ua::valid(largest, sizeof(largest), 1));
  largest[2] = 255;
  assert(!dab_ua::valid(largest, sizeof(largest), 1));

  uint8_t many[252] = {};
  for (size_t i = 0; i < sizeof(many); i += 4) many[i] = 7;
  assert(dab_ua::valid(many, sizeof(many), 63));  // maximum feasible count
  assert(!dab_ua::valid(many, sizeof(many), 64));
  assert(!dab_ua::valid(many, sizeof(many), 255));
  uint8_t oversized[256] = {};
  assert(!dab_ua::valid(oversized, sizeof(oversized), 64));
}

static void testVendorIntegration() {
  uint8_t reply[32] = {};
  reply[4] = 5;
  reply[6] = 0xC9;
  memcpy(reply + 8, "Component label!", 16);
  reply[26] = 1;
  reply[27] = 4;
  reply[28] = 7;
  reply[30] = 1;
  reply[31] = 0xAB;
  si468x::DabComponentInfo info;
  assert(si468x::Si468x::parseDabComponentInfo(reply, sizeof(reply), info) ==
         si468x::Result::Ok);
  assert(info.globalId == 5 && info.language == 9 && info.label[16] == '\0');
  assert(info.userApplicationRaw == reply + 28);
  assert(dab_ua::valid(info.userApplicationRaw, info.userApplicationBytes,
                       info.numberOfUserApplications));
  assert(si468x::Si468x::parseDabComponentInfo(reply, sizeof(reply) - 1, info) ==
         si468x::Result::MalformedReply);
  reply[30] = 255;  // fixed header valid, variable application malformed
  assert(si468x::Si468x::parseDabComponentInfo(reply, sizeof(reply), info) ==
         si468x::Result::Ok);
  assert(!dab_ua::valid(info.userApplicationRaw, info.userApplicationBytes,
                        info.numberOfUserApplications));
}

static void testAllDeclaredLengths() {
  // Exercise every UADATALEN against every possible truncated LENUA. The
  // backing allocation is exactly the declared input length on each trial.
  for (unsigned length = 0; length <= 255; ++length) {
    for (size_t bytes = 0; bytes <= 255; ++bytes) {
      uint8_t* input = new uint8_t[bytes];
      memset(input, 0, bytes);
      if (bytes > 0) input[0] = 7;
      if (bytes > 2) input[2] = static_cast<uint8_t>(length);
      const size_t required = 3U + length + ((length & 1U) ? 0U : 1U);
      assert(dab_ua::valid(input, bytes, 1) ==
             (bytes >= 3U && bytes == required));
      delete[] input;
    }
  }
}

int main() {
  testKnownAndUnknown();
  testLengthsAndCounts();
  testVendorIntegration();
  testAllDeclaredLengths();
  puts("DAB User Application tests passed (including 65,536 length cases)");
}
