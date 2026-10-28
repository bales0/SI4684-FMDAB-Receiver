#pragma once
#include <stdint.h>

namespace dab_health {
// Only cached observations. Neither capture nor replay can contact the chip.
struct CtsSnapshot {
 uint32_t atMs=0,elapsedMs=0,latenessUs=0,gapUs=0,statusAgeMs=0;
 uint32_t generation=0,epoch=0,resets=0,intervalMs=0,queueMax=0;
 uint32_t overflow=0,overflowDelta=0,packet=0,pad=0;
 uint32_t free8=0,largest8=0,free32=0,largest32=0,stackMinBytes=0;
 uint8_t status[4]={},sticky=0,operation=0,command=0,args[4]={},argCount=0;
 uint8_t epgState=0,recoveryReason=0;
 bool statusKnown=false,core=false;
};
struct CtsEvidence {
 CtsSnapshot first;
 uint32_t statusAt=0;
 uint8_t last[4]={},sticky=0;
 bool known=false,armed=true,valid=false,pending=false,recoveryPending=false;
 void status(uint8_t s0,uint8_t s1,uint8_t s2,uint8_t s3,uint32_t now) {
  last[0]=s0;last[1]=s1;last[2]=s2;last[3]=s3;sticky|=s3&0x1f;
  known=true;statusAt=now;
 }
 void context() {known=false;sticky=0;armed=true;for(auto& b:last)b=0;}
 bool needsCapture(bool timedOut,bool answered) {
  if(answered)armed=true;
  if(!timedOut || !armed)return false;
  armed=false;return true;
 }
 void freeze(uint32_t now) {
  first.atMs=now;first.statusAgeMs=known?uint32_t(now-statusAt):0;
  for(unsigned i=0;i<4;++i)first.status[i]=last[i];
  first.sticky=sticky;first.statusKnown=known;
  valid=pending=true;
 }
 void recovery(uint8_t reason) {if(valid){first.recoveryReason=reason;recoveryPending=true;}}
 void request() {if(valid)pending=true;}
};
static_assert(sizeof(CtsEvidence)<=128,"CTS snapshot must remain small");
} // namespace dab_health
