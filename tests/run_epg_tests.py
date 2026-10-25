"""One entry point for EPG host acceptance; raw data stays outside firmware/git."""
from pathlib import Path
import argparse,subprocess,sys,os,time,json
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--input',type=Path,default=ROOT/'_usertesting/device-monitor-261023-160924.log')
p.add_argument('--reference',type=Path,default=ROOT/'Specification/SI4684_MUX11B_EPG_kompletni_dekodovani.zip')
p.add_argument('--compiler',default='C:/msys64/ucrt64/bin/g++.exe' if Path('C:/msys64/ucrt64/bin/g++.exe').exists() else 'g++')
p.add_argument('--skip-pc',action='store_true',help='skip full Python COM-owner simulation')
a=p.parse_args();out=ROOT/'.pio/epg_acceptance';out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,EPG_TEST_INPUT=str(a.input.resolve()))
log=[];results=[]
def run(name,args):
 started=time.monotonic();r=subprocess.run(args,cwd=ROOT,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
 log.append('COMMAND: '+str(args)+'\n'+r.stdout+r.stderr)
 results.append({'test':name,'exit':r.returncode,'seconds':round(time.monotonic()-started,3)})
 (out/'results.log').write_text('\n'.join(log),encoding='utf-8')
 (out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
 print(name+(': PASS' if r.returncode==0 else ': FAIL'),flush=True)
 if r.returncode: print(r.stdout+r.stderr);raise SystemExit(r.returncode)
run('RAW/oracle production core',[sys.executable,'tests/test_epg_replay.py','--input',str(a.input),'--reference',str(a.reference),'--compiler',a.compiler])
for name,extra in [('errors',[]),('cache',[]),('uart',['src/epg_uart.cpp'])]:
 exe=out/(name+'.exe')
 run('compile '+name,[a.compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','tests/test_epg_'+name+'.cpp','src/epg_core.cpp',*extra,'-o',str(exe)])
 run(name,[str(exe),str(ROOT/'.pio/epg_test/objects')])
run('X-PAD config/diagnostics',[sys.executable,'tests/test_dab_xpad_integration.py',a.compiler])
for name in ['runtime','ui','transport','synthetic_paths','synthetic_uart','storage','png_adam7']+([] if a.skip_pc else ['pc']):
 run(name,[sys.executable,'tests/test_epg_'+name+'.py',a.compiler])
print('All EPG host checks passed. Reports: .pio/epg_acceptance/results.log and results.json')
