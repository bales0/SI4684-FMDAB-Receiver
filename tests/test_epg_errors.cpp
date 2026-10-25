#include "../src/epg_core.h"
#include "../src/epg_demo.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <memory>
#include <fstream>
using Bytes=std::vector<uint8_t>;
static void crc(Bytes& b){const uint16_t c=epg::crc16(b.data(),b.size()-2);b[b.size()-2]=uint8_t(c>>8);b.back()=uint8_t(c);}
static Bytes group(uint16_t tid,unsigned segment,bool last,const uint8_t* data,size_t n){
 Bytes b{0x74,0,uint8_t((segment>>8)|(last?0x80:0)),uint8_t(segment),0x12,uint8_t(tid>>8),uint8_t(tid),uint8_t(n>>8),uint8_t(n)};
 b.insert(b.end(),data,data+n);b.push_back(0);b.push_back(0);crc(b);return b;
}
static std::vector<Bytes> piGroups(){
 std::vector<Bytes> result;
 for(const auto& g:epgDemoGroups)if((g.data[0]&15)==4 && g.data[5]==0xf6 && g.data[6]==0x29)result.emplace_back(g.data,g.data+g.size);
 return result;
}
static void directory(epg::Core& c){
 for(int i=int(sizeof(epgDemoGroups)/sizeof(epgDemoGroups[0]))-1;i>=0;--i){const auto& g=epgDemoGroups[i];if((g.data[0]&15)==6) assert(c.feed(g.data,g.size,1)==epg::Feed::Accepted);}
 assert(c.entryCount()==70);
}
static void feed(epg::Core& c,const Bytes& data,uint16_t tid=0xf629){
 for(size_t pos=0,segment=0;pos<data.size();pos+=949,++segment){const size_t n=data.size()-pos<949?data.size()-pos:949;auto b=group(tid,unsigned(segment),pos+n==data.size(),data.data()+pos,n);c.feed(b.data(),b.size(),100);while(c.busy())c.pump(1);}
}
static void put(uint8_t* p,unsigned at,unsigned n,uint32_t v){for(unsigned i=0;i<n;++i){const unsigned bit=at+i;p[bit/8]|=uint8_t(((v>>(n-1-i))&1)<<(7-bit%8));}}
static void timeTests(){
  for(bool longForm:{false,true})for(int offset:{-60,0,60,120}){
    uint8_t b[7]={};const uint32_t expected=epg::epoch(2024,3,31,1,30,longForm?59:0);
    put(b,1,17,expected/86400+40587);put(b,20,1,longForm);put(b,19,1,1);put(b,21,5,1);put(b,26,6,30);
    if(longForm)put(b,32,6,59);
    b[longForm?6:4]=uint8_t((offset<0?32:0)+(offset<0?-offset:offset)/30);
    uint32_t decoded=0;int16_t lto=0;assert(epg::timePoint(b,longForm?7:5,decoded,lto)&&decoded==expected&&lto==offset);
    int y;unsigned m,d;epg::calendar(uint32_t(int64_t(decoded)+lto*60),y,m,d);assert(y==2024&&m==3&&d==31);
    assert(!epg::timePoint(b,longForm?6:4,decoded,lto));
  }
  const uint8_t impossible[]={0x3b,0x34,0x96,0x00,0x02};uint32_t utc=0;int16_t off=0;
  assert(!epg::timePoint(impossible,sizeof(impossible),utc,off));
  uint64_t id=0;const uint8_t data[]={0x50,0xe2,0x31,0x53,0x12,0x34,0x56,0x78};
  assert(epg::bearerValue(data,sizeof(data),id)&&id==0x50e2315312345678ULL);
  assert(!epg::bearerValue(data,6,id));
  assert(!epg::parseIso("4294969320-11-12T04:30:00Z",utc,off));
  assert(!epg::parseIso("2024-11-12T04:30:00+15:00",utc,off));
  assert(epg::parseIso("2024-12-31T23:45:00-01:00",utc,off));int y;unsigned m,d;epg::calendar(utc,y,m,d);assert(y==2025&&m==1&&d==1);
}
int main(int argc,char** argv){
 timeTests();
 auto groups=piGroups();assert(groups.size()==7);Bytes body;
 for(const auto& g:groups)body.insert(body.end(),g.begin()+9,g.end()-2);
 assert(body.size()==5866);
 auto c=std::unique_ptr<epg::Core>(new epg::Core);
 auto reset=[&](){c->reset();c->select(0x40e231533211ULL,epg::epoch(2024,11,12,3,30),60);directory(*c);c->focus(0xf629);};
 reset();auto bad=groups[0];bad.back()^=1;assert(c->feed(bad.data(),bad.size(),1)==epg::Feed::Rejected);
 bad=groups[0];bad[8]^=1;crc(bad);assert(c->feed(bad.data(),bad.size(),1)==epg::Feed::Rejected);
 bad=groups[0];bad[0]=0x75;crc(bad);assert(c->feed(bad.data(),bad.size(),1)==epg::Feed::Rejected);
 assert(c->count()==0 && c->entryCount()==70);
 // Reversed segments and a conflicting validated duplicate retain original bytes.
 reset();auto b=groups.back();assert(c->feed(b.data(),b.size(),1)==epg::Feed::Accepted);
 b=groups[1];assert(c->feed(b.data(),b.size(),1)==epg::Feed::Accepted);
 assert(c->feed(b.data(),b.size(),1)==epg::Feed::Accepted);b[20]^=1;crc(b);
 assert(c->feed(b.data(),b.size(),1)==epg::Feed::Rejected && c->stats.conflict==1);
 for(int i=int(groups.size())-2;i>=0;--i){c->feed(groups[i].data(),groups[i].size(),1);while(c->busy())c->pump(1);}
 assert(c->stats.pi==1 && c->stats.programmes==42 && c->current()>=0);
 bool overlap=false;const int selected=c->current(&overlap);assert(selected>=0);
 for(size_t i=0;i<c->count();++i){const auto& p=c->at(i);if(!p.uncertain && p.start<=c->now() && uint64_t(p.start)+p.duration>c->now())assert(p.start<=c->at(selected).start);}
 c->expire(900102);assert(!c->fresh() && c->current()==-1 && c->next()==-1);
 // Full TLV validation precedes any programme/cache publication.
 for(unsigned fault=0;fault<4;++fault){
  reset();auto bytes=body;
  if(fault==0){bytes[1]=0xff;bytes[2]=bytes[3]=bytes[4]=0xff;}
  if(fault==1){auto it=bytes.begin();while(it!=bytes.end() && *it!=0xc5)++it;assert(it+1<bytes.end());*(it+1)=0xff;}
  if(fault==2){assert(bytes[4]==4);bytes[7]=0xff;}
  if(fault==3){
   size_t pos=0;
   for(unsigned level=0;level<25;++level){const size_t remaining=bytes.size()-pos-4;bytes[pos++]=2;bytes[pos++]=254;bytes[pos++]=uint8_t(remaining>>8);bytes[pos++]=uint8_t(remaining);}
  }
  feed(*c,bytes);assert(c->stats.invalid && c->stats.pi==0 && c->count()==0 && c->entryCount()==70);
 }
 // Valid PNG/DG versus corrupt PNG chunk with otherwise correct group CRC.
 if(argc==2){
  std::ifstream f(std::string(argv[1])+"/4_B38D.bin",std::ios::binary);assert(f.good());Bytes png((std::istreambuf_iterator<char>(f)),{});
  reset();c->focus(0xB38D);auto damaged=png;damaged[40]^=1;feed(*c,damaged,0xB38D);assert(!c->stats.png && c->stats.invalid);
  feed(*c,png,0xB38D);assert(c->stats.png==1);
 }
 // Correctly CRC-protected hostile group lengths/indexes must never overrun.
 struct Guard{uint64_t before=0x123456789abcdefULL;epg::Core core;uint64_t after=0xfedcba987654321ULL;};
 auto guard=std::unique_ptr<Guard>(new Guard);directory(guard->core);guard->core.focus(0xf629);
 uint32_t random=1;
 for(unsigned i=0;i<2000;++i){random=random*1664525U+1013904223U;unsigned n=1+(random%1000);Bytes bytes(n);for(auto& v:bytes){random=random*1664525U+1013904223U;v=uint8_t(random>>24);}auto g=group(0xf629,random%80,(random&1)!=0,bytes.data(),bytes.size());guard->core.feed(g.data(),g.size(),i);for(unsigned step=0;guard->core.busy()&&step<10000;++step)guard->core.pump(1);assert(!guard->core.busy());}
 assert(guard->before==0x123456789abcdefULL && guard->after==0xfedcba987654321ULL);
 std::puts("EPG hostile CRC/length/TID/duplicate/reorder, UTF8/TLV/depth, PNG CRC, historical/freshness and buffer canaries: PASS");
}
