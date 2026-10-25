#include <assert.h>
#include <stdio.h>
#include "../src/dab_spi_probe.h"
#include "../src/vendor/si468x/Si468x.h"

using dab_probe::Endpoint;
using dab_probe::Probe;
using dab_probe::Route;
using dab_probe::State;

static const Endpoint audio(10, 1);
static const Endpoint sls(20, 2);
static const Endpoint spi(30, 3);

static void testRouter() {
  for (uint8_t source = 0; source < 2; ++source) {
    assert(dab_probe::route(20, 2, source, audio, sls, spi) == Route::Sls);
    assert(dab_probe::route(30, 3, source, audio, sls, spi) == Route::Spi);
  }
  assert(dab_probe::route(10, 1, 1, audio, sls, spi) == Route::Audio);
  assert(dab_probe::route(10, 1, 2, audio, sls, spi) == Route::Audio);
  assert(dab_probe::route(20, 2, 2, audio, sls, spi) == Route::Ignored);
  assert(dab_probe::route(30, 3, 2, audio, sls, spi) == Route::Ignored);
  assert(dab_probe::route(10, 2, 1, audio, sls, spi) == Route::Ignored);
  assert(dab_probe::route(20, 1, 1, audio, sls, spi) == Route::Ignored);
  assert(dab_probe::route(40, 4, 1, audio, sls, spi) == Route::Ignored);
  assert(dab_probe::route(10, 1, 3, audio, sls, spi) == Route::Ignored);
  assert(dab_probe::route(10, 1, 255, audio, sls, spi) == Route::Ignored);
  assert(dab_probe::route(10, 1, 1, Endpoint{}, sls, spi) == Route::Ignored);
  assert(dab_probe::route(20, 2, 1, audio, Endpoint{}, spi) == Route::Ignored);
  // New audio context rejects old audio/PAD packets; Transport ID never enters
  // the router, so equal TIDs on unrelated SID/CID cannot alias an application.
  assert(dab_probe::route(10, 1, 1, Endpoint(11, 1), sls, spi) == Route::Ignored);
}

static void testLegacyReceptionGate() {
  const Route targets[] = {Route::Ignored, Route::Audio, Route::Sls, Route::Spi};
  for (Route target : targets) {
    // Station teardown must still suppress all payloads, with or without SPI.
    assert(!dab_probe::feedLegacyDecoder(target, false, false));
    assert(!dab_probe::feedLegacyDecoder(target, true, false));
    assert(dab_probe::feedLegacyDecoder(target, false, true) == (target != Route::Spi));
    assert(dab_probe::feedLegacyDecoder(target, true, true) ==
           (target == Route::Audio || target == Route::Sls));
  }
  const Endpoint noSpi;
  // Reproduce the regression: a DSRV identity differs from the service-list
  // identity, while confirmed audio is running and no SPI was ever started.
  const Route mismatch = dab_probe::route(10, 0x10001, 2, audio, sls, noSpi);
  assert(mismatch == Route::Ignored);
  assert(dab_probe::feedLegacyDecoder(mismatch, noSpi.valid, true));
  const Route padMismatch = dab_probe::route(10, 0x10001, 1, audio, sls, noSpi);
  assert(dab_probe::feedLegacyDecoder(padMismatch, noSpi.valid, true));
  // A reserved/unknown source follows the old decoder's own checks outside
  // probing, but is never admitted by strict probe ownership.
  const Route unknown = dab_probe::route(10, 1, 3, audio, sls, noSpi);
  assert(dab_probe::feedLegacyDecoder(unknown, false, true));
  assert(!dab_probe::feedLegacyDecoder(unknown, true, true));
  // Cleanup failure retains spi.valid: keep quarantine despite running()==false.
  Probe cleanup;
  cleanup.spi = spi;
  cleanup.state = State::CleanupFailed;
  assert(!cleanup.running());
  assert(!dab_probe::feedLegacyDecoder(mismatch, cleanup.spi.valid, true));
  assert(!dab_probe::feedLegacyDecoder(Route::Spi, cleanup.spi.valid, true));
}

