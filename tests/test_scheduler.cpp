#include "../src/dab_scheduler_policy.h"

#include <cassert>
#include <cstdint>

int main() {
  assert(dab_scheduler::nextGeneration(1U) == 2U);
  assert(dab_scheduler::nextGeneration(0xFFFFFFFFU) == 1U);
  assert(dab_scheduler::generationMatches(7U, 7U));
  assert(!dab_scheduler::generationMatches(6U, 7U));

  uint8_t retryCount = 0;
  uint32_t notBefore = 0;
  assert(dab_scheduler::retryReady(100U, notBefore));
  assert(dab_scheduler::scheduleRetry(retryCount, notBefore,
                                      1000U, 3U, 250U));
  assert(retryCount == 1U && notBefore == 1250U);
  assert(!dab_scheduler::retryReady(1249U, notBefore));
  assert(dab_scheduler::retryReady(1250U, notBefore));
  assert(dab_scheduler::scheduleRetry(retryCount, notBefore,
                                      1250U, 3U, 250U));
  assert(retryCount == 2U && notBefore == 1750U);
  assert(dab_scheduler::scheduleRetry(retryCount, notBefore,
                                      1750U, 3U, 250U));
  assert(retryCount == 3U && notBefore == 2500U);
  assert(!dab_scheduler::scheduleRetry(retryCount, notBefore,
                                       2500U, 3U, 250U));

  // Deadline comparison is wrap-safe for intervals below 2^31 ms.
  notBefore = 0x00000020U;
  assert(!dab_scheduler::retryReady(0xFFFFFFF0U, notBefore));
  assert(dab_scheduler::retryReady(0x00000020U, notBefore));

  dab_scheduler::resetRetry(retryCount, notBefore);
  assert(retryCount == 0U && notBefore == 0U);
  return 0;
}
