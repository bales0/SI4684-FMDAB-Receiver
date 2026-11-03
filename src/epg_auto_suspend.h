#pragma once
#include <stdint.h>
#include "dab_spi_probe.h"

// Pure policy. Only the existing DAB scheduler executes the returned actions.
namespace epg_auto {
struct Config {
  uint32_t minSpan=2000, window=30000, interval=300000, observe=30000, backoff=300000;
  uint8_t repeats=3;
};
enum class State:uint8_t { Disabled, Ineligible, Active, StopPending, Cooldown, RecheckStart, RecheckActive, Error };
enum class Result:uint8_t { None, Empty, Nonempty, Inconclusive, StartError, StopError };
inline const char* name(State s) {
  static const char* const names[]={"DISABLED","INELIGIBLE","ACTIVE","STOP_PENDING","COOLDOWN","RECHECK_START","RECHECK_ACTIVE","ERROR_BACKOFF"};
  return names[unsigned(s)];
}
inline const char* name(Result r) {
  static const char* const names[]={"NONE","EMPTY","NONEMPTY","INCONCLUSIVE","START_ERROR","STOP_ERROR"};
  return names[unsigned(r)];
}
struct Context {
  uint32_t generation=0, request=0, audioSid=0, audioCid=0, sid=0, cid=0, slsSid=0, slsCid=0;
  uint8_t mode=0;
  bool sls=false;
  bool operator==(const Context& o)const {
    return generation==o.generation && request==o.request && audioSid==o.audioSid && audioCid==o.audioCid &&
      sid==o.sid && cid==o.cid && slsSid==o.slsSid && slsCid==o.slsCid && mode==o.mode && sls==o.sls;
  }
};
class Policy {
 public:
  Config config;
  Context context;
  State state=State::Ineligible;
  Result result=Result::None;
  bool enabled=true, eligible=false;
  uint32_t emptyValid=0, nonEmptyValid=0, first=0, last=0, hash=0, next=0;
  uint32_t startCount=0, stopCount=0, confirmedCount=0, stopError=0, recheckCount=0;
  uint16_t tid=0;
  uint8_t repeat=0;
  const char* reason="context";
  void reset() { eligible=false; clear(); state=enabled?State::Ineligible:State::Disabled; reason="context"; result=Result::None; }
  void enable(bool on) { if(enabled!=on){enabled=on;reset();reason=on?"enabled":"disabled";} }
  void sync(const Context& c,bool usable,bool pendingDiscovery=false) {
    // Routine UA scans temporarily withdraw discovery. A confirmed physical
    // stop remains valid: retain its deadline but forbid actions until verified.
    if(enabled && context==c && (state==State::Cooldown || state==State::Error) &&
       (usable || pendingDiscovery)){eligible=usable;return;}
    if(!(context==c) || eligible!=(enabled && usable)) {
      context=c;reset();eligible=enabled && usable;
      state=!enabled?State::Disabled:eligible?State::Active:State::Ineligible;
    }
  }
  void directory(uint32_t now,uint16_t id,uint32_t digest,uint16_t objects,uint32_t cycle) {
    if(!eligible || (state!=State::Active && state!=State::RecheckActive))return;
    if(objects){++nonEmptyValid;clear();result=Result::Nonempty;state=State::Active;reason="nonempty";return;}
    // One Core completion path only. Replayed identical last group cannot be
    // counted twice. Changing TID/content restarts confirmation conservatively.
    if(repeat && tid==id && hash==digest && cycle_==cycle)return;
    ++emptyValid;
    if(!repeat || tid!=id || hash!=digest || uint32_t(now-first)>config.window){repeat=0;first=now;}
    tid=id;hash=digest;cycle_=cycle;last=now;
    if(repeat!=255)++repeat;
    reason="empty-observed";
  }
  void content(uint32_t now) { directory(now,0,0,1,0); }
  bool stopDue(uint32_t now,bool observing) {
    if(!eligible || !observing)return false;
    const bool empty=repeat>=config.repeats && uint32_t(last-first)>=config.minSpan && uint32_t(now-first)<=config.window;
    const bool expired=state==State::RecheckActive && uint32_t(now-started_)>=config.observe;
    if((state==State::Active || state==State::RecheckActive) && (empty || expired)){
      result=empty?Result::Empty:Result::Inconclusive;state=State::StopPending;
      reason=empty?"empty-confirmed-stop-requested":"inconclusive-stop-requested";++stopCount;return true;
    }
    return false;
  }
  bool allowStart(uint32_t now) {
    if(state==State::Cooldown || state==State::Error) {
      if(!eligible || !dab_probe::due(now,next))return false;
      state=State::RecheckStart;reason="recheck-due";clear();
    }
    return state!=State::StopPending;
  }
  void started(uint32_t now,bool ok,bool uncertain) {
    if(!eligible)return;
    if(ok)++startCount;
    if(state!=State::RecheckStart && ok)return;
    if(ok){state=State::RecheckActive;started_=now;++recheckCount;reason="recheck-started";}
    else {result=Result::StartError;reason="start-error";
      state=uncertain?State::StopPending:State::Error;next=now+config.backoff;}
  }
  void stopped(uint32_t now,bool ok) {
    if(state!=State::StopPending)return;
    if(!ok){++stopError;reason="stop-unconfirmed";return;}
    ++confirmedCount;state=State::Cooldown;next=now+(result==Result::Empty?config.interval:config.backoff);
    reason="stop-confirmed";
  }
  uint32_t remaining(uint32_t now)const { return (state==State::Cooldown || state==State::Error) && !dab_probe::due(now,next)?next-now:0; }
 private:
  uint32_t cycle_=0,started_=0;
  void clear(){repeat=0;first=last=0;cycle_=0;}
};
inline Policy& policy(){static Policy p;return p;}
} // namespace epg_auto
