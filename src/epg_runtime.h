#pragma once
#include "epg_core.h"
#include "epg_transport_diag.h"
namespace epg {
Core* runtime();
bool ensure(bool reportFailure=false);
bool replayActive();
bool indicatorPresent();
Availability availability();
ResolvedClock clockStatus();
enum class IndicatorState : uint8_t { Absent, Loading, Unavailable, Ready };
constexpr uint32_t kIndicatorWaitMs=120000U; // UI only; never expires MOT/cache.
IndicatorState indicatorState();
bool liveEnabled();
void liveReset();
void liveServiceChanged();
void livePadRevoked();
void livePadData(uint32_t sid,uint32_t cid,const uint8_t* data,size_t n);
void liveContext(uint32_t sid, uint16_t eid, uint8_t ecc, uint32_t utc);
void liveEndpoint(uint32_t sid, uint32_t cid);
void livePadEndpoint(uint32_t sid, uint32_t cid); // zero releases PAD ownership
void livePayload(const uint8_t* data, size_t n);
void livePadPayload(const uint8_t* data, size_t n);
void poll();
bool command(char* input);
void stopReplay();
}
