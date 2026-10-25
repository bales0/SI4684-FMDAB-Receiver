#include <assert.h>
#include <stdio.h>
#include "../src/dab_spi_discovery.h"

using dab_spi::Identity;
using dab_spi::Result;
using dab_spi::Selector;
using dab_spi::Status;

static void expectUnavailable(const Result& result, Status status) {
  assert(result.status == status);
  assert(!result.available());
  assert(result.serviceId == 0 && result.componentId == 0);
}

static void testIdentification() {
  const uint8_t spi[] = {7, 0, 0, 0};
  const uint8_t sls[] = {2, 0, 0, 0};
  const uint8_t tpeg[] = {4, 0, 1, 7};
  const uint8_t unknown[] = {0x07, 0x01, 1, 7};
  const uint8_t multiple[] = {2, 0, 0, 0, 7, 0, 0, 0, 4, 0, 0, 0};
  const uint8_t duplicate[] = {7, 0, 0, 0, 7, 0, 0, 0};
  const uint8_t corruptTail[] = {7, 0, 0, 0, 2, 0, 255, 0};
  assert(dab_spi::identify(spi, sizeof(spi), 1) == Identity::Spi);
  assert(dab_spi::identify(sls, sizeof(sls), 1) == Identity::Slideshow);
  assert(dab_spi::identify(tpeg, sizeof(tpeg), 1) == Identity::NotSpi);
  assert(dab_spi::identify(unknown, sizeof(unknown), 1) == Identity::NotSpi);
  assert(dab_spi::identify(multiple, sizeof(multiple), 3) == Identity::SpiSlideshow);
  assert(dab_spi::identify(duplicate, sizeof(duplicate), 2) == Identity::Spi);
  assert(dab_spi::identify(corruptTail, sizeof(corruptTail), 2) == Identity::Failed);
  assert(dab_spi::identify(nullptr, 0, 0) == Identity::NotSpi);
  assert(dab_spi::identify(nullptr, 1, 1) == Identity::Failed);
  for (size_t size = 0; size < sizeof(spi); ++size)
    assert(dab_spi::identify(spi, size, 1) == Identity::Failed);
}

static void testAvailability() {
  Selector empty(10);
  expectUnavailable(empty.finish(), Status::NotSignalled);
  empty.consider(10, 1, Identity::NotSpi);
  expectUnavailable(empty.finish(), Status::NotSignalled);

  Selector pending(10);
  pending.consider(10, 1, Identity::Spi);
  pending.consider(11, 1, Identity::Unqueried);
  expectUnavailable(pending.finish(), Status::Pending);

  Selector failed(10);
  failed.consider(10, 1, Identity::Spi);
  failed.consider(11, 1, Identity::Failed);
  expectUnavailable(failed.finish(), Status::Incomplete);

  Selector lone(10);
  lone.consider(11, 99, Identity::Spi);
  const Result result = lone.finish();
  assert(result.available() && result.status == Status::SelectedEnsemble);
  assert(result.serviceId == 11 && result.componentId == 99);
}

static void testPriorityAndOrdering() {
  // Try all permutations of two unrelated SPI components and one belonging
  // to the current service: no first-reply/lowest-SID shortcut may win.
  const uint32_t sids[] = {10, 20, 30};
  for (unsigned a = 0; a < 3; ++a) {
    for (unsigned b = 0; b < 3; ++b) {
      if (a == b) continue;
      const unsigned c = 3U - a - b;
      Selector selector(20);
      selector.consider(sids[a], 7, Identity::Spi);
      selector.consider(sids[b], 7, Identity::Spi);
      selector.consider(sids[c], 7, Identity::Spi);
      const Result result = selector.finish();
      assert(result.available() && result.status == Status::SelectedService);
      assert(result.serviceId == 20 && result.componentId == 7);
    }
  }
  Selector multipleLocal(20);
  multipleLocal.consider(20, 1, Identity::Spi);
  multipleLocal.consider(20, 2, Identity::Spi);
  multipleLocal.consider(10, 1, Identity::Spi);
  expectUnavailable(multipleLocal.finish(), Status::Ambiguous);

  Selector multipleOther(99);
  multipleOther.consider(20, 1, Identity::Spi);
  multipleOther.consider(10, 1, Identity::Spi);  // equal CID, different SID
  expectUnavailable(multipleOther.finish(), Status::Ambiguous);

  Selector duplicated(20);
  for (unsigned i = 0; i < 100000; ++i)
    duplicated.consider(20, 1, Identity::Spi);
  assert(duplicated.finish().status == Status::SelectedService);
  duplicated.consider(20, 2, Identity::Spi);
  for (unsigned i = 0; i < 100000; ++i)
    duplicated.consider(20, 1, Identity::Spi);
  expectUnavailable(duplicated.finish(), Status::Ambiguous);
}

