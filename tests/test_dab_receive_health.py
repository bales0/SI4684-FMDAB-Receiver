"""Production watchdog/DSRV policy and actual SLS body dispatch/timer methods."""
from pathlib import Path
import subprocess,sys,re
ROOT=Path(__file__).resolve().parents[1];out=ROOT/'.pio/dab_receive_health';out.mkdir(exist_ok=True)
s=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
def block(start):
 a=s.index('{',start);b=a+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a+1:b-1]
def method(signature):
 a=s.index(signature+' {');return signature+' {'+block(a)+'}\n'
a=s.index('// MOT body segment.');a=s.index('const uint16_t transportID',a);brace=s.rfind('{',0,a)
body=block(brace)
timer=block(s.index('if (SlideShowInit && uint32_t(now-SlideShowLastActivity)'))
prefix=r'''
#include "epg_core.h"
#include "epg_transport_diag.h"
#include "dab_scheduler_policy.h"
#include "dab_receive_health.h"
#include "vendor/si468x/Si468x.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <vector>
static uint32_t clockMs=0;uint32_t millis(){return clockMs;}
void quiet(const char*,...){}
#define DIAG_PRINTF(...) quiet(__VA_ARGS__)
#define DIAG_PRINTLN(...) quiet(__VA_ARGS__)
static dab_health::Watchdog dabProgress;
static dab_health::Stall dabStallReason=dab_health::Stall::None;
static bool payloadReached=false;
static uint32_t diagDabCommandErrorCount=0,diagDabRecoveryRequestCount=0,diagDabHostStarvationCount=0,diagDabHostStarvationMaxGapUs=0,diagDabCtsTimeoutCount=0,diagDabGenuineCtsTimeoutCount=0;
struct Chip{si468x::Result result=si468x::Result::Timeout;unsigned gap=8500,headerCount=0;si468x::Result readCurrentReply(uint8_t* p,uint16_t){p[18]=uint8_t(headerCount);p[19]=uint8_t(headerCount>>8);return result;}si468x::Result lastResult(){return result;}unsigned lastServiceGapUs(){return gap;}}chip;
void finishCommandDiagnostics(si468x::Result){}
void SlideshowReceptionState(bool){}
struct Monitor{unsigned availableForWrite(){return 4096;}}diagSerial;
uint8_t SPIbuffer[4096];
struct DAB {
 bool dabTransportStalled=false;uint8_t dabConsecutiveCtsTimeouts=0;
 enum class DabCommand{None,StopService};DabCommand dabCommand=DabCommand::None;
 uint32_t dabCommandGeneration=1,dabGeneration=1;
 void requestDabRecovery(dab_health::Stall);void finishDabCommand();void readGuard();
 dab_scheduler::CtsTimeoutClass recordDabCtsResult(bool);
 bool transportStalled(){return dabTransportStalled;}
 static const size_t SLS_BUFFER_BYTES=51200;static const unsigned SLS_MAX_SEG_SIZE=2048,SLS_MAX_SEGMENTS=256;
 uint8_t bytes[SLS_BUFFER_BYTES]={},*slideshowSegBuf=bytes,SlideShowSegmentBitmap[32]={};uint16_t slideshowSegLen[256]={};
 bool SlideShowInit=false,SlideShowTransportIDValid=false,slideshowPublishedPending=false,SlideShowUpdate=false,SlideShowUpdate2=false,SlideShowAvailable=false,SlideShowLastSegmentValid=false,lastCompletedTransportIdValid=false,SlideShowDebug=false,SlideShowVerbose=false;
 uint32_t SlideShowLastActivity=0,slsDuplicateValid=0,slsDuplicateConflict=0,slsCrcBad=0,SlideShowByteCounter=0,SlideShowLength=0,slideshowFirstSegmentMs=0,slideshowRamSize=0;
 uint16_t SlideShowTransportID=0,lastCompletedTransportId=0,SlideShowTotalSegments=0;
 uint8_t SlideShowLastSegment=0,SlideShowHighestSegment=0;
 struct Switch{bool slsContextValid=true;}dabSwitch;
 void discardStaleMot(){}void logRepeatedCompletedMot(uint16_t){}
 bool acceptSlideshowDuplicate(uint8_t,bool,const uint8_t*,uint16_t,bool);
 bool storeSlideshowSegment(uint8_t,const uint8_t*,uint16_t);
 bool allSegmentsReceived();void clearSegmentBuffer();void resetSlideshowCollector();void lockSlideshowTransport(uint16_t);
 void beginSlideshowReception();void acknowledgeSlideshow();void assembleSlideshow();void body(uint32_t byte_count);void tick();
};
'''
threshold=re.search(r'static constexpr uint8_t RADIO_DAB_STALL_TIMEOUT_COUNT = .*?;',s)[0]
methods=threshold+'\n'+''.join(method(signature) for signature in [
 'bool DAB::acceptSlideshowDuplicate(uint8_t segment, bool last, const uint8_t* data, uint16_t length, bool crcValid)',
 'bool DAB::storeSlideshowSegment(uint8_t segmentNumber,\n                                const uint8_t* data,\n                                uint16_t dataLength)',
 'void DAB::clearSegmentBuffer(void)','void DAB::resetSlideshowCollector(void)',
 'void DAB::lockSlideshowTransport(uint16_t transportId)','void DAB::beginSlideshowReception(void)',
 'void DAB::acknowledgeSlideshow(void)','bool DAB::allSegmentsReceived(void)','void DAB::assembleSlideshow(void)',
 'void DAB::requestDabRecovery(dab_health::Stall reason)','dab_scheduler::CtsTimeoutClass DAB::recordDabCtsResult(\n    bool timedOut)'])
