import os
"""Exercise real PC replay main() against the actual firmware UART/runtime on pipes.

This is a COM API simulation, NOT a physical serial/HW acceptance test.
"""
from pathlib import Path
import sys,subprocess,types,threading,queue,time,builtins,importlib.util
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import epg_replay
# Reuse the actual runtime harness generation, then replace only its test main.
# The required generated file is produced by test_epg_runtime.py.
source=(ROOT/'.pio/epg_runtime_test/runtime.cpp').read_text(encoding='utf-8')
a=source.index('int main(int argc,char** argv)')
source=source[:a]+r'''
int main(){
 std::string line;
 while(std::getline(std::cin,line)){
  diagSerial.output.clear();std::vector<char> b(line.begin(),line.end());b.push_back(0);
  epg::command(b.data());std::printf("%s",diagSerial.output.c_str());std::fflush(stdout);
  diagSerial.output.clear();++testMillis;epg::poll();while(epg::runtime()&&epg::runtime()->busy())epg::poll();
  std::printf("%s",diagSerial.output.c_str());std::fflush(stdout);
 }
}
'''
source='#include <iostream>\n'+source
out=ROOT/'.pio/epg_pc_test';out.mkdir(parents=True,exist_ok=True)
cpp=out/'server.cpp';cpp.write_text(source,encoding='utf-8')
exe=out/'server.exe'
subprocess.run([(sys.argv[1] if len(sys.argv)>1 else 'g++'),'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(cpp),'src/epg_core.cpp','src/epg_uart.cpp','-o',str(exe)],cwd=ROOT,check=True)
frames=epg_replay.extract(Path(os.environ.get('EPG_TEST_INPUT',str(ROOT/'_usertesting/device-monitor-261023-160924.log'))))
last_sequence=sum((len(f.payload)+31)//32 for f in frames)
class FakeSerial:
 instance=None
 def __init__(self,**kwargs):
  self.incoming=queue.Queue();self.max_seq=0;self.dropped=False;self.duplicates=0;self.history=[];self.last=None
  self.process=None;FakeSerial.instance=self
 def open(self):
  self.process=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
  def read_stdout():
   for line in iter(self.process.stdout.readline,b''):
    self.history.append(line)
    if line.startswith(b'EPG ACK ') and not self.dropped:
     self.dropped=True;continue # lost first ACK: PC must retry, device must deduplicate
    for byte in line:self.incoming.put(byte)
  threading.Thread(target=read_stdout,daemon=True).start()
 @property
 def in_waiting(self):return self.incoming.qsize()
 def read(self,n):
  result=bytearray()
  try:result.append(self.incoming.get(timeout=0.05))
  except queue.Empty:return b''
  while len(result)<n:
   try:result.append(self.incoming.get_nowait())
   except queue.Empty:break
  return bytes(result)
 def write(self,line):
  if line.startswith(b'E1 '):
   seq=int(line.split()[2],16)
   if line==self.last:self.duplicates+=1
   self.last=line;self.max_seq=max(self.max_seq,seq)
  self.process.stdin.write(line);self.process.stdin.flush();return len(line)
 def close(self):
  self.process.stdin.close();self.process.wait(timeout=3)
sys.modules['serial']=types.SimpleNamespace(Serial=FakeSerial,SerialException=OSError)
original_input=builtins.input
console_once=False
def console():
 global console_once
 if console_once:raise EOFError
 while not FakeSerial.instance or FakeSerial.instance.max_seq<last_sequence:time.sleep(0.01)
 console_once=True;return 'quit'
builtins.input=console
try:
 result=epg_replay.main([str(Path(os.environ.get('EPG_TEST_INPUT',str(ROOT/'_usertesting/device-monitor-261023-160924.log')))),'--port','SIMULATED','--speed','1000','--station','3233'])
finally:builtins.input=original_input
serial=FakeSerial.instance
assert result==0 and serial.max_seq==last_sequence and serial.duplicates>=1
assert any(b'cached=68' in row for row in serial.history)
assert not any(b'NACK' in row or b'ERROR' in row for row in serial.history)
print('Real Python CLI to actual C++ runtime: full RAW, session handshake, lost ACK retry, monitor/quit/STOP: PASS (COM simulated)')
