"""Clock/DST/availability regression against real bounded Core and vendor parser."""
from pathlib import Path
import json,subprocess,sys
from datetime import datetime
ROOT=Path(__file__).resolve().parents[1];out=ROOT/'.pio/epg_live';out.mkdir(exist_ok=True)
compiler=sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe'
vectors=json.loads((ROOT/'tests/fixtures/epg/time_source/timepoint_vectors.json').read_text())
clocks=json.loads((ROOT/'tests/fixtures/epg/time_source/clock_vectors.json').read_text())
fixture=(ROOT/'tests/test_epg_cache.cpp').read_text();fixture=fixture[fixture.index('using Data='):fixture.index('int main()')]
cpp=r'''
#include "epg_core.h"
#include "vendor/si468x/Si468x.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
'''+fixture+r'''
static void loadRaw(epg::Core& c,const std::vector<Data>& frames,uint32_t sid,bool live,bool reversed=false){
 c.reset();c.setLive(live);c.setClockTrusted(true);
 c.select(0x40e231530000ULL|sid,live?epg::epoch(2026,10,25,6,33):epg::epoch(2024,11,12,3,30),60);
 uint32_t tick=1;std::vector<Data> ordered=frames;
 if(reversed){ordered.clear();for(const auto& f:frames)if((f[0]&15)==6)ordered.push_back(f);for(auto i=frames.rbegin();i!=frames.rend();++i)if(((*i)[0]&15)==4)ordered.push_back(*i);}
 for(unsigned turn=0;turn<4;++turn)for(const auto& f:ordered){
  c.feed(f.data(),f.size(),tick+=10);while(c.busy())c.pump();
  bool conflict=false;auto b=c.findStation(sid,0x3153,0xe2,-1,conflict);
  c.select(b,c.now(),60);c.identityConflict=conflict;
 }
}
int main(int argc,char** argv){
 assert(argc==3);uint32_t utc=0;int16_t off=0;bool present=false;
'''
for v in vectors:
 cpp+='{ const uint8_t b[]={'+','.join(str(x) for x in bytes.fromhex(v['hex']))+'};assert(epg::timePoint(b,sizeof(b),utc,off,present));assert(utc=='+str(v['utc'])+'U && off=='+str(v['offset'])+' && present=='+str(int(v['present']))+');}\n'
