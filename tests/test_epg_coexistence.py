"""Whole production DSRV case + UA publication + real runtime/Core coexistence.
Only chip I/O, clock, TFT boundary and allocator are mocked. No replacement owner.
"""
from pathlib import Path
import ast,subprocess,sys,json,re
ROOT=Path(__file__).resolve().parents[1];out=ROOT/'.pio/epg_coexistence';out.mkdir(exist_ok=True)
def literal(path,name):
 tree=ast.parse((ROOT/path).read_text(encoding='utf-8-sig'))
 return next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id==name for t in n.targets))
radioSource=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
def method(sig):
 a=radioSource.index(sig+' {');b=radioSource.index('{',a)+1;depth=1
 while depth:depth+=(radioSource[b]=='{')-(radioSource[b]=='}');b+=1
 return radioSource[a:b]
prefix=literal('tests/test_epg_runtime.py','prefix').split('struct Radio {')[0]
prefix=prefix.replace('struct Monitor {','static int uartRoom=4096;\nstruct Monitor {').replace('int availableForWrite() { return 4096; }','int availableForWrite() { return uartRoom; }')
prefix+='''\n#include "dab_receive_health.h"
#include "epg_ingress.h"\n#include "dab_spi_discovery.h"\n#include "dab_service_switch_policy.h"\n#include "dab_diagnostics.h"\n#include "vendor/si468x/Si468x.h"\n#include <iostream>\n#define DIAG_PRINTF(...) diagSerial.printf(__VA_ARGS__)\nstatic bool epgContinuous=true;\nuint8_t SPIbuffer[4096];
static dab_health::Dsrv dabDsrvEvents;
static dab_health::Watchdog dabProgress;
static dab_health::Stall dabStallReason=dab_health::Stall::None;
struct Chip {bool raiseDsrv=false;std::vector<uint8_t> wire;unsigned reads=0;si468x::Result result=si468x::Result::Ok;
 si468x::Result readCurrentReply(uint8_t* p,uint16_t n){++reads;assert(n==wire.size());std::memcpy(p,wire.data(),n);if(raiseDsrv){dabDsrvEvents.status(false);dabDsrvEvents.status(true);raiseDsrv=false;}return result;}
} chip;
void finishCommandDiagnostics(si468x::Result){}
struct DAB {
 enum class DabCommand {None,ComponentInfo,DsrvHeader};DabCommand dabCommand=DabCommand::None;
 bool dabTransportStalled=false,SlideShowInit=false;
 uint32_t slsDuplicateValid=0,slsDuplicateConflict=0,slsCrcBad=0,SlideShowLastActivity=0,slideshowFirstSegmentMs=0,SlideShowByteCounter=0,SlideShowLength=0,diagDabRecoveryRequestCount=0;uint16_t SlideShowTransportID=0;
 bool fm=false,ServiceStart=true,SlideShowAvailable=true,signallock=true;
 uint32_t slsSignalSid=0,slsSignalCid=0;bool slsSignalPresent=false;
 bool dabUaInventoryValid=true,dabServiceListReady=true,dabActiveServiceValid=true,dabActiveDataServiceValid=false,dabServiceRequestPending=false,dabTuneRequestPending=false;
 unsigned ServiceIndex=0,numberofservices=1,ensembleEcc=0xe2;
 struct Svc{uint32_t ServiceID=0x3201;}service[1];char EID[5]="3153";
 epg::ClockRefresh refresh;const epg::ClockRefresh& clockRefresh()const{return refresh;}
 epg::BroadcastClockSample clock;const epg::BroadcastClockSample& broadcastClock()const{return clock;}
 bool isFm()const{return fm;}bool spiPresent(){return dabSpiPresent;}
 uint32_t dabActiveServiceId=0x3201,dabActiveComponentId=0x01fe0005,dabActiveDataServiceId=0,dabActiveDataComponentId=0;
 struct Switch{dab_switch::State state=dab_switch::State::Ready;}dabSwitch;
 struct UaService{uint32_t sid;uint16_t first;uint8_t count;};
 UaService dabUaServices[2]={{0x3201,0,1},{0xe23153fe,1,1}};uint8_t dabUaServiceCount=2;
 uint32_t dabUaComponents[2]={0x01fe0005,0x01f2c00c};uint16_t dabUaComponentCount=2;
 dab_spi::Identity dabSpiIdentities[2]={dab_spi::Identity::Spi,dab_spi::Identity::Spi};
 dab_spi::Scan dabUaScan;dab_spi::RefreshCycle dabUaRefresh;dab_spi::Result dabSpiDiscovery;
 bool dabSpiPresent=true,dabSpiDiscoveryDirty=true;uint32_t dabSpiDiscoveryGeneration=0;uint8_t dabUaSummaryStage=3;
 dab_probe::Probe dabSpiProbe;dab_diag::RawLog dabRawUaLog;
 bool dabDsrvPending=false;unsigned diagDabCommandErrorCount=0,diagDabDsrvOverflowCount=0,diagDabStaleMotCount=0,legacy=0;
 uint32_t dabSpiProbeLastRouteMs[4]={},dabSpiProbeSuppressedRoutes=0;
 bool parseOk(si468x::Result r){return r==si468x::Result::Ok;}
 bool resolveDabDataService(){return false;}void requestDabDataServiceEvaluation(){}
 dab_spi::Identity xpadSpiIdentity()const;bool xpadSpiPresent()const;void refreshDabSpiDiscovery();void dsrv();
 const dab_spi::Result& spiDiscovery()const{return dabSpiDiscovery;}
 bool epgContextStable()const{return ServiceStart && dabActiveServiceValid && !dabServiceRequestPending && !dabTuneRequestPending;}
 void logEpgTransportPage(uint8_t);
 uint8_t dabRequestedFrequency=24;dab_diag::XpadReadback dabXpad;unsigned diagDabCtsTimeoutCount=0;
 uint32_t getFreq(uint8_t){return 216928;}
 void getServiceData(){++legacy;}
};
DAB radio;
unsigned char dabfreq=25;static bool view=false,visible=false,suspended=false,dirty=true,setvolume=false;
void EpgOpen(){view=visible=true;}void EpgClose(bool){view=false;}void EpgInvalidate(){}
void cmd(const char* s){char p[128];std::snprintf(p,sizeof(p),"%s",s);diagSerial.output.clear();assert(epg::command(p));}
'''
ui=(ROOT/'src/epg_ui.cpp').read_text(encoding='utf-8')
def ui_method(sig):
 a=ui.index(sig+' {');b=ui.index('{',a)+1;d=1
 while d:d+=(ui[b]=='{')-(ui[b]=='}');b+=1
 return ui[a:b]
