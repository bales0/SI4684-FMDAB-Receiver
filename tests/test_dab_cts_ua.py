"""Actual first-timeout hook, modes, UA commands and saturated UART; no RF claim."""
from pathlib import Path
import subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'.pio/cts_ua';out.mkdir(parents=True,exist_ok=True)
radio=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
header=(ROOT/'src/si4684.h').read_text(encoding='utf-8')
comms=(ROOT/'src/comms.cpp').read_text(encoding='utf-8')

def method(signature):
    a=radio.index(signature+' {');b=radio.index('{',a)+1;depth=1
    while depth:depth+=(radio[b]=='{')-(radio[b]=='}');b+=1
    return radio[a:b]

# A tiny Arduino boundary: compile the real NonBlockingSerialMonitor unchanged.
(out/'Arduino.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cassert>
#include <cstring>
#include <string>
struct Stream {
 virtual ~Stream(){}
 virtual size_t write(uint8_t)=0;virtual size_t write(const uint8_t*,size_t)=0;
 virtual int available()=0;virtual int read()=0;virtual int peek()=0;
 virtual int availableForWrite()=0;virtual void flush()=0;
 void println(const char* s){write(reinterpret_cast<const uint8_t*>(s),std::strlen(s));write(uint8_t('\n'));}
 void printf(const char* f,...){char b[1200];va_list a;va_start(a,f);int n=vsnprintf(b,sizeof b,f,a);va_end(a);assert(n>=0 && n<int(sizeof b));write(reinterpret_cast<const uint8_t*>(b),size_t(n));}
};
struct HardwareSerial {
 int room=0;std::string output;
 void begin(unsigned long){}
 int available(){return 0;}int read(){return -1;}int peek(){return -1;}
 int availableForWrite(){return room;}
 size_t write(uint8_t b){output+=char(b);return 1;}
 size_t write(const uint8_t* p,size_t n){output.append(reinterpret_cast<const char*>(p),n);return n;}
};
''',encoding='utf-8')
enum=header[header.index('    enum class DabCommand : uint8_t {'):header.index('    bool SlideShowInit;')]
commands=comms[comms.index('    if (strncmp(input,"DSRV UA PAUSE ",14)==0)'):comms.index('    if (strcmp(input, "DEBUG") == 0)')]
start=radio.index('void DAB::finishDabCommand(void) {')
terminal=radio[start:radio.index('  // Reconcile physical probe ownership',start)]+'(void)ctsClass;}\n'
cpp=r'''
#include "serial_monitor.h"
#include "dab_cts_evidence.h"
#include "dab_loss_diag_output.h"
#include "dab_receive_health.h"
#include "dab_spi_discovery.h"
#include "dab_spi_probe.h"
#include "dab_scheduler_policy.h"
#include "epg_core.h"
#include "epg_transport_diag.h"
#include "vendor/si468x/Si468x.h"
#include <algorithm>
#include <cstdlib>
#include <cctype>
static uint32_t tick=100,us=1000000;
uint32_t millis(){return tick;}uint32_t micros(){return us;}
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_32BIT 4
#define MALLOC_CAP_EXEC 8
#define RADIO_SPI_HZ 10000000UL
unsigned heap_caps_get_free_size(unsigned){return 28000;}
unsigned heap_caps_get_largest_free_block(unsigned){return 24000;}
unsigned uxTaskGetStackHighWaterMark(void*){return 3000;}
HardwareSerial serial;
NonBlockingSerialMonitor diagSerial(serial);
static dab_health::CtsEvidence ctsEvidence;
static dab_health::Watchdog dabProgress;
static dab_health::Stall dabStallReason=dab_health::Stall::None;
static uint32_t commandStartedUs=0;
static uint8_t diagLastCommand=0xb2,lastCommandArgs[4]={1,2,3,4},lastCommandArgCount=4;
static bool diagnosticDebug=false,epgContinuous=true,epgManualPending=false,epgProductionPaused=false;
static unsigned diagDabRecoveryRequestCount=0;
struct Chip {
 si468x::Result result=si468x::Result::Timeout;
 unsigned spiReads=0,spiWrites=0;
 uint32_t gap=9000,late=1500;
 uint32_t lastServiceGapUs(){return gap;}
 uint32_t lastDeadlineLatenessUs(){return late;}
 si468x::Result lastResult(){return result;}
}chip;
static epg::Core core;
namespace epg {
 Core* runtime(){return &core;}
 Availability availability(){return Availability::WaitDirectory;}
 bool replay=false;bool replayActive(){return replay;}
}
void finishCommandDiagnostics(si468x::Result){}
struct DAB {
'''+enum+r'''
 bool fm=false,dabTransportStalled=false,SlideShowDebug=false,SlideShowVerbose=false;
 bool dabServiceListReady=true,dabUaInventoryValid=true,dabActiveServiceValid=true;
 bool dabServiceRequestPending=false,dabTuneRequestPending=false;
 dab_spi::Scan dabUaScan;dab_spi::RefreshPause dabUaPause;dab_spi::RefreshCycle dabUaRefresh;
 uint32_t dabGeneration=7,dabCommandGeneration=7,diagDabDsrvOverflowCount=3;
 DabCommand dabCommand=DabCommand::None;
 dab_probe::Probe dabSpiProbe;
 bool isFm()const{return fm;}
 dab_probe::State spiProbeState(){return dabSpiProbe.state;}
 dab_scheduler::CtsTimeoutClass recordDabCtsResult(bool){return dab_scheduler::CtsTimeoutClass::HostStarved;}
 void observeDabTerminal(uint8_t operation,int result,uint32_t generation);
 void serviceCtsEvidence();void requestDsrvStatus();
 void requestDabRecovery(dab_health::Stall reason);
 bool pausePeriodicUa(unsigned seconds);void resumePeriodicUa();
 bool setDsrvDiagnosticMode(char mode);void finishDabCommand();
}radio;
'''
for signature in [
    'void DAB::observeDabTerminal(uint8_t operation,int result,uint32_t generation)',
    'void DAB::serviceCtsEvidence(void)',
    'void DAB::requestDabRecovery(dab_health::Stall reason)',
    'bool DAB::pausePeriodicUa(unsigned seconds)',
    'void DAB::resumePeriodicUa(void)',
    'void DAB::requestDsrvStatus(void)',
    'bool DAB::setDsrvDiagnosticMode(char mode)',
]:cpp+=method(signature)+'\n'
cpp+=terminal+r'''
static void process(char* input){
 for(char* p=input;*p;++p)*p=char(std::toupper(static_cast<unsigned char>(*p)));
'''+commands+r'''
 assert(!"Unrecognized command in test");
}
static void drain(){serial.room=64;while(diagSerial.queuedBytes())diagSerial.service(64);serial.room=0;}
static void command(const char* s){char input[100];std::snprintf(input,sizeof input,"%s",s);process(input);drain();}
static void scanned(){radio.dabUaScan.begin(1,true);radio.dabUaScan.scheduled=radio.dabUaScan.processed=1;}
static void terminalTimeout(unsigned operation){
 radio.dabCommand=static_cast<DAB::DabCommand>(operation);radio.dabCommandGeneration=radio.dabGeneration;
 dabProgress.started(tick,operation);commandStartedUs=us-1000000U;radio.finishDabCommand();
}
int main(){
 diagSerial.enableBuffering();
 // Saturated UART is lossy but never waits for hardware space or contacts SPI.
 uint8_t fill[2070]={};assert(diagSerial.write(fill,sizeof fill)==2048);
 assert(diagSerial.droppedBytes()==22 && diagSerial.queuedBytes()==2048);
 diagSerial.service();assert(diagSerial.queuedBytes()==2048);
 ctsEvidence.status(0,0x20,0x55,0xdf,tick);tick+=3;us+=3000;
 terminalTimeout(15);assert(ctsEvidence.valid && ctsEvidence.first.operation==15);
 assert(ctsEvidence.first.status[3]==0xdf && ctsEvidence.first.sticky==31 && ctsEvidence.first.statusAgeMs==3);
 assert(ctsEvidence.first.elapsedMs==1000 && ctsEvidence.first.core && ctsEvidence.pending);
 const auto initial=ctsEvidence.first;
 radio.serviceCtsEvidence();assert(ctsEvidence.pending && !serial.output.size());
 for(unsigned i=1;i<8;++i){tick+=1000;us+=1000000;terminalTimeout(5);}
 assert(radio.dabTransportStalled && diagDabRecoveryRequestCount==1 && ctsEvidence.recoveryPending);
 assert(ctsEvidence.first.operation==initial.operation && ctsEvidence.first.atMs==initial.atMs);
 drain();serial.output.clear();radio.serviceCtsEvidence();drain();
 assert(serial.output.find("[DAB/CTS-RECOVERY]")!=std::string::npos);
 assert(serial.output.find("result=-4 elapsedMs=1000")!=std::string::npos);
 assert(serial.output.find("DSPERR=1 REPOFERR=1 CMDOFERR=1 ARBERR=1 ERRNR=1")!=std::string::npos);
 assert(serial.output.find("[DAB/CTS-STATE]")!=std::string::npos);
 assert(!chip.spiReads && !chip.spiWrites);
 // Wrong generations cannot rearm/overwrite; a real answer can start a new episode.
 ctsEvidence.context();radio.observeDabTerminal(3,-4,radio.dabGeneration-1);assert(ctsEvidence.armed);
 tick=1;us=500;commandStartedUs=UINT32_MAX-999499;
 radio.dabTransportStalled=false;radio.observeDabTerminal(3,-4,radio.dabGeneration);
 assert(ctsEvidence.first.elapsedMs==1000 && !ctsEvidence.first.statusKnown);
 radio.observeDabTerminal(3,0,radio.dabGeneration);assert(ctsEvidence.armed);
 std::puts("First CTS snapshot: actual completion/recovery, STATUS3, wrap, stale generation, saturated UART, zero extra SPI: PASS");
 // Only periodic UA is paused, and only following a successful full scan.
 radio.dabTransportStalled=false;tick=100;command("DSRV UA PAUSE 60");assert(!radio.dabUaPause.active);
 scanned();command("dsrv ua pause 60");assert(radio.dabUaPause.active);
 assert(serial.output.find("requested=PAUSE seconds=60 result=ACCEPTED")!=std::string::npos);
 radio.dabUaRefresh.completed(tick);tick+=60000;
 assert(radio.dabUaRefresh.ready(tick,true) && !radio.dabUaPause.blocked(tick,radio.dabGeneration));
 command("DSRV UA PAUSE 900");assert(radio.dabUaPause.active);
 ++radio.dabGeneration;assert(!radio.dabUaPause.blocked(tick,radio.dabGeneration));
 radio.dabUaScan.failed=1;command("DSRV UA PAUSE 60");assert(!radio.dabUaPause.active);scanned();
 radio.dabServiceRequestPending=true;command("DSRV UA PAUSE 60");assert(!radio.dabUaPause.active);radio.dabServiceRequestPending=false;
 epg::replay=true;command("DSRV UA PAUSE 60");assert(!radio.dabUaPause.active);epg::replay=false;
 for(const auto* value:{"DSRV UA PAUSE 29","DSRV UA PAUSE 901","DSRV UA PAUSE -60","DSRV UA PAUSE 60x"}){command(value);assert(!radio.dabUaPause.active);}
 tick=UINT32_MAX-1000;command("DSRV UA PAUSE 30");tick=100;
 assert(radio.dabUaPause.blocked(tick,radio.dabGeneration));command("DSRV UA RESUME");assert(!radio.dabUaPause.active);
 std::puts("Actual UA command parser: full valid scan, range, timeout/resume, context, replay, millis wrap: PASS");
 // MODE B acknowledges a request, not the completion of physical STOP.
 radio.dabSpiProbe.spi=dab_probe::Endpoint(0xe23153fe,0x01f2c00c,true);
 radio.dabSpiProbe.audio=dab_probe::Endpoint(0x3201,0x01fe0005,true);
 radio.dabSpiProbe.state=dab_probe::State::Observing;epgContinuous=true;
 const auto revision=core.revision;command("DSRV MODE B");
 assert(epgProductionPaused && radio.dabSpiProbe.state==dab_probe::State::StopRequested);
 assert(radio.dabSpiProbe.spi.valid && core.revision==revision);
 radio.dabSpiProbe.stopped(tick,true);assert(radio.dabSpiProbe.state==dab_probe::State::Idle && !radio.dabSpiProbe.spi.valid);
 command("DSRV MODE C");assert(!epgProductionPaused && dab_loss::evidence().mode==dab_loss::Mode::NoParse);
 command("DSRV MODE D");assert(!diagnosticDebug && !dab_loss::evidence().experiment && !radio.SlideShowVerbose);
 serial.output.clear();for(unsigned n=0;n<80;++n)dab_loss::service(diagSerial,1000*n,false,4,0,0,0,0);
 assert(!diagSerial.queuedBytes());command("DSRV STATUS");assert(serial.output.find("spiHz=10000000 detail=0 core=1")!=std::string::npos);
 assert(dab_loss::evidence().reportRequested && !diagnosticDebug);
 assert(!chip.spiReads && !chip.spiWrites);
 std::puts("Actual modes A-D: safe STOP acceptance/completion, no cache changes, true Quiet and explicit STATUS: PASS");
}
'''
file=out/'cts_ua.cpp';file.write_text(cpp,encoding='utf-8');exe=out/'cts_ua.exe'
cc=sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe'
subprocess.run([cc,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(out),'-I',str(ROOT/'src'),str(file),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

# Verify the exact production gates, not an independent copy of refresh logic.
assert 'dabUaRefresh.ready(now, uaRefreshEligible) && !dabUaPause.blocked(now,dabGeneration)' in method('void DAB::Update(bool allowNewWork)')
assert 'dabUaPause.cancel();' in method('void DAB::resetDabSpiDiscovery(bool preservePresence, bool logInventory)')
assert 'ctsEvidence.status(status.status0,status.status1,status.status2,status.status3,millis());' in method('static void statusChanged(void*, const si468x::Status& status)')
print('Production periodic gate/reset/status callback bindings: PASS')
