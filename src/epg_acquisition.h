#pragma once
// DEBUG-only, first-observation timestamps. No allocations and no RF claims.
#include <stdint.h>
namespace epg_acq {
struct Path {
 uint32_t sid=0,cid=0,at[9]={},origin=0;
 uint16_t known=0;
 bool dirty=false;
 void mark(unsigned point,uint32_t now) {
  if(point>=9 || (known&(1U<<point)))return;
  if(!known)origin=now;
  at[point]=now;known|=uint16_t(1U<<point);dirty=true;
 }
};
struct Trace {
 Path paths[2];
 uint32_t audioSid=0,audioCid=0,printedAt=0;
 unsigned owner=0;
 bool enabled=false;
 void enable(bool value){if(value!=enabled){*this=Trace{};enabled=value;}}
 void reset(){const bool on=enabled;*this=Trace{};enabled=on;}
 void audio(uint32_t sid,uint32_t cid,uint32_t now) {
  if(!enabled)return;
  if(audioSid && (audioSid!=sid || audioCid!=cid))reset();
  audioSid=sid;audioCid=cid;
  for(auto& p:paths)p.mark(0,now);
 }
 void endpoint(unsigned path,uint32_t sid,uint32_t cid,uint32_t now) {
  if(!enabled || path>1 || !sid)return;
  auto& p=paths[path];
  if(p.sid!=sid || p.cid!=cid){
   const bool audioKnown=p.known&1;const uint32_t audioAt=p.at[0];p=Path{};
   if(audioKnown)p.mark(0,audioAt);
   p.sid=sid;p.cid=cid;
  }
  p.mark(1,now);
 }
 void mark(unsigned path,unsigned point,uint32_t now){if(enabled && path<2)paths[path].mark(point,now);}
};
inline Trace& trace(){static Trace value;return value;}
} // namespace epg_acq
