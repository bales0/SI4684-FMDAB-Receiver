#pragma once
// Platform-neutral clock contract. No timezone globals or PI-derived live offset.
#include <stdint.h>
namespace epg {
enum class ClockEncoding : uint8_t { Unknown, Local, UTC };
struct BroadcastClockSample {
  uint16_t year=0; uint8_t month=0, day=0, hour=0, minute=0, second=0;
  ClockEncoding encoding=ClockEncoding::Unknown;
  uint32_t calendarEpoch=0, sampledAtMonotonicMs=0, generation=0;
  int16_t offsetMinutes=0;
  bool offsetKnown=false, valid=false;
};
struct ResolvedClock { uint32_t utc=0, local=0; int16_t offset=0; bool utcValid=false, localValid=false, offsetKnown=false; };
inline ResolvedClock resolveTime(const BroadcastClockSample& s,uint32_t now,bool locked) {
  ResolvedClock r;
  const uint32_t age=now-s.sampledAtMonotonicMs;
  if(!s.valid || !locked || age>=300000U || !s.calendarEpoch) return r;
  const int64_t value=int64_t(s.calendarEpoch)+age/1000;
  if(value<=0 || value>UINT32_MAX) return r;
  r.offset=s.offsetMinutes; r.offsetKnown=s.offsetKnown;
  if(s.encoding==ClockEncoding::UTC) {
    r.utc=uint32_t(value);r.utcValid=true;
    const int64_t local=value+int32_t(r.offset)*60;
    if(s.offsetKnown && local>0 && local<=UINT32_MAX) {r.local=uint32_t(local);r.localValid=true;}
  } else if(s.encoding==ClockEncoding::Local) {
    r.local=uint32_t(value);r.localValid=true;
    const int64_t utc=value-int32_t(r.offset)*60;
    if(s.offsetKnown && utc>0 && utc<=UINT32_MAX) {r.utc=uint32_t(utc);r.utcValid=true;}
  }
  return r;
}
// Two serialized GET_TIME replies: compare at the UTC sample's monotonic time.
// Reject invalid/stale pairs, including pairs straddling a DST transition.
inline bool pairedOffset(uint32_t local,uint32_t localMs,uint32_t utc,uint32_t utcMs,int16_t& offset) {
  const uint32_t elapsed=utcMs-localMs;
  if(!local || !utc || elapsed>2000) return false;
  const int64_t seconds=int64_t(local)+elapsed/1000-int64_t(utc);
  const int64_t halfHours=seconds>=0?(seconds+900)/1800:(seconds-900)/1800;
  const int64_t residual=seconds-halfHours*1800;
  if(halfHours < -28 || halfHours>28 || residual < -2 || residual>2) return false;
  offset=int16_t(halfHours*30);return true;
}
// Prefer the LOCAL reply after UTC, so an offset change between the replies
// belongs to the later local sample, including the repeated autumn hour.
inline bool pairedOffsetAfterUtc(uint32_t utc,uint32_t utcMs,uint32_t local,uint32_t localMs,int16_t& offset) {
  const uint32_t elapsed=localMs-utcMs;
  if(elapsed>2000 || uint64_t(utc)+elapsed/1000>UINT32_MAX) return false;
  return pairedOffset(local,localMs,utc+elapsed/1000,localMs,offset);
}
} // namespace epg
