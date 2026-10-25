"""Compile the actual boot XPAD function against a fault-injecting chip mock."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "src/si4684.cpp").read_text(encoding="utf-8")

def function(signature):
    start = source.index(signature + " {")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

actual = function("void DAB::configureDabXpad(void)")
raw_start = source.index("      // Only diagnostic interpretation:")
raw_end = source.index("      // Capture identity mismatches", raw_start)
actual_raw = """
void DAB::logRawSample(const si468x::DsrvHeader& header, dab_probe::Route target,
                       uint32_t routeNow) {
""" + source[raw_start:raw_end] + "}\n"
prefix = r'''
#include "dab_diagnostics.h"
#include "dab_spi_discovery.h"
#include "dab_spi_probe.h"
#include "dab_service_switch_policy.h"
#include "vendor/si468x/Si468x.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdarg>
bool diagnosticDebug = true;
struct Uart { int space = 512; int availableForWrite() { return space; } } diagSerial;
std::vector<std::string> lines;
void logLine(const char* format, ...) {
  char text[256]; va_list args; va_start(args,format);
  int n=vsnprintf(text,sizeof(text),format,args); va_end(args);
  assert(n>0 && n<256); lines.emplace_back(text);
}
#define DIAG_PRINTF(...) logLine(__VA_ARGS__)
unsigned diagnostics = 0;
void finishCommandDiagnostics(si468x::Result) { ++diagnostics; }
struct Chip {
  si468x::Result setResult = si468x::Result::Ok;
  si468x::Result getResult = si468x::Result::Ok;
  uint16_t returned = 0x0097;
  std::vector<unsigned> calls;
  si468x::Result setProperty(si468x::Property p, uint16_t v) {
    assert(p == si468x::Property::DAB_XPAD_ENABLE && v == 0x0097);
    calls.push_back(1); return setResult;
  }
  si468x::Result getProperty(si468x::Property p, uint16_t& v, uint32_t timeout) {
    assert(p == si468x::Property::DAB_XPAD_ENABLE && timeout == 250000);
    assert(calls.size() == 1 && calls[0] == 1);
    calls.push_back(2); v = returned; return getResult;
  }
} chip;
class DAB {
 public:
  dab_diag::XpadReadback dabXpad;
  bool dabXpadLogPending = false;
  dab_diag::RawLog dabRawUaLog;
  dab_spi::Scan dabUaScan;
  bool dabActiveServiceValid = true;
  dab_switch::Controller dabSwitch;
  void configureDabXpad(void);
  void logRawSample(const si468x::DsrvHeader&, dab_probe::Route, uint32_t);
};
'''
suffix = r'''
int main() {
  DAB radio;
  for (const auto result : {si468x::Result::Ok, si468x::Result::Timeout,
                            si468x::Result::DeviceError, si468x::Result::Busy}) {
    chip.calls.clear(); diagnostics=0; chip.setResult=si468x::Result::Ok;
    chip.getResult=result; radio.configureDabXpad();
    assert(chip.calls.size()==2 && diagnostics==2 && radio.dabXpadLogPending);
    assert(radio.dabXpad.valid==(result==si468x::Result::Ok));
    assert(radio.dabXpad.getResult==static_cast<int8_t>(result));
    assert(radio.dabXpad.matches()==(result==si468x::Result::Ok));
    if(result!=si468x::Result::Ok) assert(radio.dabXpad.value==0);
  }
  chip.calls.clear(); chip.getResult=si468x::Result::Ok; chip.returned=0x0095;
  radio.configureDabXpad(); assert(radio.dabXpad.valid && !radio.dabXpad.matches());
  chip.calls.clear(); diagnostics=0; chip.setResult=si468x::Result::DeviceError;
  radio.configureDabXpad();
  assert(chip.calls.size()==1 && diagnostics==1 && !radio.dabXpad.valid);
  assert(radio.dabXpad.getResult==dab_diag::NOT_ATTEMPTED);
  assert(!radio.dabXpad.matches() && radio.dabXpad.value==0);
  std::puts("Actual boot XPAD: ordered write/read, mismatch, failure, timeout, no stale value: PASS");
  radio.dabSwitch.state=dab_switch::State::Ready;
  radio.dabUaScan.begin(1,true);
  si468x::DsrvHeader header;
  header.rawUatypeOrRfu=2; header.dataSource=1;
  header.serviceId=0x2FB9; header.componentId=5;
  diagSerial.space=0; radio.logRawSample(header,dab_probe::Route::Audio,0);
  assert(lines.empty());
  diagSerial.space=512; radio.logRawSample(header,dab_probe::Route::Audio,1);
  assert(lines.size()==1 && lines.back().find("meaning=SLS-candidate")!=std::string::npos);
  assert(lines.back().find("context=audio")!=std::string::npos);
  assert(lines.back().find("suppressed=1")!=std::string::npos);
  header.rawUatypeOrRfu=7;
  radio.logRawSample(header,dab_probe::Route::Audio,2); assert(lines.size()==1);
  radio.logRawSample(header,dab_probe::Route::Audio,5001);
  assert(lines.size()==2 && lines.back().find("meaning=SPI-candidate")!=std::string::npos);
  assert(lines.back().find("rev=unverified")!=std::string::npos);
  header.serviceId++; // foreign identity remains explicitly unmatched
  radio.logRawSample(header,dab_probe::Route::Ignored,10001);
  assert(lines.back().find("context=unmatched")!=std::string::npos);
  radio.dabActiveServiceValid=false;
  radio.logRawSample(header,dab_probe::Route::Audio,15001);
  assert(lines.back().find("context=inactive")!=std::string::npos);
  radio.dabUaScan.begin(1,true); header.dataSource=2;
  radio.logRawSample(header,dab_probe::Route::Audio,15002);
  assert(lines.back().find("meaning=DLS-raw")!=std::string::npos);
  const size_t count=lines.size(); diagnosticDebug=false;
  radio.logRawSample(header,dab_probe::Route::Audio,25002); assert(lines.size()==count);
  for(const auto& line:lines) std::fputs(line.c_str(),stdout);
  std::puts("Actual DSRV diagnostic: context, suppressed samples, generation and UART pacing: PASS");
}
'''

# Synchronous API is confined to the DAB initialization branch. Runtime command
# scheduler, IRQ/CTS mechanics and recovery policy are unchanged.
assert source.count("configureDabXpad();") == 1
boot = function("bool DAB::begin(uint8_t SSpin, RadioMode requestedMode)")
dab_start = boot.index('if (requestedMode == RADIO_MODE_DAB) {\n    DIAG_PRINTLN("[RADIO] DAB frequency')
call = boot.index("configureDabXpad();")
fm_start = boot.index('DIAG_PRINTLN("[RADIO] FM', call)
assert dab_start < call < fm_start
for signature in ("void DAB::Update(bool allowNewWork)", "void DAB::scheduleNextDabCommand(void)"):
    body = function(signature)
    assert "getProperty(" not in body and "configureDabXpad(" not in body
reset = function("void DAB::onHardwareResetComplete(void)")
for body in (boot, reset):
    assert "dabXpad = dab_diag::XpadReadback{};" in body
    assert "dabRawUaLog = dab_diag::RawLog{};" in body
    assert "dabXpadLogPending = false;" in body
assert boot.index("dabXpad =") < boot.index("if (!radioMemoryPrepared")
assert "dabXpad" not in function("void DAB::clearData(void)") # tuning doesn't alter the chip property
finish = function("void DAB::finishDabCommand(void)")
assert finish.index("generationMatches") < finish.index("case DabCommand::DsrvHeader:")
gui = (root / "src/gui.cpp").read_text(encoding="utf-8")
assert "const epg::IndicatorState state = epg::indicatorState();" in gui

out = root / ".pio/tests_dab_xpad"
out.mkdir(parents=True, exist_ok=True)
file = out / "boot.cpp"
file.write_text(prefix + actual + actual_raw + suffix, encoding="utf-8")
exe = out / "boot.exe"
compiler = sys.argv[1] if len(sys.argv) > 1 else "g++"
subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(root / "src"), str(file), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
print("DAB-only boot readback, reset/early-failure invalidation and unchanged EPG binding: PASS")
