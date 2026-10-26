#pragma once
#include "dab_loss_diag.h"
#include <stdio.h>

namespace dab_loss {
// Arduino Print::printf allocates for long lines. Format diagnostic records in
// a bounded stack buffer and enqueue directly instead.
template<class Output,class... Args> void emit(Output& out,const char* format,Args... args) {
 char line[1024];const int length=snprintf(line,sizeof(line),format,args...);
 if(length<=0 || length>=int(sizeof(line))){evidence().event(LossLogDrop);return;}
 out.write(reinterpret_cast<const uint8_t*>(line),size_t(length));
}

// One bounded record per service call, shared 100 ms pacing. Snapshotting never
// depends on UART capacity; a blocked UART keeps its frozen interval intact.
template<class Output> void service(Output& out,uint32_t now,bool debug,
                                    unsigned probeState,uint32_t uartDrop,
                                    uint32_t heap,uint32_t minHeap,uint32_t stack,
                                    int rssi=0,unsigned cnr=0,unsigned fic=0,bool lock=false) {
 auto& d=evidence();
 if(d.stage==255 && uint32_t(now-d.reportAt)>=30000U){
  d.reportMs=now-d.reportAt;d.reportAt=now;d.report=d.window;d.window=Bank{};d.stage=0;
  d.reportMode=d.mode;d.reportEpoch=d.modeEpoch;d.reportMixed=d.windowEpoch!=d.modeEpoch;d.windowEpoch=d.modeEpoch;
 }
 if(!(debug || d.experiment) || uint32_t(now-d.logAt)<100U || out.availableForWrite()<1200)return;
 d.logAt=now;
 if(d.lossCount){
  const auto& l=d.losses[d.lossHead];char mask[65];static const char hex[]="0123456789ABCDEF";
  for(unsigned i=0;i<32;++i){mask[2*i]=hex[l.mask[i]>>4];mask[2*i+1]=hex[l.mask[i]&15];}mask[64]=0;
  emit(out,"[SLS/LOSS] tid=%u reason=%s total=%u received=%u missing=%u firstMissing=%u missingMask=%s exact=%u bytes=%u/%u lastSeen=%u ageMs=%u lastActivityAgeMs=%u modeEpoch=%u/%u\n",
    unsigned(l.tid),l.reason,unsigned(l.expected),unsigned(l.received),unsigned(l.missing),unsigned(l.firstMissing),mask,l.last,unsigned(l.receivedBytes),unsigned(l.expectedBytes),l.last,unsigned(l.ageMs),unsigned(l.idleMs),unsigned(l.epochStart),unsigned(l.epochEnd));
  emit(out,"[SLS/PATH] tid=%u overflow=%u physical=%u readFail=%u byteCountMismatch=%u crcBad=%u rejected=%u maxRemaining=%u maxDrainMs=%u padSlsHeaders=%u fullReplyOk=%u\n",
    unsigned(l.tid),unsigned(l.overflow),unsigned(l.physical),unsigned(l.readFail),unsigned(l.mismatch),unsigned(l.crcBad),unsigned(l.rejected),unsigned(l.queueMax),unsigned(l.drainMaxMs),unsigned(l.padHeaders),unsigned(l.fullOk));
  d.lossHead=(d.lossHead+1)%2;--d.lossCount;return;
 }
 if(d.stage==255)return;
 const auto& b=d.report;
 if(d.stage==0){
  uint64_t poll=0,irq=0,other=0;
  for(unsigned c=0;c<commands;++c){poll+=b.cmd[c][Poll];irq+=b.cmd[c][Irq];if(c!=3)other+=uint64_t(b.cmd[c][Poll])+b.cmd[c][Irq];}
  emit(out,"[DSRV/RATE] ms=%u mode=%s probe=%u ctsPoll=%llu ctsIrq=%llu cmdDsrvStart=%u cmdDsrvOk=%u dsrvCts=%llu nonDsrv=%llu replyEmpty=%u packet=%u pad=%u packetEmptyDir=%u readFail=%u overflowDelta=%u queueMax=%u waitMaxMs=%u drainMaxMs=%u UARTdrop=%u heap=%u minHeap=%u stackHWM=%u lastReadResult=%d RSSIx10=%d CNR=%u FIC=%u lock=%u modeEpoch=%u mixed=%u\n",
    unsigned(d.reportMs),modeName(d.reportMode),probeState,(unsigned long long)poll,(unsigned long long)irq,unsigned(b.cmd[3][Start]),unsigned(b.cmd[3][Ok]),(unsigned long long)(uint64_t(b.cmd[3][Poll])+b.cmd[3][Irq]),(unsigned long long)other,
    unsigned(b.event[Empty]),unsigned(b.event[Source0]),unsigned(b.event[Source1]),unsigned(b.event[PacketEmptyDir]),unsigned(b.event[ReadFail]),unsigned(b.event[Overflow]),unsigned(b.queueMax),unsigned(b.waitMaxMs),unsigned(b.drainMaxMs),unsigned(uartDrop),unsigned(heap),unsigned(minHeap),unsigned(stack),d.lastReadResult,rssi,cnr,fic,lock,unsigned(d.reportEpoch),d.reportMixed);
 }else if(d.stage<=commands){
  const unsigned c=d.stage-1;const auto* v=b.cmd[c];const auto* t=d.total.cmd[c];
  emit(out,"[DSRV/CMD] op=%s delta=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u total=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",commandName(c),
   unsigned(v[0]),unsigned(v[1]),unsigned(v[2]),unsigned(v[3]),unsigned(v[4]),unsigned(v[5]),unsigned(v[6]),unsigned(v[7]),unsigned(v[8]),unsigned(v[9]),
   unsigned(t[0]),unsigned(t[1]),unsigned(t[2]),unsigned(t[3]),unsigned(t[4]),unsigned(t[5]),unsigned(t[6]),unsigned(t[7]),unsigned(t[8]),unsigned(t[9]));
 }else if(d.stage==commands+1){
  emit(out,"[DSRV/QUEUE] histDelta=%u,%u,%u,%u histTotal=%u,%u,%u,%u fullReads=%u transactions=%u\n",
   unsigned(b.remaining[0]),unsigned(b.remaining[1]),unsigned(b.remaining[2]),unsigned(b.remaining[3]),unsigned(d.total.remaining[0]),unsigned(d.total.remaining[1]),unsigned(d.total.remaining[2]),unsigned(d.total.remaining[3]),unsigned(d.fullReads),unsigned(d.transaction));
 }else{
  unsigned first=(d.stage-commands-2)*4;
  emit(out,"[DSRV/PATH]");
  for(unsigned i=first;i<first+4 && i<EventCount;++i)emit(out," %s=%u/%u",eventName(i),unsigned(b.event[i]),unsigned(d.total.event[i]));
  emit(out,"\n");
 }
 ++d.stage;if(d.stage>=commands+2+(EventCount+3)/4)d.stage=255;
}
} // namespace dab_loss
