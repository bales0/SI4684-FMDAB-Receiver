#include "../src/epg_core.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
static void hex(const char* s) { for(;*s;++s) std::printf("%02x",static_cast<unsigned char>(*s)); }
static void programme(void*, uint16_t tid, const epg::Programme& p) {
  std::printf("P\t%04X\t%llx\t%u\t%u\t%d\t%u\t%u\t",tid,static_cast<unsigned long long>(p.bearer),p.start,p.duration,p.lto,p.id,p.version);
  hex(p.medium); std::printf("\t"); hex(p.name); std::printf("\t"); hex(p.description); std::printf("\t");
  for(unsigned i=0;i<p.rejectedTimeLength;++i) std::printf("%02x",p.rejectedTime[i]);
  std::printf("\n");
}
static void service(void*,uint16_t tid,const epg::Service& s) {
  std::printf("S\t%04X\t%llx\t%u\t",tid,static_cast<unsigned long long>(s.bearer),s.png);
  hex(s.name); std::printf("\t"); hex(s.logo); std::printf("\n");
}
static std::string directory;
static void object(void*, uint16_t tid, uint8_t type, epg::Bytes data, size_t n) {
  char path[512]; std::snprintf(path,sizeof(path),"%s/%u_%04X.bin",directory.c_str(),type,tid);
  std::ofstream f(path,std::ios::binary); assert(f.good()); std::vector<uint8_t> bytes(n); data.copyTo(bytes.data(),n); f.write(reinterpret_cast<const char*>(bytes.data()),n);
}
int main(int argc,char** argv) {
  assert(epg::crc16(reinterpret_cast<const uint8_t*>("123456789"),9)==0xd64e);
  assert(epg::crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926);
  uint32_t utc=0; int16_t offset=0;
  assert(epg::parseIso("2024-11-12T04:30:00+01:00",utc,offset));
  assert(utc==epg::epoch(2024,11,12,3,30) && offset==60);
  assert(!epg::parseIso("2024-02-30T04:30:00Z",utc,offset));
  assert(!epg::parseIso("2024-11-12T04:30:00",utc,offset));
  int y; unsigned m,d; epg::calendar(epg::epoch(2024,2,29),y,m,d); assert(y==2024 && m==2 && d==29);
  epg::Assembly assembly; epg::Stats stats; uint8_t buffer[16]={};
  const uint8_t a[]={1,2,3,4}, b[]={5,6,7,8}, c[]={9,10};
  assert(assembly.add(1,2,true,c,2,buffer,sizeof(buffer),1,stats)==0);
  assert(assembly.add(1,1,false,b,4,buffer,sizeof(buffer),2,stats)==0);
  assert(assembly.add(1,1,false,b,4,buffer,sizeof(buffer),2,stats)==0);
  assert(assembly.add(1,1,false,a,4,buffer,sizeof(buffer),2,stats)==-1);
  assert(assembly.add(1,0,false,a,4,buffer,sizeof(buffer),3,stats)==1);
  assert(assembly.size()==10 && !std::memcmp(buffer,a,4) && !std::memcmp(buffer+4,b,4) && !std::memcmp(buffer+8,c,2));
  auto core=std::unique_ptr<epg::Core>(new epg::Core);
  uint8_t bad[12]={}; assert(core->feed(bad,sizeof(bad),0)==epg::Feed::Rejected);
  assert(core->stats.crcBad==1);
  std::fprintf(stderr,"EPG core size=%zu bytes, C++11 synthetic bounds/CRC/time/reorder: PASS\n",epg::Core::memoryBytes());
  if(argc!=3) return 0;
  directory=argv[2]; std::ifstream f(argv[1],std::ios::binary); assert(f.good());
  core->observer.service=service; core->observer.programme=programme; core->observer.object=object;
  core->select(0x40e231533233ULL,epg::epoch(2024,11,12,3,30),60);
  std::vector<std::vector<uint8_t> > frames;
  while(true) {
    uint8_t h[10]; f.read(reinterpret_cast<char*>(h),10); if(!f.gcount()) break; assert(f.gcount()==10);
    const unsigned n=h[8]|(unsigned(h[9])<<8); assert(n<=epg::kMaxGroup);
    std::vector<uint8_t> data(n); f.read(reinterpret_cast<char*>(data.data()),n); assert(unsigned(f.gcount())==n);
    frames.push_back(data);
  }
  // Directory itself is assembled out of order from repeated CRC-valid groups.
  for(const auto& data:frames) if((data[0]&15)==6) core->feed(data.data(),data.size(),0);
  assert(core->entryCount()==70);
  std::vector<uint16_t> ids;
  for(size_t i=0;i<core->entryCount();++i) ids.push_back(core->entries()[i].tid);
  unsigned objects=0, pi=0, si=0, png=0, programmes=0;
  for(uint16_t tid:ids) {
    core->reset(); core->focus(tid);
    uint32_t clock=0;
    for(const auto& data:frames) {
      core->feed(data.data(),data.size(),clock+=50);
      while(core->busy()) core->pump();
    }
    const auto& s=core->stats;
    objects+=s.objects; pi+=s.pi; si+=s.si; png+=s.png; programmes+=s.programmes;
    if(s.objects!=1) std::fprintf(stderr,"MISSING %04X invalid=%u conflicts=%u\n",tid,s.invalid,s.conflict);
  }
  std::fprintf(stderr,"Bounded target acquisition: objects=%u PI=%u SI=%u PNG=%u programmes=%u\n",objects,pi,si,png,programmes);
  assert(objects==70 && pi==35 && si==5 && png==30 && programmes==1087);
  core->reset(); core->focus(0);
  core->observer=epg::Observer{};
  core->select(0x40e231533233ULL,epg::epoch(2024,11,12,3,30),60);
  uint32_t clock=0;
  for(unsigned turn=0;turn<2;++turn) for(const auto& data:frames) {
    core->feed(data.data(),data.size(),clock+=50);
    while(core->busy()) core->pump();
  }
  std::fprintf(stderr,"Selected station/day: cache=%zu NOW=%d NEXT=%d limited=%d ambiguous=%d\n",core->count(),core->current(),core->next(),core->limited,core->ambiguous());
  assert(core->count() && core->current()>=0 && core->next()>=0);
}
