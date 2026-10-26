#pragma once
#include <stdint.h>
#include "dab_loss_diag.h"
namespace dab_health {
inline void count(uint32_t& value){if(value!=UINT32_MAX)++value;}
enum class Stall : uint8_t { None, CtsGenuine, CtsNoProgress, Transport };
inline const char* name(Stall reason){switch(reason){case Stall::CtsGenuine:return "CTS_GENUINE";case Stall::CtsNoProgress:return "CTS_NO_PROGRESS";case Stall::Transport:return "SPI_TRANSPORT_ERROR";default:return "NONE";}}
struct Watchdog {
 static constexpr uint32_t failureWindowMs=5000;
 static constexpr unsigned failureLimit=8;
 uint32_t failures=0,firstFailure=0,lastSuccess=0,inFlightSince=0;
 uint16_t operation=0;
 bool successKnown=false,inFlight=false;
 void started(uint32_t now,uint16_t op){inFlight=true;inFlightSince=now;operation=op;}
 Stall terminal(uint32_t now,bool answered,bool transportError){
  inFlight=false;
  if(answered){failures=0;firstFailure=0;lastSuccess=now;successKnown=true;return Stall::None;}
  if(!failures)firstFailure=now;
  count(failures);
  return failures>=failureLimit && uint32_t(now-firstFailure)>=failureWindowMs
    ?(transportError?Stall::Transport:Stall::CtsNoProgress):Stall::None;
 }
};
// Status callbacks can repeat a latched bit while reading one preserved reply.
// Track rising observations, and allow one bounded verification read if the
// acknowledged source remains high after a payload or a malformed reply.
struct Dsrv {
 uint32_t sequence=0,atStart=0,epoch=0,requestEpoch=0,startedAt=0,notBefore=0;
 uint32_t irqSets=0,cleared=0,remaining=0,newer=0,suspect=0,maxDrainMs=0,errors=0;
 bool level=false,pending=false,inFlight=false,verification=false,delayed=false;
 void reset(){++epoch;level=pending=inFlight=verification=delayed=false;notBefore=0;}
 void status(bool high){if(high && !level){++sequence;pending=true;verification=false;count(irqSets);}level=high;}
 bool ready(uint32_t now)const{return pending && (!delayed || int32_t(now-notBefore)>=0);}
 void start(uint32_t now){atStart=sequence;requestEpoch=epoch;inFlight=true;pending=false;startedAt=now;count(cleared);}
 bool finish(uint32_t now,unsigned buffers,unsigned bytes,bool valid){
  if(!inFlight || requestEpoch!=epoch)return pending;
  inFlight=false;const uint32_t elapsed=now-startedAt;if(elapsed>maxDrainMs)maxDrainMs=elapsed;
  const bool fresh=sequence!=atStart;
  if(!valid)count(errors);
  if(fresh){pending=true;verification=false;count(newer);}
  else if(valid && buffers){pending=true;verification=false;count(remaining);}
  else if(level && !verification && (bytes || !valid)){
   pending=true;verification=true;count(suspect);
  }else pending=false;
  // An empty verification terminates even if all reply snapshots stay high.
  delayed=!valid;notBefore=now+(valid?0U:20U);
  return pending;
 }
};
} // namespace dab_health
