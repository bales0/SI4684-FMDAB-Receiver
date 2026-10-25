"""Focused acquisition policy tests using the preserved programme/SI/PNG stream.
Private access is only for inspecting the target and constructing edge states;
all actual objects still pass through production MOT/PI/SI/PNG parsing.
"""
from pathlib import Path
import subprocess, sys

ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'.pio/epg_priority';out.mkdir(parents=True,exist_ok=True)
cpp=r'''
#define private public
#include "epg_core.h"
#undef private
#include "epg_acquisition.h"
#include "dab_spi_probe.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <fstream>
using Data=std::vector<uint8_t>;
static std::vector<Data> frames;
static uint32_t tick=100;
static unsigned tid(const Data& f){return unsigned(f[5])*256+f[6];}
static void feed(epg::Core& c,const Data& f){assert(c.feed(f.data(),f.size(),++tick)==epg::Feed::Accepted);while(c.busy())c.pump();}
static void object(epg::Core& c,unsigned id){for(const auto& f:frames)if((f[0]&15)==4 && tid(f)==id)feed(c,f);}
static void load(epg::Core& c){c.reset();c.setLive(true);c.setClockTrusted(true);for(const auto& f:frames)if((f[0]&15)==6)feed(c,f);}
int main(int argc,char** argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);
 while(in.peek()!=EOF){unsigned lo=unsigned(in.get()),hi=unsigned(in.get());Data f(lo+256*hi);in.read(reinterpret_cast<char*>(f.data()),f.size());assert(in);frames.push_back(f);}
 epg::Core c;load(c);unsigned pi=0,si=0,png=0;uint64_t bearer=0;
 for(size_t i=0;i<c.entryCount();++i){const auto& e=c.entries()[i];if(e.type==7 && e.subtype==1){pi=e.tid;bearer=e.bearer;}if(e.type==7 && e.subtype==0)si=e.tid;if(e.type==2)png=e.tid;}
 assert(pi && si && png && bearer);
 const uint32_t now=epg::epoch(2024,11,12,1,1);
 auto select=[&](){c.select(bearer,now,0);};
 auto entry=[&](unsigned id)->epg::Entry&{for(size_t i=0;i<c.entryCount_;++i)if(c.entries_[i].tid==id)return c.entries_[i];assert(false);return c.entries_[0];};
 select();assert(c.wanted()==pi);std::puts("T01 PASS: exact selected PI before SI");
 c.focus(si);assert(c.wanted()==si);c.focus(0);assert(c.wanted()==pi);
 object(c,pi);assert(c.count()==3 && c.current()>=0 && c.wanted()==si);
 object(c,si);assert(c.count()==3 && c.current()>=0 && c.wanted()==png && c.station());
 object(c,png);assert(c.logoSize()==551 && c.availability(true)==epg::Availability::ReadyLive);
 std::puts("T02 PASS: PI -> SI -> own Adam7 PNG; NOW survives later SI");
 load(c);select();entry(pi).type|=128;assert(c.wanted()==si);object(c,si);assert(c.wanted()==png);object(c,png);assert(c.hasSI() && c.logoSize() && !c.count());
 std::puts("T03 PASS: SI-only progresses to own PNG without PI wait");
 load(c);c.select(bearer+1,now,0);assert(c.wanted()==si);std::puts("T04 PASS: other station PI excluded");
 load(c);c.select(0,now,0);entry(pi).bearer=0;assert(c.wanted()==si);
 load(c);select();c.identityConflict=true;assert(c.wanted()==si);
 std::puts("T05 PASS: zero/unknown/conflicting identity falls back to SI");
 load(c);c.select(bearer,epg::epoch(2026,10,25,1,1),0);entry(pi).start=epg::epoch(2024,11,12);entry(pi).stop=epg::epoch(2024,11,13);
 assert(c.wanted()==si);object(c,si);assert(!c.count() && c.current()==-1 && c.wanted()==png);object(c,png);assert(c.logoSize());
 std::puts("T06 PASS: archival PI not preferred over SI/PNG; no date rewriting");
 load(c);select();object(c,pi);object(c,si);object(c,png);auto n=c.count();auto dirs=c.stats.directories;
 for(unsigned repeat=0;repeat<3;++repeat)for(const auto& f:frames)feed(c,f);
 assert(c.count()==n && n==3 && c.logoSize()==551 && c.stats.directories==dirs && c.wanted()==0);
 std::puts("T16 PASS: repeated carousel keeps cache/logo without duplicate records");
 // A missing segmented PI must not starve metadata forever.
 load(c);select();for(auto f:frames)if((f[0]&15)==4 && tid(f)==pi){f[2]&=0x7f;auto crc=epg::crc16(f.data(),f.size()-2);f[f.size()-2]=uint8_t(crc>>8);f.back()=uint8_t(crc);feed(c,f);break;}assert(c.bodyAssembly_.active);
 tick+=5001;c.expire(tick);object(c,si);assert(c.hasSI());
 // Both baseline lengths, rollover, cancellation and uncertain START cleanup.
 const dab_probe::Endpoint audio(1,2),sls,packet(3,4);
 for(unsigned baseline:{500U,5000U}){
  dab_probe::Probe p;const uint32_t start=UINT32_MAX-100;
  assert(p.request(audio,sls,packet,1,start,baseline));p.advance(start+baseline-1,true);assert(p.state==dab_probe::State::Baseline);
  p.advance(start+baseline,true);assert(p.state==dab_probe::State::StartRequested);
  p.started(start+baseline,false,true);assert(p.state==dab_probe::State::StopRequested);
  p.stopped(start+baseline,true);assert(p.state==dab_probe::State::Idle);
  assert(p.request(audio,sls,packet,2,start,baseline));p.advance(start+1,false);assert(p.state==dab_probe::State::Idle);
 }
 std::puts("T13/T14 PASS: rollover baselines; uncertain START -> bounded STOP");
 epg_acq::Trace trace;trace.audio(1,2,100);assert(!trace.paths[0].known);
 trace.enable(true);trace.audio(1,2,UINT32_MAX-100);trace.endpoint(0,3,4,UINT32_MAX-90);trace.mark(0,2,UINT32_MAX-80);trace.mark(0,3,399);
 assert(uint32_t(trace.paths[0].at[3]-trace.paths[0].origin)==500 && !(trace.paths[0].known&(1<<4)));
 const auto first=trace.paths[0].at[3];trace.mark(0,3,500);assert(trace.paths[0].at[3]==first);
 trace.endpoint(1,1,2,400);assert(trace.paths[1].known==3);trace.audio(5,6,600);assert(trace.paths[0].known==1 && trace.paths[1].known==1 && !trace.paths[0].sid);
 trace.reset();trace.endpoint(1,5,6,700);trace.audio(5,6,710);assert(trace.paths[1].known==3 && trace.paths[1].at[1]==700);
 trace.enable(false);assert(!trace.paths[0].known);std::puts("DEBUG timing PASS: first-only, unknown stages, rollover, path/context isolation");
}
'''
source=out/'priority.cpp';source.write_text(cpp,encoding='utf-8')
exe=out/'priority.exe'
compiler=sys.argv[1] if len(sys.argv)>1 else 'g++'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(source),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe),str(ROOT/'tests/fixtures/epg/synthetic_logo_1_groups_v2.bin')],check=True)
