#include "../src/epg_auto_suspend.h"
#include "../src/epg_core.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace epg_auto;
static Context context(){Context c;c.generation=1;c.request=2;c.audioSid=3;c.audioCid=4;c.sid=5;c.cid=0xc006;return c;}
static Policy active(){Policy p;p.sync(context(),true);assert(p.state==State::Active);return p;}
static void empty(Policy& p,uint32_t base=0){p.directory(base,42,123,0,1);p.directory(base+1000,42,123,0,2);p.directory(base+2000,42,123,0,3);}
static void confirmed(Policy& p,uint32_t base=0){empty(p,base);assert(p.stopDue(base+2000,true));p.stopped(base+2100,true);assert(p.state==State::Cooldown);}
static void recheck(Policy& p){assert(!p.allowStart(p.next-1));assert(p.allowStart(p.next));assert(p.state==State::RecheckStart);p.started(p.next+500,true,false);assert(p.state==State::RecheckActive);}
using Data=std::vector<uint8_t>;
static Data group(const Data& body,uint16_t segment=0x8000,uint8_t continuity=0){
 Data b={0x76,continuity,uint8_t(segment>>8),uint8_t(segment),0x12,0,42,uint8_t(body.size()>>8),uint8_t(body.size())};
 b.insert(b.end(),body.begin(),body.end());auto crc=epg::crc16(b.data(),b.size());b.push_back(uint8_t(crc>>8));b.push_back(uint8_t(crc));return b;
}
static unsigned completions=0;static uint32_t tick=0;
static void directory(void* user,uint16_t tid,uint32_t hash,uint16_t objects,uint32_t cycle){++completions;static_cast<Policy*>(user)->directory(tick,tid,hash,objects,cycle);}
int main(){
 {auto p=active();p.directory(0,42,123,0,1);assert(!p.stopDue(2000,true));p.directory(1000,42,123,0,2);assert(!p.stopDue(2000,true));p.directory(2000,42,123,0,3);assert(p.stopDue(2000,true));assert(!p.stopDue(3000,true));assert(p.stopCount==1);assert(!p.allowStart(999999));assert(p.state==State::StopPending);p.stopped(3000,false);assert(p.state!=State::Cooldown && p.stopError==1);assert(!p.allowStart(999999));p.stopped(3500,true);assert(p.next==303500 && p.confirmedCount==1);}
 {auto p=active();p.directory(0,42,123,0,1);p.directory(1,42,123,0,2);p.directory(2,42,123,0,3);assert(!p.stopDue(2000,true));p.directory(2000,42,123,0,4);assert(p.stopDue(2000,true));}
 {auto p=active();for(unsigned i=0;i<5;++i)p.directory(i*1000,42,123,0,1);assert(p.repeat==1 && !p.stopDue(4000,true));p.directory(40000,42,123,0,2);assert(p.repeat==1);p.directory(41000,43,123,0,3);assert(p.repeat==1);p.directory(42000,43,124,0,4);assert(p.repeat==1);}
 {auto p=active();empty(p);p.directory(2100,42,987,1,4);assert(!p.stopDue(2200,true) && p.nonEmptyValid==1 && p.repeat==0);}
 {auto p=active();confirmed(p);recheck(p);p.content(p.next+1000);assert(p.state==State::Active && p.result==Result::Nonempty);}
 {auto p=active();confirmed(p);const auto deadline=p.next;p.sync(context(),false,true);assert(p.state==State::Cooldown && !p.eligible && !p.allowStart(deadline));p.sync(context(),true);assert(p.state==State::Cooldown && p.next==deadline && p.allowStart(deadline));}
 for(unsigned change=0;change<12;++change){auto p=active();confirmed(p);auto c=context();switch(change){case 0:++c.generation;break;case 1:++c.request;break;case 2:++c.audioSid;break;case 3:++c.audioCid;break;case 4:++c.sid;break;case 5:++c.cid;break;case 6:c.sls=true;break;case 7:++c.slsSid;break;case 8:++c.slsCid;break;case 9:c.mode=1;break;default:break;}p.sync(c,change<10);assert(p.state!=State::Cooldown && !p.stopDue(999999,true));}
 {auto p=active();confirmed(p);recheck(p);const auto started=p.next+500;empty(p,started);assert(p.stopDue(started+2000,true));assert(p.result==Result::Empty);p.stopped(started+2100,true);assert(p.next==started+302100);assert(p.startCount==1 && p.recheckCount==1 && p.stopCount==2 && p.confirmedCount==2);}
 {auto p=active();confirmed(p);recheck(p);p.directory(p.next+600,42,999,7,1);assert(p.state==State::Active && p.result==Result::Nonempty);assert(!p.stopDue(p.next+999999,true));}
 {auto p=active();confirmed(p);recheck(p);const auto started=p.next+500;assert(!p.stopDue(started+29999,true));assert(p.stopDue(started+30000,true));assert(p.result==Result::Inconclusive && p.emptyValid==3);p.stopped(started+30100,true);assert(p.next==started+330100);}
 for(bool uncertain:{false,true}){auto p=active();confirmed(p);assert(p.allowStart(p.next));p.started(p.next,false,uncertain);assert(p.result==Result::StartError);if(uncertain){assert(p.state==State::StopPending && !p.allowStart(9999999));p.stopped(400000,false);assert(p.state==State::StopPending);p.stopped(400500,true);assert(p.state==State::Cooldown);}else{assert(p.state==State::Error && !p.allowStart(p.next-1));}}
 {auto p=active();confirmed(p);p.enable(false);p.sync(context(),true);assert(p.state==State::Disabled && p.allowStart(0));p.enable(true);p.sync(context(),false);assert(p.state==State::Ineligible);empty(p);assert(!p.stopDue(2000,true));}
 {auto p=active();const uint32_t base=UINT32_MAX-1000;confirmed(p,base);assert(p.next==uint32_t(base+302100));recheck(p);assert(!p.stopDue(p.next+30499,true));assert(p.stopDue(p.next+30500,true));}
 // Actual Core envelope/CRC/full segmented validation, unchanged-hash hook,
 // and duplicate callback protection; no diagnostic-counter input.
 {auto p=active();epg::Core c;c.observer.user=&p;c.observer.directory=directory;
 Data body(13);body[3]=13;auto good=group(body);auto bad=good;bad.back()^=1;
 assert(c.feed(bad.data(),bad.size(),0)==epg::Feed::Rejected && completions==0);
 auto first=group(Data(body.begin(),body.begin()+7),0);assert(c.feed(first.data(),first.size(),0)==epg::Feed::Accepted && completions==0);
 c.reset();for(unsigned i=0;i<3;++i){tick=i*1000;good=group(body,0x8000,uint8_t(i<<4));assert(c.feed(good.data(),good.size(),tick)==epg::Feed::Accepted);assert(c.feed(good.data(),good.size(),tick)==epg::Feed::Accepted);}
 assert(completions==6 && p.emptyValid==3 && p.repeat==3 && p.stopDue(tick,true));assert(c.stats.directories==1);
 c.reset();p=active();completions=0;tick=10000;
 first=group(Data(body.begin(),body.begin()+7),0);auto last=group(Data(body.begin()+7,body.end()),0x8001);
 c.feed(first.data(),first.size(),tick);c.feed(first.data(),first.size(),tick);assert(completions==0);
 c.feed(last.data(),last.size(),tick);assert(completions==1 && p.repeat==1);
 c.feed(last.data(),last.size(),tick);assert(completions==1);
 }
 std::printf("EPG AUTO policy + actual Core CRC/segmentation/repeats/wrap: PASS; Policy=%u bytes\n",unsigned(sizeof(Policy)));
}