static Result forService(uint32_t sid) {
  Selector selector(sid);
  selector.consider(10, 100, Identity::Spi);
  selector.consider(20, 200, Identity::Spi);
  return selector.finish();
}

static void testServiceChangeAndFullInventory() {
  assert(forService(10).serviceId == 10);
  assert(forService(20).serviceId == 20);
  expectUnavailable(forService(30), Status::Ambiguous);
  // Current-service identity must not be inferred from CompID alone.
  Selector full(0xFFFFFFFEU);
  for (uint32_t i = 0; i < 448; ++i)
    full.consider(i, 0xFFFFFFFFU, Identity::NotSpi);
  full.consider(0xFFFFFFFEU, 0xFFFFFFFFU, Identity::Spi);
  const Result result = full.finish();
  assert(result.status == Status::SelectedService);
  assert(result.serviceId == 0xFFFFFFFEU && result.componentId == 0xFFFFFFFFU);
  assert(sizeof(Selector) <= 48U);  // fixed storage independent of candidates
}

static void testAsyncScanCompletion() {
  // A: every request has been sent, but the final reply is still in flight.
  dab_spi::Scan scan;
  scan.begin(17, true);
  const uint32_t token = scan.generation;
  for (unsigned i = 0; i < 17; ++i) {
    assert(scan.issue());
    assert(!scan.issue());  // one outstanding request only
    assert(!scan.functionalComplete(true));
    if (i == 16) {
      assert(scan.scheduled == scan.total);
      assert(scan.processed == 16);
      assert(!scan.functionalComplete(false));
    }
    assert(scan.commit(token, true));
    assert(!scan.commit(token, true));  // no duplicate completion
    assert(scan.functionalComplete(false) == (i == 16)); // logs are independent
  }
  assert(scan.functionalComplete(false));
  assert(!scan.functionalComplete(true));
  assert(!scan.issue());
  assert(scan.processed == 17 && scan.failed == 0);

  // D: a failure settles the request, but cannot mean SPI is absent.
  scan.begin(2, true);
  Selector failed(1);
  assert(scan.issue() && scan.commit(scan.generation, true));
  failed.consider(1, 1, Identity::NotSpi);
  assert(scan.issue() && scan.commit(scan.generation, false));
  failed.consider(1, 2, Identity::Failed);
  assert(scan.functionalComplete(false));
  assert(scan.total == scan.processed + scan.failed);
  assert(scan.processed == 1 && scan.failed == 1);
  expectUnavailable(failed.finish(), Status::Incomplete);

  // E: changed list/periodic restart invalidates even same-SID/CID replies.
  scan.begin(1, true);
  assert(scan.issue());
  const uint32_t stale = scan.generation;
  scan.begin(1, true);
  assert(!scan.commit(stale, true));
  assert(scan.issue());
  assert(!scan.commit(stale, false));
  assert(scan.active && scan.processed == 0 && scan.failed == 0);
  assert(scan.commit(scan.generation, true));
  assert(scan.functionalComplete(false));
  scan.begin(0, false);
  assert(!scan.functionalComplete(false) && !scan.issue());
  scan.begin(0, true);
  assert(scan.functionalComplete(false));
  scan.generation = 0xFFFFFFFFUL;
  scan.begin(1, true);
  assert(scan.generation == 1);
  assert(sizeof(dab_spi::Scan) <= 20U);
}

