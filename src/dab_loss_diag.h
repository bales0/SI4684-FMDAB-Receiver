#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef SI4684_DIAG_DETAIL
#define SI4684_DIAG_DETAIL 0
#endif

// Observations only. No receiver state, allocation, SPI or UART in this file.
namespace dab_loss {
inline void inc(uint32_t& n) { if(n!=UINT32_MAX) ++n; }
inline uint32_t delta(uint32_t n,uint32_t before) { return n>=before?n-before:0; }
enum Event : uint8_t {
 Empty, Nonempty, Source0, Source1, Source2, Source3,
 Ignored, Audio, Sls, Spi, PadSls, Physical, State, Stale,
 ReadOk, ReadFail, LengthMismatch, Oversize, Unsupported,
 HeaderValid, Candidate, CrcOk, CrcBad, Stored, Duplicate, Conflict,
 RejectTid, Hold, CompletedRepeat, Invalid, PacketEmptyDir, PacketOther,
 ParseSkipped, Identical, Overflow, VerifyStart, VerifyEmpty, LossLogDrop,
 ReadTimeout, ReadTransport, ReadDevice, ReadOther, HeaderIdentity, HeaderCrcBad, IrqObserved,
 EventCount
};
inline const char* eventName(unsigned i) {
 static const char* const names[]={"empty","nonempty","src0","src1","src2","src3",
 "ignored","audio","sls","spi","padSls","physical","state","stale",
 "readOk","readFail","lengthMismatch","oversize","unsupported",
 "headerValid","candidate","crcOk","crcBad","stored","duplicate","conflict",
 "rejectTid","hold","completedRepeat","invalid","packetEmptyDir","packetOther",
 "parseSkipped","identical","overflow","verifyStart","verifyEmpty","lossLogDrop",
 "readTimeout","readTransport","readDevice","readOther","headerIdentity","headerCrcBad","irqObserved"};
 return names[i];
}
// Order mirrors DAB::DabCommand, checked at the production call site.
static constexpr unsigned commands=22;
inline const char* commandName(unsigned i) {
 static const char* const names[]={"other","tune","tuneStatus","dsrv","event","signal",
 "serviceList","ensemble","local","utc","confirm","serviceType","audioInfo",
 "subchannel","serviceInfo","UA","packetStart","packetStop","slsStop","audioStop",
 "audioStart","slsStart"};return names[i];
}
enum Action : uint8_t { Start, Ok, Timeout, Transport, Device, Other, StartFail, Busy, Poll, Irq, Actions };
// Normal builds retain individual DSRV/signal/UA rows and aggregate every
// other opcode. The service build retains all 22 rows, with identical counters.
static constexpr unsigned commandSlots=SI4684_DIAG_DETAIL?commands:4;
inline unsigned commandAt(unsigned slot) {
  if(SI4684_DIAG_DETAIL)return slot;
  static const uint8_t selected[]={0,3,5,15};return selected[slot];
}
struct CommandCounters {
 uint32_t values[commandSlots][Actions]={};
 static unsigned slot(unsigned command) {
  if(SI4684_DIAG_DETAIL)return command<commands?command:0;
  return command==3?1:command==5?2:command==15?3:0;
 }
 uint32_t* operator[](unsigned command){return values[slot(command)];}
 const uint32_t* operator[](unsigned command)const{return values[slot(command)];}
};
struct Bank {
 CommandCounters cmd;uint32_t event[EventCount]={},remaining[4]={};
 uint32_t queueMax=0,waitMaxMs=0,drainMaxMs=0;
};
enum class Mode:uint8_t { Normal, NoPacket, NoParse, Quiet };
inline const char* modeName(Mode m) {return m==Mode::NoPacket?"B":m==Mode::NoParse?"C":m==Mode::Quiet?"D":"A";}
struct Loss {
 uint32_t tid=0,receivedBytes=0,expectedBytes=0,ageMs=0,idleMs=0;
 uint32_t overflow=0,physical=0,readFail=0,mismatch=0,crcBad=0,rejected=0;
 uint32_t queueMax=0,drainMaxMs=0,padHeaders=0,fullOk=0,epochStart=0,epochEnd=0,contextStart=0,contextEnd=0;
 uint16_t expected=0,received=0,missing=0,firstMissing=256;
 uint8_t mask[32]={};const char* reason="OTHER";bool last=false,credibleInternal=false;
};
struct Snapshot {uint32_t overflow=0,physical=0,readFail=0,mismatch=0,crcBad=0,rejected=0,padHeaders=0,fullOk=0;};
struct Evidence {
 Bank total,window,report; // report is frozen while paced lines are sent
 Mode mode=Mode::Normal,reportMode=Mode::Normal;bool experiment=false,active=false,reportMixed=false,reportRequested=false;
 uint32_t windowEpoch=0,reportEpoch=0,objectEpoch=0,contextEpoch=0,objectContext=0;
 uint8_t activeCommand=0,stage=255;uint32_t reportAt=0,reportMs=0,logAt=0;
 uint32_t pendingAt=0;bool pendingKnown=false,verifyInFlight=false;
 uint32_t objectAt=0,objectQueueMax=0,objectDrainMax=0;Snapshot objectStart,transactionStart;
 Loss losses[2];uint8_t lossHead=0,lossCount=0;
 uint32_t previousHash=0,previousSid=0,previousCid=0,previousGeneration=0,previousTransaction=0;
 uint16_t previousLength=0;bool hashKnown=false;
 uint32_t transaction=0,fullReads=0;int lastReadResult=0;
 uint32_t modeEpoch=0,lastRemaining=0;
 void contextChanged(){inc(contextEpoch);}
 void event(Event e){inc(total.event[e]);inc(window.event[e]);}
 void action(unsigned c,Action a){if(c>=commands)c=0;inc(total.cmd[c][a]);inc(window.cmd[c][a]);}
 void started(unsigned c){active=true;activeCommand=uint8_t(c);action(c,Start);if(c==3)inc(transaction);}
 void terminal(unsigned c,int result){active=false;action(c,result==0?Ok:result==-4?Timeout:result==-6?Device:result==-5 || result==-2?Transport:Other);}
 void cts(bool irq){action(active?activeCommand:0,irq?Irq:Poll);}
 void maximum(uint32_t Bank::*field,uint32_t value){if(value>total.*field)total.*field=value;if(value>window.*field)window.*field=value;}
 void pending(uint32_t now){if(!pendingKnown){pendingAt=now;pendingKnown=true;}}
 void drainStart(uint32_t now,bool verification){verifyInFlight=verification;transactionStart=snapshot();if(pendingKnown)maximum(&Bank::waitMaxMs,now-pendingAt);pendingKnown=false;if(verification)event(VerifyStart);}
 void header(unsigned source,unsigned route,unsigned bytes,unsigned buffers,bool physical,bool state){
  event(bytes?Nonempty:Empty);event(Event(Source0+(source&3)));event(Event(Ignored+(route&3)));
  if(physical)event(Physical);
  if(!state)event(State);
  lastRemaining=buffers;
  const unsigned bin=buffers==0?0:buffers==1?1:buffers<=3?2:3;
  inc(total.remaining[bin]);inc(window.remaining[bin]);maximum(&Bank::queueMax,buffers);
  if(buffers>objectQueueMax)objectQueueMax=buffers;
 }
 Snapshot snapshot()const {
  Snapshot s;s.padHeaders=total.event[PadSls];s.fullOk=total.event[ReadOk];s.overflow=total.event[Overflow];s.physical=total.event[Physical];s.readFail=total.event[ReadFail];s.mismatch=total.event[LengthMismatch];s.crcBad=total.event[CrcBad];
  for(unsigned e=0;e<EventCount;++e) {
   if(e==RejectTid || e==Hold || e==Invalid || e==Stale || e==Oversize || e==Unsupported) {
    const uint32_t n=total.event[e];s.rejected=UINT32_MAX-s.rejected<n?UINT32_MAX:s.rejected+n;
   }
  }
  return s;
 }
 void begin(uint32_t now){objectEpoch=modeEpoch;objectContext=contextEpoch;objectAt=now;objectStart=transactionStart;objectQueueMax=lastRemaining;objectDrainMax=0;}
 void close(uint32_t now,uint16_t tid,const uint8_t* bitmap,unsigned highest,bool last,unsigned lastSegment,uint32_t bytes,uint32_t expectedBytes,uint32_t activity,const char* reason){
  if(mode==Mode::Quiet)return; // Quiet mode does not construct bitmap reports.
  if(lossCount==2){event(LossLogDrop);return;}
  Loss& l=losses[(lossHead+lossCount)%2];l=Loss{};l.tid=tid;l.reason=reason;l.last=last;l.epochStart=objectEpoch;l.epochEnd=modeEpoch;l.contextStart=objectContext;l.contextEnd=contextEpoch;
  l.expected=uint16_t(last?lastSegment+1:bytes?highest+1:0); // no invented tail when LAST absent
  l.receivedBytes=bytes;l.expectedBytes=expectedBytes;l.ageMs=now-objectAt;l.idleMs=now-activity;
  for(unsigned i=0;i<l.expected && i<256;++i){if(bitmap[i/8]&(1U<<(i%8)))++l.received;else{++l.missing;l.mask[i/8]|=uint8_t(1U<<(i%8));if(l.firstMissing==256)l.firstMissing=i;}}
  const bool interrupted=!strcmp(reason,"SWITCH") || !strcmp(reason,"TUNE") || !strcmp(reason,"RECOVERY") || !strcmp(reason,"RESET");
  l.credibleInternal=last && (bitmap[0]&1) && l.missing && l.firstMissing>0 && l.contextStart==l.contextEnd && l.epochStart==l.epochEnd && !interrupted;
  const Snapshot s=snapshot();l.padHeaders=delta(s.padHeaders,objectStart.padHeaders);l.fullOk=delta(s.fullOk,objectStart.fullOk);l.overflow=delta(s.overflow,objectStart.overflow);l.physical=delta(s.physical,objectStart.physical);l.readFail=delta(s.readFail,objectStart.readFail);l.mismatch=delta(s.mismatch,objectStart.mismatch);l.crcBad=delta(s.crcBad,objectStart.crcBad);l.rejected=delta(s.rejected,objectStart.rejected);l.queueMax=objectQueueMax;l.drainMaxMs=objectDrainMax;++lossCount;
 }
 // Equality is content evidence, never a claim of the same physical RF message.
 void payload(uint32_t sid,uint32_t cid,uint32_t generation,const uint8_t* p,unsigned n){
  if(mode==Mode::Quiet)return; // no per-packet content hash in the quiet experiment
  uint32_t hash=2166136261UL;for(unsigned i=0;i<n;++i)hash=(hash^p[i])*16777619UL;
  if(hashKnown && previousHash==hash && previousSid==sid && previousCid==cid && previousGeneration==generation && previousLength==n && previousTransaction!=transaction)event(Identical);
  previousHash=hash;previousSid=sid;previousCid=cid;previousGeneration=generation;previousLength=uint16_t(n);previousTransaction=transaction;hashKnown=true;
 }
};
inline Evidence& evidence(){static Evidence e;return e;}
} // namespace dab_loss
