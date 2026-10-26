#include "../src/vendor/si468x/Si468x.h"
#include "../src/dab_spi_probe.h"
#include "../src/dab_receive_health.h"
#include <cassert>
#include <cstdio>
#include <cstring>


struct PreservedWire {
 uint8_t reply[51]={};uint32_t now=0;unsigned commands=0,reads=0,callbacks=0;
 bool queued=true,failRead=false;dab_health::Dsrv pending;
 static bool write(void* ctx,uint8_t cmd,const uint8_t* args,uint16_t n){
  auto& w=*static_cast<PreservedWire*>(ctx);assert(cmd==0x84 && n==1 && *args==1);++w.commands;
  memset(w.reply,0,sizeof w.reply);w.reply[0]=0x80;
  if(w.queued){w.reply[18]=27;w.reply[7]=60;w.reply[24]=0x36;w.reply[25]=0;w.queued=false;}
  return true;
 }
 static bool read(void* ctx,uint8_t* p,uint16_t n){auto& w=*static_cast<PreservedWire*>(ctx);++w.reads;if(w.failRead)return false;assert(n<=sizeof w.reply);memcpy(p,w.reply,n);return true;}
 static uint32_t time(void* ctx){return static_cast<PreservedWire*>(ctx)->now;}
 static void status(void* ctx,const si468x::Status& s){auto& w=*static_cast<PreservedWire*>(ctx);++w.callbacks;w.pending.status(s.dsrvInt());}
};
static void preservedReplyTest(){
 using namespace si468x;PreservedWire w;HostInterface host;host.context=&w;host.writeCommand=PreservedWire::write;host.readReply=PreservedWire::read;host.timeUs=PreservedWire::time;
 Si468x chip(host);chip.setStatusCallback(PreservedWire::status,&w);
 uint8_t header[24],full[51],ack=1;w.pending.status(true);w.pending.start(0);
 assert(chip.startCommand(Command::GET_DIGITAL_SERVICE_DATA,&ack,1,header,24,100000)==Result::Pending);
 assert(chip.startCommand(Command::GET_DIGITAL_SERVICE_DATA,&ack,1,header,24,100000)==Result::Busy);
 assert(chip.service()==Result::Ok);assert(chip.readCurrentReply(full,51)==Result::Ok && w.commands==1);
 uint8_t copy[51];memcpy(copy,full,51);assert(chip.readCurrentReply(full,51)==Result::Ok && !memcmp(full,copy,51) && w.commands==1);
 w.pending.finish(1,0,27,true);assert(!w.pending.pending); // no new event -> no second GET
 assert(w.callbacks>=3); // header/full read both call status callback
 // A new event can deliver exactly the same content in a different transaction.
 w.queued=true;w.pending.status(true);w.pending.start(2);
 assert(chip.startCommand(Command::GET_DIGITAL_SERVICE_DATA,&ack,1,header,24,100000)==Result::Pending && chip.service()==Result::Ok);
 assert(chip.readCurrentReply(full,51)==Result::Ok && !memcmp(full,copy,51) && w.commands==2);
 w.failRead=true;assert(chip.readCurrentReply(full,51)==Result::TransportError);
 w.failRead=false;w.pending.status(true);w.pending.start(3);
 assert(chip.startCommand(Command::GET_DIGITAL_SERVICE_DATA,&ack,1,header,24,100000)==Result::Pending && chip.service()==Result::Ok);
 DsrvHeader h;assert(Si468x::parseDsrvHeader(header,24,h)==Result::Ok && h.byteCount==0);
 w.pending.finish(4,0,0,true);assert(!w.pending.pending && w.commands==3);
 std::puts("Actual vendor preserved reply: header/full callbacks, repeat reads vs new GET, Busy, read transport error, empty drain ACK: PASS");
}
int main() {
 preservedReplyTest();
  // Synthetic parser fixtures, not evidence of firmware UATYPE support.
  const uint16_t rawValues[] = {0, 2, 7, 0xABCD};
  const dab_probe::Endpoint audio(0x2FD9, 0x01FE002A, true);
  const dab_probe::Endpoint sls(0x12345678, 0x00F0E321, true);
  const dab_probe::Endpoint spi;
  for (unsigned source = 0; source < 3; ++source) {
    for (uint16_t raw : rawValues) {
      uint8_t reply[27] = {};
      reply[0] = 0x80;
      reply[4] = 1; reply[5] = 3; reply[6] = 3;
      reply[7] = static_cast<uint8_t>((source << 6) | (source == 2 ? 0 : 60));
      const uint32_t sid = source == 0 ? sls.sid : audio.sid;
      const uint32_t cid = source == 0 ? sls.cid : 0x2A;
      for (unsigned i = 0; i < 4; ++i) {
        reply[8+i] = static_cast<uint8_t>(sid >> (8*i));
        reply[12+i] = static_cast<uint8_t>(cid >> (8*i));
      }
      reply[16] = static_cast<uint8_t>(raw); reply[17] = raw >> 8;
      reply[18] = 3; reply[20] = 0x34; reply[21] = 0x12;
      reply[22] = 0x78; reply[23] = 0x56;
      reply[24] = 0xFF; reply[25] = 0xD8; reply[26] = 0x42;
      uint8_t saved[sizeof(reply)]; std::memcpy(saved, reply, sizeof(reply));
      si468x::DsrvHeader header;
      for (size_t length = 0; length < 24; ++length) {
        header.rawUatypeOrRfu = 0x9876;
        assert(si468x::Si468x::parseDsrvHeader(reply, length, header) ==
               si468x::Result::MalformedReply);
        assert(header.rawUatypeOrRfu == 0x9876);
      }
      assert(si468x::Si468x::parseDsrvHeader(nullptr, 24, header) ==
             si468x::Result::MalformedReply);
      assert(si468x::Si468x::parseDsrvHeader(reply, 24, header) == si468x::Result::Ok);
      assert(header.rawUatypeOrRfu == raw);
      assert(header.dataSource == source && header.dscType == (source == 2 ? 0 : 60));
      assert(header.interruptSource == 1 && header.buffersRemaining == 3);
      assert(header.serviceState == 3 && header.serviceId == sid && header.componentId == cid);
      assert(header.byteCount == 3 && header.segmentNumber == 0x1234);
      assert(header.numberOfSegments == 0x5678);
      assert(std::memcmp(saved, reply, sizeof(reply)) == 0);
      assert(reply[24] == 0xFF && reply[25] == 0xD8 && reply[26] == 0x42);
      const auto route = dab_probe::route(header.serviceId, header.componentId,
                                          header.dataSource, audio, sls, spi);
      assert(route == (source == 0 ? dab_probe::Route::Sls : dab_probe::Route::Audio));
      assert(dab_probe::feedLegacyDecoder(route, false, true));
    }
  }
  std::puts("DSRV raw field, short replies, offsets, payload and source routing: PASS");
}
