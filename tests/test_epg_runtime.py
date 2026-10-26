import os
"""Compile actual runtime commands and framing with fake clock/radio/UART, real core."""
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from epg_replay import extract,fragment_lines
from dab_clock_harness import harness
out=ROOT/'.pio/epg_runtime_test';out.mkdir(parents=True,exist_ok=True)
source=(ROOT/'src/epg_runtime.cpp').read_text(encoding='utf-8-sig')
for header in ['gui.h','serial_monitor.h','esp_heap_caps.h']:
 source=source.replace('#include '+('"'+header+'"' if header.endswith('.h') and not header.startswith('esp_') else '<'+header+'>'),'')
gui=(ROOT/'src/gui.cpp').read_text(encoding='utf-8')
icon=gui.split('static void ShowEpgIcon(void) {',1)[1].split('\nvoid ShowAudioMode(void)',1)[0]
icon='void ShowEpgIcon(void) {'+icon
prefix=r'''
#include "epg_runtime.h"
#include "dab_clock_refresh.h"
#include "dab_spi_discovery.h"
#include "epg_uart.h"
#include "epg_ui.h"
#include <cassert>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <strings.h>
static uint32_t testMillis=100, heapBytes=82640, largestBytes=25588, free8Bytes=28360, freeWordBytes=54280;
static unsigned allocationAttempt=0, allocationFailAt=0, maximumAllocation=0;
struct Allocation { void* pointer; size_t bytes; bool word; };
static std::vector<Allocation> allocations;
uint32_t millis() { return testMillis; }
uint32_t esp_random() { return 0x1234; }
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_32BIT 4
#define MALLOC_CAP_EXEC 8
size_t heap_caps_get_free_size(unsigned caps) { return caps&MALLOC_CAP_EXEC?freeWordBytes:free8Bytes; }
size_t heap_caps_get_largest_free_block(unsigned caps) {
  const unsigned limit=caps&MALLOC_CAP_EXEC?53236:largestBytes;
  const unsigned available=unsigned(heap_caps_get_free_size(caps)); return limit<available?limit:available;
}
void* heap_caps_malloc(size_t bytes,unsigned caps) {
  ++allocationAttempt;
  if((allocationFailAt && allocationAttempt==allocationFailAt) || bytes>heap_caps_get_largest_free_block(caps)) return nullptr;
  unsigned char* raw=static_cast<unsigned char*>(std::malloc(bytes+32));
  assert(raw); std::memset(raw,0xa5,16); std::memset(raw+16+bytes,0x5a,16);
  void* pointer=raw+16; const bool word=caps&MALLOC_CAP_EXEC;
  allocations.push_back({pointer,bytes,word}); heapBytes-=unsigned(bytes);
  if(word) freeWordBytes-=unsigned(bytes); else free8Bytes-=unsigned(bytes);
  if(bytes>maximumAllocation) maximumAllocation=unsigned(bytes);
  return pointer;
}
void heap_caps_free(void* pointer) {
  for(size_t i=0;i<allocations.size();++i) if(allocations[i].pointer==pointer) {
    const size_t bytes=allocations[i].bytes; unsigned char* raw=static_cast<unsigned char*>(pointer)-16;
    for(unsigned j=0;j<16;++j) { assert(raw[j]==0xa5); assert(raw[16+bytes+j]==0x5a); }
    heapBytes+=unsigned(bytes);
    if(allocations[i].word) freeWordBytes+=unsigned(bytes); else free8Bytes+=unsigned(bytes);
    std::free(raw); allocations.erase(allocations.begin()+i); return;
  }
  assert(!"unknown allocation");
}
unsigned uxTaskGetStackHighWaterMark(void*){return 4096;}
struct Esp { unsigned getFreeHeap(){return heapBytes;} unsigned getMinFreeHeap(){return heapBytes;} } ESP;
struct Monitor {
  std::string output;
  int availableForWrite() { return 4096; }
  unsigned droppedBytes() { return 0; }
  void println(const char* s) { output+=s; output+='\n'; }
  void printf(const char* format,...) { char b[1200]; va_list args; va_start(args,format); vsnprintf(b,sizeof(b),format,args); va_end(args); output+=b; }
} diagSerial;
bool diagnosticDebug=false;
struct Radio {
 bool stable=true,fm=false, spi=true, ServiceStart=true, SlideShowAvailable=true, signallock=true;
 epg::ClockRefresh refresh;const epg::ClockRefresh& clockRefresh()const{return refresh;}
 epg::BroadcastClockSample clock;
 const epg::BroadcastClockSample& broadcastClock() const{return clock;}
 unsigned ServiceIndex=0, numberofservices=1, ensembleEcc=0xe2,Year=2026,Months=10,Days=23,Hours=10,Minutes=0,Seconds=0;
 char EID[5]="3153";
 struct Svc { uint32_t ServiceID=0x3233; } service[1];
 dab_spi::Result spiDiscovery()const{dab_spi::Result r;r.status=spi?dab_spi::Status::SelectedEnsemble:dab_spi::Status::NotSignalled;r.serviceId=0xe23153fe;r.componentId=0x01f2c00c;return r;}
 bool xpadSpiPresent()const{return false;}bool epgContextStable()const{return ServiceStart && stable;}
 void logEpgTransportPage(uint8_t){}
 bool isFm() { return fm; } bool spiPresent(){return spi;}
 unsigned timeSampleGeneration(){return 1;}
} radio;
unsigned char dabfreq=25;
static bool view=false;
void EpgOpen(){view=true;} void EpgClose(bool){view=false;} void EpgInvalidate(){}
void cmd(const char* text) { char line[128]; std::snprintf(line,sizeof(line),"%s",text); diagSerial.output.clear(); assert(epg::command(line)); }
'''
painter=r'''
bool displayreset=false;
const uint16_t Background[1]={};
const uint16_t BackgroundColor2=0;
#define TC_DATUM 0
unsigned iconDraws=0,iconRestores=0;
uint16_t iconColor=0;
struct IconSprite {
 void pushImage(int,int,int,int,const uint16_t*){}
 void setTextDatum(int){}
 void setTextColor(uint16_t color,uint16_t,bool fill){assert(!fill);iconColor=color;}
 void drawString(const char* value,int,int){assert(std::strcmp(value,"EPG")==0);}
} FullLineSprite;
void RestoreMainBackgroundRegion(int,int,int,int){++iconRestores;}
void PushMainSprite(IconSprite&,int,int,int,int){++iconDraws;}
'''
checks=r'''
void checkIcon(epg::IndicatorState state,uint16_t color){
 assert(epg::indicatorState()==state);const unsigned before=iconDraws;ShowEpgIcon();
 if(state==epg::IndicatorState::Absent)assert(iconDraws==before);else assert(iconColor==color);
}
'''
suffix=r'''
int main(int argc,char** argv) {
  assert(epg::indicatorPresent() && !epg::runtime());
  radio.spi=false; assert(!epg::indicatorPresent() && !epg::runtime());
  checkIcon(epg::IndicatorState::Absent,0x3a4a);
  unsigned draws=iconDraws;ShowEpgIcon();assert(draws==iconDraws);
  radio.spi=true;checkIcon(epg::IndicatorState::Loading,0xf800);
  free8Bytes=1000; freeWordBytes=100000; heapBytes=free8Bytes+freeWordBytes; cmd("EPG TEST DEMO"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos && !epg::runtime());
  cmd("EPG TEST STATUS"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos);
  cmd("EPG TEST START"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos);
  heapBytes=82640; free8Bytes=28360; freeWordBytes=54280; largestBytes=25588;
  // Aggregate INTERNAL may be ample while either required capability pool is short.
  free8Bytes=100000; freeWordBytes=4000; heapBytes=free8Bytes+freeWordBytes;
  cmd("EPG TEST DEMO"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos && allocations.empty());
  free8Bytes=28360; freeWordBytes=54280; heapBytes=82640;
  // Every partial construction failure must roll back all six possible blocks.
  for(unsigned failure=1;failure<=6;++failure) {
    allocationAttempt=0; allocationFailAt=failure;
    cmd("EPG TEST DEMO"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos);
    assert(!epg::runtime() && heapBytes==82640 && free8Bytes==28360 && freeWordBytes==54280 && allocations.empty());
  }
  allocationFailAt=0; allocationAttempt=0;
  largestBytes=1000; cmd("EPG TEST DEMO"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos);
  assert(allocations.empty() && heapBytes==82640);
  largestBytes=25588;
  cmd("EPG TEST HELP"); assert(diagSerial.output.find("OK HELP")!=std::string::npos);
  cmd("EPG TEST DEMO"); assert(epg::replayActive() && view);
  checkIcon(epg::IndicatorState::Absent,0x3a4a);
  assert(!epg::indicatorPresent()); // START is not proof of EPG data.
  for(unsigned i=0;i<1000;++i) { ++testMillis; epg::poll(); }
  assert(epg::indicatorPresent()); // Valid MOT directory, independent of FIC.
  radio.spi=false; assert(epg::indicatorPresent());
  auto* core=epg::runtime();
  checkIcon(epg::IndicatorState::Ready,0x06df);
  core->identityConflict=true;checkIcon(epg::IndicatorState::Unavailable,0x3a4a);
  core->identityConflict=false;checkIcon(epg::IndicatorState::Ready,0x06df);
  assert(allocations.size()==6 && maximumAllocation<=25588 && free8Bytes>=12000 && freeWordBytes>=4096);
  const unsigned allocatedBytes=82640-heapBytes, attempts=allocationAttempt;
  assert(allocatedBytes==epg::Core::memoryBytes());
  assert(free8Bytes==28360-sizeof(epg::Core) && free8Bytes>=12000);
  assert(freeWordBytes==54280-epg::Core::bufferBytes());
  for(const auto& allocation:allocations) assert(allocation.word==(allocation.pointer!=core));
  for(unsigned repeat=0;repeat<8;++repeat) {
    cmd("EPG TEST STOP"); cmd("EPG TEST DEMO");
    for(unsigned i=0;i<1000;++i) { ++testMillis; epg::poll(); }
    assert(allocationAttempt==attempts && 82640-heapBytes==allocatedBytes);
  }
  std::printf("Split ESP32 pools: free8=28360 largest8=25588 free32=54280 largest32=53236 blocks=%zu bytes=%u maximum=%u, rollback/repeated errors/reuse: PASS\n",allocations.size(),allocatedBytes,maximumAllocation);
  assert(core->stats.directories==1 && core->stats.pi==1 && core->stats.si==1 && core->stats.programmes==42);
  assert(core->current()>=0 && core->next()>=0 && core->count());
  std::printf("Actual built-in DEMO: dirs=%u PI=%u SI=%u programmes=%u cached=%zu NOW=%d NEXT=%d\n",core->stats.directories,core->stats.pi,core->stats.si,core->stats.programmes,core->count(),core->current(),core->next());
  cmd("EPG TEST STATUS"); assert(diagSerial.output.find("mode=TEST")!=std::string::npos);
  cmd("EPG TEST STATION 32A3"); assert(diagSerial.output.find("ERROR STATION")!=std::string::npos);
  cmd("EPG TEST TIME 2026-10-23T10:30:00Z"); assert(core->current()==-1 && core->count()==0);
  checkIcon(epg::IndicatorState::Ready,0x06df); // Decoded current-service SI remains usable.
  cmd("EPG TEST TIME 2024-11-12T04:30:00+01:00");
  assert(core->count()==42);checkIcon(epg::IndicatorState::Ready,0x06df);
  testMillis+=900001;epg::poll();
  checkIcon(epg::IndicatorState::Ready,0x06df); // Stale PI does not invalidate usable SI.
  cmd("EPG TEST STOP"); assert(!epg::replayActive() && !view && core->count()==0);
  assert(!epg::indicatorPresent());
  checkIcon(epg::IndicatorState::Absent,0x3a4a);
  radio.fm=true;draws=iconDraws;ShowEpgIcon();assert(draws==iconDraws);radio.fm=false;
  radio.spi=true; assert(epg::indicatorPresent());
  cmd("EPG TEST START E23153FE 01F2C00C"); assert(diagSerial.output.find("session=1235")!=std::string::npos);
  assert(!epg::indicatorPresent());
  cmd("EPG TEST STATION 3233"); assert(diagSerial.output.find("pending SI")!=std::string::npos);
  cmd("EPG TEST TIME 2024-11-12T04:30:00+01:00");
  if(argc>1) {
    std::ifstream f(argv[1]); assert(f.good()); std::string line; unsigned received=0;
    while(std::getline(f,line)) {
      for(unsigned attempt=0;attempt<100;++attempt) {
        diagSerial.output.clear(); std::vector<char> value(line.begin(),line.end()); value.push_back(0);
        assert(epg::command(value.data()));
        if(diagSerial.output.find("EPG ACK ")!=std::string::npos) {
          if(received==0) {
            const uint32_t before=core->stats.frames;
            assert(epg::command(value.data()) && core->stats.frames==before); // lost ACK retry
            value[4]=value[4]=='0'?'1':'0';
            diagSerial.output.clear(); epg::command(value.data()); assert(diagSerial.output.find("NACK CRC")!=std::string::npos);
          }
          ++received; ++testMillis; epg::poll(); break;
        }
        if(diagSerial.output.find("NACK BUSY")==std::string::npos) std::fprintf(stderr,"at fragment %u response=%s line=%s\n",received,diagSerial.output.c_str(),line.c_str());
        assert(diagSerial.output.find("NACK BUSY")!=std::string::npos);
        epg::poll(); assert(attempt<99);
      }
    }
    while(core->busy()) epg::poll();
    assert(core->count() && core->current()>=0 && core->next()>=0);
    assert(core->selected()==0x40e231533233ULL);
    std::printf("UART fragments=%u, cached=%zu NOW=%d NEXT=%d: PASS\n",received,core->count(),core->current(),core->next());
  }
  cmd("EPG TEST START"); testMillis+=31000; epg::poll(); assert(!epg::replayActive() && epg::indicatorPresent());
  cmd("EPG TEST START"); ++dabfreq; epg::poll(); assert(!epg::replayActive());
  cmd("EPG TEST START"); radio.fm=true; epg::poll(); assert(!epg::replayActive());
  cmd("EPG TEST DEMO"); assert(epg::replayActive()); epg::liveReset(); assert(!epg::replayActive());
  cmd("EPG TEST CLEAR"); assert(!epg::replayActive() && core->count()==0);
  // One shared decoder: source switches reset assemblies and directories;
  // background MSC cannot corrupt an active PAD stream, including colliding TIDs.
  radio.fm=false;
  epg::liveEndpoint(0xe23153fe,0x01f2c00c);
  epg::livePadEndpoint(0x3211,0x01fe0005);
  core->select(0x40e231533211ULL,epg::epoch(2024,11,12,3,30),60);
  for(const auto& group:epgDemoGroups) epg::livePayload(group.data,group.size);
  assert(core->stats.frames==0 && core->count()==0);
  for(const auto& group:epgDemoGroups){epg::livePadPayload(group.data,group.size);while(core->busy())core->pump();}
  assert(core->count()==42 && core->stats.pi==1);
  const uint32_t padFrames=core->stats.frames;
  epg::liveEndpoint(0xe23153ff,0x01f2c00d); // A packet START cannot evict PAD data.
  epg::livePadEndpoint(0x3211,0x01fe0005);
  epg::livePayload(epgDemoGroups[0].data,epgDemoGroups[0].size);
  assert(core->count()==42 && core->stats.frames==padFrames);
  epg::livePadEndpoint(0x3222,0x01fe0006);
  assert(core->count()==0 && core->stats.frames==0);
  epg::livePadEndpoint(0,0);
  core->select(0x40e231533211ULL,epg::epoch(2024,11,12,3,30),60);
  for(const auto& group:epgDemoGroups){epg::livePayload(group.data,group.size);while(core->busy())core->pump();}
  assert(core->count()==42 && core->stats.pi==1);
  radio.service[0].ServiceID=0x3211;
  radio.clock.calendarEpoch=epg::epoch(2026,10,25,6,33);radio.clock.encoding=epg::ClockEncoding::UTC;
  radio.clock.valid=true;radio.clock.offsetKnown=true;radio.clock.offsetMinutes=60;
  radio.clock.generation=1;radio.clock.sampledAtMonotonicMs=testMillis;
  epg::poll();assert(core->selected()==0x40e231533211ULL && core->current()==-1);
  assert(epg::availability()==epg::Availability::PiDateStale);
  const uint32_t realNow=core->now();cmd("EPG ARCHIVE 2024-11-12");
  assert(core->archivePreview() && core->now()==realNow && core->current()==-1);
  cmd("EPG ARCHIVE OFF");assert(!core->archivePreview() && core->now()==realNow);
  radio.signallock=false;epg::poll();assert(epg::availability()==epg::Availability::TimeUnknown);
  radio.signallock=true;epg::poll();assert(!core->clockTrusted());
  ++radio.clock.generation;epg::poll();assert(core->clockTrusted() && core->current()==-1);
  cmd("EPG TEST START");
  epg::livePadEndpoint(0x3211,0x01fe0005);
  epg::livePadPayload(epgDemoGroups[0].data,epgDemoGroups[0].size);
  assert(core->stats.frames==0 && core->count()==0 && epg::replayActive());
  cmd("EPG TEST STOP");
  std::puts("Actual MSC/PAD source ownership, colliding TID isolation, fallback and replay quarantine: PASS");

  // Four UI states, exact painter colours and a non-destructive UI-only clock.
  const auto freshClock=[&](){radio.clock.sampledAtMonotonicMs=testMillis;++radio.clock.generation;};
  const auto beginIndicator=[&](){
    epg::stopReplay();radio.fm=false;radio.spi=true;radio.stable=true;radio.ServiceStart=true;radio.signallock=true;
    radio.service[0].ServiceID=0x3211;
    radio.clock.valid=radio.clock.offsetKnown=true;radio.clock.encoding=epg::ClockEncoding::UTC;
    radio.clock.calendarEpoch=epg::epoch(2024,11,12,3,30);radio.clock.offsetMinutes=60;freshClock();
    epg::liveReset();epg::liveEndpoint(0xe23153fe,0x01f2c00c);epg::poll();
  };
  beginIndicator();checkIcon(epg::IndicatorState::Loading,0xf800);
  const auto uiResets=epg_diag::registry().totalResets;const auto uiFrames=core->stats.frames;
  testMillis+=epg::kIndicatorWaitMs;epg::poll();checkIcon(epg::IndicatorState::Unavailable,0x3a4a);
  assert(core->stats.frames==uiFrames && epg_diag::registry().totalResets==uiResets && core->stats.timeout==0);
  const auto restores=iconRestores;for(unsigned i=0;i<100;++i)ShowEpgIcon();assert(iconRestores==restores);
  const EpgDemoGroup* firstDir=nullptr;
  for(const auto& g:epgDemoGroups)if((g.data[0]&15)==6){firstDir=&g;break;}
  assert(firstDir);epg::livePayload(firstDir->data,firstDir->size);epg::poll();checkIcon(epg::IndicatorState::Loading,0xf800);
  const auto mask=core->directoryAssembly().mask;assert(mask);
  testMillis+=epg::kIndicatorWaitMs;freshClock();epg::livePayload(firstDir->data,firstDir->size);epg::poll();
  checkIcon(epg::IndicatorState::Unavailable,0x3a4a);
  assert(core->directoryAssembly().mask==mask && core->stats.timeout==0 && epg_diag::registry().totalResets==uiResets);
  for(const auto& g:epgDemoGroups)if((g.data[0]&15)==6 && &g!=firstDir)epg::livePayload(g.data,g.size);
  epg::poll();checkIcon(epg::IndicatorState::Loading,0xf800);
  // First partial PI progress recovers red; a matching duplicate cannot do so.
  const EpgDemoGroup* firstPi=nullptr;
  for(const auto& g:epgDemoGroups)if((g.data[0]&15)==4 && g.data[5]==0xf6 && g.data[6]==0x29){firstPi=&g;break;}
  assert(firstPi);epg::livePayload(firstPi->data,firstPi->size);epg::poll();
  const auto piMask=core->assembly(0).mask;assert(piMask);
  testMillis+=epg::kIndicatorWaitMs;freshClock();epg::livePayload(firstPi->data,firstPi->size);epg::poll();checkIcon(epg::IndicatorState::Unavailable,0x3a4a);
  assert(core->assembly(0).mask==piMask && core->stats.timeout==0);
  for(const auto& g:epgDemoGroups)if((g.data[0]&15)==4 && g.data[5]==0xf6 && g.data[6]==0x29 && &g!=firstPi){epg::livePayload(g.data,g.size);while(core->busy())core->pump();}
  epg::poll();assert(!core->hasSI() && core->count()==42);checkIcon(epg::IndicatorState::Ready,0x06df);
  radio.clock.calendarEpoch=epg::epoch(2026,10,26,6,33);freshClock();epg::poll();checkIcon(epg::IndicatorState::Unavailable,0x3a4a);
  // Decoded SI is independently useful, including stale PI and uncertain clocks.
  for(const auto& g:epgDemoGroups)if((g.data[0]&15)==4 && g.data[5]==0xf6 && g.data[6]==0x4c){epg::livePayload(g.data,g.size);while(core->busy())core->pump();}
  epg::poll();assert(core->hasSI());checkIcon(epg::IndicatorState::Ready,0x06df);
  radio.signallock=false;epg::poll();checkIcon(epg::IndicatorState::Ready,0x06df);
  radio.stable=false;checkIcon(epg::IndicatorState::Unavailable,0x3a4a);radio.stable=true;
  core->identityConflict=true;checkIcon(epg::IndicatorState::Unavailable,0x3a4a);
  radio.spi=false;checkIcon(epg::IndicatorState::Absent,0);core->identityConflict=false;radio.spi=true;
  radio.service[0].ServiceID=0x9999;epg::poll();assert(!core->hasSI() && !core->selected());checkIcon(epg::IndicatorState::Loading,0xf800);
  radio.service[0].ServiceID=0x3211;epg::poll();checkIcon(epg::IndicatorState::Ready,0x06df);
  radio.fm=true;checkIcon(epg::IndicatorState::Absent,0);radio.fm=false;checkIcon(epg::IndicatorState::Ready,0x06df);
  const auto oldRestores=iconRestores;displayreset=true;ShowEpgIcon();displayreset=false;assert(iconRestores==oldRestores+1);
  beginIndicator();assert(argc>2);std::ifstream emptyFile(argv[2],std::ios::binary);assert(emptyFile.good());
  std::vector<uint8_t> empty((std::istreambuf_iterator<char>(emptyFile)),{});
  const auto emptyResets=epg_diag::registry().totalResets;
  for(unsigned i=0;i<101;++i){epg::livePayload(empty.data(),empty.size());++testMillis;epg::poll();checkIcon(epg::IndicatorState::Unavailable,0x3a4a);}
  assert(core->stats.directories==1 && core->stats.pi==0 && core->stats.si==0 && epg_diag::registry().totalResets==emptyResets);
  testMillis=UINT32_MAX-60000;beginIndicator();checkIcon(epg::IndicatorState::Loading,0xf800);
  testMillis+=epg::kIndicatorWaitMs;epg::poll();checkIcon(epg::IndicatorState::Unavailable,0x3a4a);
  std::puts("EPG indicator: hidden/loading/red/usable/cyan/unavailable/grey, SI-only/current identity, empty101, progress120s/wrap, no cache reset/repaint: PASS");

  {
  // C02-C07/C09-C16: actual driver replies feed the public sample consumed by
  // actual epg::poll() and the bounded production Core, with PI/SI/own PNG.
  epg::liveReset();radio.service[0].ServiceID=0x3201;radio.fm=false;radio.signallock=true;
  radio.ServiceStart=true;radio.spi=true;radio.stable=true;
  static_cast<ClockRadio&>(radio)=ClockRadio{};
  epg::liveEndpoint(0xe23153fe,0x01f2c00c);
  auto drive=[&](uint32_t utc){const int off=60;
    radio.dabTimeRefreshPending=true;radio.schedule();assert(radio.sentType==0);radio.replyEpoch(utc+off*60);epg::poll();
    testMillis+=100;radio.schedule();assert(radio.sentType==1);radio.replyEpoch(utc);epg::poll();
    testMillis+=100;radio.schedule();assert(radio.sentType==0);radio.replyEpoch(utc+off*60);epg::poll();
  };
  const uint32_t originalUtc=epg::epoch(2024,11,12,1,1);drive(originalUtc);
  assert(argc>3);std::ifstream logos(argv[3],std::ios::binary);assert(logos.good());
  std::vector<std::vector<uint8_t>> carousel;
  while(logos.peek()!=EOF){uint8_t h[2];logos.read(reinterpret_cast<char*>(h),2);unsigned n=h[0]|unsigned(h[1])<<8;
    carousel.emplace_back(n);logos.read(reinterpret_cast<char*>(carousel.back().data()),n);assert(logos.good());}
  // Add a second, not-yet-decoded PI TID to this test Directory before any
  // cache is loaded. A previously decoded TID is correctly skipped by Core.
  auto& extra=carousel[0];std::vector<uint8_t> entry(extra.begin()+31,extra.begin()+48);entry[0]=4;entry[1]=0;
  extra.insert(extra.end()-2,entry.begin(),entry.end());extra[8]+=17;extra[12]+=17;extra[14]=4;
  auto fixCrc=[](std::vector<uint8_t>& f){const uint16_t crc=epg::crc16(f.data(),f.size()-2);f[f.size()-2]=uint8_t(crc>>8);f.back()=uint8_t(crc);};fixCrc(extra);
  for(const auto& f:carousel){epg::livePayload(f.data(),f.size());while(core->busy())epg::poll();epg::poll();}
  assert(core->count()==3 && core->current()==1 && core->next()==2 && core->hasSI() && core->logoSize()==551);
  const auto bearer=core->selected();const auto day=core->localDay();const auto records=core->count();
  const auto bytes=core->logoSize();const auto pi=core->stats.pi,si=core->stats.si,png=core->stats.png;
  const auto resets=epg_diag::registry().totalResets;
  // Start another copy of the PI but leave it incomplete across pending refresh.
  auto partial=carousel[4];partial[5]=4;partial[6]=0;fixCrc(partial);epg::livePayload(partial.data(),partial.size());epg::poll();
  const uint32_t mask=core->assembly(0).mask|core->assembly(1).mask;assert(mask);
  for(unsigned minute=0;minute<11;++minute){
    testMillis+=60000;epg::livePayload(partial.data(),partial.size());epg::poll();const auto revision=core->revision;const auto clockGeneration=radio.clock.generation;const int expectedNow=core->current(),expectedNext=core->next();assert(expectedNow>=0);
    const uint32_t at=epg::resolveTime(radio.clock,testMillis,true).utc;
    radio.dabTimeRefreshPending=true;radio.schedule();radio.replyEpoch(at+3600);epg::poll();
    assert(core->clockTrusted() && core->current()==expectedNow && core->next()==expectedNext && core->revision==revision);
    assert(radio.clock.generation==clockGeneration);
    testMillis+=100;radio.schedule();radio.replyEpoch(at);epg::poll();
    assert(core->clockTrusted() && core->revision==revision && core->selected()==bearer && core->localDay()==day);
    testMillis+=100;radio.schedule();radio.replyEpoch(at+3600);epg::poll();
    assert(radio.clock.generation==clockGeneration+1 && core->current()==expectedNow && core->next()==expectedNext);
    assert(core->count()==records && core->logoSize()==bytes && core->stats.pi==pi && core->stats.si==si && core->stats.png==png);
    assert((core->assembly(0).mask|core->assembly(1).mask)==mask);
  }
  assert(epg_diag::registry().totalResets==resets);
  std::puts("C02/C03/C04/C15 runtime PASS: >10min READY, same bearer/day/revision/PI/SI/PNG/logo and partial MOT");
  // Failed UTC preserves the published clock and exact data until TTL; after
  // TTL only trust/NOW/NEXT disappear, not the cached date, offset or assembly.
  const auto cacheRevision=core->revision;const auto oldOffset=core->offset();
  radio.dabTimeRefreshPending=true;radio.schedule();radio.replyEpoch(core->now()+3600);epg::poll();
  radio.schedule();radio.finish(si468x::Result::Timeout);epg::poll();
  assert(core->clockTrusted() && core->revision==cacheRevision);
  testMillis=radio.clock.sampledAtMonotonicMs+299000;epg::livePayload(partial.data(),partial.size());epg::poll();
  testMillis=radio.clock.sampledAtMonotonicMs+300000;epg::poll();
  assert(!core->clockTrusted() && core->current()==-1 && core->next()==-1);
  assert(core->count()==records && core->logoSize()==bytes && core->selected()==bearer && core->localDay()==day && core->offset()==oldOffset);
  assert(core->stats.pi==pi && core->stats.si==si && core->stats.png==png && (core->assembly(0).mask|core->assembly(1).mask)==mask);
  assert(epg_diag::registry().totalResets==resets);
  std::puts("C05/C06/C07 runtime PASS: failed refresh/TTL inhibit NOW/NEXT while retaining full cache and MOT");
  // C09 local midnight: candidate LOCAL alone must not choose a day. The
  // legitimate boundary is crossed by confirmed/extrapolated UTC instead.
  testMillis=radio.refresh.retryAt+300000;drive(epg::epoch(2024,11,12,22,59,59));
  const auto beforeDay=core->localDay();const auto beforeClockDay=uint32_t((uint64_t(core->now())+core->offset()*60)/86400);
  radio.dabTimeRefreshPending=true;radio.schedule();radio.replyEpoch(epg::epoch(2024,11,14));epg::poll();
  assert(core->localDay()==beforeDay && uint32_t((uint64_t(core->now())+core->offset()*60)/86400)==beforeClockDay);
  testMillis+=1000;epg::poll();assert(uint32_t((uint64_t(core->now())+core->offset()*60)/86400)==beforeClockDay+1);
  radio.schedule();radio.finish(si468x::Result::Timeout);
  std::puts("C09 runtime PASS: pending LOCAL cannot change day; trusted extrapolation crosses midnight");
  // C10: first LOCAL generation cannot release the RF-loss gate.
  radio.signallock=false;radio.maintainDabClock();epg::poll();radio.signallock=true;radio.schedule();
  radio.replyEpoch(epg::epoch(2024,11,13));epg::poll();assert(!core->clockTrusted());
  radio.schedule();radio.replyEpoch(epg::epoch(2024,11,12,23));epg::poll();assert(!core->clockTrusted());
  radio.schedule();radio.replyEpoch(epg::epoch(2024,11,13));epg::poll();assert(core->clockTrusted());
  std::puts("C10 runtime PASS: RF reacquisition requires completed newly accepted generation");
  // C16: virtual TIME remains private; returning LIVE does not publish it.
  const auto liveSample=radio.clock;cmd("EPG TEST INJECT PAD");cmd("EPG TEST TIME 2020-01-01T00:00:00Z");
  assert(radio.clock.calendarEpoch==liveSample.calendarEpoch && radio.clock.generation==liveSample.generation);
  radio.fm=true;epg::poll();assert(!epg::replayActive() && !core->clockTrusted());radio.fm=false;
  std::puts("C16 runtime PASS: TEST/TIME/FM/LIVE clock quarantine");
  }

  // Check all heap canaries and release the harness allocations at process exit.
  core->~Core(); heap_caps_free(core);
  assert(allocations.empty() && heapBytes==82640);
  std::puts("Actual EPG commands/DEMO/UART/time/station/timeout/switch/memory/STOP/CLEAR: PASS");
}
'''
frames=extract(Path(os.environ.get('EPG_TEST_INPUT',str(ROOT/'_usertesting/device-monitor-261023-160924.log'))))
sequence=1
with (out/'lines.txt').open('wb') as f:
 for frame in frames:
  for seq,line in fragment_lines(frame,0x1235,sequence):f.write(line);sequence=seq+1
prefix=prefix.replace('struct Radio {',harness()+'\nstruct Radio : ClockRadio {').replace('epg::ClockRefresh refresh;','epg::ClockRefresh& refresh=dabClockRefresh;').replace('epg::BroadcastClockSample clock;','epg::BroadcastClockSample& clock=dabEpgClock;').replace('bool stable=true,fm=false, spi=true, ServiceStart=true, SlideShowAvailable=true, signallock=true;','bool stable=true,spi=true,ServiceStart=true,SlideShowAvailable=true;')
cpp=out/'runtime.cpp';cpp.write_text(prefix+source+painter+icon+checks+suffix,encoding='utf-8')
compiler=sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe'
exe=out/'runtime.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(cpp),'src/epg_core.cpp','src/epg_uart.cpp','-o',str(exe)],cwd=ROOT,check=True)
subprocess.run([str(exe),str(out/'lines.txt'),str(ROOT/'tests/fixtures/epg/mux11a_empty_group.bin'),str(ROOT/'tests/fixtures/epg/synthetic_logo_1_groups_v2.bin')],check=True)
