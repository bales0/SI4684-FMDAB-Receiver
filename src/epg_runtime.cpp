#include "epg_runtime.h"
#include "epg_uart.h"
#include "epg_ui.h"
#include "epg_demo.h"
#include "epg_ingress.h"
#include "vendor/si468x/Si468x.h"
#include "gui.h"
#include "serial_monitor.h"
#include <esp_heap_caps.h>
#include <new>
#include <stdlib.h>
#include <string.h>
extern bool diagnosticDebug;
namespace epg {
static Core* core=nullptr;
static UartReceiver receiver;
static bool replay=false, demo=false;
static ResolvedClock liveClock;
static bool clockLost=false;
static uint32_t lostClockGeneration=0;
static uint32_t activity=0;
static unsigned demoIndex=0;
static bool snapshotFm=false;
static uint8_t snapshotFreq=0;
static uint32_t snapshotSid=0;
static bool allocationReported=false;
static uint8_t transportPage=255;
static uint32_t transportPrintedAt=0,transportResetSequence=0,transportEpoch=0;
static uint32_t endpointSid=0, endpointCid=0, pendingSid=0;
static uint32_t padSid=0, padCid=0;
static bool injectDsrv=false, injectPad=false, injectUa=false;
static uint32_t injectAccepted=0, injectLegacy=0, injectRejected=0, injectBlocked=0;
static void clearInjection() {
  injectDsrv=injectPad=injectUa=false;
  injectAccepted=injectLegacy=injectRejected=injectBlocked=0;
}
// Isolated test boundary: emulate a DSRV reply, never read the real chip reply
// or feed simulated bytes into the live SLS/DLS collector.
static Feed injectedDsrv(const uint8_t* data,size_t n,uint32_t now) {
  si468x::DsrvHeader h;
  if(!replay || !injectDsrv || !core ||
     si468x::Si468x::parseDsrvHeader(data,n,h)!=si468x::Result::Ok ||
     n!=24U+h.byteCount || !h.byteCount ||
     (h.serviceState!=0U && h.serviceState!=3U) || h.physicalError()) {
    ++injectRejected; return Feed::Rejected;
  }
  const dab_probe::Endpoint audio(0x3201U,0x01FE0005U), packet(0xE23153FEU,0x01F2C00CU), sls;
  const auto target=dab_probe::route(h.serviceId,h.componentId,h.dataSource,audio,sls,packet);
  const auto pad=epg_ingress::pad(h.dataSource,h.dscType,h.rawUatypeOrRfu,
      h.serviceId,h.componentId,audio,injectUa,true,true);
  bool deliver=injectPad && pad==epg_ingress::PadRoute::Epg;
  if(pad==epg_ingress::PadRoute::Discard) { ++injectRejected; return Feed::Rejected; }
  if(h.dataSource==0U && target==dab_probe::Route::Spi) {
    if(injectPad) { ++injectBlocked; return Feed::Accepted; }
    deliver=true;
  }
  if(deliver) {
    const Feed result=core->feed(data+24,h.byteCount,now);
    if(result==Feed::Accepted) ++injectAccepted; else ++injectRejected;
    return result;
  }
  if((h.dataSource==1U || h.dataSource==2U) && target==dab_probe::Route::Audio) {
    ++injectLegacy; return Feed::Accepted; // Hand-off decision only, no SLS mutation.
  }
  ++injectRejected; return Feed::Rejected;
}
static void objectReady(void*,uint16_t tid,uint8_t type,Bytes data,size_t n) {
  if(!replay && type==6)epg_acq::trace().mark(padSid?1:0,5,millis());
  if(!replay) epg_diag::registry().object(padSid?epg_diag::Path::Pad:epg_diag::Path::Packet,tid,type,data,n,core->stats.programmes);
  if(diagSerial.availableForWrite()<192) return;
  const char* kind=type==6?"DIR":n && data[0]==2?"PI":n && data[0]==3?"SI":"PNG";
  diagSerial.printf("EPG MOT tid=%04X kind=%s bytes=%u programmes=%u cached=%u\n",tid,kind,unsigned(n),core->stats.programmes,unsigned(core->count()));
}
Core* runtime() { return core; }
bool replayActive() { return replay; }
// UART replay has its own discovery state; never alter the live radio probe.
bool indicatorPresent() { return replay ? core && core->stats.directories!=0 : radio.spiPresent() || padSid!=0; }
Availability availability() {return core?core->availability(indicatorPresent()):(indicatorPresent()?Availability::WaitDirectory:Availability::NoSpi);}
ResolvedClock clockStatus() {return liveClock;}
IndicatorState indicatorState() {
  if(core && availability()==Availability::ReadyLive) return IndicatorState::Ready;
  return indicatorPresent()?IndicatorState::Present:IndicatorState::Absent;
}
bool liveEnabled() { return core && !replay; }
static const unsigned byteCaps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
// EXEC selects the classic ESP32 IRAM heap, avoiding consumption of DRAM.
// The memory contains data only; every access goes through Bytes/Buffer.
static const unsigned wordCaps=MALLOC_CAP_INTERNAL|MALLOC_CAP_32BIT|MALLOC_CAP_EXEC;
static void* allocateWordBlock(size_t bytes) { return heap_caps_malloc(bytes,wordCaps); }
bool ensure(bool reportFailure) {
  if(core) return true;
  const size_t free8=heap_caps_get_free_size(byteCaps), largest8=heap_caps_get_largest_free_block(byteCaps);
  const size_t free32=heap_caps_get_free_size(wordCaps), largest32=heap_caps_get_largest_free_block(wordCaps);
  // Reserves apply to each actual capability pool, never aggregate INTERNAL.
  if(largest8>=sizeof(Core) && free8>=sizeof(Core)+12000U &&
     largest32>=kObjectBytes && free32>=Core::bufferBytes()+4096U) {
    void* storage=heap_caps_malloc(sizeof(Core),byteCaps);
    if(storage) {
      Core* candidate=new(storage) Core(allocateWordBlock,heap_caps_free,true);
      if(candidate->ready()) core=candidate;
      else { candidate->~Core(); heap_caps_free(storage); }
    }
  }
  if(!core && (reportFailure || !allocationReported)) {
    allocationReported=true;
    diagSerial.printf("EPG ERROR MEMORY need8=%u free8=%u largest8=%u need32=%u free32=%u largest32=%u\n",unsigned(sizeof(Core)),unsigned(free8),unsigned(largest8),unsigned(Core::bufferBytes()),unsigned(free32),unsigned(largest32));
  }
  if(core) core->observer.object=objectReady;
  return core!=nullptr;
}
static void resetLiveCore(epg_diag::Reset reason) {
  if(core) { epg_diag::registry().reset(reason);core->reset(); core->setLive(true); core->setClockTrusted(false); core->select(0,0,0); EpgInvalidate(); }
}
void liveReset() {
  epg_acq::trace().reset();
  clearInjection();clockLost=false;liveClock=ResolvedClock{};
  if(replay) { replay=demo=false; pendingSid=0; receiver.reset(); EpgClose(false); }
  endpointSid=endpointCid=padSid=padCid=0;
  epg_diag::registry().context();
  resetLiveCore(epg_diag::Reset::Tune);
}
void liveContext(uint32_t sid,uint16_t eid,uint8_t ecc,uint32_t utc) {
  if(!core || replay) return;
  bool conflict=false;
  // SCIdS is not separately exposed; reject multiple matching bearers.
  const uint64_t match=core->findStation(sid,eid,ecc,-1,conflict);
  core->select(match,utc,liveClock.offsetKnown?liveClock.offset:0); core->identityConflict=conflict;

}
void liveEndpoint(uint32_t sid,uint32_t cid) {
  if(replay) return;
  if(endpointSid!=sid || endpointCid!=cid) {
    endpointSid=sid; endpointCid=cid;
    epg_diag::registry().endpoint(epg_diag::Path::Packet,sid,cid,millis());
    if(!padSid) resetLiveCore(epg_diag::Reset::PacketEndpoint);
  }
}
static void setPadEndpoint(uint32_t sid,uint32_t cid,epg_diag::Reset reason) {
  if(replay || (padSid==sid && padCid==cid)) return;
  padSid=sid; padCid=cid;epg_acq::trace().owner=sid?1:0;
  if(sid)epg_diag::registry().endpoint(epg_diag::Path::Pad,sid,cid,millis());
  resetLiveCore(reason);
}
void livePadEndpoint(uint32_t sid,uint32_t cid) {
  setPadEndpoint(sid,cid,!sid?epg_diag::Reset::PadToPacket:padSid?epg_diag::Reset::PadEndpoint:endpointSid?epg_diag::Reset::PacketToPad:epg_diag::Reset::PadAcquire);
}
void livePadRevoked(){setPadEndpoint(0,0,epg_diag::Reset::UaRevoked);}
void liveServiceChanged(){epg_acq::trace().reset();setPadEndpoint(0,0,epg_diag::Reset::Service);}
static void liveGroup(epg_diag::Path path,const uint8_t* data,size_t n,bool owns) {
  auto& diagnostic=epg_diag::registry();auto& counters=diagnostic.at(path);
  diagnostic.group(path,data,n,owns,millis());
  if(replay){diagnostic.reject(path,epg_diag::Reject::Replay);return;}
  if(!owns){epg_diag::increment(counters.blocked);diagnostic.reject(path,epg_diag::Reject::Owner);return;}
  if(!core){diagnostic.reject(path,epg_diag::Reject::NoEndpoint);return;}
  const auto result=core->feed(data,n,millis());
  if(result==Feed::Accepted)epg_diag::increment(counters.admitted);
  else diagnostic.reject(path,result==Feed::Busy?epg_diag::Reject::Busy:epg_diag::Reject::Data);
}
void livePayload(const uint8_t* data,size_t n){liveGroup(epg_diag::Path::Packet,data,n,!padSid);}
void livePadPayload(const uint8_t* data,size_t n){liveGroup(epg_diag::Path::Pad,data,n,padSid!=0);}
void livePadData(uint32_t sid,uint32_t cid,const uint8_t* data,size_t n) {
  if(replay){epg_diag::registry().reject(epg_diag::Path::Pad,epg_diag::Reject::Replay);return;}
  // Do not discard packet cache on a failed read or corrupt first PAD group.
  // A completed UA publication may independently choose the signalled PAD owner.
  GroupEnvelope envelope;
  if((padSid!=sid || padCid!=cid) && (inspectGroup(data,n,envelope)!=GroupCheck::Valid || (envelope.segment&32767)>=32)) {
    epg_diag::registry().group(epg_diag::Path::Pad,data,n,false,millis());
    epg_diag::registry().reject(epg_diag::Path::Pad,epg_diag::Reject::Data);return;
  }
  livePadEndpoint(sid,cid);livePadPayload(data,n);
}
void stopReplay() {
  clearInjection();
  replay=demo=false; pendingSid=0; receiver.reset(); liveClock=ResolvedClock{};
  endpointSid=endpointCid=padSid=padCid=0;
  epg_diag::registry().context();
  if(core) { epg_diag::registry().reset(epg_diag::Reset::ReplayStop);core->reset(); core->setLive(true); core->setClockTrusted(false); core->select(0,0,0); }
  EpgClose();
}
static uint32_t radioSid() { return radio.ServiceStart && radio.ServiceIndex<radio.numberofservices ? radio.service[radio.ServiceIndex].ServiceID:0; }
static void startReplay() {
  clearInjection();
  endpointSid=endpointCid=padSid=padCid=0;
  epg_diag::registry().reset(epg_diag::Reset::ReplayStart);
  core->reset(); core->setLive(false); core->setClockTrusted(true); replay=true; demo=false; activity=millis(); receiver.reset();
  snapshotFm=radio.isFm(); snapshotFreq=dabfreq; snapshotSid=radioSid();
  core->select(0x40e231533233ULL,epoch(2024,11,12,3,30),60);
}
static void transportOutput() {
  if(transportPage==255 || diagSerial.availableForWrite()<512 || uint32_t(millis()-transportPrintedAt)<100U)return;
  transportPrintedAt=millis();auto& d=epg_diag::registry();
  if(transportResetSequence!=d.totalResets || transportEpoch!=d.epoch){diagSerial.println("EPG TRANSPORT CANCELLED context changed");transportPage=255;return;}
  const unsigned page=transportPage++;
  if(page<3){radio.logEpgTransportPage(uint8_t(page));return;}
  if(page==3){diagSerial.printf("EPG OWNER mode=%s owner=%s channelIndex=%u epoch=%u resets=%u last=%s stateChanges=%u state=%s headerErrors=%u\n",replay?"TEST":"LIVE",replay?"TEST":padSid?"PAD":endpointSid?"PACKET":"NONE",unsigned(dabfreq),unsigned(d.epoch),unsigned(d.totalResets),epg_diag::resetName(d.lastReset),unsigned(d.stateChanges),availabilityName(availability()),unsigned(d.headerErrors));return;}
  if(page>=4 && page<10){
    const unsigned index=(page-4)/3,part=(page-4)%3;const auto& c=d.paths[index];const char* name=index?"PAD":"PACKET";
    if(part==0)diagSerial.printf("EPG TRANSPORT path=%s endpoint=%08X/%08X headers=%u/%u/%u/%u candidate=%u admitted=%u blocked=%u legacy=%u last=%08X/%08X src=%u ua=%04X dscty=%u state=%u bytes=%u gen=%u waitMs=%u idleMs=%u reason=%s\n",name,unsigned(c.sid),unsigned(c.cid),unsigned(c.headers[0]),unsigned(c.headers[1]),unsigned(c.headers[2]),unsigned(c.headers[3]),unsigned(c.candidates),unsigned(c.admitted),unsigned(c.blocked),unsigned(c.legacy),unsigned(c.lastSid),unsigned(c.lastCid),c.source,c.ua,c.dscty,c.state,c.bytes,unsigned(c.generation),c.sid && !c.dirs?unsigned(millis()-c.endpointMs):0,c.validGroups?unsigned(millis()-c.lastGroupMs):0,!c.sid?"not-owned":c.dirs?(c.objects?"directory-known":"empty-directory"):uint32_t(millis()-c.endpointMs)>=300000U?"directory-timeout":c.validGroups?"incomplete-directory":"waiting-data");
    else if(part==1)diagSerial.printf("EPG MOTPATH path=%s groups=%u valid=%u crcBad=%u invalid=%u type4=%u type6=%u dirs=%u unique=%u repeats=%u empty=%u tid=%04X dirBytes=%u objects=%u SI=%u PI=%u PNG=%u programmes=%u historyLimited=%u\n",name,unsigned(c.groups),unsigned(c.validGroups),unsigned(c.crcBad),unsigned(c.invalid),unsigned(c.objectGroups),unsigned(c.dirGroups),unsigned(c.dirs),unsigned(c.unique),unsigned(c.repeats),unsigned(c.empty),c.tid,unsigned(c.dirBytes),unsigned(c.objects),unsigned(c.si),unsigned(c.pi),unsigned(c.png),unsigned(c.programmes),c.historyLimited);
    else {diagSerial.printf("EPG REJECT path=%s",name);for(unsigned i=0;i<unsigned(epg_diag::Reject::Count);++i)diagSerial.printf(" %s=%u",epg_diag::rejectName(i),unsigned(c.rejected[i]));diagSerial.println("");}
    return;
  }
  if(page==10){diagSerial.printf("EPG RESETS total=%u",unsigned(d.totalResets));for(unsigned i=0;i<unsigned(epg_diag::Reset::Count);++i)diagSerial.printf(" %s=%u",epg_diag::resetName(i),unsigned(d.resets[i]));diagSerial.println("");return;}
  if(page==11){diagSerial.printf("EPG HEALTH free8=%u largest8=%u free32=%u largest32=%u min=%u UARTdrop=%u audio=%u SLS=%u core=%u arena=%u cached=%u limited=%u timeout=%u busy=%u invalid=%u crcBad=%u\n",unsigned(heap_caps_get_free_size(byteCaps)),unsigned(heap_caps_get_largest_free_block(byteCaps)),unsigned(heap_caps_get_free_size(wordCaps)),unsigned(heap_caps_get_largest_free_block(wordCaps)),ESP.getMinFreeHeap(),diagSerial.droppedBytes(),radio.ServiceStart,radio.SlideShowAvailable,core!=nullptr,unsigned(Core::memoryBytes()),core?unsigned(core->count()):0,core?core->limited:0,core?unsigned(core->stats.timeout):0,core?unsigned(core->stats.busy):0,core?unsigned(core->stats.invalid):0,core?unsigned(core->stats.crcBad):0);return;}
  diagSerial.println("EPG TRANSPORT END");transportPage=255;
}
static void acquisitionOutput() {
  auto& trace=epg_acq::trace();
  if(!trace.enabled || diagSerial.availableForWrite()<320 || uint32_t(millis()-trace.printedAt)<250U)return;
  for(unsigned path=0;path<2;++path){auto& p=trace.paths[path];if(!p.dirty || !p.sid)continue;
    char line[320];int used=snprintf(line,sizeof(line),"EPG ACQ path=%s sid=%08X cid=%08X base=first-observed",path?"XPAD":"PACKET",unsigned(p.sid),unsigned(p.cid));
    for(unsigned point=0;point<9;++point) {
      if(p.known&(1U<<point))used+=snprintf(line+used,sizeof(line)-used," t%u=%u",point,unsigned(p.at[point]-p.origin));
      else used+=snprintf(line+used,sizeof(line)-used," t%u=N/A",point);
    }
    diagSerial.println(line);p.dirty=false;trace.printedAt=millis();return;
  }
}
void poll() {
  epg_acq::trace().enable(diagnosticDebug && !replayActive());
  epg_acq::trace().owner=padSid?1:0;
  acquisitionOutput();
  transportOutput();
  auto& diagnostic=epg_diag::registry();const uint8_t state=uint8_t(availability());
  if(diagnostic.lastState!=state){diagnostic.lastState=state;epg_diag::increment(diagnostic.stateChanges);}
  static bool autoAttempted=false;
  if(!core && !autoAttempted && !radio.isFm() && (radio.spiPresent() || padSid) && radio.ServiceStart) { autoAttempted=true; ensure(); }
  if(!core) return;
  const uint32_t nowMs=millis();
  if(replay && (snapshotFm!=radio.isFm() || snapshotFreq!=dabfreq || (snapshotSid && radioSid() && snapshotSid!=radioSid()))) {
    diagSerial.println("EPG INFO replay cancelled by radio service/mux change"); stopReplay();
  }
  if(replay && !demo && uint32_t(nowMs-activity)>30000U) {
    diagSerial.println("EPG INFO replay idle timeout; returning to live"); stopReplay();
  }
  if(demo && demoIndex<sizeof(epgDemoGroups)/sizeof(epgDemoGroups[0]) && !core->busy()) {
    const EpgDemoGroup& g=epgDemoGroups[demoIndex++];
    if((g.data[0]&15)==4) core->focus(uint16_t(g.data[5])<<8|g.data[6]);
    core->feed(g.data,g.size,nowMs);
  }
  core->pump(48); core->expire(nowMs); receiver.expire(nowMs);
  if(replay && pendingSid && core->stats.si) {
    if(!core->selectSid(pendingSid)) diagSerial.println("EPG ERROR STATION unknown or ambiguous after SI");
    pendingSid=0; EpgInvalidate();
  }
  if(!replay && (radio.isFm() || !radio.ServiceStart || !radio.signallock)) {
    clockLost=true;lostClockGeneration=radio.broadcastClock().generation;
    liveClock=ResolvedClock{};core->setClockTrusted(false);
  }
  if(!replay && !radio.isFm() && radio.ServiceStart) {
    char* end=nullptr; const unsigned eid=strtoul(radio.EID,&end,16);
    const auto& clock=radio.broadcastClock();
    if(!radio.signallock) {clockLost=true;lostClockGeneration=clock.generation;}
    if(clockLost && clock.generation!=lostClockGeneration) clockLost=false;
    liveClock=resolveTime(clock,nowMs,radio.signallock && !clockLost);
    core->setLive(true);core->setClockTrusted(liveClock.utcValid && liveClock.offsetKnown);
    liveContext(radioSid(),uint16_t(eid),uint8_t(radio.ensembleEcc),liveClock.utcValid?liveClock.utc:0);
    if(!core->busy() && core->stats.pi && core->count() && core->hasPI() && !core->ambiguous())epg_acq::trace().mark(padSid?1:0,6,nowMs);
    if(core->hasSI() && core->logoSize())epg_acq::trace().mark(padSid?1:0,8,nowMs);
  }
}
static bool hexValue(const char* text,uint64_t& value) {
  if(!*text || strlen(text)>16) return false;
  value=0;
  for(;*text;++text) { unsigned d=0; if(*text>='0' && *text<='9') d=*text-'0'; else if(*text>='a' && *text<='f') d=*text-'a'+10; else if(*text>='A' && *text<='F') d=*text-'A'+10; else return false; value=(value<<4)|d; }
  return true;
}
static void status() {
  const Stats& s=core->stats;
  diagSerial.printf("EPG OK STATUS mode=%s frames=%u crcBad=%u invalid=%u dup=%u conflict=%u incomplete=%u busy=%u\n",replay?"TEST":"LIVE",s.frames,s.crcBad,s.invalid,s.duplicate,s.conflict,s.incomplete,s.busy);
  diagSerial.printf("EPG DATA dirs=%u objects=%u PI=%u SI=%u PNG=%u programmes=%u cached=%u limited=%u ambiguous=%u now=%d next=%d\n",s.directories,s.objects,s.pi,s.si,s.png,s.programmes,unsigned(core->count()),core->limited,core->ambiguous(),core->current(),core->next());
  diagSerial.printf("EPG RX unmapped=%u oversize=%u timeout=%u fresh=%u continuityJumps=%u unsupported=%u\n",s.unmapped,s.oversize,s.timeout,core->fresh(),s.continuityJumps,s.unsupported);
  diagSerial.printf("EPG RAM arena=%u free=%u min=%u largest=%u audio=%u SLS=%u UARTdrop=%u rxCRC=%u rxOrder=%u rxIdentity=%u\n",unsigned(Core::memoryBytes()),ESP.getFreeHeap(),ESP.getMinFreeHeap(),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),radio.ServiceStart,radio.SlideShowAvailable,diagSerial.droppedBytes(),receiver.crcErrors,receiver.orderErrors,receiver.identityErrors);
  diagSerial.printf("EPG LOGO bytes=%u interlace=%u\n",unsigned(core->logoSize()),core->logoSize()>28?unsigned(core->logo()[28]):0U);
  diagSerial.printf("EPG POOLS free8=%u largest8=%u free32=%u largest32=%u\n",unsigned(heap_caps_get_free_size(byteCaps)),unsigned(heap_caps_get_largest_free_block(byteCaps)),unsigned(heap_caps_get_free_size(wordCaps)),unsigned(heap_caps_get_largest_free_block(wordCaps)));
  uint32_t first=0,last=0;core->scheduleRange(first,last);
  diagSerial.printf("EPG STATE state=%s transportFresh=%u scheduleDateValid=%u serviceHasPI=%u serviceHasSI=%u clockValid=%u sourceStart=%u sourceEnd=%u archive=%u\n",availabilityName(availability()),core->transportFresh(),core->scheduleDateValid(),core->hasPI(),core->hasSI(),core->clockTrusted(),unsigned(first==UINT32_MAX?0:first),unsigned(last),core->archivePreview());
  const auto& sample=radio.broadcastClock();
  diagSerial.printf("EPG CLOCK encoding=%u valid=%u offsetKnown=%u offset=%d utc=%u local=%u generation=%u age=%u\n",unsigned(sample.encoding),liveClock.utcValid,liveClock.offsetKnown,int(liveClock.offset),unsigned(liveClock.utc),unsigned(liveClock.local),unsigned(sample.generation),unsigned(millis()-sample.sampledAtMonotonicMs));
  if(injectDsrv) diagSerial.printf("EPG INJECT path=%s ua=%u accepted=%u legacy=%u rejected=%u blocked=%u\n",injectPad?"PAD":"MSC",injectUa,unsigned(injectAccepted),unsigned(injectLegacy),unsigned(injectRejected),unsigned(injectBlocked));
}
bool command(char* input) {
  if(!strcasecmp(input,"EPG TEST TRANSPORT")) {
    if(diagSerial.availableForWrite()<64)return true;
    if(transportPage!=255){diagSerial.println("EPG OK TRANSPORT pending");return true;}
    transportPage=0;transportResetSequence=epg_diag::registry().totalResets;transportEpoch=epg_diag::registry().epoch;transportPrintedAt=millis();activity=millis();diagSerial.println("EPG OK TRANSPORT queued");return true;
  }
  if(!strncasecmp(input,"EPG ARCHIVE ",12)) {
    if(!core || replay) {diagSerial.println("EPG ERROR ARCHIVE requires LIVE data");return true;}
    uint32_t date=0;
    if(strcasecmp(input+12,"OFF")) {
      char value[32];uint32_t utc=0;int16_t off=0;
      if(strlen(input+12)!=10) {diagSerial.println("EPG ERROR ARCHIVE use YYYY-MM-DD or OFF");return true;}
      snprintf(value,sizeof(value),"%sT00:00:00Z",input+12);
      if(!parseIso(value,utc,off)) {diagSerial.println("EPG ERROR ARCHIVE date");return true;}
      date=utc;
    }
    core->archivePreview(date);EpgOpen();diagSerial.println(date?"EPG OK ARCHIVE preview; original dates; NOW disabled":"EPG OK ARCHIVE OFF");return true;
  }
  if(!strncmp(input,"E1 ",3) || !strncmp(input,"E2 ",3)) {
    if(!replay || demo || !core) { diagSerial.println("EPG ERROR START required"); return true; }
    if(diagSerial.availableForWrite()<160) return true; // sender retries; no silent commit
    activity=millis();
    const auto result=receiver.receive(input,*core,activity,injectedDsrv);
    if(result==UartReceiver::Result::Ack) diagSerial.printf("EPG ACK %04X %08X\n",receiver.session(),unsigned(receiver.sequence()));
    else {
      const char* reasons[]={"ACK","BUSY","CRC","FORMAT","ORDER","IDENTITY","DATA"};
      diagSerial.printf("EPG NACK %s\n",reasons[unsigned(result)]);
    }
    return true;
  }
  // EPG command spelling is intentionally case-insensitive, framed CRC input is not modified.
  if(strncasecmp(input,"EPG TEST ",9)) return false;
  if(diagSerial.availableForWrite()<1024) return true;
  char* arg=input+9; activity=millis();
  if(!strcasecmp(arg,"HELP")) {
    diagSerial.println("EPG OK HELP: ARCHIVE via EPG ARCHIVE YYYY-MM-DD/OFF | DEMO | START [SID FULLCID] | INJECT MSC/PAD [NOSPI] | TIME ISO8601 | STATION HEX | STATUS | TRANSPORT | STOP | CLEAR");
    return true;
  }
  if(!strcasecmp(arg,"STOP") || !strcasecmp(arg,"CLEAR")) { const bool clear=!strcasecmp(arg,"CLEAR"); stopReplay(); diagSerial.printf("EPG OK %s\n",clear?"CLEAR":"STOP"); return true; }
  if(!ensure(true)) return true;
  if(!strcasecmp(arg,"INJECT MSC") || !strcasecmp(arg,"INJECT PAD") || !strcasecmp(arg,"INJECT PAD NOSPI")) {
    const bool pad=strncasecmp(arg+7,"PAD",3)==0;
    const bool ua=pad && strcasecmp(arg,"INJECT PAD NOSPI")!=0;
    startReplay(); injectDsrv=true; injectPad=pad; injectUa=ua;
    const unsigned session=(esp_random()&0xffffU)|1U;
    receiver.resetDsrv(pad?0x3201U:0xE23153FEU,pad?0x01FE0005U:0x01F2C00CU,session,pad?1:0);
    EpgOpen(); diagSerial.printf("EPG OK INJECT V2 session=%04X path=%s fragment=32 group=4071\n",session,pad?"PAD":"MSC"); return true;
  }
  if(!strcasecmp(arg,"DEMO")) {
    startReplay(); demo=true; demoIndex=0;
    core->select(0x40e231533211ULL,epoch(2024,11,12,3,30),60);
    EpgOpen(); diagSerial.println("EPG OK DEMO source=actual-MUX11B MSC directory+F64C+F629; TEST historical data"); return true;
  }
  if(!strncasecmp(arg,"START",5) && (!arg[5] || arg[5]==' ')) {
    unsigned sid=0xE23153FEU,cid=0x01F2C00CU; int used=0;
    if(arg[5] && (sscanf(arg+5," %8x %8x%n",&sid,&cid,&used)!=2 || arg[5+used] || (cid&0xc000)!=0xc000)) { diagSerial.println("EPG ERROR START identity"); return true; }
    startReplay(); const unsigned session=(esp_random()&0xffffU)|1U;
    receiver.reset(sid,cid,session); EpgOpen(); diagSerial.printf("EPG OK START V1 session=%04X fragment=32 group=4071 timeout=5000 idle=30000\n",session); return true;
  }
  if(!strcasecmp(arg,"STATUS")) { status(); return true; }
  if(!replay) { diagSerial.println("EPG ERROR START required"); return true; }
  if(!strncasecmp(arg,"TIME ",5)) {
    uint32_t utc=0; int16_t offset=0;
    if(!parseIso(arg+5,utc,offset)) diagSerial.println("EPG ERROR TIME use ISO8601 with Z or explicit offset");
    else { core->setTime(utc,offset); EpgInvalidate(); diagSerial.println("EPG OK TIME"); }
    return true;
  }
  if(!strncasecmp(arg,"STATION ",8)) {
    uint64_t id=0;
    if(!hexValue(arg+8,id)) diagSerial.println("EPG ERROR STATION hex expected");
    else if(id<=UINT32_MAX) {
      if(core->selectSid(uint32_t(id))) { EpgInvalidate(); diagSerial.println("EPG OK STATION"); }
      else if(!core->stats.si) { pendingSid=uint32_t(id); core->select(0,core->now(),core->offset()); diagSerial.println("EPG OK STATION pending SI"); }
      else diagSerial.println("EPG ERROR STATION unknown or ambiguous; use full bearer");
    } else {
      uint8_t bytes[8]; const unsigned n=id>>48?8:6;
      for(unsigned i=0;i<n;++i) bytes[n-1-i]=uint8_t(id>>(8*i));
      uint64_t validated=0;
      if(!bearerValue(bytes,n,validated)) diagSerial.println("EPG ERROR STATION invalid bearer");
      else { pendingSid=0; core->select(validated,core->now(),core->offset()); EpgInvalidate(); diagSerial.println("EPG OK STATION"); }
    }
    return true;
  }
  diagSerial.println("EPG ERROR unknown command; EPG TEST HELP"); return true;
}
}