cpp+=r'''
 const uint8_t invalidHour[]={0x3b,0x34,0x96,0x00,0x02};assert(!epg::timePoint(invalidHour,5,utc,off,present));
 const uint8_t invalidMinute[]={0x3b,0x34,0x90,0x3c,0x02};assert(!epg::timePoint(invalidMinute,5,utc,off,present));
 const uint8_t invalidSeconds[]={0x3b,0x34,0x98,0x00,0xf0,0x00,0x02};assert(!epg::timePoint(invalidSeconds,7,utc,off,present));
 const uint8_t invalidLto[]={0x3b,0x34,0x90,0x00,0x1d};assert(!epg::timePoint(invalidLto,5,utc,off,present));
 const uint8_t missing[]={0x3b,0x34,0x90,0x00};assert(!epg::timePoint(missing,4,utc,off,present));
 epg::BroadcastClockSample s;s.valid=true;s.generation=1;s.sampledAtMonotonicMs=100;s.encoding=epg::ClockEncoding::Local;
'''
for v in clocks:
 u=int(datetime.fromisoformat(v['utc']).timestamp());l=int(datetime.fromisoformat(v['local']).timestamp())
 cpp+=f'''s.calendarEpoch={l}U;s.offsetMinutes={v['offset']};s.offsetKnown=true;
 {{auto r=epg::resolveTime(s,100,true);assert(r.utcValid && r.localValid && r.utc=={u}U && r.local=={l}U);int16_t paired=0;assert(epg::pairedOffset({l}U,100,{u}U,100,paired)&&paired=={v['offset']});}}
'''
cpp+=r'''
 assert(!epg::resolveTime(s,300100,true).utcValid);assert(!epg::resolveTime(s,100,false).utcValid);
 s.offsetKnown=false;assert(!epg::resolveTime(s,100,true).utcValid && epg::resolveTime(s,100,true).localValid);
 s.encoding=epg::ClockEncoding::Unknown;assert(!epg::resolveTime(s,100,true).utcValid);
 s.encoding=epg::ClockEncoding::UTC;assert(epg::resolveTime(s,100,true).utcValid);
 int16_t delta=0;assert(!epg::pairedOffset(100000,0,100000,2100,delta));
 assert(!epg::pairedOffset(100900,0,100000,100,delta));
 const uint8_t rawClock[]={128,0,0,0,0xea,0x07,10,25,2,0,0};si468x::DabTimeInfo parsed;
 assert(si468x::Si468x::parseDabTime(rawClock,sizeof(rawClock),parsed)==si468x::Result::Ok);
 assert(parsed.year==2026 && parsed.month==10 && parsed.day==25 && parsed.hour==2);
 auto ptr=std::unique_ptr<epg::Core>(new epg::Core);auto& c=*ptr;
 assert(c.availability(false)==epg::Availability::NoSpi);
 assert(c.availability(true)==epg::Availability::WaitDirectory);
 Data empty(27);empty[3]=27;empty[12]=14;
 std::ifstream emptyFile(argv[2],std::ios::binary);Data savedEmpty((std::istreambuf_iterator<char>(emptyFile)),std::istreambuf_iterator<char>());assert(savedEmpty==empty);
 auto f=group(1,6,0,true,empty);
 assert(c.feed(f.data(),f.size(),1)==epg::Feed::Accepted);
 assert(c.availability(true)==epg::Availability::DirectoryEmpty && c.stats.invalid==0);
 std::ifstream input(argv[1],std::ios::binary);std::vector<Data> frames;
 while(true){uint8_t h[10];input.read(reinterpret_cast<char*>(h),10);if(!input.gcount())break;assert(input.gcount()==10);Data b(h[8]|(unsigned(h[9])<<8));input.read(reinterpret_cast<char*>(b.data()),b.size());assert(size_t(input.gcount())==b.size());frames.push_back(b);}
 assert(frames.size()==1939);
 for(bool reversed:{false,true}) for(uint32_t sid:{0x37fcU,0x37fdU}) {
  loadRaw(c,frames,sid,true,reversed);
  assert(c.station() && c.logoSize()>0 && !c.hasPI() && c.count()==0);
  assert(c.availability(true)==epg::Availability::SiOnly && c.current()==-1 && c.next()==-1);
  assert(std::string(c.station()->logo)==(sid==0x37fc?"krakow.png":"off.png"));
 }
 loadRaw(c,frames,0x3233,true);
 assert(c.hasSI() && c.hasPI() && c.logoSize()>0 && c.count()==68);
 assert(c.availability(true)==epg::Availability::PiDateStale && c.current()==-1 && c.next()==-1);
 uint32_t first=0,last=0;assert(c.scheduleRange(first,last) && first<epg::epoch(2025,1,1));
 assert(c.now()==epg::epoch(2026,10,25,6,33));
 assert(c.archivePreview(epg::epoch(2024,11,12)));assert(c.current()==-1 && c.now()==epg::epoch(2026,10,25,6,33));
 assert(c.archivePreview(0));c.setClockTrusted(false);assert(c.availability(true)==epg::Availability::TimeUnknown && c.current()==-1);
 c.setClockTrusted(true);assert(c.availability(true)==epg::Availability::PiDateStale);
 // Replace stale Directory with a current PI without resetting the receiver.
 Object updated{0x123,0x40e231533233ULL,epg::epoch(2026,10,25),{}};
 Data p=tlv(0x81,number(1,3));append(p,tlv(0x12,tlv(1,str("Current replacement"))));
 Data t=tlv(0x80,timeValue(updated.day+6*3600));append(t,tlv(0x81,number(3600,2)));
 append(t,tlv(0x82,timeValue(updated.day+6*3600+600)));append(t,tlv(0x83,number(2400,2)));append(p,tlv(0x2c,t));
 Data r=tlv(5,number(updated.bearer,6));append(r,tlv(0x1c,p));updated.body=tlv(2,r);
 directory(c,{updated});c.select(updated.bearer,epg::epoch(2026,10,25,6,33),0);feed(c,updated);
 assert(c.availability(true)==epg::Availability::ReadyLive && c.count()==1 && c.current()==0);
 assert(c.at(0).actual==updated.day+6*3600+600 && c.at(0).actualDuration==2400 && c.at(0).duration==3600);
 c.setTime(updated.day+7*3600,0);assert(c.current()==-1); // [start,end), planned interval preserved.
 c.setTime(updated.day+6*3600,0);assert(c.current()==0);
 loadRaw(c,frames,0x3233,false);assert(c.count()==68 && c.current()>=0 && c.next()>=0);
 loadRaw(c,frames,0x32a3,true);assert(c.availability(true)==epg::Availability::IdentityConflict && !c.logoSize());
 c.reset();c.setLive(true);c.setClockTrusted(true);Object now=object(0,1,0,3);directory(c,{now});select(c,now);feed(c,now);verify(c,now,3);
 assert(c.availability(true)==epg::Availability::ReadyLive);
 c.setClockTrusted(false);assert(c.availability(true)==epg::Availability::TimeUnknown && c.current()==-1 && c.next()==-1);
 c.setClockTrusted(true);assert(c.current()>=0);
 const uint32_t day=c.localDay();c.select(c.selected(),0,0);assert(c.localDay()==day);
 c.reset();assert(!c.count() && !c.logoSize() && c.availability(true)==epg::Availability::WaitDirectory);
 std::printf("Clock/DST/RAW metadata/archive/conflict/empty-directory: PASS (arena=%zu)\n",epg::Core::memoryBytes());
}
'''
source=out/'live.cpp';source.write_text(cpp,encoding='utf-8');exe=out/'live.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(source),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe),str(ROOT/'.pio/epg_test/frames.bin'),str(ROOT/'tests/fixtures/epg/time_source/empty_directory_11a.bin')],check=True)
