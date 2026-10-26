"""Actual production SPI acquisition scheduling and DSRV payload admission bodies."""
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
def function(sig):
 a=s.index(sig+' {');b=s.index('{',a)+1;d=1
 while d:d+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
a=s.index('  const uint32_t now = millis();',s.index('void DAB::scheduleNextDabCommand'))
b=s.index('  const bool cleanupPending',a)
schedule=s[a:b]
a=s.index('      if (header.byteCount > 0U)',s.index('case DabCommand::DsrvHeader:'))
b=s.index('      dabDsrvPending =',a)
payload=s[a:b]
a=s.index('  const bool cleanupPending',s.index('void DAB::scheduleNextDabCommand'))
b=s.index('  // During bounded STOP backoff',a)
cleanup=s[a:b]
a=s.index('  if (dabSpiProbe.state == dab_probe::State::StartRequested) {',b)
b=s.index('  // UA discovery',a)
start_packet=s[a:b]

prefix=r'''
#include "dab_spi_probe.h"
#include "dab_receive_health.h"
#include "epg_ingress.h"
#include "epg_transport_diag.h"
#include "vendor/si468x/Si468x.h"
#include "dab_spi_discovery.h"
#include "dab_service_switch_policy.h"
#include <cassert>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <string>
static bool epgContinuous=false, epgManualPending=false,epgProductionPaused=false;
static uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
static std::string probeLog;
static void discard(const char* format,...){char b[512];va_list args;va_start(args,format);vsnprintf(b,sizeof(b),format,args);va_end(args);probeLog+=b;}
#define DIAG_PRINTF(...) discard(__VA_ARGS__)
#define DIAG_PRINTLN(...) discard(__VA_ARGS__)
namespace epg {
bool replay=false,enabled=true;
bool replayActive(){return replay;} bool liveEnabled(){return enabled && !replay;}
std::vector<uint8_t> received;
void livePayload(const uint8_t* p,size_t n){received.assign(p,p+n);}
void livePadEndpoint(uint32_t,uint32_t){}
void livePadPayload(const uint8_t* p,size_t n){received.assign(p,p+n);}
void livePadData(uint32_t sid,uint32_t cid,const uint8_t* p,size_t n){livePadEndpoint(sid,cid);livePadPayload(p,n);}
}
struct Monitor { uint32_t droppedBytes(){return 0;} } diagSerial;

uint8_t SPIbuffer[4096];
struct Chip {
 unsigned reads=0,length=0;si468x::Result result=si468x::Result::Ok;
 si468x::Result readCurrentReply(uint8_t* out,uint16_t n){++reads;length=n;for(unsigned i=0;i<n;++i)out[i]=uint8_t(i);memset(out+8,0,8);out[18]=uint8_t(n-24);out[19]=uint8_t((n-24)>>8);return result;}
} chip;
void finishCommandDiagnostics(si468x::Result){}
class DAB { public:
 uint32_t dabGeneration=1;si468x::Result readDsrvPayload(uint16_t);
 bool SlideShowDebug=false,SlideShowVerbose=false;bool setDsrvDiagnosticMode(char);
 bool fm=false,signallock=true,diagnosticDebug=true,dabSpiPresent=true;
 bool dabActiveServiceValid=true,dabActiveDataServiceValid=false,dabServiceRequestPending=false,dabTuneRequestPending=false;
 uint32_t dabActiveServiceId=0x3802,dabActiveComponentId=0x01FE000B;
 uint32_t dabActiveDataServiceId=0,dabActiveDataComponentId=0;
 struct Switch { uint32_t requestId=1;dab_switch::State state=dab_switch::State::Ready; } dabSwitch;
 struct Discovery { dab_spi::Status status=dab_spi::Status::SelectedEnsemble; bool available(){return true;}uint32_t serviceId=0xE231B3FE,componentId=0x01F2C00B; } dabSpiDiscovery;
 dab_probe::Probe dabSpiProbe;
 uint32_t dabSpiProbeLastSummaryMs=0,dabSpiProbeSuppressedRoutes=0,dabSpiProbeLastRouteMs[4]={};
 uint32_t dabSpiProbeCtsStart=0,dabSpiProbeOverflowStart=0,dabSpiProbeUartDropStart=0;
 uint32_t diagDabCtsTimeoutCount=0,diagDabDsrvOverflowCount=0,diagDabCommandErrorCount=0,diagDabStaleMotCount=0;
 unsigned legacy=0;
 dab_health::Dsrv dabDsrvEvents;
 bool xpadConfirmed=false;
 bool ServiceStart=true,dabUaInventoryValid=true;
 dab_spi::Identity xpadSpiIdentity() const{return xpadConfirmed?dab_spi::Identity::Spi:dab_spi::Identity::NotSpi;}
 bool xpadSpiPresent() const{return xpadConfirmed;}
 bool isFm(){return fm;}
 bool requestSpiProbe(bool production=false);
 enum class DabCommand{SpiProbeStart,SpiProbeStop};
 uint32_t dabLastLowPriorityCommandMs=0;unsigned submits=0;bool submitOk=true;
 si468x::Command submitted=si468x::Command::GET_SYS_STATE;uint32_t submittedSid=0,submittedCid=0;
 bool startDabCommand(DabCommand,uint8_t cmd,const uint8_t* args,unsigned n){assert(n==11);++submits;submitted=si468x::Command(cmd);submittedSid=si468x::readLe32(args+3);submittedCid=si468x::readLe32(args+7);return submitOk;}
 void logSpiProbeSummary(const char*){}
 void cleanup();void startPacket();void modes();
 void advance();
 void handle(const si468x::DsrvHeader& header,dab_probe::Route target,bool feedLegacy);
 void getServiceData(){++legacy;}
};
'''
suffix=r'''

void DAB::modes(){
 using namespace dab_loss;
 evidence()=Evidence{};epgContinuous=false;epgManualPending=false;epgProductionPaused=false;
 DAB r;clockMs=500;r.advance();clockMs+=500;r.advance();r.startPacket();
 assert(r.submitted==si468x::Command::START_DIGITAL_SERVICE && r.submits==1);
 assert(r.submittedSid==r.dabSpiProbe.spi.sid && r.submittedCid==r.dabSpiProbe.spi.cid);
 r.dabSpiProbe.started(clockMs,true,false);assert(epgContinuous && r.setDsrvDiagnosticMode('B'));
 assert(epgProductionPaused && r.dabActiveServiceValid && r.dabSpiProbe.state==dab_probe::State::StopRequested);
 r.cleanup();assert(r.submits==2 && r.submitted==si468x::Command::STOP_DIGITAL_SERVICE);
 assert(r.submittedSid==0xE231B3FE && r.submittedCid==0x01F2C00B); // packet only
 r.dabSpiProbe.stopped(clockMs,true);r.advance();assert(r.dabSpiProbe.state==dab_probe::State::Idle);
 assert(!r.requestSpiProbe() && r.setDsrvDiagnosticMode('A') && !epgProductionPaused);
 r.advance();clockMs+=500;r.advance();r.startPacket();assert(r.submits==3 && r.submitted==si468x::Command::START_DIGITAL_SERVICE);
 r.dabSpiProbe.started(clockMs,true,false);
 assert(r.setDsrvDiagnosticMode('C') && epgContinuous);si468x::DsrvHeader h;h.byteCount=27;unsigned reads=chip.reads;chip.result=si468x::Result::Ok;epg::received.clear();
 r.handle(h,dab_probe::Route::Spi,false);assert(chip.reads==reads+1 && epg::received.empty() && evidence().total.event[ParseSkipped]==1);
 h.dataSource=1;h.rawUatypeOrRfu=2;r.handle(h,dab_probe::Route::Audio,true);assert(r.legacy==1);
 assert(r.setDsrvDiagnosticMode('D') && !r.diagnosticDebug && !r.SlideShowDebug && !r.SlideShowVerbose);
 h.dataSource=0;r.handle(h,dab_probe::Route::Spi,false);assert(!epg::received.empty()); // D parses normally
 assert(r.setDsrvDiagnosticMode('A') && r.diagnosticDebug);
 // A request during STOP cannot issue another START before cleanup finishes.
 assert(r.setDsrvDiagnosticMode('B'));r.cleanup();assert(r.setDsrvDiagnosticMode('A'));r.advance();assert(r.dabSpiProbe.state==dab_probe::State::Stopping);
 r.dabSpiProbe.stopped(clockMs,true);r.advance();assert(r.dabSpiProbe.state==dab_probe::State::Baseline);
 // In-flight START cancelled by B is followed by the existing conservative STOP.
 clockMs+=500;r.advance();r.startPacket();assert(r.setDsrvDiagnosticMode('B'));
 r.dabSpiProbe.started(clockMs,true,false);assert(r.dabSpiProbe.state==dab_probe::State::StopRequested);
 r.cleanup();r.dabSpiProbe.stopped(clockMs,true);assert(r.setDsrvDiagnosticMode('A'));r.advance();
 // Shared SPI/audio endpoint is unsupported, never STOP audio or SLS.
 r.dabSpiProbe.spi=r.dabSpiProbe.audio;epgContinuous=true;unsigned before=r.submits;
 assert(!r.setDsrvDiagnosticMode('B') && r.submits==before);
 r.dabSpiProbe.state=dab_probe::State::CleanupFailed;assert(!r.setDsrvDiagnosticMode('A'));
 evidence()=Evidence{};epgContinuous=false;epgProductionPaused=false;
 std::puts("Actual A/B/C/D mode setter + production START/STOP submissions, pending START/STOP transitions, parser-only bypass/PAD and shared-owner refusal: PASS");
}
int main(){
 // Baseline cancellation uses the actual scheduler context predicate.
 for(unsigned failure=0;failure<4;++failure){
  DAB r;epgContinuous=false;epgManualPending=false;clockMs=100;r.advance();assert(r.dabSpiProbe.state==dab_probe::State::Baseline);
  if(failure==0)r.signallock=false;else if(failure==1)r.dabTuneRequestPending=true;else if(failure==2)r.dabServiceRequestPending=true;else ++r.dabSwitch.requestId;
  ++clockMs;r.advance();assert(r.dabSpiProbe.state==dab_probe::State::Idle && chip.reads==0);
 }
 epgContinuous=false;epgManualPending=false;clockMs=0;
 DAB radio;radio.advance();assert(epgContinuous && radio.dabSpiProbe.state==dab_probe::State::Baseline);
 assert(probeLog.find("mode=LIVE baseline=500 ms")!=std::string::npos);
 clockMs=499;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::Baseline);
 clockMs=500;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::StartRequested);
 radio.dabSpiProbe.state=dab_probe::State::Starting;radio.dabSpiProbe.started(clockMs,true,false);
 clockMs=100000;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::Observing);
 assert(radio.requestSpiProbe() && epgManualPending && radio.dabSpiProbe.state==dab_probe::State::StopRequested);
 radio.dabSpiProbe.stopped(clockMs,true);radio.advance();assert(!epgContinuous && !epgManualPending && radio.dabSpiProbe.state==dab_probe::State::Baseline);
 assert(probeLog.find("mode=DEBUG baseline=5000 ms")!=std::string::npos);
 clockMs+=4999;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::Baseline);
 clockMs+=1;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::StartRequested);
 radio.dabSpiProbe.state=dab_probe::State::Starting;radio.dabSpiProbe.started(clockMs,true,false);
 clockMs+=30000;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::StopRequested);
 radio.dabSpiProbe.stopped(clockMs,true);radio.advance();assert(epgContinuous);
 clockMs+=5000;radio.advance();radio.dabSpiProbe.state=dab_probe::State::Starting;radio.dabSpiProbe.started(clockMs,true,false);
 epg::replay=true;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::StopRequested);
 epg::replay=false;
 for(unsigned i=1;i<=3;++i){radio.dabSpiProbe.stopAttempts=i;radio.dabSpiProbe.stopped(clockMs,false);}
 assert(radio.dabSpiProbe.state==dab_probe::State::CleanupFailed && radio.dabSpiProbe.spi.valid && !radio.requestSpiProbe());
 radio.dabSpiProbe=dab_probe::Probe{};epgContinuous=false;radio.advance();clockMs+=5000;radio.advance();radio.dabSpiProbe.state=dab_probe::State::Starting;radio.dabSpiProbe.started(clockMs,true,false);
 radio.signallock=false;radio.advance();assert(radio.dabSpiProbe.state==dab_probe::State::StopRequested);
 // Actual payload branch: whole preserved reply and exact 24-byte offset.
 radio.signallock=true;radio.dabSpiProbe.state=dab_probe::State::Observing;
 si468x::DsrvHeader h;h.byteCount=960;
 radio.handle(h,dab_probe::Route::Spi,false);
 assert(chip.reads==1 && chip.length==984 && epg::received.size()==960 && epg::received[0]==24 && radio.legacy==0);
 h.byteCount=4071;radio.handle(h,dab_probe::Route::Spi,false);assert(chip.length==4095 && epg::received.size()==4071);
 unsigned reads=chip.reads;
 h.byteCount=4072;radio.handle(h,dab_probe::Route::Spi,false);assert(chip.reads==reads);
 h.byteCount=10;h.dataSource=1;radio.handle(h,dab_probe::Route::Spi,false);assert(chip.reads==reads);
 h.dataSource=0;epg::replay=true;radio.handle(h,dab_probe::Route::Spi,false);assert(chip.reads==reads);epg::replay=false;
 radio.dabTuneRequestPending=true;radio.handle(h,dab_probe::Route::Spi,false);assert(chip.reads==reads);radio.dabTuneRequestPending=false;
 chip.result=si468x::Result::Timeout;epg::received.clear();radio.handle(h,dab_probe::Route::Spi,false);assert(epg::received.empty() && radio.diagDabCommandErrorCount==1);
 radio.handle(h,dab_probe::Route::Audio,true);assert(radio.legacy==1);
 radio.handle(h,dab_probe::Route::Ignored,false);assert(radio.diagDabStaleMotCount==1);
 radio.modes();
 std::puts("Actual SPI production/manual windows, replay/lock cleanup, quarantine, full DSRV read/bounds/legacy isolation: PASS");
}
'''
out=ROOT/'.pio/epg_transport_test';out.mkdir(parents=True,exist_ok=True)
cpp=out/'transport.cpp';cpp.write_text(prefix+function('si468x::Result DAB::readDsrvPayload(uint16_t length)')+'\n'+function('bool DAB::setDsrvDiagnosticMode(char mode)')+'\n'+function('bool DAB::requestSpiProbe(bool production)')+'\nvoid DAB::cleanup(){const uint32_t now=millis();\n'+cleanup+'}\nvoid DAB::startPacket(){const uint32_t now=millis();\n'+start_packet+'}\nvoid DAB::advance(){\n'+schedule+'}\nvoid DAB::handle(const si468x::DsrvHeader& header,dab_probe::Route target,bool feedLegacy){\n'+payload+'}\n'+suffix,encoding='utf-8')
exe=out/'transport.exe'
subprocess.run([(sys.argv[1] if len(sys.argv)>1 else 'g++'),'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(cpp),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