static void testMultipleComponentsAndFinalSpi() {
  // B/C: exercise the firmware CID retention helper, then asynchronous scan
  // of all three components; only the last reply advertises SPI.
  const uint8_t raw[] = {1, 0, 0xFE, 1, 2, 0, 0xFE, 2, 3, 0, 0xFE, 3};
  uint32_t cids[3] = {};
  bool changed = false;
  assert(dab_spi::retainComponents(cids, 3, 0, raw, sizeof(raw), 3, changed));
  assert(changed);
  assert(cids[0] == 0x01FE0001 && cids[1] == 0x02FE0002 && cids[2] == 0x03FE0003);
  changed = false;
  assert(dab_spi::retainComponents(cids, 3, 0, raw, sizeof(raw), 3, changed));
  assert(!changed);  // identical periodic topology must not restart a pass
  assert(!dab_spi::retainComponents(cids, 3, 1, raw, sizeof(raw), 3, changed));
  assert(!dab_spi::retainComponents(cids, 3, 0, raw, sizeof(raw) - 1, 3, changed));
  assert(!dab_spi::retainComponents(cids, 3, 4, raw, sizeof(raw), 0, changed));
  assert(!dab_spi::retainComponents(nullptr, 3, 0, raw, sizeof(raw), 3, changed));
  assert(!dab_spi::retainComponents(cids, 3, 0, nullptr, sizeof(raw), 3, changed));
  const uint8_t sls[] = {2, 0, 0, 0};
  const uint8_t spi[] = {7, 0, 0, 0};
  Identity identities[3] = {};
  dab_spi::Scan scan;
  scan.begin(3, true);
  for (unsigned i = 0; i < 3; ++i) {
    assert(scan.issue());
    Selector pending(0x1234);
    for (unsigned j = 0; j < 3; ++j) pending.consider(0x1234, cids[j], identities[j]);
    expectUnavailable(pending.finish(), Status::Pending);
    assert(!scan.functionalComplete(true));
    identities[i] = dab_spi::identify(i == 2 ? spi : sls, 4, 1);
    assert(scan.commit(scan.generation, true));
    assert(scan.functionalComplete(false) == (i == 2)); // no UART condition
  }
  assert(scan.functionalComplete(false) && scan.processed == 3);
  Selector selected(0x1234);
  for (unsigned j = 0; j < 3; ++j) selected.consider(0x1234, cids[j], identities[j]);
  assert(selected.finish().available());
  assert(selected.finish().componentId == cids[2]);

  // A maximum-capacity inventory retains/scans its final slot as well.
  uint32_t maximum[448] = {};
  changed = false;
  assert(dab_spi::retainComponents(maximum, 448, 445, raw, sizeof(raw), 3, changed));
  assert(maximum[447] == cids[2]);
  assert(!dab_spi::retainComponents(maximum, 448, 446, raw, sizeof(raw), 3, changed));
  scan.begin(448, true);
  for (unsigned j = 0; j < 448; ++j)
    assert(scan.issue() && scan.commit(scan.generation, true));
  assert(scan.functionalComplete(false) && scan.processed == 448);
}

static void testContinuousRefresh() {
  dab_spi::RefreshCycle cycle;
  assert(!cycle.ready(100000, true));
  // Repeat indefinitely rather than expiring a detection window after boot.
  uint32_t now = 100;
  for (unsigned pass = 0; pass < 100; ++pass) {
    cycle.completed(now);
    assert(!cycle.ready(now + cycle.INTERVAL_MS - 1, true));
    assert(!cycle.ready(now + cycle.INTERVAL_MS, false)); // switching/probe/no lock
    assert(cycle.ready(now + cycle.INTERVAL_MS, true));
    assert(cycle.ready(now + cycle.INTERVAL_MS + 1000, true)); // postponed, not lost
    cycle.cancel();
    assert(!cycle.ready(now + cycle.INTERVAL_MS, true));
    now += cycle.INTERVAL_MS + 10000; // asynchronous pass duration
  }
  cycle.completed(0xFFFFFF00UL);
  assert(!cycle.ready(0xFFFFFF10UL, true));
  assert(!cycle.ready(cycle.deadline - 1, true));
  assert(cycle.ready(cycle.deadline, true));
  cycle.cancel(); // retune/reset invalidates the old deadline
  assert(!cycle.ready(cycle.deadline, true));
}

int main() {
  testIdentification();
  testAvailability();
  testPriorityAndOrdering();
  testServiceChangeAndFullInventory();
  testAsyncScanCompletion();
  testMultipleComponentsAndFinalSpi();
  testContinuousRefresh();
  puts("DAB SPI discovery tests passed");
}
