#include "../src/epg_uart.h"
#include "../src/epg_demo.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
using Result=epg::UartReceiver::Result;
static std::string message(unsigned seq,unsigned offset,unsigned total,const uint8_t* bytes,size_t n,
                           unsigned sid=0xE23153FE,unsigned cid=0xC00C,unsigned source=0,unsigned session=0x1234) {
  char header[100]; std::snprintf(header,sizeof(header),"E1 %04X %08X %04X %04X %08X %08X %1X ",session,seq,offset,total,sid,cid,source);
  std::string value=header;
  for(size_t i=0;i<n;++i) { char hex[3]; std::snprintf(hex,sizeof(hex),"%02X",bytes[i]); value+=hex; }
  char crc[12]; std::snprintf(crc,sizeof(crc)," %08X",epg::crc32(reinterpret_cast<const uint8_t*>(value.data()),value.size())); value+=crc;
  assert(value.size()<127); return value;
}
int main() {
  assert(epg::Core::memoryBytes()<=50*1024);
  static_assert(sizeof(epg::UartReceiver)<4200,"bounded UART receiver");
  auto core=std::unique_ptr<epg::Core>(new epg::Core);
  epg::UartReceiver rx;
  const auto& g=epgDemoGroups[0];
  auto first=message(1,0,g.size,g.data,32);
  rx.reset(0xE23153FE,0x01F2C00C,0x1234);
  assert(rx.receive(first.c_str(),*core,1)==Result::Ack);
  assert(rx.receive(first.c_str(),*core,1)==Result::Ack && core->stats.frames==0);
  auto corrupt=first; corrupt[corrupt.size()-1]^=1;
  assert(rx.receive(corrupt.c_str(),*core,1)==Result::Crc);
  auto changed=message(1,0,g.size,g.data+1,32);
  assert(rx.receive(changed.c_str(),*core,1)==Result::Order);
  auto skipped=message(3,64,g.size,g.data+64,32);
  assert(rx.receive(skipped.c_str(),*core,1)==Result::Order);
  for(unsigned source=1;source<256;++source) {
    if(source>15) break; // framed source field is a single hex digit
    auto bad=message(2,32,g.size,g.data+32,32,0xE23153FE,0xC00C,source);
    assert(rx.receive(bad.c_str(),*core,1)==Result::Identity);
  }
  for(unsigned sid:{0xE23153FFU,0x00003233U}) {
    auto bad=message(2,32,g.size,g.data+32,32,sid);
    assert(rx.receive(bad.c_str(),*core,1)==Result::Identity);
  }
  for(unsigned cid:{0xC00DU,0x02F2C00CU,0x0000000CU,0x0000800CU}) {
    auto bad=message(2,32,g.size,g.data+32,32,0xE23153FE,cid);
    assert(rx.receive(bad.c_str(),*core,1)==Result::Identity);
  }
  auto old=message(2,32,g.size,g.data+32,32,0xE23153FE,0xC00C,0,0x1233);
  assert(rx.receive(old.c_str(),*core,1)==Result::Order);
  auto large=message(2,32,0xffff,g.data+32,32);
  assert(rx.receive(large.c_str(),*core,1)==Result::Format);
  rx.expire(5002);
  auto next=message(2,32,g.size,g.data+32,32);
  assert(rx.receive(next.c_str(),*core,5003)==Result::Order);
  // Full canonical CID retains exact-match behavior for standard data only.
  rx.reset(0xE23153FE,0x01F2C00C,0x1234);
  auto full=message(1,0,g.size,g.data,32,0xE23153FE,0x01F2C00C);
  assert(rx.receive(full.c_str(),*core,1)==Result::Ack);
  // Complete a bad Data Group: envelope CRC passes, inner CRC must reject it.
  rx.reset(0xE23153FE,0x01F2C00C,0x1234);
  std::string data(reinterpret_cast<const char*>(g.data),g.size); data.back()^=1;
  Result result=Result::Ack;
  for(size_t pos=0,seq=1;pos<data.size();pos+=32,++seq) {
    const size_t n=data.size()-pos<32?data.size()-pos:32;
    auto line=message(unsigned(seq),unsigned(pos),g.size,reinterpret_cast<const uint8_t*>(data.data()+pos),n);
    result=rx.receive(line.c_str(),*core,1);
  }
  assert(result==Result::Data && core->stats.crcBad==1 && core->entryCount()==0);
  // Actual built-in packet stream uses the same MOT/TLV core, never static text.
  core->reset(); core->select(0x40e231533211ULL,epg::epoch(2024,11,12,3,30),60);
  for(const auto& group:epgDemoGroups) {
    while(core->busy()) core->pump(1);
    if((group.data[0]&15)==4) core->focus(uint16_t(group.data[5])<<8|group.data[6]);
    assert(core->feed(group.data,group.size,1)==epg::Feed::Accepted);
  }
  assert(core->busy());
  rx.reset(0xE23153FE,0x01F2C00C,0x1234);
  assert(rx.receive(first.c_str(),*core,1)==Result::Busy && rx.sequence()==0);
  while(core->busy()) core->pump(1);
  assert(core->stats.si==1 && core->stats.pi==1 && core->stats.programmes==42);
  assert(core->current()>=0 && core->next()>=0);
  assert(rx.receive(first.c_str(),*core,1)==Result::Ack);
  const uint32_t now=core->now(); core->browseDay(1);
  assert(core->dayOffset()==1 && core->now()==now && !core->count());
  core->setTime(epg::epoch(2026,10,23),0); assert(core->current()==-1);
  std::puts("UART v1 bounds/CRC/sequence/session/source/CID/duplicate/expiry/BUSY + actual DEMO: PASS");
}
