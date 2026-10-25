"""Run the hardware synthetic client against actual runtime/E2 code on pipes."""
from pathlib import Path
import queue,subprocess,sys,threading
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import epg_synthetic
out=ROOT/'.pio/epg_synthetic_uart';out.mkdir(parents=True,exist_ok=True)
source=(ROOT/'.pio/epg_runtime_test/runtime.cpp').read_text(encoding='utf-8')
source='#include <iostream>\n'+source.split('int main(int argc,char** argv)',1)[0]+r'''
int main(){
 std::string line;
 while(std::getline(std::cin,line)){
  diagSerial.output.clear();std::vector<char> input(line.begin(),line.end());input.push_back(0);
  epg::command(input.data());std::printf("%s",diagSerial.output.c_str());std::fflush(stdout);
  diagSerial.output.clear();++testMillis;epg::poll();
  while(epg::runtime() && epg::runtime()->busy())epg::poll();
  std::printf("%s",diagSerial.output.c_str());std::fflush(stdout);
 }
}
'''
cpp=out/'server.cpp';cpp.write_text(source,encoding='utf-8');exe=out/'server.exe'
compiler=sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(cpp),str(ROOT/'src/epg_core.cpp'),str(ROOT/'src/epg_uart.cpp'),'-o',str(exe)],check=True)
class Pipes:
    def __init__(self):
        self.proc=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
        self.queue=queue.Queue();threading.Thread(target=self.read,daemon=True).start()
    def read(self):
        for line in self.proc.stdout:self.queue.put(line)
    def write(self,data):self.proc.stdin.write(data);self.proc.stdin.flush()
    def readline(self):
        try:return self.queue.get(timeout=.1)
        except queue.Empty:return b''
    def close(self):
        self.proc.stdin.close();self.proc.wait(timeout=5)
port=Pipes()
try:
    results=epg_synthetic.suite(epg_synthetic.Client(port),epg_synthetic.groups(ROOT/'.pio/epg_synthetic_paths/groups.bin'))
    assert len(results)==9 and all(r['pass'] for r in results)
finally:port.close()
print('Real E2/runtime and hardware client on pipes: PASS (not physical hardware)')
