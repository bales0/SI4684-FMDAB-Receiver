"""Host harness for actual DAB parser, discovery, logging and SLS resolver bodies.

Run: python tests/test_dab_packet_runtime.py [path-to-g++]
Extracted production code is compiled with host time/UART fixtures; no radio I/O.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
cpp = (root / "src/si4684.cpp").read_text(encoding="utf-8")
header = (root / "src/si4684.h").read_text(encoding="utf-8")

def function(signature):
    start = cpp.index(signature + " {")
    brace = cpp.index("{", start)
    level = 1
    end = brace + 1
    while level:
        level += (cpp[end] == "{") - (cpp[end] == "}")
        end += 1
    return cpp[start:end] + "\n"

methods = "\n".join(function(signature) for signature in (
    "void DAB::parseDabServiceListReply(uint16_t replyLength)",
    "void DAB::resetDabSpiDiscovery(bool preservePresence, bool logInventory)",
    "void DAB::invalidateDabInventory(void)",
    "void DAB::refreshDabSpiDiscovery(void)",
    "void DAB::logDabUserApplications(uint32_t now)",
    "bool DAB::resolveDabDataService(void)",
))
fields = header[header.index("    struct UaService {"):header.index("    uint32_t dabSignalNextDueMs")]
service = header[header.index("typedef struct _Services"):header.index("} DABService;") + len("} DABService;")]
prefix = r'''
#include "dab_spi_discovery.h"
#include "dab_diagnostics.h"
#include "dab_spi_probe.h"
#include "dab_scheduler_policy.h"
#include "dab_service_switch_policy.h"
#include "vendor/si468x/Si468x.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdarg>
using byte = uint8_t;
uint8_t SPIbuffer[4096];
uint8_t dabServiceCharsetValue[32];
bool diagnosticDebug = false;
uint32_t nowMs = 100;
uint32_t millis() { return nowMs; }
std::vector<std::string> lines;
struct Uart { int space = 512; int availableForWrite() { return space; } } diagSerial;
void logLine(const char* format, ...) {
  if (!diagnosticDebug) return;
  char text[256]; va_list args; va_start(args, format);
  const int length = vsnprintf(text, sizeof(text), format, args); va_end(args);
  assert(length > 0 && length < static_cast<int>(sizeof(text)));
  lines.emplace_back(text);
}
#define DIAG_PRINTF(...) logLine(__VA_ARGS__)
#define DIAG_PRINTLN(text) logLine("%s\n", text)
'''
definition = r'''
class DAB {
 public:
  char FirmwVersion[6] = "6.0.9";
  DABService service[32] = {};
  uint8_t numberofservices = 0, ServiceIndex = 0;
  uint32_t serviceID = 0, componentID = 0, CurrentServiceID = 0;
  bool dabServiceListReady = false, dabServiceRequestPending = false;
  bool dabActiveServiceValid = true, dabActiveDataServiceValid = false;
  uint32_t dabRequestedComponentId = 0, dabActiveComponentId = 0x01FE0008;
  uint32_t dabActiveServiceId = 0x2100, dabServiceListGeneration = 0;
  uint32_t dabDataServiceId = 0, dabDataComponentId = 0;
  dab_switch::Controller dabSwitch;
  enum class DabCommand { None, ComponentInfo };
  DabCommand dabCommand = DabCommand::None;
  unsigned evaluationRequests = 0;
  void requestDabDataServiceEvaluation() { ++evaluationRequests; }
  void parseDabServiceListReply(uint16_t);
  bool resolveDabDataService();
'''
suffix = r'''
};
static int compareCompID(const void* a, const void* b) {
  uint32_t left = static_cast<const DABService*>(a)->CompID & 255;
  uint32_t right = static_cast<const DABService*>(b)->CompID & 255;
  return left < right ? -1 : left > right ? 1 : 0;
}
'''
tests = r'''
using Bytes = std::vector<uint8_t>;
void append32(Bytes& b, uint32_t value) {
  for (unsigned i=0; i<4; ++i) b.push_back(value >> (8*i));
}
void addService(Bytes& list, uint32_t sid, bool data,
                std::initializer_list<uint32_t> components) {
  ++list[4]; append32(list, sid);
  list.push_back(data); list.push_back(components.size());
  list.push_back(0); list.push_back(0);
  for (unsigned i=0; i<16; ++i) list.push_back('A');
  for (auto cid : components) append32(list, cid);
  list[0]=(list.size()-2)&255; list[1]=(list.size()-2)>>8;
}
void load(DAB& radio, const Bytes& list, int truncate=0) {
  memset(SPIbuffer, 0xEE, sizeof(SPIbuffer));
  std::copy(list.begin(), list.end(), SPIbuffer+5);
  radio.parseDabServiceListReply(list.size()+4-truncate);
}
Bytes audioMux() {
  Bytes list{6,0,7,0,0,0,0,0};
  for (unsigned i=0; i<10; ++i) addService(list, 0x2100+i, false, {0x01FE0008+i});
  return list;
}
void settle(DAB& radio) { radio.dabSwitch.state = dab_switch::State::Ready; }
void commit(DAB& radio, dab_spi::Identity identity, bool success=true) {
  assert(radio.dabUaScan.issue());
  const unsigned index = radio.dabUaScan.scheduled-1;
  radio.dabSpiIdentities[index] = identity;
  assert(radio.dabUaScan.commit(radio.dabUaScan.generation, success));
}
void pump(DAB& radio) {
  const size_t before = lines.size();
  radio.refreshDabSpiDiscovery();
  radio.logDabUserApplications(nowMs);
  assert(lines.size() <= before+1); // one line maximum, shared throttle
}
void drain(DAB& radio) {
  for (unsigned i=0; i<2000 && (radio.dabUaListLogPending || radio.dabSpiDiscoveryDirty || radio.dabUaSummaryStage < 3U || radio.dabUaLogPending || radio.dabXpadLogPending); ++i) {
    nowMs += 100; pump(radio);
  }
}
int main() {
  // Identical functional result with DEBUG off, UART blocked, or a fast UART.
  for(unsigned mode=0;mode<3;++mode) {
    diagnosticDebug=mode!=0; diagSerial.space=mode==1 ? 0 : 512;
    DAB fixture; settle(fixture);
    fixture.dabUaDebugWasEnabled=diagnosticDebug;
    Bytes fixtureList{6,0,33,0,0,0,0,0};
    addService(fixtureList,0x2100,false,{0x01FE0002,0x00F0E321});
    load(fixture,fixtureList);
    commit(fixture,dab_spi::Identity::NotSpi);
    fixture.refreshDabSpiDiscovery(); assert(fixture.dabSpiDiscoveryDirty);
    commit(fixture,dab_spi::Identity::Spi);
    const size_t before=lines.size();
    fixture.refreshDabSpiDiscovery();
    assert(lines.size()==before && fixture.dabSpiPresent);
    assert(fixture.dabSpiDiscovery.status==dab_spi::Status::SelectedService);
    assert(fixture.dabSpiDiscovery.componentId==0x00F0E321);
    assert(fixture.dabUaRefresh.deadline==nowMs+60000);
    if(mode==0) assert(fixture.dabUaSummaryStage==3 && !fixture.dabUaListLogPending);
    // Same-mux refresh preserves evidence, invalidates replies, settles once.
    fixture.resetDabSpiDiscovery(true,false);
    commit(fixture,dab_spi::Identity::NotSpi);
    commit(fixture,dab_spi::Identity::Failed,false);
    fixture.refreshDabSpiDiscovery();
    assert(fixture.dabSpiDiscovery.status==dab_spi::Status::Incomplete);
    assert(!fixture.dabSpiDiscovery.available());
  }
  // Standalone SLS evaluation also ignores pending diagnostics and UART capacity.
  diagnosticDebug=true; diagSerial.space=0;
  DAB sls; settle(sls); sls.dabUaDebugWasEnabled=true;
  Bytes slsList{6,0,33,0,0,0,0,0};
  addService(slsList,0x2100,false,{0x01FE0008,0x00F0E321}); load(sls,slsList);
  commit(sls,dab_spi::Identity::NotSpi); commit(sls,dab_spi::Identity::Slideshow);
  sls.dabUaLogPending=true;
  sls.refreshDabSpiDiscovery();
  assert(sls.resolveDabDataService() && sls.evaluationRequests==1);
  assert(sls.dabDataComponentId==0x00F0E321 && !sls.dabSpiPresent);
  sls.refreshDabSpiDiscovery(); assert(sls.evaluationRequests==1);
  diagnosticDebug=false; diagSerial.space=512; lines.clear();
  // Actual XPAD logger prints only measured values and never fabricates match.
  DAB xpadLog; xpadLog.dabUaDebugWasEnabled=true;
  diagnosticDebug=true; diagSerial.space=0;
  xpadLog.dabXpad.setResult=0; xpadLog.dabXpad.getResult=0;
  xpadLog.dabXpad.value=0x0097; xpadLog.dabXpad.valid=true;
  xpadLog.dabXpadLogPending=true;
  xpadLog.logDabUserApplications(nowMs);
  assert(lines.empty() && xpadLog.dabXpadLogPending);
  diagSerial.space=512; xpadLog.logDabUserApplications(nowMs);
  assert(lines.size()==1 && lines.back().find("readback=0x0097")!=std::string::npos);
  assert(lines.back().find("match=1")!=std::string::npos);
  xpadLog.dabXpad.value=0x0095; xpadLog.dabXpadLogPending=true; nowMs+=100;
  xpadLog.logDabUserApplications(nowMs);
  assert(lines.back().find("match=0")!=std::string::npos);
  xpadLog.dabXpad.valid=false; xpadLog.dabXpad.getResult=-4;
  xpadLog.dabXpadLogPending=true; nowMs+=100; xpadLog.logDabUserApplications(nowMs);
  assert(lines.back().find("readback=unavailable getResult=-4")!=std::string::npos);
  assert(lines.back().find("match=")==std::string::npos);
  xpadLog.dabXpad=dab_diag::XpadReadback{};
  xpadLog.dabXpadLogPending=true; nowMs+=100; xpadLog.logDabUserApplications(nowMs);
  assert(lines.back().find("getResult=not-attempted")!=std::string::npos);
  lines.clear(); diagnosticDebug=false;
  // Check actual +1 reply prefix, four status bytes, SIZE, VERSION and service offsets.
  DAB radio; settle(radio);
  auto list = audioMux(); load(radio,list);
  assert(radio.numberofservices==10 && radio.dabUaComponentCount==10);
  assert(radio.dabUaServices[0].sid==0x2100 && radio.dabUaListVersion==7);
  assert(radio.dabUaComponents[0]==0x01FE0008);
  const uint32_t gen=radio.dabUaScan.generation;
  load(radio,list); assert(radio.dabUaScan.generation==gen); // stable topology
  for (unsigned i=0;i<9;++i) commit(radio,dab_spi::Identity::Slideshow);
  radio.refreshDabSpiDiscovery(); assert(radio.dabSpiDiscoveryDirty);
  commit(radio,dab_spi::Identity::Slideshow);
  diagnosticDebug=true; radio.dabUaDebugWasEnabled=true;
  drain(radio);
  assert(radio.dabSpiDiscovery.status==dab_spi::Status::NotSignalled && !radio.dabSpiPresent);
  assert(!radio.resolveDabDataService() && radio.evaluationRequests==0);
  for (const auto& line : lines) std::fputs(line.c_str(),stdout);
  lines.clear();

  addService(list,0xE1234567,true,{0x00F2E321}); load(radio,list);
  assert(radio.dabUaServices[10].info1 & 1);
  assert(radio.dabUaComponents[10]==0x00F2E321);
  // DEBUG queues only diagnostics, drains inventory at 100 ms, respects UART headroom.
  radio.dabUaDebugWasEnabled=false; radio.logDabUserApplications(nowMs);
  diagSerial.space=255; const size_t before=lines.size();
  nowMs+=100; pump(radio); assert(lines.size()==before);
  diagSerial.space=512;
  while(radio.dabUaListLogPending) { nowMs+=100; pump(radio); }
  for(unsigned i=0;i<10;++i) commit(radio,dab_spi::Identity::Slideshow);
  commit(radio,dab_spi::Identity::NotSpi);
  drain(radio);
  assert(radio.dabSpiDiscovery.status==dab_spi::Status::PacketMotUnverified);
  assert(!radio.dabSpiPresent && !radio.dabSpiDiscovery.available());
  assert(!radio.resolveDabDataService() && radio.evaluationRequests==0);
  for (const auto& line : lines) std::fputs(line.c_str(),stdout);
  lines.clear();
  // Last UA reply publishes immediately even with DEBUG and a full UART.
  radio.resetDabSpiDiscovery(false,false);
  for(unsigned i=0;i<10;++i) commit(radio,dab_spi::Identity::Slideshow);
  assert(radio.dabUaScan.issue());
  radio.refreshDabSpiDiscovery(); assert(!radio.dabSpiPresent);
  radio.dabSpiIdentities[10]=dab_spi::Identity::Spi;
  assert(radio.dabUaScan.commit(radio.dabUaScan.generation,true));
  radio.dabUaListLogPending=true;
  diagSerial.space=0;
  radio.dabUaLogPending=true; radio.dabUaHeaderPending=true;
  radio.dabUaRemaining=0; // retained empty UA diagnostic slot
  const size_t blockedLines=lines.size();
  radio.refreshDabSpiDiscovery(); assert(radio.dabSpiPresent);
  assert(!radio.dabSpiDiscoveryDirty && radio.dabUaRefresh.armed);
  assert(radio.dabSpiDiscoveryGeneration==radio.dabUaScan.generation);
  const auto published=radio.dabSpiDiscovery;
  const auto deadline=radio.dabUaRefresh.deadline;
  for(unsigned i=0;i<5;++i) { nowMs+=1000; pump(radio); }
  assert(lines.size()==blockedLines && radio.dabSpiPresent);
  assert(radio.dabUaSummaryStage==0 && radio.dabUaRefresh.deadline==deadline);
  assert(!radio.dabUaRefresh.ready(deadline-1,true));
  assert(radio.dabUaRefresh.ready(deadline,true));
  radio.dabUaListLogPending=false;
  diagSerial.space=512;
  for(unsigned stage=0;stage<3;++stage) {
    nowMs+=100; pump(radio);
    assert(radio.dabUaSummaryStage==stage+1);
    assert(radio.dabSpiDiscovery.componentId==published.componentId);
    assert(radio.dabUaRefresh.deadline==deadline); // no repeated finalization
  }
  drain(radio); assert(radio.dabSpiPresent && radio.dabSpiDiscovery.available());
  assert(radio.dabSpiDiscovery.componentId==0x00F2E321);
  assert(radio.evaluationRequests==0); // SPI never goes through slideshow START

  // DEBUG toggles neither generation nor completion; completed result logs on demand.
  const uint32_t completedGeneration=radio.dabUaScan.generation;
  diagnosticDebug=false; pump(radio);
  diagnosticDebug=true; diagSerial.space=0; pump(radio);
  assert(radio.dabUaScan.generation==completedGeneration && radio.dabSpiPresent);
  // Retune during a partially printed summary discards old diagnostics and answers.
  radio.dabUaListLogPending=false; radio.dabXpadLogPending=false;
  radio.dabUaSummaryStage=1;
  radio.resetDabSpiDiscovery(true,false);
  assert(radio.dabUaSummaryStage==3 && !radio.dabUaLogPending);
  const auto newGeneration=radio.dabUaScan.generation;
  pump(radio); assert(lines.size()>=blockedLines && radio.dabUaScan.generation==newGeneration);
  // Complete a periodic pass once with no logger progress.
  for(unsigned i=0;i<10;++i) commit(radio,dab_spi::Identity::Slideshow);
  commit(radio,dab_spi::Identity::Spi);
  radio.refreshDabSpiDiscovery(); assert(radio.dabSpiPresent);
  const auto periodicDeadline=radio.dabUaRefresh.deadline;
  radio.refreshDabSpiDiscovery(); assert(radio.dabUaRefresh.deadline==periodicDeadline);
  diagSerial.space=512;

  // Short, reserved NUM_COMP and excess-capacity lists keep BOTH valid inventories.
  const auto oldStation=radio.service[0]; const uint32_t oldCid=radio.dabUaComponents[10];
  const uint32_t token=radio.dabUaScan.generation;
  for(unsigned mode=0;mode<3;++mode) {
    auto bad=list;
    if(mode==1) bad[13]=15; // first NUM_COMP in list (8+5)
    if(mode==2) bad[4]=32;
    load(radio,bad,mode==0 ? 1 : 0);
    assert(radio.service[0].ServiceID==oldStation.ServiceID && radio.numberofservices==11);
    assert(radio.dabUaComponents[10]==oldCid && !radio.dabUaInventoryValid);
    assert(radio.dabSpiDiscovery.status==dab_spi::Status::Incomplete);
    assert(radio.dabSpiPresent && !radio.dabSpiDiscovery.available());
    assert(!radio.dabUaScan.commit(token,true));
    drain(radio);
    assert(radio.dabSpiDiscovery.status==dab_spi::Status::Incomplete);
  }
  // New VERSION with the same CID invalidates an active old response.
  diagnosticDebug=false; load(radio,list);
  assert(radio.dabUaScan.issue()); const uint32_t activeToken=radio.dabUaScan.generation;
  list[2]++; load(radio,list);
  assert(!radio.dabUaScan.commit(activeToken,true) && radio.dabUaScan.processed==0);
  for(unsigned i=0;i<10;++i) commit(radio,dab_spi::Identity::Slideshow);
  commit(radio,dab_spi::Identity::Failed,false);
  radio.refreshDabSpiDiscovery(); assert(radio.dabSpiDiscovery.status==dab_spi::Status::Incomplete);
  // Same-mux audio change uses this actual reset method, invalidating old UA work.
  radio.resetDabSpiDiscovery(true,false);
  assert(radio.dabUaScan.issue()); const uint32_t audioToken=radio.dabUaScan.generation;
  radio.resetDabSpiDiscovery(true,false);
  assert(!radio.dabUaScan.commit(audioToken,true) && !radio.dabUaScan.active);
  // Zero-component services log once without reading past the component inventory.
  Bytes zero{6,0,8,0,0,0,0,0}; addService(zero,0x2200,false,{});
  load(radio,zero); diagnosticDebug=true; radio.dabUaDebugWasEnabled=true;
  radio.resetDabSpiDiscovery(); drain(radio);
  assert(!radio.dabUaListLogPending && radio.dabSpiDiscovery.status==dab_spi::Status::NotSignalled);
  std::puts("Actual DAB parser/discovery/logging: offsets, retained UI, stale generation, UART pacing: PASS");
}
'''
# Guard that the manual SPI probe continues to require an actual selected endpoint.
probe = cpp[cpp.index("bool DAB::requestSpiProbe("):cpp.index("void DAB::stopSpiProbe(")]
assert "dabSpiDiscovery.available()" in probe
assert "resetDabSpiDiscovery(true, false);" in function("void DAB::setService(uint8_t _index)")
submission = cpp[cpp.index("if (startDabCommand(DabCommand::ServiceListHeader,"):]
assert "} else {\n      invalidateDabInventory();" in submission.split("return;", 1)[0]
for signature in ("void DAB::refreshDabSpiDiscovery(void)", "bool DAB::resolveDabDataService(void)"):
    assert "START_DIGITAL_SERVICE" not in function(signature)

ua_schedule = cpp[cpp.index("  const bool uaQueryPending ="):cpp.index("  const bool lowPriorityPending =")]
assert "dabUaLogPending" not in ua_schedule and "dabUaListLogPending" not in ua_schedule
for signature in ("void DAB::refreshDabSpiDiscovery(void)", "bool DAB::resolveDabDataService(void)"):
    body = function(signature)
    assert "availableForWrite" not in body and "dabUaLogPending" not in body
# Raw field may only be printed; it is never passed to the router/legacy decoder.
route_call = cpp[cpp.index("      const dab_probe::Route target = dab_probe::route("):]
assert "rawUatypeOrRfu" not in route_call.split(";", 1)[0]
assert "si468x::Property::DAB_XPAD_ENABLE, dab_diag::XPAD_REQUESTED" in cpp

out = root / ".pio/tests_dab_packet"
out.mkdir(parents=True, exist_ok=True)
file = out / "runtime.cpp"
file.write_text(prefix + service + definition + fields + suffix + methods + tests, encoding="utf-8")
exe = out / "runtime.exe"
compiler = sys.argv[1] if len(sys.argv)>1 else "g++"
subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(root / "src"), str(file), "-o", str(exe)], check=True)
result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
(out / "synthetic_runtime.log").write_text(result.stdout, encoding="utf-8")
print(result.stdout, end="")
