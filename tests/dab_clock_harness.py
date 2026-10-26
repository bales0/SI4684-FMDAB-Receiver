"""Compile production clock completion, failure and queue branches together.

Only SPI command submission/replies and monotonic time are supplied by the host.
Used by both the driver test and the real EPG runtime integration.
"""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def harness():
    source = (ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
    start = source.index('    case DabCommand::Time:', source.index('void DAB::finishDabCommand'))
    cases = source[start:source.index('    case DabCommand::ServiceType:', start)]
    start = source.index('  if(dabClockConfirmPending && signallock)')
    queue = source[start:source.index('  if (dabAudioRefreshPending && dabActiveServiceValid)', start)]
    start = source.index('  if (result != si468x::Result::Ok && (completed==DabCommand::Time')
    failed = source[start:source.index('  if (result != si468x::Result::Ok) {', start)]
    start = source.index('void DAB::maintainDabClock(void) {')
    maintenance = source[start:source.index('\nvoid DAB::scheduleNextDabCommand', start)]
    maintenance = maintenance.replace('void DAB::maintainDabClock(void)', 'void maintainDabClock(void)')
    # The real finish function fences superseded replies before all these cases.
    fence = 'if (!dab_scheduler::generationMatches(completedGeneration, dabGeneration))'
    assert source.index(fence, source.index('void DAB::finishDabCommand')) < source.index(failed.strip())
    return r'''
#include "epg_core.h"
#include "dab_clock_refresh.h"
#include "dab_scheduler_policy.h"
#include "vendor/si468x/Si468x.h"
#include <cassert>
#include <initializer_list>
#include <cstring>
#define DIAG_PRINTF(...) ((void)0)
struct ClockRadio {
 enum class DabCommand {None,Time,TimeUtc,TimeLocalConfirm};
 unsigned Year=0,Months=0,Days=0,Hours=0,Minutes=0,Seconds=0,diagDabCommandErrorCount=0;
 uint32_t dabTimeGeneration=0,dabGeneration=1,dabLastLowPriorityCommandMs=0,sentGeneration=0;
 bool dabUtcRefreshPending=false,dabClockConfirmPending=false,dabTimeRefreshPending=true;
 bool signallock=true,diagnosticDebug=false,dabTuneRequestPending=false,fm=false;
 epg::BroadcastClockSample dabEpgClock;epg::ClockRefresh dabClockRefresh;
 DabCommand sent=DabCommand::None;uint8_t sentType=255,SPIbuffer[32]={};
 bool isFm()const{return fm;}
 bool parseOk(si468x::Result r){return r==si468x::Result::Ok;}
 bool startDabCommand(DabCommand cmd,uint8_t command,const uint8_t* args,unsigned n,unsigned response){
  assert(command==0xbc && n==1 && response==11 && sent==DabCommand::None);
  sent=cmd;sentType=*args;sentGeneration=dabGeneration;return true;
 }
''' + maintenance + r'''
 void schedule(){maintainDabClock();if(sent!=DabCommand::None)return;
  const uint32_t now=millis();const uint8_t zero=0;
''' + queue + r'''
 }
 void finish(si468x::Result result=si468x::Result::Ok){
  assert(sent!=DabCommand::None);const auto completed=sent;sent=DabCommand::None;
  const auto completedGeneration=sentGeneration;
  if(!dab_scheduler::generationMatches(completedGeneration,dabGeneration))return;
''' + failed + r'''
  switch(completed){
''' + cases + r'''
  default:assert(false);
  }
 }
 void reply(unsigned y,unsigned m,unsigned d,unsigned h,unsigned mi,unsigned sec){
  std::memset(SPIbuffer,0,sizeof(SPIbuffer));SPIbuffer[1]=128;
  SPIbuffer[5]=uint8_t(y);SPIbuffer[6]=uint8_t(y>>8);SPIbuffer[7]=uint8_t(m);SPIbuffer[8]=uint8_t(d);
  SPIbuffer[9]=uint8_t(h);SPIbuffer[10]=uint8_t(mi);SPIbuffer[11]=uint8_t(sec);finish();
 }
 void replyEpoch(uint32_t value){
  int y=0;unsigned m=0,d=0;epg::calendar(value,y,m,d);
  reply(y,m,d,(value/3600)%24,(value/60)%60,value%60);
 }
};
#undef DIAG_PRINTF
'''
