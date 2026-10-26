"""C01-C11/C14-C15 against actual serialized completion/error/scheduler paths."""
from pathlib import Path
import subprocess,sys
from dab_clock_harness import harness
ROOT=Path(__file__).resolve().parents[1];out=ROOT/'.pio/dab_clock';out.mkdir(exist_ok=True)
cpp=r'''
#include <cstdint>
#include <cstdio>
static uint32_t tick=100;
uint32_t millis(){return tick;}
'''+harness()+r'''
static void cycle(ClockRadio& r,uint32_t utc,int offset=60) {
 r.dabTimeRefreshPending=true;r.schedule();assert(r.sentType==0);r.replyEpoch(utc+offset*60);
 tick+=100;r.schedule();assert(r.sentType==1);r.replyEpoch(utc);
 tick+=100;r.schedule();assert(r.sentType==0);r.replyEpoch(utc+offset*60);
}
static uint32_t now(const ClockRadio& r){return epg::resolveTime(r.dabEpgClock,tick,r.signallock).utc;}
int main(){
 const uint32_t base=epg::epoch(2026,10,25,6);
 ClockRadio r;r.schedule();assert(r.sentType==0);r.replyEpoch(base+3600);
 assert(!r.dabEpgClock.valid && r.dabClockRefresh.state==epg::ClockCandidateState::AwaitUtc);
 tick+=100;r.schedule();r.replyEpoch(base);assert(!r.dabEpgClock.valid);
 tick+=100;r.schedule();r.replyEpoch(base+3600);
 assert(r.dabEpgClock.valid && r.dabEpgClock.generation==1 && now(r)==base);
 std::puts("C01/C04 PASS: first complete cycle only; atomic publication");
 for(int i=0;i<11;++i) {
  tick+=60000;const auto before=r.dabEpgClock;const auto gen=before.generation;
  r.dabTimeRefreshPending=true;r.schedule();r.replyEpoch(now(r)+3600);
  assert(r.dabEpgClock.generation==gen && r.dabEpgClock.sampledAtMonotonicMs==before.sampledAtMonotonicMs);
  tick+=100;r.schedule();const auto utc=now(r);r.replyEpoch(utc);
  assert(r.dabEpgClock.generation==gen && epg::resolveTime(r.dabEpgClock,tick,true).utcValid);
  tick+=100;r.schedule();r.replyEpoch(now(r)+3600);assert(r.dabEpgClock.generation==gen+1);
 }
 std::puts("C02/C03/C15 PASS: 11 minutes of pending refreshes preserve confirmed snapshot");
 for(auto failure:{0,1,2,3,4,5,6}) {
  if(r.dabClockRefresh.retryPending)tick=r.dabClockRefresh.retryAt;
  auto before=r.dabEpgClock;r.dabTimeRefreshPending=true;r.schedule();
  if(failure==0)r.reply(2026,2,30,12,0,0);
  else if(failure==1)r.reply(2026,10,25,24,0,0);
  else {
   r.replyEpoch(now(r)+3600);tick+=100;r.schedule();
   if(failure==2)r.finish(si468x::Result::Timeout);
   else if(failure==3)r.reply(2026,10,25,12,60,0);
   else {
    r.replyEpoch(now(r));tick+=failure==4?2101:100;r.schedule();
    if(failure==5)r.replyEpoch(now(r)+900); // not a half-hour offset
    else if(failure==6)r.finish(si468x::Result::DeviceError);
    else r.replyEpoch(now(r)+3600);
   }
  }
  assert(r.dabEpgClock.generation==before.generation && r.dabEpgClock.calendarEpoch==before.calendarEpoch);
  assert(r.dabClockRefresh.retryPending && !r.dabUtcRefreshPending && !r.dabClockConfirmPending);
  r.schedule();assert(r.sent==ClockRadio::DabCommand::None);
 }
 std::puts("C05/C06 PASS: invalid calendar/time, UTC/confirm error, pairing window and offset; bounded retry");
 tick=r.dabEpgClock.sampledAtMonotonicMs+300000;assert(!epg::resolveTime(r.dabEpgClock,tick,true).utcValid);
 std::puts("C07 PASS: holdover expires exactly at 300 seconds");
 for(bool autumn:{true,false}) {
  ClockRadio dst;uint32_t utc=autumn?epg::epoch(2026,10,25,0,59,59):epg::epoch(2026,3,29,0,59,59);
  dst.schedule();dst.replyEpoch(utc+(autumn?120:60)*60);tick+=100;dst.schedule();dst.replyEpoch(utc);
  tick+=1000;dst.schedule();dst.replyEpoch(utc+1+(autumn?60:120)*60);
  assert(dst.dabEpgClock.valid && dst.dabEpgClock.offsetMinutes==(autumn?60:120));
  assert(now(dst)==utc+1 && dst.Hours==(autumn?2U:3U));
 }
 std::puts("C08 PASS: actual DST spring/autumn LOCAL-UTC-LOCAL across boundary");
 ClockRadio reacq;cycle(reacq,base);auto gen=reacq.dabEpgClock.generation;
 reacq.signallock=false;reacq.maintainDabClock();reacq.signallock=true;reacq.schedule();reacq.replyEpoch(base+3600);
 assert(reacq.dabEpgClock.generation==gen);reacq.dabGeneration++;reacq.schedule();
 assert(!reacq.dabEpgClock.valid);reacq.dabGeneration++;reacq.replyEpoch(base); // old in-flight generation ignored
 reacq.maintainDabClock();assert(!reacq.dabEpgClock.valid);
 cycle(reacq,base);assert(reacq.dabEpgClock.generation==gen+1);
 std::puts("C10 PASS: RF lock loss, generation changes, superseded reply, new confirmed cycle");
 ClockRadio jumps;cycle(jumps,base);gen=jumps.dabEpgClock.generation;
 cycle(jumps,base+3600);assert(jumps.dabEpgClock.generation==gen && jumps.dabClockRefresh.reason==epg::ClockReject::Jump);
 tick=jumps.dabClockRefresh.retryAt;cycle(jumps,base+3605);assert(jumps.dabEpgClock.generation==gen+1);
 // A one-off year jump is rejected too; ordinary time then recovers immediately.
 auto accepted=now(jumps);cycle(jumps,accepted+366U*86400);assert(jumps.dabEpgClock.generation==gen+1);
 tick=jumps.dabClockRefresh.retryAt;cycle(jumps,now(jumps));assert(jumps.dabEpgClock.generation==gen+2);
 std::puts("C11 PASS: one-off hour/year jump rejected; independent repeated correction accepted");
 tick=0xffffff00U;ClockRadio wrap;cycle(wrap,base);assert(wrap.dabEpgClock.valid);
 tick+=2000;assert(epg::resolveTime(wrap.dabEpgClock,tick,true).utcValid);
 tick=0xffffff00U;ClockRadio pair;pair.schedule();pair.replyEpoch(base+3600);tick+=100;pair.schedule();pair.replyEpoch(base);
 tick+=1000;pair.schedule();pair.replyEpoch(base+3601);assert(now(pair)==base+1);
 std::puts("C14 PASS: millis wrap during accepted holdover and UTC/LOCAL pairing");
 ClockRadio pending;pending.schedule();tick+=10001;pending.maintainDabClock();assert(pending.dabClockRefresh.reason==epg::ClockReject::Timeout);
 std::puts("Clock candidate size and public sample remain bounded; actual driver paths PASS");
}
'''
source=out/'clock.cpp';source.write_text(cpp,encoding='utf-8');exe=out/'clock.exe'
subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe','-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(source),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
