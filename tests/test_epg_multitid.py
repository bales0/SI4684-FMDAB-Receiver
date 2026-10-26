"""Real Core with interleaved/reversed PI/SI/PNG and two bounded RX buffers."""
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[1];out=ROOT/'.pio/epg_multitid';out.mkdir(exist_ok=True)
cpp=r'''
#include "epg_core.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
using Data=std::vector<uint8_t>;
static uint32_t now=1;
static Data group(uint16_t tid,unsigned segment,bool last,const Data& bytes){
 Data f{0x74,0,uint8_t((segment>>8)|(last?0x80:0)),uint8_t(segment),0x12,uint8_t(tid>>8),uint8_t(tid),uint8_t(bytes.size()>>8),uint8_t(bytes.size())};
 f.insert(f.end(),bytes.begin(),bytes.end());const auto c=epg::crc16(f.data(),f.size());f.push_back(uint8_t(c>>8));f.push_back(uint8_t(c));return f;
}
struct Object{unsigned tid=0;Data bytes;std::vector<Data> parts;};
static epg::Feed feed(epg::Core& c,const Data& f){return c.feed(f.data(),f.size(),now++);}
static void pump(epg::Core& c){for(unsigned i=0;c.busy() && i<10000;++i)c.pump(1);assert(!c.busy());}
int main(int argc,char** argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);std::vector<Data> frames;
 while(in.peek()!=EOF){uint8_t h[2];in.read(reinterpret_cast<char*>(h),2);Data f(h[0]+256U*h[1]);in.read(reinterpret_cast<char*>(f.data()),f.size());assert(in);frames.push_back(f);}
 epg::Core c;auto load=[&](){c.reset();for(const auto& f:frames)if((f[0]&15)==6)assert(feed(c,f)==epg::Feed::Accepted);};load();
 Object pi,si,png;uint64_t bearer=0;
 for(size_t i=0;i<c.entryCount();++i){const auto& e=c.entries()[i];Object* obj=e.type==2?&png:e.subtype==1?&pi:&si;obj->tid=e.tid;if(e.subtype==1)bearer=e.bearer;}
 for(const auto& f:frames)if((f[0]&15)==4){unsigned tid=unsigned(f[5])*256+f[6];Object* o=tid==pi.tid?&pi:tid==si.tid?&si:&png;o->bytes.insert(o->bytes.end(),f.begin()+9,f.end()-2);}
 for(auto* o:{&pi,&si,&png})for(size_t offset=0;offset<o->bytes.size();offset+=23){const size_t n=o->bytes.size()-offset<23?o->bytes.size()-offset:23;o->parts.push_back(group(uint16_t(o->tid),unsigned(offset/23),offset+n==o->bytes.size(),Data(o->bytes.begin()+offset,o->bytes.begin()+offset+n)));}
 auto select=[&](){c.select(bearer,epg::epoch(2024,11,12,1,1),0);};select();
 for(bool reversed:{false,true}){
  load();select();
  for(size_t i=0;i<pi.parts.size() || i<si.parts.size();++i){
   if(i<pi.parts.size())assert(feed(c,pi.parts[reversed?pi.parts.size()-1-i:i])==epg::Feed::Accepted);
   now+=6001; // Real fragments survive gaps longer than the old preference timer.
   if(i<si.parts.size())assert(feed(c,si.parts[reversed?si.parts.size()-1-i:i])==epg::Feed::Accepted);
  }
  assert(c.slotHighWater==2 && c.busy());
  // No buffer used by the TLV pump can be overwritten by another TID/read.
  const auto pinned=c.reception(c.decoding(0)?0:1).tid;assert(feed(c,pinned==pi.tid?pi.parts[0]:si.parts[0])==epg::Feed::Busy);
  for(const auto& f:frames)if((f[0]&15)==6)assert(feed(c,f)==epg::Feed::Busy);
  pump(c);assert(c.hasSI() && c.count()==3 && c.current()>=0);
  for(size_t i=0;i<png.parts.size();++i){assert(feed(c,png.parts[reversed?png.parts.size()-1-i:i])==epg::Feed::Accepted);pump(c);}
  assert(c.logoSize()==551 && c.slotHighWater==2);
  const auto count=c.count();const auto dirs=c.stats.directories;const std::string title=c.title(0);
  for(unsigned turn=0;turn<3;++turn)for(const auto& f:frames){feed(c,f);pump(c);}
  assert(c.count()==count && c.stats.directories==dirs && title==c.title(0) && c.logoSize()==551);
 }
 // A third object waits; no eviction just because preference changed after 5s.
 load();select();for(const auto& f:si.parts){feed(c,f);pump(c);}assert(c.hasSI());c.browseDay(1);
 assert(feed(c,pi.parts[0])==epg::Feed::Accepted);assert(feed(c,si.parts[0])==epg::Feed::Accepted);
 const auto firstMask=c.assembly(0).mask,secondMask=c.assembly(1).mask;
 auto damaged=pi.parts[0];damaged.back()^=1;assert(feed(c,damaged)==epg::Feed::Rejected);
 assert(c.assembly(0).mask==firstMask && c.assembly(1).mask==secondMask);
 assert(feed(c,png.parts[0])==epg::Feed::Accepted && c.reception(0).skipped && c.reception(1).skipped);
 for(unsigned i=0;i<3;++i){now+=10000;feed(c,pi.parts[0]);feed(c,si.parts[0]);c.expire(now);assert(c.assembly(0).mask==firstMask && c.assembly(1).mask==secondMask);}
 assert(c.reception(0).duplicates && c.reception(1).duplicates);
 now+=300001;feed(c,pi.parts[0]);feed(c,si.parts[0]);c.expire(now);
 assert(!c.assembly(0).active && !c.assembly(1).active && c.reception(0).evictions && c.reception(1).evictions);
 // Explicit context invalidation clears both slots, never combines equal TIDs.
 load();select();feed(c,pi.parts[0]);feed(c,si.parts[0]);c.reset();assert(!c.assembly(0).active && !c.assembly(1).active && !c.count());
 // Assembly itself now refuses silent replacement by another transport/TID.
 epg::Assembly a;epg::Stats stats;uint8_t memory[16]={};Data bytes{1,2,3};assert(a.add(1,0,false,bytes.data(),3,memory,16,0,stats)==0);
 assert(a.add(2,1,true,bytes.data(),3,memory,16,1,stats)==-1 && a.tid==1 && a.mask==1);
 std::puts("Multi-TID: interleaved/reversed PI+SI, >5s gaps, pinned pump, own PNG, cache, CRC, pressure/progress expiry/context: PASS");
}
'''
source=out/'multitid.cpp';source.write_text(cpp,encoding='utf-8');exe=out/'multitid.exe'
subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'g++','-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(source),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe),str(ROOT/'tests/fixtures/epg/synthetic_logo_1_groups_v2.bin')],check=True)
