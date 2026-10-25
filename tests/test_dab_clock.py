"""Execute the actual DAB clock cases and queued command scheduling with mock SPI."""
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[1];out=ROOT/'.pio/dab_clock';out.mkdir(exist_ok=True)
radio=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
cases=radio[radio.index('    case DabCommand::Time:'):radio.index('    case DabCommand::ServiceType:',radio.index('    case DabCommand::Time:'))]
queue=radio[radio.index('  if(dabClockConfirmPending && signallock)'):radio.index('  if (dabAudioRefreshPending && dabActiveServiceValid)',radio.index('  if(dabClockConfirmPending && signallock)'))]
cpp=r'''
#include "epg_core.h"
#include "dab_scheduler_policy.h"
#include "vendor/si468x/Si468x.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#define DIAG_PRINTF(...) ((void)0)
static uint32_t tick=100;
uint32_t millis(){return tick;}
uint8_t SPIbuffer[32]={};
struct Radio {
 enum class DabCommand {None,Time,TimeUtc,TimeLocalConfirm};
 unsigned Year=0,Months=0,Days=0,Hours=0,Minutes=0,Seconds=0,diagDabCommandErrorCount=0;
 uint32_t dabTimeGeneration=0,dabLocalClockEpoch=0,dabLocalClockMs=0,dabLastLowPriorityCommandMs=0;
 bool dabUtcRefreshPending=false,dabClockConfirmPending=false,dabTimeRefreshPending=true,signallock=true,diagnosticDebug=false;
 epg::BroadcastClockSample dabEpgClock;
 DabCommand sent=DabCommand::None;uint8_t sentType=255;
 bool parseOk(si468x::Result result){return result==si468x::Result::Ok;}
 bool startDabCommand(DabCommand cmd,uint8_t command,const uint8_t* args,unsigned n,unsigned response){
  assert(command==0xbc && n==1 && response==11);sent=cmd;sentType=*args;return true;
 }
 void schedule(){const uint32_t now=millis();const uint8_t zero=0;
'''+queue+r'''
 }
 void reply(unsigned year,unsigned month,unsigned day,unsigned hour,unsigned minute,unsigned second){
  assert(sent!=DabCommand::None);const auto completed=sent;sent=DabCommand::None;
  std::memset(SPIbuffer,0,sizeof(SPIbuffer));SPIbuffer[1]=128;
  SPIbuffer[5]=uint8_t(year);SPIbuffer[6]=uint8_t(year>>8);SPIbuffer[7]=uint8_t(month);SPIbuffer[8]=uint8_t(day);SPIbuffer[9]=uint8_t(hour);SPIbuffer[10]=uint8_t(minute);SPIbuffer[11]=uint8_t(second);
  switch(completed){
'''+cases+r'''
  default:assert(false);
  }
 }
};
int main(){
 Radio r;
 r.schedule();assert(r.sentType==0);r.reply(2026,10,25,2,59,59);assert(r.Hours==2 && !r.dabEpgClock.offsetKnown);
 tick+=100;r.schedule();assert(r.sentType==1);r.reply(2026,10,25,0,59,59);assert(r.Hours==2 && !r.dabEpgClock.valid);
 tick+=1000;r.schedule();assert(r.sentType==0);r.reply(2026,10,25,2,0,0);
 auto clock=epg::resolveTime(r.dabEpgClock,tick,true);
 assert(clock.utcValid && clock.offsetKnown && clock.offset==60 && clock.utc==epg::epoch(2026,10,25,1,0,0));
 assert(clock.local==epg::epoch(2026,10,25,2,0,0) && r.Hours==2 && r.Minutes==0);
 r.dabTimeRefreshPending=true;tick+=100;r.schedule();r.reply(2026,3,29,1,59,59);
 tick+=100;r.schedule();r.reply(2026,3,29,0,59,59);
 tick+=1000;r.schedule();r.reply(2026,3,29,3,0,0);
 clock=epg::resolveTime(r.dabEpgClock,tick,true);
 assert(clock.utc==epg::epoch(2026,3,29,1) && clock.local==epg::epoch(2026,3,29,3) && clock.offset==120);
 assert(!epg::resolveTime(r.dabEpgClock,tick,false).utcValid);
 r.dabTimeRefreshPending=true;r.schedule();r.reply(2026,10,25,8,0,0);
 tick+=100;r.schedule();r.reply(2026,10,25,7,0,0);
 tick+=2100;r.schedule();r.reply(2026,10,25,8,0,2);
 assert(!r.dabEpgClock.valid && !r.dabEpgClock.offsetKnown);
 r.dabTimeRefreshPending=true;r.schedule();r.reply(2026,10,25,8,0,0);
 tick+=100;r.schedule();r.reply(2026,10,25,24,0,0);
 assert(!r.dabEpgClock.valid && !r.dabClockConfirmPending);
 r.signallock=false;r.dabTimeRefreshPending=true;r.schedule();assert(r.sent==Radio::DabCommand::None);
 std::puts("Actual serialized LOCAL/UTC/LOCAL reply parser and queue: DST boundary, local display preservation, stale pair and invalid calendar rejection: PASS");
}
'''
source=out/'clock.cpp';source.write_text(cpp,encoding='utf-8');exe=out/'clock.exe'
subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe','-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(source),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