static void testCapturedAudioPadIdentity() {
  const Endpoint capturedAudio(0x00002FD9U, 0x01FE002AU);
  const Endpoint data(0x00002FD9U, 0x013CC02AU);
  const Endpoint programmeInfo(0x00002FD9U, 0x013CC02BU);
  for (uint8_t source = 1; source <= 2; ++source) {
    // Exact values captured on hardware for both non-DLS PAD and DLS PAD.
    const Route target = dab_probe::route(0x2FD9U, 0x2AU, source,
                                         capturedAudio, data, programmeInfo);
    assert(target == Route::Audio);
    assert(dab_probe::feedLegacyDecoder(target, true, true));
    assert(dab_probe::route(0x2FD9U, 0x01FE002AU, source,
                            capturedAudio, data, programmeInfo) == Route::Audio);
    // Matching low bits alone cannot admit another service, component or
    // nonzero upper half. The full 16-bit field is compared, including TMId.
    assert(dab_probe::route(0x2FD8U, 0x2AU, source,
                            capturedAudio, data, programmeInfo) == Route::Ignored);
    assert(dab_probe::route(0x2FD9U, 0x2BU, source,
                            capturedAudio, data, programmeInfo) == Route::Ignored);
    assert(dab_probe::route(0x2FD9U, 0x02FE002AU, source,
                            capturedAudio, data, programmeInfo) == Route::Ignored);
    assert(dab_probe::route(0x2FD9U, 0xC02AU, source,
                            capturedAudio, data, programmeInfo) == Route::Ignored);
    assert(dab_probe::route(0x2FD9U, 0x2AU, source,
                            Endpoint{}, data, programmeInfo) == Route::Ignored);
    assert(dab_probe::route(0x2FD9U, 0x2AU, source,
                            Endpoint(0x2FD9U, 0x013CC02AU), data,
                            programmeInfo) == Route::Ignored);
  }
  // Audio fallback stays PAD/DLS-only; packet data uses its own TMID=3 match.
  assert(dab_probe::route(0x2FD9U, 0x2AU, 0,
                          capturedAudio, data, programmeInfo) == Route::Ignored);
  assert(dab_probe::route(0x2FD9U, 0x2AU, 3,
                          capturedAudio, data, programmeInfo) == Route::Ignored);
  assert(dab_probe::route(0x2FD9U, 0xC02AU, 0,
                          capturedAudio, data, programmeInfo) == Route::Sls);
  assert(dab_probe::route(0x2FD9U, 0xC02BU, 0,
                          capturedAudio, data, programmeInfo) == Route::Spi);
  assert(dab_probe::route(0x2FD9U, 0x013CC02AU, 0,
                          capturedAudio, data, programmeInfo) == Route::Sls);
  assert(dab_probe::route(0x2FD9U, 0x013CC02BU, 0,
                          capturedAudio, data, programmeInfo) == Route::Spi);
  // Wire-command ownership retains the original complete service-list CID.
  assert(capturedAudio.cid == 0x01FE002AU);
  assert(capturedAudio.matches(0x2FD9U, 0x01FE002AU));
  assert(!capturedAudio.matches(0x2FD9U, 0x2AU));
}

