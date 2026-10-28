#pragma once
// Bounded, allocation-free transport evidence. This is not a second MOT decoder.
#include "epg_core.h"
#include "epg_acquisition.h"
namespace epg_diag {
enum class Path:uint8_t { Packet, Pad };
enum class Reject:uint8_t { Ua, Generation, Sid, Cid, Inactive, Dscty, State, Physical, Length, Read, Replay, Owner, Data, NoEndpoint, Busy, Count };
enum class Reset:uint8_t { Tune, Service, PacketEndpoint, PacketToPad, PadToPacket, PadEndpoint, UaRevoked, ReplayStart, ReplayStop, PadAcquire, Count };
inline const char* rejectName(unsigned n) {
 static const char* const names[]={"ua","generation","sid","cid","inactive","dscty","state","physical","length","read","replay","owner","data","no-endpoint","busy"};return names[n];
}
inline const char* resetName(unsigned n) {
 static const char* const names[]={"tune","service","packet-endpoint","packet->PAD","PAD->packet","pad-endpoint","ua-revoked","replay-start","replay-stop","PAD-acquire"};return names[n];
}
inline void increment(uint32_t& n) {if(n!=UINT32_MAX)++n;}
struct Counters {
 uint32_t sid=0,cid=0,headers[4]={},candidates=0,admitted=0,blocked=0,legacy=0,rejected[unsigned(Reject::Count)]={};
 uint32_t lastSid=0,lastCid=0,generation=0;uint16_t ua=0,bytes=0;uint8_t source=0,dscty=0,state=0;
 uint32_t groups=0,validGroups=0,crcBad=0,invalid=0,objectGroups=0,dirGroups=0,dirs=0,unique=0,repeats=0,empty=0;
 uint32_t endpointMs=0,lastGroupMs=0,lastHeaderMs=0;
 uint32_t hash=0,dirBytes=0,objects=0,notedAtGroup=UINT32_MAX;uint16_t tid=0;
 uint32_t si=0,pi=0,png=0,programmes=0,hashes[4]={};
 uint8_t historyUsed=0;bool historyLimited=false;
};
struct Registry {
 Counters paths[2];uint32_t resets[unsigned(Reset::Count)]={},totalResets=0,epoch=0,stateChanges=0;
 uint8_t lastReset=0,lastState=255;
 uint32_t headerErrors=0;
 Counters& at(Path p){return paths[unsigned(p)];}
 void endpoint(Path p,uint32_t sid,uint32_t cid,uint32_t now=0){
  auto& c=at(p);if(c.sid && (c.sid!=sid || c.cid!=cid))c=Counters{};if(!c.sid)c.endpointMs=now;c.sid=sid;c.cid=cid;
 }
 void context(){paths[0]=Counters{};paths[1]=Counters{};increment(epoch);}
 void reset(Reset why){increment(totalResets);increment(resets[unsigned(why)]);lastReset=uint8_t(why);}
 void reject(Path path,Reject reason){increment(at(path).rejected[unsigned(reason)]);}
 void header(uint8_t src,uint16_t ua,uint8_t dsc,uint8_t state,uint32_t sid,uint32_t cid,uint16_t bytes,uint32_t generation,uint32_t now=0,bool candidate=false){
  auto& c=at(src==0?Path::Packet:Path::Pad);if(src<4)increment(c.headers[src]);
  if(candidate)increment(c.candidates);
  if(candidate || !c.candidates){c.lastSid=sid;c.lastCid=cid;c.ua=ua;c.dscty=dsc;c.state=state;c.bytes=bytes;c.generation=generation;c.source=src;c.lastHeaderMs=now;}
 }
 void directory(Path p,uint32_t hash,uint32_t bytes,uint16_t objects,uint16_t tid){
  auto& c=at(p);increment(c.dirs);if(!objects)increment(c.empty);
  bool found=false;for(unsigned i=0;i<c.historyUsed;++i)if(c.hashes[i]==hash)found=true;
  if(found)increment(c.repeats);else if(c.historyUsed<4){c.hashes[c.historyUsed++]=hash;increment(c.unique);}else c.historyLimited=true;
  c.hash=hash;c.dirBytes=bytes;c.objects=objects;c.tid=tid;c.notedAtGroup=c.groups;
 }
 void group(Path path,const uint8_t* p,size_t n,bool owned,uint32_t now=0){
  auto& c=at(path);increment(c.groups);
  epg::GroupEnvelope envelope;const auto checked=epg::inspectGroup(p,n,envelope);
  if(checked!=epg::GroupCheck::Valid){if(checked==epg::GroupCheck::Crc)increment(c.crcBad);else increment(c.invalid);return;}
  increment(c.validGroups);c.lastGroupMs=now;
  if(envelope.type==4){increment(c.objectGroups);return;}
  increment(c.dirGroups);
  const size_t offset=envelope.offset,length=envelope.length;
  if(envelope.segment!=0x8000 || length<13)return;
  const uint8_t* body=p+offset;const uint32_t hash=epg::crc32(body,length);
  const uint32_t declared=(uint32_t(body[0])<<24|uint32_t(body[1])<<16|uint32_t(body[2])<<8|body[3]);
  const uint16_t objects=uint16_t(body[4])<<8|body[5],ext=uint16_t(body[11])<<8|body[12];
  // A complete empty directory needs no object-header parser. Nonempty
  // directories are verified only by the owned Core, or an exact known hash.
  const bool emptyValid=objects==0 && declared==length && !(body[0]&0xc0) && !(body[9]&0x80) && 13U+ext==length;
  const bool known=c.unique && hash==c.hash && length==c.dirBytes;
  if(emptyValid || known)directory(path,hash,uint32_t(length),objects,envelope.tid);
  (void)owned; // Counts above are observations; owner is printed separately.
 }
 void object(Path path,uint16_t tid,uint8_t kind,epg::Bytes data,size_t n,uint32_t programmes){
  auto& c=at(path);
  if(kind==6){if(c.notedAtGroup!=c.groups)directory(path,epg::crc32(data,n),uint32_t(n),uint16_t(data[4])<<8|data[5],tid);return;}
  if(n && data[0]==3)increment(c.si);else if(n && data[0]==2){increment(c.pi);c.programmes=programmes;}else increment(c.png);
 }
};
inline bool emptyDirectoryGroup(const uint8_t* p,size_t n) {
 // Ordinary PI/PNG packets need no diagnostic CRC/hash or registry initialization.
 if(!p || n<24 || (p[0]&15)!=6)return false;
 epg::GroupEnvelope h;
 if(epg::inspectGroup(p,n,h)!=epg::GroupCheck::Valid || h.type!=6 || h.segment!=0x8000 || h.length<13)return false;
 const uint8_t* b=p+h.offset;
 const uint32_t declared=uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];
 const uint16_t ext=uint16_t(b[11])<<8|b[12];
 return !b[4] && !b[5] && !(b[0]&0xc0) && !(b[9]&0x80) && declared==h.length && size_t(13)+ext==h.length;
}
inline Registry& registry(){static Registry counters;return counters;}
} // namespace epg_diag
