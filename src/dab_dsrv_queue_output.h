#pragma once
#include "dab_dsrv_queue_diag.h"
#include <stdio.h>

namespace dsrv_queue {
inline char modeName(unsigned mode){return mode<4?char('A'+mode):'?';}
// One complete, bounded record. A full UART never consumes the evidence cursor.
template<class Output,class... Args> bool emit(Evidence& d,Output& out,const char* format,Args... args){
 char line[448];const int n=snprintf(line,sizeof line,format,args...);
 if(n<=0 || n>=int(sizeof line)){inc(d.reportDrops);return false;}
 if(int64_t(out.availableForWrite())<n)return false;
 if(out.write(reinterpret_cast<const uint8_t*>(line),size_t(n))!=size_t(n)){inc(d.reportDrops);return false;}
 return true;
}
template<class Output> bool stats(Evidence& d,Output& out,const Stats& s,const char* scope,unsigned part){
 const auto* v=s.value;
 if(part==0)return emit(d,out,"[DSRV/QUEUE-COUNTS] scope=%s headers=%u data=%u empty=%u qmaxRaw=%u packetMax=%u padMax=%u over8=%u hist=%u,%u,%u,%u,%u,%u,%u,%u sat=%u\n",
  scope,unsigned(s.headers),v[WithData],v[Empty],s.qmax,s.sourceMax[0],s.sourceMax[1],v[Over8],s.hist[0],s.hist[1],s.hist[2],s.hist[3],s.hist[4],s.hist[5],s.hist[6],s.hist[7],s.atLimit());
 if(part==1)return emit(d,out,"[DSRV/QUEUE-FLAGS] scope=%s ready=%u intsrcOvf=%u phy=%u state2=%u state1=%u state4=%u stateOther=%u over8NoOvf=%u state2NoOvf=%u over8Ovf=%u over8State2=%u sat=%u\n",
  scope,v[Ready],v[Overflow],v[Physical],v[State2],v[State1],v[State4],v[StateOther],v[Over8NoOverflow],v[State2NoOverflow],v[Over8Overflow],v[Over8State2],s.atLimit());
 return emit(d,out,"[DSRV/QUEUE-ERRORS] scope=%s headerErrors=%u short=%u stale=%u cmdTimeout=%u cmdTransport=%u cmdDevice=%u cmdOther=%u noCts=%u errCmd=%u status3=%u sidCidChanged=%u byteCountChanged=%u replyHeaderChanged=%u sat=%u\n",
  scope,v[HeaderError],v[ShortHeader],v[StaleHeader],v[CmdTimeout],v[CmdTransport],v[CmdDevice],v[CmdOther],v[NoCts],v[ErrCmd],v[Status3Error],v[SidCidChanged],v[ByteCountChanged],unsigned(v[SidCidChanged])+unsigned(v[ByteCountChanged]),s.atLimit());
}
template<class Output> bool sample(Evidence& d,Output& out,const Sample& s,const char* tag,uint32_t sequence){
 char hex[49];static const char digits[]="0123456789ABCDEF";
 for(unsigned i=0;i<24;++i){hex[2*i]=digits[s.raw[i]>>4];hex[2*i+1]=digits[s.raw[i]&15];}hex[48]=0;
 return emit(d,out,"[DSRV/QUEUE-%s] capture=%u seq=%u atMs=%u gen=%u mode=%c read=HEADER cts=%u errCmd=%u status3=%02X intsrc=%02X buffRaw=%u srvState=%u src=%u byteCount=%u SID=%08X CID=%08X raw24=%s\n",
  tag,d.captureId,unsigned(sequence),unsigned(s.atMs()),unsigned(s.generation()),modeName(s.mode),bool(s.raw[0]&128),bool(s.raw[0]&64),s.raw[3],s.raw[4],s.raw[5],s.raw[6],s.raw[7]>>6,unsigned(s.raw[18])|(unsigned(s.raw[19])<<8),unsigned(load32(s.raw+8)),unsigned(load32(s.raw+12)),hex);
}
template<class Output> void service(Evidence& d,Output& out,uint32_t now,uint32_t generation,uint8_t mode,
 bool ctsPending,bool core,uint32_t readFailBoot,uint32_t serialDrop,unsigned queued){
 if(ctsPending || uint32_t(now-d.attemptAt)<100U)return;
 if(d.cursor==255 && !d.anomalyPending && uint32_t(now-d.windowAt)<30000U)return;
 d.attemptAt=now;
 if(out.availableForWrite()<512){inc(d.suppressed);return;}
 if(d.cursor!=255){
  bool sent=false;
  if(d.cursor==0)sent=emit(d,out,"[DSRV/QUEUE-FROZEN] capture=%u atMs=%u reason=CTS_FIRST count=%u gen=%u mode=%c op=%u command=%02X firstOver8Known=%u firstOver8Ms=%u over8Known=%u over8AgeMs=%u ovfKnown=%u ovfAgeMs=%u state2Known=%u state2AgeMs=%u qmaxRaw=%u\n",
    d.captureId,unsigned(d.freezeAt),d.count,unsigned(d.freezeGeneration),modeName(d.freezeMode),d.freezeOp,d.freezeCommand,d.firstKnown,d.firstKnown?unsigned(d.firstOver8.atMs()):0,d.frozenStats.value[Over8]!=0,unsigned(d.over8Age),d.frozenStats.value[Overflow]!=0,unsigned(d.overflowAge),d.frozenStats.value[State2]!=0,unsigned(d.state2Age),d.frozenStats.qmax);
  else if(d.cursor<4)sent=stats(d,out,d.frozenStats,"FROZEN",d.cursor-1);
  else {
   if(d.cursor<4+capacity && d.cursor>=4+d.count)d.cursor=4+capacity;
   if(d.cursor<4+capacity){const unsigned index=d.cursor-4;sent=sample(d,out,d.oldest(index),"RAW",d.sequence(index));}
   else {
    if(d.cursor==4+capacity && !d.firstKnown)++d.cursor;
    if(d.cursor==4+capacity)sent=sample(d,out,d.firstOver8,"FIRST-OVER8",0);
    else if(d.cursor==8+capacity)sent=emit(d,out,"[DSRV/QUEUE-TIMES] scope=BOOT capture=%u over8Known=%u over8FirstAtMs=%u over8LastAtMs=%u state2Known=%u state2LastAtMs=%u ovfKnown=%u ovfLastAtMs=%u\n",
      d.captureId,d.total.value[Over8]!=0,unsigned(d.bootFirstOver8),unsigned(d.lastOver8),d.total.value[State2]!=0,unsigned(d.lastState2),d.total.value[Overflow]!=0,unsigned(d.lastOverflow));
    else sent=stats(d,out,d.total,"BOOT",d.cursor-(5+capacity));
   }
  }
  if(sent){++d.cursor;if(d.cursor>=9+capacity)d.cursor=255;}
  return;
 }
 if(d.anomalyPending){
  const auto& s=d.firstOver8;
  if(emit(d,out,"[DSRV/QUEUE-OVER8] capture=%u atMs=%u gen=%u mode=%c buffRaw=%u rawSaved=1\n",d.captureId,unsigned(s.atMs()),unsigned(s.generation()),modeName(s.mode),s.raw[5]))d.anomalyPending=false;
  return;
 }
 const auto& s=d.window;
 const bool mixed=d.mixed || (d.windowKnown && (d.windowGeneration!=generation || d.windowMode!=mode));
 if(emit(d,out,"[DSRV/QUEUE] scope=WINDOW ms=%u mode=%c gen=%u firstGen=%u mixed=%u headers=%u qmaxRaw=%u over8=%u intsrcOvf=%u state2=%u phy=%u hist=%u,%u,%u,%u,%u,%u,%u,%u readFailBoot=%u serialDrop=%u queued=%u core=%u free8Cached=%u suppressed=%u reportDrop=%u frozen=%u sat=%u\n",
  unsigned(now-d.windowAt),modeName(mode),unsigned(generation),unsigned(d.windowGeneration),mixed,unsigned(s.headers),s.qmax,s.value[Over8],s.value[Overflow],s.value[State2],s.value[Physical],s.hist[0],s.hist[1],s.hist[2],s.hist[3],s.hist[4],s.hist[5],s.hist[6],s.hist[7],unsigned(readFailBoot),unsigned(serialDrop),queued,core,unsigned(d.cachedFree8),d.suppressed,d.reportDrops,d.frozen,s.atLimit())){
  d.window=Stats{};d.windowAt=now;d.windowKnown=d.mixed=false;
 }
}
} // namespace dsrv_queue
