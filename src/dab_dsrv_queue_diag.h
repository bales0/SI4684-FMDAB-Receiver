#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef SI4684_DSRV_QUEUE_DIAG
#define SI4684_DSRV_QUEUE_DIAG 0
#endif

// Raw command evidence only: no chip, allocator, Arduino or output dependency.
namespace dsrv_queue {
constexpr unsigned capacity=12;
template<class T> inline void inc(T& value){if(value!=T(~T(0)))++value;}
inline uint32_t load32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
inline void store32(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
struct Sample {
 uint8_t raw[24]={},timeBytes[4]={},generationBytes[4]={},mode=0;
 uint32_t atMs()const{return load32(timeBytes);}
 uint32_t generation()const{return load32(generationBytes);}
 void set(const uint8_t* p,uint32_t at,uint32_t gen,uint8_t m){memcpy(raw,p,24);store32(timeBytes,at);store32(generationBytes,gen);mode=m;}
};
static_assert(sizeof(Sample)==33,"Byte metadata must not require unaligned word accesses");
enum Metric:uint8_t {
 WithData,Empty,HeaderError,ShortHeader,StaleHeader,CmdTimeout,CmdTransport,CmdDevice,CmdOther,
 Over8,Over8NoOverflow,Ready,Overflow,Physical,State2,State1,State4,StateOther,
 State2NoOverflow,Over8Overflow,Over8State2,NoCts,ErrCmd,Status3Error,SidCidChanged,ByteCountChanged,MetricCount
};
struct Stats {
 uint32_t headers=0;
 uint16_t value[MetricCount]={},hist[8]={};
 uint8_t qmax=0,sourceMax[2]={};
 bool atLimit()const {
  if(headers==UINT32_MAX)return true;
  for(auto v:value)if(v==UINT16_MAX)return true;
  for(auto v:hist)if(v==UINT16_MAX)return true;
  return false;
 }
};
inline unsigned bin(uint8_t q){return q==0?0:q==1?1:q<4?2:q<8?3:q==8?4:q<16?5:q<32?6:7;}
struct Evidence {
 Sample ring[capacity],firstOver8;
 Stats total,window,frozenStats;
 uint32_t lastOver8=0,lastState2=0,lastOverflow=0,bootFirstOver8=0;
 uint32_t over8Age=0,state2Age=0,overflowAge=0;
 uint32_t freezeAt=0,freezeGeneration=0,windowAt=0,attemptAt=0,windowGeneration=0;
 uint32_t cachedFree8=0;
 uint16_t suppressed=0,reportDrops=0,captureId=0;
 uint8_t head=0,count=0,cursor=255,freezeMode=0,freezeOp=0,freezeCommand=0,windowMode=0;
 bool firstKnown=false,frozen=false,anomalyPending=false,windowKnown=false,mixed=false;

 void note(uint32_t gen,uint8_t mode){
  if(!windowKnown){windowKnown=true;windowGeneration=gen;windowMode=mode;}
  else if(windowGeneration!=gen || windowMode!=mode)mixed=true;
 }
 void metric(Metric m){inc(total.value[m]);inc(window.value[m]);}
 // Only the actual DSRV command error path calls this. Never inspect its buffer.
 void commandError(int result,uint32_t gen,uint8_t mode){
  note(gen,mode);
  metric(result==-4?CmdTimeout:result==-6?CmdDevice:result==-5 || result==-2?CmdTransport:CmdOther);
 }
 bool header(const uint8_t* raw,size_t n,bool commandOk,uint32_t gen,uint32_t expected,uint8_t mode,uint32_t now){
  if(!commandOk)return false;
  if(gen!=expected){metric(StaleHeader);return false;}
  note(gen,mode);
  if(!raw || n<24){metric(HeaderError);metric(ShortHeader);return false;}
  inc(total.headers);inc(window.headers);
  metric((raw[18]|raw[19])?WithData:Empty);
  const uint8_t q=raw[5],state=raw[6],source=raw[7]>>6;
  const bool overflow=(raw[4]&2)!=0;
  inc(total.hist[bin(q)]);inc(window.hist[bin(q)]);
  if(q>total.qmax)total.qmax=q;
  if(q>window.qmax)window.qmax=q;
  if(source<2){if(q>total.sourceMax[source])total.sourceMax[source]=q;if(q>window.sourceMax[source])window.sourceMax[source]=q;}
  if(raw[4]&1)metric(Ready);
  if(overflow){metric(Overflow);lastOverflow=now;}
  if(raw[4]&4)metric(Physical);
  if(state==2){metric(State2);lastState2=now;if(!overflow)metric(State2NoOverflow);}
  else if(state==1)metric(State1);
  else if(state==4)metric(State4);
  else if(state!=0 && state!=3)metric(StateOther);
  if(!(raw[0]&128))metric(NoCts);
  if(raw[0]&64)metric(ErrCmd);
  if(raw[3]&31)metric(Status3Error);
  if(q>8){if(!total.value[Over8])bootFirstOver8=now;metric(Over8);lastOver8=now;if(!overflow)metric(Over8NoOverflow);if(overflow)metric(Over8Overflow);if(state==2)metric(Over8State2);}
  if(!frozen){
   auto& sample=ring[head];sample.set(raw,now,gen,mode);
   if(q>8 && !firstKnown){firstOver8=sample;firstKnown=anomalyPending=true;}
   head=uint8_t((head+1)%capacity);if(count<capacity)++count;
  }
  return true;
 }
 // No large ring copy. The immutable sample bank is retired until manual CLEAR.
 bool freeze(uint32_t now,uint32_t gen,uint8_t mode,uint8_t op,uint8_t command){
  if(frozen)return false;
  frozen=true;freezeAt=now;freezeGeneration=gen;freezeMode=mode;freezeOp=op;freezeCommand=command;
  frozenStats=total;
  over8Age=now-lastOver8;state2Age=now-lastState2;overflowAge=now-lastOverflow;
  cursor=0;return true;
 }
 const Sample& oldest(unsigned index)const{return ring[(head+capacity-count+index)%capacity];}
 uint32_t sequence(unsigned index)const{return frozenStats.headers-count+1+index;}
 void request(){cursor=frozen?0:firstKnown?uint8_t(4+capacity):uint8_t(5+capacity);}
 void clear(uint32_t now){
  head=count=0;cursor=255;firstKnown=frozen=anomalyPending=false;
  freezeAt=freezeGeneration=over8Age=state2Age=overflowAge=0;
  attemptAt=now;inc(captureId); // Boot totals and 30 s window are intentionally retained.
 }
};
static_assert(sizeof(Evidence)<=768,"RAW queue evidence INCLUDING reporter state exceeds DRAM budget");
} // namespace dsrv_queue
