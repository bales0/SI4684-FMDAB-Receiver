"""Real SPI framing/retained-read code plus bounded queue evidence and reporter."""
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'.pio/dsrv_queue';out.mkdir(parents=True,exist_ok=True)
source=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
def method(signature):
    a=source.index(signature+' {');b=source.index('{',a)+1;depth=1
    while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
cpp=r'''
#define SI4684_DSRV_QUEUE_DIAG 1
#include "dab_dsrv_queue_output.h"
#include "dab_loss_diag.h"
#include "vendor/si468x/Si468x.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
using namespace dsrv_queue;
static uint32_t tick=0;
uint32_t millis(){return tick;}
static Evidence dsrvQueue;
static uint8_t SPIbuffer[4096]={};
struct Chip {
 unsigned reads=0,commands=0;std::vector<uint8_t> wire;
 si468x::Result result=si468x::Result::Ok;
 si468x::Result readCurrentReply(uint8_t* p,uint16_t n){++reads;assert(n==wire.size());if(result==si468x::Result::Ok)memcpy(p,wire.data(),n);return result;}
}chip;
struct DAB {uint32_t dabGeneration=7;si468x::Result readDsrvPayload(uint16_t length);};
struct SPISettings{SPISettings(unsigned hz,unsigned,unsigned){assert(hz==10000000);}};
static constexpr unsigned RADIO_SPI_HZ=10000000,MSBFIRST=1,SPI_MODE0=0,LOW=0,HIGH=1;
static int slaveSelectPin=15;
static bool diagnosticDebug=false;static uint8_t diagLastCommand=0x84;
static unsigned selectChanges=0;
void digitalWrite(int pin,unsigned){assert(pin==15);++selectChanges;}
void quiet(const char* ="",...){}
#define DIAG_PRINTF(...) quiet(__VA_ARGS__)
#define DIAG_PRINT(...) quiet(__VA_ARGS__)
#define DIAG_PRINTLN(...) quiet(__VA_ARGS__)
struct RadioBus{
 std::vector<uint8_t> wire;unsigned at=0,begin=0,end=0;
 void beginTransaction(SPISettings){++begin;}void endTransaction(){++end;}
 uint8_t transfer(unsigned value){assert(value==0 && at<wire.size());return wire[at++];}
}RadioSPI;
'''+method('static bool hostReadReply(void*, uint8_t* destination, uint16_t length)')+'\n'+method('si468x::Result DAB::readDsrvPayload(uint16_t length)')+r'''
struct Output {
 int room=2048;unsigned calls=0;size_t longest=0;std::string text;
 int availableForWrite(){return room;}
 size_t write(const uint8_t* p,size_t n){assert(n<=size_t(room));++calls;longest=std::max(longest,n);room-=int(n);text.append(reinterpret_cast<const char*>(p),n);return n;}
};
static void header(uint8_t* r,uint8_t q=19,uint8_t state=0,uint8_t interrupt=1){
 for(unsigned i=0;i<24;++i)r[i]=uint8_t(3+i*7);
 r[0]=128;r[3]=0xc0;r[4]=interrupt;r[5]=q;r[6]=state;r[7]=0x7c;r[18]=3;r[19]=0;
}
static void drain(Evidence& d,Output& o,uint32_t& t){
 for(unsigned i=0;i<40 && d.cursor!=255;++i){o.room=2048;t+=100;const auto before=o.calls;service(d,o,t,7,3,false,true,0,0,0);assert(o.calls-before<=1);}
 assert(d.cursor==255);
}
int main(){
 static_assert(sizeof(Evidence)<=768,"entire new state incl reporting");
 uint8_t r[24];header(r);
 RadioSPI.wire.push_back(0xed);RadioSPI.wire.insert(RadioSPI.wire.end(),r,r+24);
 SPIbuffer[0]=0xfa;assert(hostReadReply(nullptr,SPIbuffer+1,24));
 assert(SPIbuffer[0]==0xfa && memcmp(SPIbuffer+1,r,24)==0 && RadioSPI.at==25 && RadioSPI.begin==1 && RadioSPI.end==1 && selectChanges==2);
 si468x::DsrvHeader parsed;assert(si468x::Si468x::parseDsrvHeader(SPIbuffer+1,24,parsed)==si468x::Result::Ok);
 assert(parsed.interruptSource==1 && parsed.buffersRemaining==19 && parsed.serviceState==0 && parsed.dataSource==1 && parsed.byteCount==3);
 assert(parsed.serviceId==load32(r+8) && parsed.componentId==load32(r+12));
 Evidence e;assert(e.header(SPIbuffer+1,24,true,7,7,0,0));
 assert(e.firstKnown && e.firstOver8.atMs()==0 && !memcmp(e.firstOver8.raw,r,24));
 for(uint8_t q:{uint8_t(0),uint8_t(1),uint8_t(7),uint8_t(8),uint8_t(9),uint8_t(19),uint8_t(255)}){header(r,q);assert(e.header(r,24,true,7,7,0,10));}
 assert(e.total.qmax==255 && e.total.hist[0]==1 && e.total.hist[1]==1 && e.total.hist[3]==1 && e.total.hist[4]==1 && e.total.hist[5]==1 && e.total.hist[6]==2 && e.total.hist[7]==1);
 assert(e.total.value[Over8]==4 && e.firstOver8.raw[5]==19);
 const auto count=e.total.headers;
 assert(!e.header(r,24,false,7,7,0,20));assert(e.total.headers==count);
 e.commandError(-4,7,0);e.commandError(-5,7,0);e.commandError(-6,7,0);e.commandError(-7,7,0);
 assert(e.total.value[CmdTimeout]==1 && e.total.value[CmdTransport]==1 && e.total.value[CmdDevice]==1 && e.total.value[CmdOther]==1);
 assert(!e.header(r,23,true,7,7,0,20) && !e.header(r,24,true,6,7,0,20));assert(e.total.headers==count);
 assert(e.total.value[HeaderError]==1 && e.total.value[ShortHeader]==1 && e.total.value[StaleHeader]==1);
 for(unsigned flags=0;flags<4;++flags){header(r,19,(flags&1)?2:0,(flags&2)?7:1);e.header(r,24,true,7,7,0,30);}
 assert(e.total.value[State2]==2 && e.total.value[State2NoOverflow]==1 && e.total.value[Overflow]==2 && e.total.value[Physical]==2 && e.total.value[Over8State2]==2);
 header(r,1,1);e.header(r,24,true,7,7,0,32);header(r,1,4);e.header(r,24,true,7,7,0,33);
 header(r,1,255);r[18]=r[19]=0;r[0]=64;r[3]=31;e.header(r,24,true,7,7,0,34);
 assert(e.total.value[State1]==1 && e.total.value[State4]==1 && e.total.value[StateOther]==1 && e.total.value[Empty]==1);
 assert(e.total.value[NoCts]==1 && e.total.value[ErrCmd]==1 && e.total.value[Status3Error]==1);
 e.total.value[Over8]=e.window.value[Over8]=UINT16_MAX;header(r);e.header(r,24,true,7,7,0,40);assert(e.total.value[Over8]==UINT16_MAX && e.total.atLimit());
 e.total.headers=UINT32_MAX;e.header(r,24,true,7,7,0,40);assert(e.total.headers==UINT32_MAX);
 std::puts("Raw offsets/SPIbuffer+1/framing, queue 0..255, independent state2/overflow, malformed/stale/errors and saturation: PASS");
 // The original full read overwrites SPIbuffer, not the stored first header.
 dsrvQueue=Evidence{};header(r);memcpy(SPIbuffer+1,r,24);dsrvQueue.header(SPIbuffer+1,24,true,7,7,0,100);
 chip.wire.assign(r,r+24);chip.wire.resize(27);chip.wire[0]=0x90;chip.wire[4]=0;chip.wire[5]=1;
 DAB d;const auto reads=chip.reads;assert(d.readDsrvPayload(27)==si468x::Result::Ok);
 assert(chip.reads==reads+1 && !chip.commands && dsrvQueue.firstOver8.raw[0]==128 && dsrvQueue.firstOver8.raw[5]==19);
 assert(dsrvQueue.total.value[SidCidChanged]==0 && dsrvQueue.total.value[ByteCountChanged]==0);
 memcpy(SPIbuffer+1,r,24);chip.wire[8]^=1;assert(d.readDsrvPayload(27)==si468x::Result::MalformedReply);
 assert(dsrvQueue.total.value[SidCidChanged]==1);chip.wire[8]^=1;
 memcpy(SPIbuffer+1,r,24);chip.wire[18]=4;assert(d.readDsrvPayload(27)==si468x::Result::MalformedReply);
 assert(dsrvQueue.total.value[ByteCountChanged]==1);
 assert(!dab_loss::evidence().total.event[dab_loss::ReadFail]); // Existing API-vs-semantic counters unchanged.
 std::puts("Actual retained read: status/queue changes allowed, stable SID/CID/count mismatches counted once, no extra read: PASS");
 e=Evidence{};
 for(unsigned i=0;i<30;++i){header(r,uint8_t(i));e.header(r,24,true,i<25?6:7,i<25?6:7,uint8_t(i%4),UINT32_MAX-50+i);}
 assert(e.count==12 && e.oldest(0).raw[5]==18 && e.firstOver8.raw[5]==9);
 assert(e.freeze(100,7,2,3,0x84));
 const auto frozen=e;assert(e.over8Age==122);
 for(unsigned i=0;i<30;++i){header(r,255,2,7);e.header(r,24,true,8,8,3,200+i);}
 assert(!e.freeze(300,8,3,5,0xb2));
 assert(!memcmp(e.ring,frozen.ring,sizeof e.ring) && !memcmp(&e.firstOver8,&frozen.firstOver8,sizeof(Sample)) && !memcmp(&e.frozenStats,&frozen.frozenStats,sizeof(Stats)) && e.freezeAt==100 && e.freezeGeneration==7);
 Output o;o.room=0;service(e,o,400,8,3,false,true,0,0,2048);assert(e.cursor==0 && o.text.empty());
 o.room=2048;service(e,o,500,8,3,true,true,0,0,0);assert(e.cursor==0 && o.text.empty()); // CTS always wins.
 uint32_t time=500;drain(e,o,time);
 size_t rawLines=0,pos=0;while((pos=o.text.find("[DSRV/QUEUE-RAW]",pos))!=std::string::npos){++rawLines;++pos;}
 size_t at=0;
 for(unsigned index=0;index<12;++index){
  at=o.text.find("[DSRV/QUEUE-RAW]",at);assert(at!=std::string::npos);
  const auto start=o.text.find("raw24=",at)+6,end=o.text.find('\n',start);assert(end-start==48);
  for(unsigned byte=0;byte<24;++byte){const auto hex=o.text.substr(start+2*byte,2);assert(std::strtoul(hex.c_str(),nullptr,16)==e.oldest(index).raw[byte]);}
  at=end;
 }
 assert(rawLines==12 && o.text.find("gen=6")!=std::string::npos && o.text.find("gen=7")!=std::string::npos && o.text.find("[DSRV/QUEUE-FIRST-OVER8]")!=std::string::npos);
 assert(o.text.find("reason=CTS_FIRST count=12 gen=7 mode=C")!=std::string::npos && o.longest<448);
 const auto before=o.calls;o.room=2048;service(e,o,time+100,8,3,false,true,0,0,0); // at most the single anomaly notice
 assert(o.calls-before<=1);e.request();drain(e,o,time);assert(!memcmp(e.ring,frozen.ring,sizeof e.ring));
 const auto bootCount=e.total.headers;e.clear(time);assert(!e.frozen && !e.count && e.total.headers==bootCount && e.captureId==1);
 header(r);e.header(r,24,true,9,9,0,time+1);assert(e.freeze(time+2,9,0,5,0xb2) && e.freezeGeneration==9);
 std::puts("12-slot wrap, first over8, first CTS freeze, generations/recovery, read-only replay/CLEAR, slow UART/CTS priority: PASS");
 // Maximal report fields and the diagnostic-only periodic row in quiet mode D.
 e=Evidence{};e.window.headers=UINT32_MAX;e.window.qmax=255;e.windowGeneration=UINT32_MAX;e.windowKnown=true;e.windowMode=0;
 for(auto& v:e.window.value)v=UINT16_MAX;
 for(auto& v:e.window.hist)v=UINT16_MAX;
 e.cachedFree8=UINT32_MAX;o=Output{};
 service(e,o,UINT32_MAX,UINT32_MAX,3,false,true,UINT32_MAX,UINT32_MAX,2048);
 assert(o.calls==1 && o.text.find("scope=WINDOW")!=std::string::npos && o.text.find("mode=D")!=std::string::npos && o.text.find("sat=1")!=std::string::npos && o.text.find("mixed=1")!=std::string::npos && o.longest<448);
 assert(!e.window.headers && !e.reportDrops && !chip.commands);
 e=Evidence{};e.total.headers=UINT32_MAX;e.total.qmax=e.total.sourceMax[0]=e.total.sourceMax[1]=255;
 for(auto& v:e.total.value)v=UINT16_MAX;
 for(auto& v:e.total.hist)v=UINT16_MAX;
 header(r,255,255,255);memset(r+8,255,12);
 for(auto& item:e.ring)item.set(r,UINT32_MAX,UINT32_MAX,3);
 e.count=12;e.firstOver8=e.ring[0];e.firstKnown=true;e.freeze(UINT32_MAX,UINT32_MAX,3,255,255);
 uint32_t wrap=UINT32_MAX-200;drain(e,o,wrap);assert(!e.reportDrops && o.longest<448);
 std::printf("Bounded reporter/max fields/quiet D/no heap or extra SPI; state=%u bytes, longest=%u bytes: PASS\n",unsigned(sizeof(Evidence)),unsigned(o.longest));
}
'''
file=out/'queue.cpp';file.write_text(cpp,encoding='utf-8');exe=out/'queue.exe'
cc=sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe'
subprocess.run([cc,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(file),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

# Binding audit: successful/current reply only, before ALL existing full reads.
body=method('void DAB::finishDabCommand(void)')
a=body.index('case DabCommand::DsrvHeader:');b=body.index('case DabCommand::EventStatus:',a)
case=body[a:b]
assert body.index('generationMatches')<a
assert case.index('if (!parseOk(parseResult))')<case.index('dsrvQueue.header(')<case.index('readDsrvPayload(')
for signature in ['bool DAB::begin(uint8_t SSpin, RadioMode requestedMode)','void DAB::onHardwareResetComplete(void)','void DAB::setFreq(uint8_t freq)']:
    # Tuning/reset signatures are checked against actual declarations, below.
    assert 'dsrvQueue.clear(' not in method(signature)
assert source.count('dsrvQueue.clear(')==1 # The explicit manual method only.
assert 'dsrvQueue.freeze(' in method('void DAB::observeDabTerminal(uint8_t operation,int result,uint32_t generation)')
assert '#if SI4684_DSRV_QUEUE_DIAG' in case
print('Actual successful-command/raw-before-overwrite/freeze/manual-only-clear bindings: PASS')
