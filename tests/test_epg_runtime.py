import os
"""Compile actual runtime commands and framing with fake clock/radio/UART, real core."""
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from epg_replay import extract,fragment_lines
out=ROOT/'.pio/epg_runtime_test';out.mkdir(parents=True,exist_ok=True)
source=(ROOT/'src/epg_runtime.cpp').read_text(encoding='utf-8-sig')
for header in ['gui.h','serial_monitor.h','esp_heap_caps.h']:
 source=source.replace('#include '+('"'+header+'"' if header.endswith('.h') and not header.startswith('esp_') else '<'+header+'>'),'')
gui=(ROOT/'src/gui.cpp').read_text(encoding='utf-8')
icon=gui.split('static void ShowEpgIcon(void) {',1)[1].split('\nvoid ShowAudioMode(void)',1)[0]
icon='void ShowEpgIcon(void) {'+icon
prefix=r'''
#include "epg_runtime.h"
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
struct Esp { unsigned getFreeHeap(){return heapBytes;} unsigned getMinFreeHeap(){return heapBytes;} } ESP;
struct Monitor {
  std::string output;
  int availableForWrite() { return 4096; }
  unsigned droppedBytes() { return 0; }
  void println(const char* s) { output+=s; output+='\n'; }
  void printf(const char* format,...) { char b[1200]; va_list args; va_start(args,format); vsnprintf(b,sizeof(b),format,args); va_end(args); output+=b; }
} diagSerial;
struct Radio {
 bool fm=false, spi=true, ServiceStart=true, SlideShowAvailable=true;
 unsigned ServiceIndex=0, numberofservices=1, ensembleEcc=0xe2,Year=2026,Months=10,Days=23,Hours=10,Minutes=0,Seconds=0;
 char EID[5]="3153";
 struct Svc { uint32_t ServiceID=0x3233; } service[1];
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
 assert(epg::indicatorState()==state);ShowEpgIcon();assert(iconColor==color);
}
'''
suffix=r'''
int main(int argc,char** argv) {
  assert(epg::indicatorPresent() && !epg::runtime());
  radio.spi=false; assert(!epg::indicatorPresent() && !epg::runtime());
  checkIcon(epg::IndicatorState::Absent,0x3a4a);
  unsigned draws=iconDraws;ShowEpgIcon();assert(draws==iconDraws);
  radio.spi=true;checkIcon(epg::IndicatorState::Present,0x06df);
  free8Bytes=1000; freeWordBytes=100000; heapBytes=free8Bytes+freeWordBytes; cmd("EPG TEST DEMO"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos && !epg::runtime());
  cmd("EPG TEST STATUS"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos);
  cmd("EPG TEST START"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos);
  heapBytes=82640; free8Bytes=28360; freeWordBytes=54280; largestBytes=25588;
  // Aggregate INTERNAL may be ample while either required capability pool is short.
  free8Bytes=100000; freeWordBytes=4000; heapBytes=free8Bytes+freeWordBytes;
  cmd("EPG TEST DEMO"); assert(diagSerial.output.find("ERROR MEMORY")!=std::string::npos && allocations.empty());
  free8Bytes=28360; freeWordBytes=54280; heapBytes=82640;
  // Every partial construction failure must roll back all five possible blocks.
  for(unsigned failure=1;failure<=5;++failure) {
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
  checkIcon(epg::IndicatorState::Ready,0x07e0);
  core->identityConflict=true;checkIcon(epg::IndicatorState::Present,0x06df);
  core->identityConflict=false;checkIcon(epg::IndicatorState::Ready,0x07e0);
  assert(allocations.size()==5 && maximumAllocation<=25588 && heapBytes>=24000);
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
  checkIcon(epg::IndicatorState::Present,0x06df);
  cmd("EPG TEST TIME 2024-11-12T04:30:00+01:00");
  assert(core->count()==39);checkIcon(epg::IndicatorState::Ready,0x07e0);
  testMillis+=900001;epg::poll();
  checkIcon(epg::IndicatorState::Present,0x06df); // Stale programmes are not ready.
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
  assert(core->count()==39 && core->stats.pi==1);
  const uint32_t padFrames=core->stats.frames;
  epg::liveEndpoint(0xe23153ff,0x01f2c00d); // A packet START cannot evict PAD data.
  epg::livePadEndpoint(0x3211,0x01fe0005);
  epg::livePayload(epgDemoGroups[0].data,epgDemoGroups[0].size);
  assert(core->count()==39 && core->stats.frames==padFrames);
  epg::livePadEndpoint(0x3222,0x01fe0006);
  assert(core->count()==0 && core->stats.frames==0);
  epg::livePadEndpoint(0,0);
  core->select(0x40e231533211ULL,epg::epoch(2024,11,12,3,30),60);
  for(const auto& group:epgDemoGroups){epg::livePayload(group.data,group.size);while(core->busy())core->pump();}
  assert(core->count()==39 && core->stats.pi==1);
  cmd("EPG TEST START");
  epg::livePadEndpoint(0x3211,0x01fe0005);
  epg::livePadPayload(epgDemoGroups[0].data,epgDemoGroups[0].size);
  assert(core->stats.frames==0 && core->count()==0 && epg::replayActive());
  cmd("EPG TEST STOP");
  std::puts("Actual MSC/PAD source ownership, colliding TID isolation, fallback and replay quarantine: PASS");
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
cpp=out/'runtime.cpp';cpp.write_text(prefix+source+painter+icon+checks+suffix,encoding='utf-8')
compiler=sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe'
exe=out/'runtime.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(cpp),'src/epg_core.cpp','src/epg_uart.cpp','-o',str(exe)],cwd=ROOT,check=True)
subprocess.run([str(exe),str(out/'lines.txt')],check=True)