static void testCapturedPacketIdentity() {
  const uint32_t sid = 0xE231B3FEU, full = 0x01F2C00BU, shortCid = 0xC00BU;
  const Endpoint packet(sid, full);
  // The unchanged production parser must retain the incoming short CID;
  // normalization belongs only to routing, never to the wire parser.
  uint8_t reply[24] = {};
  reply[0] = 0x80; reply[4] = 1; reply[6] = 3; reply[7] = 60;
  si468x::writeLe32(reply + 8, sid);
  si468x::writeLe32(reply + 12, shortCid);
  si468x::writeLe16(reply + 18, 512);
  si468x::DsrvHeader header;
  assert(si468x::Si468x::parseDsrvHeader(reply, sizeof(reply), header) == si468x::Result::Ok);
  assert(header.serviceId == sid && header.componentId == shortCid && header.byteCount == 512);
  assert(dab_probe::route(header.serviceId, header.componentId, header.dataSource,
                          audio, sls, packet) == Route::Spi);
  assert(packet.matches(sid, full) && !packet.matches(sid, shortCid));
  for (unsigned source = 0; source < 256; ++source) {
    const Route abbreviated = dab_probe::route(sid, shortCid, source, audio, sls, packet);
    assert(abbreviated == (source == 0 ? Route::Spi : Route::Ignored));
    // Preserve existing exact-CID semantics: source 0/1 allowed, DLS/reserved rejected.
    assert(dab_probe::route(sid, full, source, audio, sls, packet) ==
           (source < 2 ? Route::Spi : Route::Ignored));
  }
  assert(dab_probe::route(sid + 1, shortCid, 0, audio, sls, packet) == Route::Ignored);
  assert(dab_probe::route(sid, shortCid + 1, 0, audio, sls, packet) == Route::Ignored);
  assert(dab_probe::route(sid, 0x02F2C00BU, 0, audio, sls, packet) == Route::Ignored);
  assert(dab_probe::route(sid, shortCid, 0, audio, sls, Endpoint(sid, full, false)) == Route::Ignored);
  for (uint32_t tmid = 0; tmid < 3; ++tmid) {
    const uint32_t cid = 0x0100000BU | (tmid << 14);
    assert(!dab_probe::matchesPacketData(Endpoint(sid, cid), sid, cid & 0xFFFFU));
    assert(dab_probe::route(sid, cid & 0xFFFFU, 0, audio, sls, Endpoint(sid, cid)) == Route::Ignored);
  }
  assert(dab_probe::route(sid, shortCid, 0, audio, packet, Endpoint{}) == Route::Sls);
  // SPI keeps priority over overlapping audio/SLS contexts.
  assert(dab_probe::route(sid, shortCid, 0, Endpoint(sid, shortCid), packet, packet) == Route::Spi);
  // Same SID as packet SPI does not steal audio PAD or DLS.
  const Endpoint sharedAudio(sid, 0x01FE000BU);
  for (uint8_t source = 1; source <= 2; ++source)
    assert(dab_probe::route(sid, 0xBU, source, sharedAudio, sls, packet) == Route::Audio);

  Probe probe;
  assert(probe.request(audio, sls, packet, 9, 0));
  assert(probe.spi.matches(sid, full) && !probe.spi.matches(sid, shortCid));
  probe.advance(5000, true);
  probe.state = State::Starting;
  probe.started(5001, true, false);
  const Route target = dab_probe::route(sid, shortCid, 0, audio, sls, probe.spi);
  probe.observe(target, 0, false);
  assert(probe.after.spi == 1 && probe.after.ignored == 0);
  assert(!dab_probe::feedLegacyDecoder(target, probe.spi.valid, true));
  probe.advance(6000, false); // switch cancels observation, retaining full STOP ownership
  assert(probe.state == State::StopRequested && probe.spi.matches(sid, full));
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    probe.state = State::Stopping;
    probe.stopAttempts = attempt;
    probe.stopped(6001, false);
  }
  assert(probe.state == State::CleanupFailed && probe.spi.matches(sid, full));
  const Route late = dab_probe::route(sid, shortCid, 0, Endpoint(11, 1), sls, probe.spi);
  assert(late == Route::Spi && !dab_probe::feedLegacyDecoder(late, probe.spi.valid, true));
  // After cleanup/tune, old packet identity has no matching current endpoint.
  probe.stopped(7000, true);
  assert(dab_probe::route(sid, shortCid, 0, audio, sls, probe.spi) == Route::Ignored);
  assert(dab_probe::route(sid, shortCid, 0, audio, sls, Endpoint(sid + 1, full)) == Route::Ignored);
}

static Probe observing(uint32_t now = 100) {
  Probe probe;
  assert(probe.request(audio, sls, spi, 9, now));
  probe.observe(Route::Sls, 1, false);
  probe.observe(Route::Audio, 1, false);
  probe.advance(now + 4999, true);
  assert(probe.state == State::Baseline);
  probe.advance(now + 5000, true);
  assert(probe.state == State::StartRequested);
  probe.state = State::Starting;
  probe.started(now + 5001, true, false);
  assert(probe.state == State::Observing);
  return probe;
}