finish=s[s.index('void DAB::finishDabCommand(void) {'):s.index('  // Reconcile physical probe ownership')]
methods+=finish+'(void)ctsClass;(void)completed;}\n'
a=s.index('  byte_count = SPIbuffer[19]',s.index('void DAB::getServiceData'))
b=s.index('        // Read DAB Dynamic Label',a)
methods+='void DAB::readGuard(){uint32_t byte_count=0;'+s[a:b]+'payloadReached=true;}}}}\n'

sketch=(ROOT/'SI4684-DAB-Receiver.ino').read_text(encoding='utf-8')
start=sketch.index('    if (radioMode == RADIO_MODE_DAB && radio.transportStalled()) {')
a=sketch.index('{',start);b=a+1;depth=1
while depth:depth+=(sketch[b]=='{')-(sketch[b]=='}');b+=1
methods+='''
DAB radio;unsigned radioMode=1;static const unsigned RADIO_MODE_DAB=1;
bool RadioStallRecoveryPerformed=false,dabServiceSelectionPending=false;
uint32_t LastRadioStallRecoveryMs=0;static const uint32_t RADIO_STALL_RECOVERY_MIN_INTERVAL_MS=60000;
unsigned coldRecoveries=0;void doRecovery(){++coldRecoveries;}
void recoveryTick(){'''+sketch[start:b]+'}\n'
main=r'''
static void send(DAB& d,const std::vector<uint8_t>& image,unsigned segment,uint16_t tid=7,bool conflict=false,bool badCrc=false,bool flipLast=false){
 unsigned start=segment*80,n=unsigned(image.size())-start;if(n>80)n=80;
 memset(SPIbuffer,0,sizeof(SPIbuffer));SPIbuffer[25]=0x74;
 bool last=start+n==image.size();if(flipLast)last=!last;
 SPIbuffer[27]=last?0x80:0;SPIbuffer[28]=uint8_t(segment);SPIbuffer[29]=0x12;SPIbuffer[30]=uint8_t(tid>>8);SPIbuffer[31]=uint8_t(tid);
 SPIbuffer[32]=uint8_t(n>>8);SPIbuffer[33]=uint8_t(n);memcpy(SPIbuffer+34,image.data()+start,n);if(conflict)SPIbuffer[34]^=1;
 const auto crc=epg::crc16(SPIbuffer+25,n+9);SPIbuffer[34+n]=uint8_t(crc>>8);SPIbuffer[35+n]=uint8_t(crc)^badCrc;
 d.body(n+11);
}
int main(int argc,char** argv){
 assert(argc==2);std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)),{});assert(image.size()==267);
 DAB d;chip.result=si468x::Result::Ok;SPIbuffer[19]=20;chip.headerCount=65535;d.readGuard();assert(!payloadReached && diagDabCommandErrorCount==1);
 SPIbuffer[19]=20;SPIbuffer[20]=0;chip.headerCount=20;d.readGuard();assert(payloadReached);payloadReached=false;SPIbuffer[19]=0xe8;SPIbuffer[20]=0x0f;d.readGuard();assert(!payloadReached);
 chip.result=si468x::Result::Timeout;d.SlideShowLength=unsigned(image.size());
 // Out of order packed offsets, last segment absent until 90 seconds.
 send(d,image,2);send(d,image,0);send(d,image,1);
 const auto bytes=d.SlideShowByteCounter;uint8_t bitmap[32];memcpy(bitmap,d.SlideShowSegmentBitmap,32);
 for(unsigned t=10000;t<90000;t+=10000){clockMs=t;send(d,image,(t/10000)%3);d.tick();assert(d.SlideShowInit && d.SlideShowTransportID==7 && d.SlideShowByteCounter==bytes && !memcmp(bitmap,d.SlideShowSegmentBitmap,32));}
 assert(d.slsDuplicateValid==8);clockMs=90000;send(d,image,3);assert(d.SlideShowAvailable && d.slideshowPublishedPending && d.slideshowRamSize==image.size() && !memcmp(d.bytes,image.data(),image.size()));
 const auto activity=d.SlideShowLastActivity;clockMs+=1000;send(d,image,0);assert(!d.SlideShowInit && d.SlideShowLastActivity==activity);
 d.acknowledgeSlideshow();d.lastCompletedTransportIdValid=false;d.SlideShowLength=unsigned(image.size());clockMs=100000;send(d,image,0);
 clockMs=120000;send(d,image,0,7,true);assert(d.SlideShowLastActivity==100000 && d.slsDuplicateConflict==1);
 send(d,image,0,7,false,true);assert(d.SlideShowLastActivity==100000 && d.slsCrcBad==1);
 send(d,image,0,7,false,false,true);assert(d.SlideShowLastActivity==100000 && d.slsDuplicateConflict==2);
 send(d,image,1,8);assert(d.SlideShowLastActivity==100000 && d.SlideShowTransportID==7);
 d.dabSwitch.slsContextValid=false;send(d,image,0);assert(d.SlideShowLastActivity==100000);d.dabSwitch.slsContextValid=true;
 clockMs=130001;d.tick();assert(!d.SlideShowInit);
 // Timestamp zero and wrap are genuine activity, not sentinel values.
 clockMs=0;send(d,image,0);clockMs=30001;d.tick();assert(!d.SlideShowInit);
 clockMs=UINT32_MAX-100;send(d,image,0);clockMs=100;send(d,image,0);clockMs=30101;d.tick();assert(!d.SlideShowInit);
 std::puts("Production SLS duplicate/CRC/content/last flag/ownership/90s completion/silence/wrap: PASS");
 using namespace dab_health;
 Watchdog w;for(unsigned i=0;i<3;++i){w.started(i*1000,5);assert(w.terminal(i*1000+999,false,false)==Stall::None);}assert(w.failures==3);
 assert(w.terminal(4000,true,false)==Stall::None && !w.failures);
 bool trip=false;for(unsigned i=0;i<156;++i){w.started(5000+i*1000,5);if(w.terminal(5999+i*1000,false,false)==Stall::CtsNoProgress)trip=true;}assert(trip);
 w=Watchdog{};for(unsigned i=0;i<156;++i){w.started(i*10000,5);assert(w.terminal(i*10000+1000,true,false)==Stall::None);}
 assert(!w.failures);w=Watchdog{};for(unsigned i=0;i<8;++i)w.terminal(i*1000,false,true);assert(w.failures==8 && w.terminal(8000,false,true)==Stall::Transport);
 w=Watchdog{};w.started(UINT32_MAX-100,5);for(unsigned i=0;i<8;++i)trip=w.terminal(UINT32_MAX-100+i*1000,false,false)!=Stall::None;assert(trip);
 std::puts("Production watchdog: 1/3/156 host-gap failures, valid CTS reset, idle, transport error, rollover: PASS");
 // Actual completion/CTS classification and idempotent recovery gate together.
 for(unsigned i=0;i<156;++i){clockMs=i*1000;radio.dabCommand=DAB::DabCommand::StopService;dabProgress.started(clockMs,5);radio.finishDabCommand();}
 assert(radio.dabTransportStalled && dabStallReason==Stall::CtsNoProgress && diagDabRecoveryRequestCount==1 && diagDabHostStarvationCount==156);
 radio.requestDabRecovery(Stall::Transport);assert(diagDabRecoveryRequestCount==1);
 clockMs=0;recoveryTick();assert(coldRecoveries==1);clockMs=59999;recoveryTick();assert(coldRecoveries==1);clockMs=60000;recoveryTick();assert(coldRecoveries==2);
 std::puts("Actual terminal completion -> stall gate -> sketch 60s recovery limiter: PASS");
 Dsrv q;q.status(true);assert(q.ready(0));q.start(0);q.status(false);q.status(true);q.finish(1,0,0,true);assert(q.pending && q.newer==1);
 q.start(2);q.finish(3,0,0,true);assert(!q.pending);q.status(true);assert(!q.pending); // unchanged latch must not loop
 q.reset();q.status(false);q.status(true);q.start(10);q.status(true);q.finish(11,0,27,true);assert(q.pending && q.suspect==1);q.start(12);q.status(true);q.finish(13,0,0,true);assert(!q.pending);
 for(unsigned n:{1U,3U,255U}){q.start(20);q.finish(21,n,27,true);assert(q.pending);}
 q.start(30);q.status(false);q.status(true);q.finish(31,0,0,false);assert(!q.ready(32) && q.ready(51));
 q.start(52);q.reset();q.finish(53,10,27,true);assert(!q.pending);
 q.reset();q.status(true);q.start(UINT32_MAX-5);q.finish(UINT32_MAX-4,0,0,false);assert(!q.ready(0) && q.ready(16));
 std::puts("Production DSRV pending: start/completion/full-reply race, sticky-high bound, 0/1/N, malformed, generation, wrap: PASS");
}
'''
source=out/'health.cpp';source.write_text(prefix+methods+'void DAB::body(uint32_t byte_count){'+body+'}\nvoid DAB::tick(){const uint32_t now=millis();if(SlideShowInit && uint32_t(now-SlideShowLastActivity)>30000U){'+timer+'}}\n'+main,encoding='utf-8')
exe=out/'health.exe';cc=sys.argv[1] if len(sys.argv)>1 else 'g++'
subprocess.run([cc,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(source),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe),str(ROOT/'tests/fixtures/epg/synthetic_logo_0.png')],check=True)