prefix+='\n'+ui_method('void EpgSuspend()')+'\n'+ui_method('void EpgResume()')+'\n'
# Retain the complete DSRV case, including parse, routing, counters and exact read.
a=radioSource.index('    case DabCommand::DsrvHeader:');b=radioSource.index('    case DabCommand::EventStatus:',a)
case=radioSource[a:b]
runtime=(ROOT/'src/epg_runtime.cpp').read_text(encoding='utf-8-sig')
for include in ('#include "gui.h"','#include "serial_monitor.h"','#include <esp_heap_caps.h>'):runtime=runtime.replace(include,'')
fixture=(ROOT/'tests/test_epg_cache.cpp').read_text();fixture=fixture[fixture.index('using Data='):fixture.index('int main()')]
# Builders from tests provide the same correct PI wire format; only directory/body used here.
fixture=fixture[:fixture.index('static void directory(')]
main=r'''
static void completeScan(){radio.dabUaScan.begin(radio.dabUaComponentCount,true);radio.dabUaScan.scheduled=radio.dabUaComponentCount;radio.dabUaScan.processed=radio.dabUaComponentCount;radio.dabSpiDiscoveryDirty=true;}
static void inject(const Data& payload,unsigned source=1,unsigned ua=7,uint32_t sid=0,uint32_t cid=0,unsigned state=0,unsigned interrupt=0,unsigned dsc=60){
 if(!sid)sid=source==0?0xe23153fe:0x3201;
 if(!cid)cid=source==0?0xc00c:5;
 chip.wire.assign(24,0);chip.wire[0]=128;chip.wire[4]=uint8_t(interrupt);chip.wire[6]=uint8_t(state);chip.wire[7]=uint8_t((source<<6)|dsc);
 for(unsigned i=0;i<4;++i){chip.wire[8+i]=uint8_t(sid>>(8*i));chip.wire[12+i]=uint8_t(cid>>(8*i));}
 chip.wire[16]=uint8_t(ua);chip.wire[17]=uint8_t(ua>>8);chip.wire[18]=uint8_t(payload.size());chip.wire[19]=uint8_t(payload.size()>>8);
 chip.wire.insert(chip.wire.end(),payload.begin(),payload.end());std::memcpy(SPIbuffer+1,chip.wire.data(),24);++testMillis;radio.dsrv();
 auto* c=epg::runtime();while(c && c->busy())c->pump();epg::poll();
}
static std::vector<Data> readGroups(const char* path){std::ifstream f(path,std::ios::binary);assert(f.good());std::vector<Data> out;while(f.peek()!=EOF){uint8_t h[2];f.read(reinterpret_cast<char*>(h),2);Data b(h[0]|unsigned(h[1])<<8);f.read(reinterpret_cast<char*>(b.data()),b.size());assert(size_t(f.gcount())==b.size());out.push_back(b);}return out;}
static void configure(){
 dabfreq=24;radio.dabUaServiceCount=2;radio.dabUaComponentCount=2;
 radio.dabActiveServiceId=radio.service[0].ServiceID=radio.dabUaServices[0].sid=0x3201;radio.dabActiveComponentId=radio.dabUaComponents[0]=0x01fe0005;radio.fm=false;
 epg::liveReset();chip.result=si468x::Result::Ok;radio.ServiceStart=radio.signallock=radio.dabActiveServiceValid=radio.dabUaInventoryValid=true;
 radio.dabServiceRequestPending=radio.dabTuneRequestPending=false;radio.dabSwitch.state=dab_switch::State::Ready;
 radio.dabSpiIdentities[0]=dab_spi::Identity::NotSpi;radio.dabSpiIdentities[1]=dab_spi::Identity::Spi;
 radio.dabSpiProbe.spi=dab_probe::Endpoint(0xe23153fe,0x01f2c00c);radio.dabSpiProbe.state=dab_probe::State::Observing;
 radio.clock.valid=true;radio.clock.offsetKnown=true;radio.clock.offsetMinutes=0;radio.clock.encoding=epg::ClockEncoding::UTC;
 radio.clock.calendarEpoch=epg::epoch(2024,11,12,1,1);radio.clock.sampledAtMonotonicMs=testMillis;++radio.clock.generation;
 epg::liveEndpoint(0xe23153fe,0x01f2c00c);completeScan();radio.refreshDabSpiDiscovery();epg::poll();
}
static void result(const char* name,uint32_t baseline,unsigned expected){
 const auto& d=epg_diag::registry();assert(d.totalResets-baseline==expected);
 std::printf("COEX {\"case\":\"%s\",\"pass\":true,\"packetGroups\":%u,\"packetEmpty\":%u,\"packetBlocked\":%u,\"padGroups\":%u,\"padPI\":%u,\"padSI\":%u,\"padPNG\":%u,\"cached\":%u,\"resets\":%u,\"expectedResets\":%u,\"resetReasons\":[",name,unsigned(d.paths[0].groups),unsigned(d.paths[0].empty),unsigned(d.paths[0].blocked),unsigned(d.paths[1].groups),unsigned(d.paths[1].pi),unsigned(d.paths[1].si),unsigned(d.paths[1].png),unsigned(epg::runtime()->count()),unsigned(d.totalResets-baseline),expected);
 for(unsigned i=0;i<unsigned(epg_diag::Reset::Count);++i)std::printf("%s%u",i?",":"",unsigned(d.resets[i]));
 std::printf("],\"padRejects\":[");for(unsigned i=0;i<unsigned(epg_diag::Reject::Count);++i)std::printf("%s%u",i?",":"",unsigned(d.paths[1].rejected[i]));std::puts("]}");
}
static void refreshPad(dab_spi::Identity identity){radio.dabSpiIdentities[0]=identity;completeScan();radio.refreshDabSpiDiscovery();}
int main(int argc,char** argv){
 (void)object;
 assert(argc==4);free8Bytes-=1024;heapBytes-=1024;largestBytes-=1024;assert(epg::ensure());const auto allocatedAtStart=allocationAttempt;auto* c=epg::runtime();auto data=readGroups(argv[1]);
 // Intentionally collide PAD directory TID with captured packet directory TID.
 data[0][5]=0x8c;data[0][6]=0xc2;const auto crc=epg::crc16(data[0].data(),data[0].size()-2);data[0][data[0].size()-2]=uint8_t(crc>>8);data[0].back()=uint8_t(crc);
 std::ifstream file(argv[2],std::ios::binary);Data empty((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());assert(empty.size()==27);
 // A: packet before PAD, interleaved through PI assembly, then after UI/menu return.
 configure();uint32_t before=epg_diag::registry().totalResets;inject(empty,0);assert(c->availability(true)==epg::Availability::DirectoryEmpty);
 radio.dabSpiIdentities[0]=dab_spi::Identity::Spi;completeScan();
 for(size_t i=0;i<data.size();++i){inject(data[i],1,7,0,i&1?0x01fe0005:5);inject(empty,0);}
 assert(c->count()==3 && c->stats.pi==1 && c->stats.si==1 && c->current()==1 && c->next()==2);
 auto frames=c->stats.frames;const std::string title=c->title(1);const auto state=epg::indicatorState();
 EpgOpen();EpgSuspend();for(unsigned i=0;i<10;++i)inject(empty,0);EpgResume();
 assert(visible && c->count()==3 && c->stats.frames==frames && title==c->title(1) && epg::indicatorState()==state);
 auto& diagnostic=epg_diag::registry();assert(diagnostic.paths[0].empty==1+data.size()+10 && diagnostic.paths[0].blocked==data.size()+10);
 inject(data[0]);assert(c->count()==3 && diagnostic.paths[1].repeats>=1);
 const auto selectedBearer=c->selected();
 for(unsigned cycle=0;cycle<3;++cycle){
  auto& probe=radio.dabSpiProbe;const auto packet=probe.spi;
  probe.stop(testMillis);assert(probe.state==dab_probe::State::StopRequested);probe.stopped(testMillis,true);
  assert(probe.request(dab_probe::Endpoint(radio.dabActiveServiceId,radio.dabActiveComponentId),dab_probe::Endpoint{},packet,1,testMillis,500));
  probe.advance(testMillis+499,true);assert(probe.state==dab_probe::State::Baseline);
  testMillis+=500;probe.advance(testMillis,true);assert(probe.state==dab_probe::State::StartRequested);
  probe.started(testMillis,true,false);epg::liveEndpoint(packet.sid,packet.cid);inject(empty,0);
  assert(c->count()==3 && c->selected()==selectedBearer && c->current()==1 && c->next()==2 && epg::indicatorState()==state);
 }
 result("A-empty-packet-full-PAD-same-TID-menu",before,1);
 cmd("EPG TEST TRANSPORT");for(unsigned i=0;i<20;++i){testMillis+=100;epg::poll();}
 assert(diagSerial.output.find("EPG TRANSPORT END")!=std::string::npos);
 std::puts("SNAPSHOT A BEGIN");std::printf("%s",diagSerial.output.c_str());std::puts("SNAPSHOT A END");
 // B: absent/unknown PAD UA never makes packet empty a global NO_SPI claim.
 configure();before=diagnostic.totalResets;for(unsigned i=0;i<4;++i)inject(empty,0);
 inject(data[0]);assert(c->availability(true)==epg::Availability::DirectoryEmpty && epg::indicatorState()==epg::IndicatorState::Unavailable);
 radio.dabSpiIdentities[0]=dab_spi::Identity::Unqueried;radio.dabUaScan.begin(2,true);radio.dabSpiDiscoveryDirty=true;radio.refreshDabSpiDiscovery();inject(data[0]);
 assert(c->entryCount()==0 && diagnostic.paths[1].rejected[unsigned(epg_diag::Reject::Generation)]==1);
 result("B-empty-packet-PAD-not-confirmed",before,0);
 // B2/E: full packet, SLS/DLS, and failed/corrupt first PAD group do not evict it.
 configure();before=diagnostic.totalResets;for(const auto& item:data)inject(item,0);assert(c->count()==3);
 inject(empty,0,7,0,0,4);assert(c->count()==3 && "Faulted packet DSRV header must not publish empty Directory over packet PI");
 const auto legacy=radio.legacy;inject(data[0],1,2);inject(data[0],2,7);assert(radio.legacy==legacy+2 && c->count()==3);
 radio.dabSpiIdentities[0]=dab_spi::Identity::Spi;chip.result=si468x::Result::Timeout;inject(data[0]);chip.result=si468x::Result::Ok;
 Data malformed=data[0];malformed[8]^=1;const auto fixedCrc=epg::crc16(malformed.data(),malformed.size()-2);malformed[malformed.size()-2]=uint8_t(fixedCrc>>8);malformed.back()=uint8_t(fixedCrc);
 inject(malformed);assert(c->count()==3 && "Malformed MOT envelope with valid CRC must not evict packet cache");
 Data bad=data[0];bad.back()^=1;inject(bad);assert(c->count()==3 && c->stats.pi==1 && c->current()==1);
 result("B2-E-packet-SLS-DLS-failed-first-PAD",before,0);
 // PAD-only can acquire EPG with no packet endpoint at all.
 configure();radio.dabUaServiceCount=1;radio.dabUaComponentCount=1;radio.dabSpiIdentities[1]=dab_spi::Identity::NotSpi;radio.dabSpiProbe.spi=dab_probe::Endpoint{};
 epg::liveEndpoint(0,0);before=diagnostic.totalResets;refreshPad(dab_spi::Identity::Spi);
 assert(radio.dabSpiDiscovery.status==dab_spi::Status::PadOnly && !radio.dabSpiDiscovery.available());
 for(const auto& item:data)inject(item);
 assert(c->count()==3 && epg::indicatorState()==epg::IndicatorState::Ready);
 result("B3-full-PAD-without-packet-endpoint",before,1);
 // C: generate SI + own Adam7 PNG without any PI declaration.
 configure();refreshPad(dab_spi::Identity::Spi);before=diagnostic.totalResets;
 const auto logo=readGroups(argv[3]);Data dir(logo[0].begin()+9,logo[0].end()-2);dir.erase(dir.begin()+22,dir.begin()+39);dir[5]=2;
 const auto len=number(dir.size(),4);std::copy(len.begin(),len.end(),dir.begin());inject(group(0x8cc2,6,0,true,dir));
 for(size_t i=1;i<logo.size();++i)if(!(logo[i][5]==1 && logo[i][6]==0)){inject(logo[i]);inject(empty,0);}
 assert(c->availability(true)==epg::Availability::SiOnly && c->station() && c->logoSize()==551 && !c->hasPI() && c->count()==0);
 assert(epg::indicatorState()==epg::IndicatorState::Ready && c->current()==-1 && c->next()==-1);
 result("C-PAD-SI-own-PNG-no-PI",before,0);
 // D: partial packet -> PAD, interrupted UA discovery and delayed packet ACK.
 configure();before=diagnostic.totalResets;inject(data[0],0);inject(data[1],0);inject(data[2],0);assert(c->count()==0);
 refreshPad(dab_spi::Identity::Spi);inject(data.back());assert(c->count()==0); // Old packet partial cannot complete this.
 inject(data[0]);inject(data[1]);for(size_t i=data.size();i>2;--i)inject(data[i-1]);assert(c->count()==3);
 const auto steady=diagnostic.totalResets;
 radio.ServiceStart=false;completeScan();radio.refreshDabSpiDiscovery();radio.ServiceStart=true;assert(c->count()==3 && diagnostic.totalResets==steady);
 radio.dabSpiIdentities[0]=dab_spi::Identity::Unqueried;radio.dabUaScan.begin(2,true);radio.dabSpiDiscoveryDirty=true;radio.refreshDabSpiDiscovery();inject(data[0]);inject(empty,0);
 assert(c->count()==3 && diagnostic.totalResets==steady);
 radio.dabSpiIdentities[0]=dab_spi::Identity::Failed;completeScan();radio.dabUaScan.processed=1;radio.dabUaScan.failed=1;radio.refreshDabSpiDiscovery();assert(c->count()==3);
 for(unsigned i=0;i<3;++i)refreshPad(dab_spi::Identity::Spi);
 assert(radio.dabSpiDiscovery.serviceId==0xe23153fe && radio.dabSpiDiscovery.componentId==0x01f2c00c);
 epg::liveEndpoint(0xe23153ff,0x01f2c00d);epg::liveEndpoint(0xe23153fe,0x01f2c00c);assert(c->count()==3 && diagnostic.totalResets==steady);
 refreshPad(dab_spi::Identity::NotSpi);assert(c->count()==0 && diagnostic.resets[unsigned(epg_diag::Reset::UaRevoked)]>0);
 inject(data[0]);assert(c->count()==0);for(const auto& item:data)inject(item,0);assert(c->count()==3);
 result("D-owner-handoff-UA-unknown-failed-duplicate-revoked",before,2);
 configure();refreshPad(dab_spi::Identity::Spi);for(const auto& item:data)inject(item);before=diagnostic.totalResets;
 epg::liveServiceChanged();epg::liveServiceChanged();assert(c->count()==0 && diagnostic.totalResets==before+1);
 radio.dabActiveServiceId=radio.service[0].ServiceID=radio.dabUaServices[0].sid=0x3202;radio.dabActiveComponentId=radio.dabUaComponents[0]=0x01fe0006;
 refreshPad(dab_spi::Identity::Spi);inject(data[0]);assert(c->count()==0);
 auto newService=data;const auto oldBearer=number(base+1,6);const auto newBearer=number(base+2,6);
 for(auto& frame:newService){for(size_t i=9;i+6<=frame.size()-2;++i)if(std::equal(oldBearer.begin(),oldBearer.end(),frame.begin()+i))std::copy(newBearer.begin(),newBearer.end(),frame.begin()+i);
  const auto crc=epg::crc16(frame.data(),frame.size()-2);frame[frame.size()-2]=uint8_t(crc>>8);frame.back()=uint8_t(crc);inject(frame,1,7,0x3202,6);}
 assert(c->selected()==base+2 && c->count()==3);inject(data[2]);assert(c->selected()==base+2 && c->count()==3);
 ++dabfreq;epg::liveReset();assert(!c->count());radio.fm=true;epg::liveReset();epg::poll();assert(!c->count() && epg::indicatorState()!=epg::IndicatorState::Ready);radio.fm=false;
 result("D-service-frequency-FM-isolation",before,4);
 // E: actual header admission gates, read faults and CRC; no legacy fallthrough.
 configure();refreshPad(dab_spi::Identity::Spi);for(const auto& item:data)inject(item);before=diagnostic.totalResets;
 const auto oldLegacy=radio.legacy;
 inject(data[0],1,7,0x3202,5);inject(data[0],1,7,0,6);inject(data[0],1,7,0,0x01fd0005);
 inject(data[0],1,7,0,0,0,0,4);inject(data[0],1,7,0,0,4);inject(data[0],1,7,0,0,0,4);
 inject(Data(4072));radio.dabTuneRequestPending=true;inject(data[0]);radio.dabTuneRequestPending=false;
 radio.dabUaInventoryValid=false;inject(data[0]);radio.dabUaInventoryValid=true;
 chip.result=si468x::Result::Timeout;inject(data[0]);chip.result=si468x::Result::Ok;bad=data[2];bad.back()^=1;inject(bad);
 inject(data[0],3,7);assert(radio.legacy==oldLegacy && c->count()==3 && c->current()==1);
 for(auto reason:{epg_diag::Reject::Sid,epg_diag::Reject::Cid,epg_diag::Reject::Dscty,epg_diag::Reject::State,epg_diag::Reject::Physical,epg_diag::Reject::Length,epg_diag::Reject::Inactive,epg_diag::Reject::Generation,epg_diag::Reject::Read,epg_diag::Reject::Data})assert(diagnostic.paths[1].rejected[unsigned(reason)]>0);
 testMillis+=300001;epg::poll();assert(c->count()==3 && epg::indicatorState()==epg::IndicatorState::Ready); // Valid SI survives a stale clock, not a transport reset
 radio.clock.sampledAtMonotonicMs=testMillis;++radio.clock.generation;epg::poll();assert(c->current()==1);
 result("E-negative-header-identity-CRC-read-timeout-clock",before,0);
 // F: explicit signal only, waiting for PAD Directory rather than reusing packet empty.
 configure();inject(empty,0);before=diagnostic.totalResets;refreshPad(dab_spi::Identity::Spi);
 assert(epg::availability()==epg::Availability::WaitDirectory && epg::indicatorState()==epg::IndicatorState::Loading && c->stats.directories==0);
 for(unsigned i=0;i<10;++i)inject(empty,0);
 assert(c->stats.directories==0 && diagnostic.paths[1].dirs==0 && diagnostic.paths[0].empty==11);
 testMillis+=300001;epg::poll();cmd("EPG TEST TRANSPORT");for(unsigned i=0;i<20;++i){testMillis+=100;epg::poll();}
 assert(diagSerial.output.find("reason=directory-timeout")!=std::string::npos && c->stats.directories==0);
 result("F-PAD-signalled-no-data-is-WAITING",before,1);
 // Lifecycle and asynchronous diagnostic snapshot use the actual runtime command.
 before=diagnostic.totalResets;cmd("EPG TEST START");inject(data[0]);inject(empty,0);assert(epg::replayActive() && c->stats.frames==0);
 assert(diagnostic.paths[1].rejected[unsigned(epg_diag::Reject::Replay)]>0 && diagnostic.paths[0].rejected[unsigned(epg_diag::Reject::Replay)]>0);
 result("F-replay-quarantine",before,1);
 cmd("EPG TEST STOP");assert(!epg::replayActive() && c->count()==0);
 uartRoom=0;cmd("EPG TEST TRANSPORT");assert(diagSerial.output.empty());uartRoom=4096;
 cmd("EPG TEST TRANSPORT");assert(diagSerial.output.find("queued")!=std::string::npos);
 uartRoom=511;for(unsigned i=0;i<20;++i){testMillis+=100;epg::poll();}assert(diagSerial.output.find("EPG AUDIO sid=")==std::string::npos);uartRoom=512;
 for(unsigned i=0;i<30;++i){testMillis+=100;epg::poll();}
 assert(diagSerial.output.find("EPG AUDIO sid=")!=std::string::npos && diagSerial.output.find("EPG PACKET selected=")!=std::string::npos && diagSerial.output.find("EPG RADIO mode=")!=std::string::npos);
 assert(diagSerial.output.find("EPG TRANSPORT END")!=std::string::npos && diagSerial.output.find("EPG RESETS total=")!=std::string::npos);
 for(size_t start=0;start<diagSerial.output.size();){auto end=diagSerial.output.find('\n',start);if(end==std::string::npos)break;assert(end-start<512);start=end+1;}
 cmd("EPG TEST TRANSPORT");testMillis+=100;epg::poll();epg::liveReset();testMillis+=100;epg::poll();assert(diagSerial.output.find("CANCELLED context changed")!=std::string::npos);
 auto saved=diagnostic;
 for(unsigned i=0;i<2;++i){auto& path=diagnostic.paths[i];for(auto& header:path.headers)header=UINT32_MAX;for(auto& reject:path.rejected)reject=UINT32_MAX;
 path.candidates=path.admitted=path.blocked=path.legacy=path.groups=path.validGroups=path.crcBad=path.invalid=path.objectGroups=path.dirGroups=path.dirs=path.unique=path.repeats=path.empty=path.si=path.pi=path.png=path.programmes=UINT32_MAX;}
 cmd("EPG TEST TRANSPORT");for(unsigned i=0;i<20;++i){testMillis+=100;epg::poll();}
 for(size_t start=0;start<diagSerial.output.size();){auto end=diagSerial.output.find('\n',start);if(end==std::string::npos)break;assert(end-start<512);start=end+1;}
 diagnostic=saved;uint32_t maximum=UINT32_MAX;epg_diag::increment(maximum);assert(maximum==UINT32_MAX);
 // New DSRV status during the actual full-reply payload read survives dispatch.
 configure();dabDsrvEvents.reset();dabDsrvEvents.status(true);dabDsrvEvents.start(testMillis);chip.raiseDsrv=true;
 inject(empty,0);assert(radio.dabDsrvPending && dabDsrvEvents.newer);
 dabDsrvEvents.start(testMillis);inject(Data{},0);assert(!radio.dabDsrvPending);
 std::puts("Actual DSRV full-reply race -> pending rearm -> empty drain terminates: PASS");
 // DEBUG timeline output respects headroom; unknown milestones remain explicit.
 diagnosticDebug=true;auto& timing=epg_acq::trace();timing.enable(true);
 timing.audio(0x3201,0x01fe0005,testMillis);timing.endpoint(0,0xe23153fe,0x01f2c00c,testMillis+10);timing.mark(0,2,testMillis+20);
 diagSerial.output.clear();testMillis+=250;uartRoom=319;epg::poll();assert(diagSerial.output.find("EPG ACQ")==std::string::npos);
 uartRoom=320;epg::poll();assert(diagSerial.output.find("EPG ACQ path=PACKET")!=std::string::npos && diagSerial.output.find("t0=0 t1=10 t2=20 t3=N/A")!=std::string::npos);
 const auto printed=diagSerial.output;testMillis+=1000;epg::poll();assert(diagSerial.output==printed);
 diagnosticDebug=false;epg::poll();assert(!timing.enabled);
 assert(allocationAttempt==allocatedAtStart && free8Bytes>=12000);
 std::puts("Production DSRV -> actual UA/route/owner -> exact payload -> Core -> cache/UI status: A-F PASS");
}
'''

