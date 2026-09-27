#pragma once

#include <stdint.h>

namespace dab_scheduler {

inline uint32_t nextGeneration(uint32_t current) {
  ++current;
  return current == 0U ? 1U : current;
}

inline bool generationMatches(uint32_t completed, uint32_t current) {
  return completed == current;
}

inline bool retryReady(uint32_t now, uint32_t notBefore) {
  return notBefore == 0U || static_cast<int32_t>(now - notBefore) >= 0;
}

// Returns true when another retry is allowed. The first failure schedules
// retry #1; after maxRetries have been scheduled the following failure is
// terminal. Unsigned time arithmetic remains valid across millis() wraparound.
inline bool scheduleRetry(uint8_t& retryCount, uint32_t& notBefore,
                          uint32_t now, uint8_t maxRetries,
                          uint32_t baseBackoffMs) {
  if (retryCount >= maxRetries) return false;
  ++retryCount;
  notBefore = now + baseBackoffMs * retryCount;
  return true;
}

inline void resetRetry(uint8_t& retryCount, uint32_t& notBefore) {
  retryCount = 0;
  notBefore = 0;
}

}  // namespace dab_scheduler
