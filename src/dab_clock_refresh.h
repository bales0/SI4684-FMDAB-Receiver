#pragma once
#include "epg_time.h"

namespace epg {
enum class ClockCandidateState : uint8_t { Idle, AwaitLocal, AwaitUtc, AwaitConfirm, Failed };
enum class ClockReject : uint8_t { None, Parse, Order, PairWindow, OffsetMismatch, Jump, Timeout, LockLoss, TuneChanged };
inline const char* clockStateName(ClockCandidateState s) {
  const char* const names[]={"idle","local","utc","confirm","failed"};return names[unsigned(s)];
}
inline const char* clockRejectName(ClockReject r) {
  const char* const names[]={"none","parse","order","pair-window","offset-mismatch","jump","timeout","lock-loss","tune-changed"};return names[unsigned(r)];
}
// One small transaction; published belongs exclusively to fully accepted cycles.
// Command scheduling and every deadline use unsigned monotonic differences.
struct ClockRefresh {
  BroadcastClockSample candidate, jump;
  uint32_t context=0, started=0, retryAt=0, jumpAt=0;
  uint32_t attempt=0, ok=0, reject=0, timeout=0;
  ClockCandidateState state=ClockCandidateState::Idle;
  ClockReject reason=ClockReject::None;
  uint8_t failures=0;
  bool initialized=false, locked=false, retryPending=false, jumpPending=false;

  bool active() const {return state==ClockCandidateState::AwaitLocal || state==ClockCandidateState::AwaitUtc || state==ClockCandidateState::AwaitConfirm;}
  bool ready(uint32_t now) const {return !active() && (!retryPending || int32_t(now-retryAt)>=0);}
  void fail(ClockReject why,uint32_t now) {
    state=ClockCandidateState::Failed;reason=why;++reject;
    if(why==ClockReject::Timeout)++timeout;
    if(failures<4)++failures;
    const uint32_t delayMs=failures<4 ? (5000U << (failures-1)) : 30000U;
    retryAt=now+delayMs;retryPending=true;candidate=BroadcastClockSample{};
  }
  // Returns true when a new context/lock demands a fresh cycle. Never clears EPG.
  bool observe(uint32_t generation,bool hasLock,uint32_t now,BroadcastClockSample& published) {
    bool fresh=false;
    if(!initialized || generation!=context) {
      initialized=true;context=generation;published.valid=false;candidate=BroadcastClockSample{};
      state=ClockCandidateState::Idle;reason=ClockReject::TuneChanged;
      jumpPending=false;retryPending=false;failures=0;fresh=true;
    }
    if(locked && !hasLock) {
      fail(ClockReject::LockLoss,now);jumpPending=false;
    } else if(!locked && hasLock) {
      state=ClockCandidateState::Idle;retryPending=false;fresh=true;
    }
    locked=hasLock;
    if(active() && uint32_t(now-started)>10000U)fail(ClockReject::Timeout,now);
    return fresh && hasLock;
  }
  void invalidate(uint32_t now,BroadcastClockSample& published) {
    published.valid=false;candidate=BroadcastClockSample{};jumpPending=false;
    state=ClockCandidateState::Idle;reason=ClockReject::TuneChanged;
    locked=false;retryPending=false;failures=0;started=now;
  }
  void begin(uint32_t generation,uint32_t now) {
    context=generation;initialized=true;started=now;candidate=BroadcastClockSample{};
    state=ClockCandidateState::AwaitLocal;retryPending=false;++attempt;
  }
  bool acceptLocal(const BroadcastClockSample& sample,uint32_t generation,uint32_t now,bool confirm,BroadcastClockSample& published) {
    if(generation!=context || !locked || state!=(confirm?ClockCandidateState::AwaitConfirm:ClockCandidateState::AwaitLocal)) {
      fail(generation!=context?ClockReject::TuneChanged:ClockReject::Order,now);return false;
    }
    if(!sample.calendarEpoch){fail(ClockReject::Parse,now);return false;}
    if(!confirm){state=ClockCandidateState::AwaitUtc;return true;}
    if(uint32_t(now-candidate.sampledAtMonotonicMs)>2000U){fail(ClockReject::PairWindow,now);return false;}
    int16_t offset=0;
    if(!pairedOffsetAfterUtc(candidate.calendarEpoch,candidate.sampledAtMonotonicMs,sample.calendarEpoch,now,offset)) {
      fail(ClockReject::OffsetMismatch,now);return false;
    }
    candidate.offsetMinutes=offset;candidate.offsetKnown=candidate.valid=true;
    // Five seconds covers whole-second GET_TIME quantization and scheduling.
    // A larger UTC correction needs a second complete cycle agreeing to 3 s,
    // separated by the retry delay, within 120 s. DST changes only the offset.
    const auto old=resolveTime(published,now,true);
    const auto proposed=resolveTime(candidate,now,true);
    const int64_t delta=int64_t(proposed.utc)-old.utc;
    if(old.utcValid && (delta < -5 || delta > 5)) {
      const auto corroborated=resolveTime(jump,now,true);
      const int64_t difference=int64_t(proposed.utc)-corroborated.utc;
      if(!jumpPending || uint32_t(now-jumpAt)>120000U || uint32_t(now-jumpAt)<1000U || !corroborated.utcValid || difference < -3 || difference > 3) {
        jump=candidate;jumpAt=now;jumpPending=true;fail(ClockReject::Jump,now);return false;
      }
    }
    candidate.generation=published.generation+1U;if(!candidate.generation)candidate.generation=1;
    published=candidate;++ok;state=ClockCandidateState::Idle;reason=ClockReject::None;
    retryPending=jumpPending=false;failures=0;return true;
  }
  bool acceptUtc(const BroadcastClockSample& sample,uint32_t generation,uint32_t now) {
    if(generation!=context || !locked || state!=ClockCandidateState::AwaitUtc) {
      fail(generation!=context?ClockReject::TuneChanged:ClockReject::Order,now);return false;
    }
    if(!sample.calendarEpoch){fail(ClockReject::Parse,now);return false;}
    candidate=sample;candidate.encoding=ClockEncoding::UTC;candidate.sampledAtMonotonicMs=now;
    state=ClockCandidateState::AwaitConfirm;return true;
  }
};
} // namespace epg