source=out/'coexistence.cpp';source.write_text(prefix+runtime+'\n'+method('void DAB::logEpgTransportPage(uint8_t page)')+'\n'+method('dab_spi::Identity DAB::xpadSpiIdentity(void) const')+'\n'+method('bool DAB::xpadSpiPresent(void) const')+'\n'+method('void DAB::refreshDabSpiDiscovery(void)')+'\nvoid DAB::dsrv(){switch(DabCommand::DsrvHeader){\n'+case+'default:break;}}\n'+fixture+main,encoding='utf-8')
exe=out/'coexistence.exe'
subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe','-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(source),str(ROOT/'src/epg_core.cpp'),str(ROOT/'src/epg_uart.cpp'),'-o',str(exe)],check=True)
result=subprocess.run([str(exe),str(ROOT/'tests/fixtures/epg/synthetic_programme_groups_v2.bin'),str(ROOT/'tests/fixtures/epg/mux11a_empty_group.bin'),str(ROOT/'tests/fixtures/epg/synthetic_logo_1_groups_v2.bin')],capture_output=True,text=True)
print(result.stdout+result.stderr,end='');rows=[json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith('COEX ')]
diagnostic=(ROOT/'src/epg_transport_diag.h').read_text(encoding='utf-8')
def names(function):
 body=re.search(function+r'.*?names\[\]=\{([^}]+)',diagnostic,re.S)[1]
 return re.findall(r'"([^"]+)"',body)
snapshot=re.search(r'SNAPSHOT A BEGIN\n(.*?)SNAPSHOT A END',result.stdout,re.S)
(out/'results.json').write_text(json.dumps({'kind':'host full production DSRV/runtime/Core, not RF','pass':result.returncode==0,'unexpectedResets':0 if result.returncode==0 else None,'mockExtraDramPressure':1024,'physical_hardware_tested':False,'resetReasonNames':names('resetName'),'padRejectNames':names('rejectName'),'cases':rows,'synthetic_snapshot':snapshot[1] if snapshot else '', 'stderr':result.stderr},indent=2)+'\n',encoding='utf-8')
result.check_returncode()