static void testWindowAndEvidence() {
  Probe probe = observing();
  probe.observe(Route::Spi, 0, false);
  probe.observe(Route::Sls, 1, false);
  probe.observe(Route::Audio, 2, false);
  probe.observe(Route::Audio, 1, false);
  probe.observe(Route::Ignored, 1, false);
  assert(probe.before.sls == 1 && probe.before.pad == 1 && probe.before.spi == 0);
  assert(probe.after.spi == 1 && probe.after.sls == 1 && probe.after.audio == 2);
  assert(probe.after.pad == 1 && probe.after.ignored == 1);
  probe.observe(Route::Sls, 0, true, false);  // zero-byte STOP notification
  assert(probe.after.slsStopped && probe.after.sls == 1);
  probe.advance(35100, true);
  assert(probe.state == State::Observing);
  probe.advance(35101, true);
  assert(probe.state == State::StopRequested);
  probe.state = State::Stopping;
  probe.stopAttempts = 1;
  probe.stopped(35102, true);
  assert(probe.state == State::Idle && !probe.spi.valid);
  assert(probe.after.spi == 1);  // retained final evidence
}

static void testRefusalAndCancellation() {
  Probe probe;
  assert(!probe.request(Endpoint{}, sls, spi, 9, 0));
  assert(!probe.request(audio, sls, Endpoint{}, 9, 0));
  assert(!probe.request(audio, sls, audio, 9, 0));
  assert(!probe.request(audio, sls, sls, 9, 0));
  assert(probe.request(audio, sls, spi, 9, 0));
  assert(!probe.request(audio, sls, spi, 9, 0));
  probe.advance(20, false);
  assert(probe.state == State::Idle && !probe.spi.valid);

  assert(probe.request(audio, sls, spi, 9, 0));
  probe.advance(5000, true);
  probe.stop(5000);
  assert(probe.state == State::Idle);  // no START was submitted

  assert(probe.request(audio, sls, spi, 9, 0));
  probe.state = State::Starting;
  probe.stop(5);  // station switch while START waits for CTS
  probe.started(10, true, false);
  assert(probe.state == State::StopRequested && probe.spi.valid);

  probe = observing();
  probe.advance(6000, false);
  assert(probe.state == State::StopRequested && probe.spi.valid);
}

static void testFailuresAndWraparound() {
  Probe probe = observing();
  probe.stop(0xFFFFFFF0U);
  assert(dab_probe::due(0xFFFFFFF0U, probe.nextActionMs));
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    probe.state = State::Stopping;
    probe.stopAttempts = attempt;
    probe.stopped(0xFFFFFFF0U, false);
    if (attempt < 3) {
      assert(probe.state == State::StopRequested);
      assert(!dab_probe::due(0xFFFFFFF0U, probe.nextActionMs));
      probe.advance(0, false);  // cancellation must not defeat STOP backoff
      assert(!dab_probe::due(0, probe.nextActionMs));
      assert(dab_probe::due(484, probe.nextActionMs));
    }
  }
  assert(probe.state == State::CleanupFailed && probe.spi.valid);
  assert(!probe.running() && !probe.request(audio, sls, spi, 9, 0));

  probe = Probe{};
  assert(probe.request(audio, sls, spi, 9, 0));
  probe.state = State::Starting;
  probe.started(1, false, true);  // unanswered START may have reached hardware
  assert(probe.state == State::StopRequested && probe.spi.valid);
  probe.started(1, false, false);  // explicit device rejection
  assert(probe.state == State::Idle && !probe.spi.valid);

  probe = observing(0xFFFFFF00U);
  probe.advance(34744U, true);
  assert(probe.state == State::Observing);
  probe.advance(34745U, true);
  assert(probe.state == State::StopRequested);
  uint32_t counter = UINT32_MAX;
  dab_probe::increment(counter);
  assert(counter == UINT32_MAX);
}

int main() {
  testRouter();
  testLegacyReceptionGate();
  testCapturedAudioPadIdentity();
  testCapturedPacketIdentity();
  testWindowAndEvidence();
  testRefusalAndCancellation();
  testFailuresAndWraparound();
  puts("DAB SPI probe/router tests passed");
}
