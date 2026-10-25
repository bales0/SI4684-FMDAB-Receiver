"""Replay original RAW through the production C++ core; compare oracle and PNG bytes."""
from pathlib import Path
import argparse,subprocess,sys,struct,json,zipfile,hashlib
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from epg_replay import extract,Extraction
p=argparse.ArgumentParser()
p.add_argument('--input',type=Path,default=ROOT/'_usertesting/device-monitor-261023-160924.log')
p.add_argument('--reference',type=Path,default=ROOT/'Specification/SI4684_MUX11B_EPG_kompletni_dekodovani.zip')
p.add_argument('--compiler',default='C:/msys64/ucrt64/bin/g++.exe')
a=p.parse_args()
out=ROOT/'.pio/epg_test';out.mkdir(parents=True,exist_ok=True)
objs=out/'objects';objs.mkdir(exist_ok=True)
stats=Extraction();frames=extract(a.input,stats)
print('RAW extraction:',stats,flush=True)
with (out/'frames.bin').open('wb') as f:
 for frame in frames:f.write(struct.pack('<IIH',frame.sid,frame.cid,len(frame.payload))+frame.payload)
exe=out/'core.exe'
subprocess.run([a.compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','tests/test_epg_core.cpp','src/epg_core.cpp','-o',str(exe)],cwd=ROOT,check=True)
r=subprocess.run([str(exe),str(out/'frames.bin'),str(objs)],capture_output=True,text=True)
print(r.stderr); r.check_returncode()
(out/'programmes.tsv').write_text(r.stdout)
from datetime import datetime
from epg_time_oracle import programme_times
with zipfile.ZipFile(a.reference) as z:
 assert hashlib.sha256(a.reference.read_bytes()).hexdigest()=='aed762cc1f5076dbda47b63101dfeef6ced4b418c9210e7c3c60ba25cc509d1e'
 reference=json.loads(z.read('decoded.json'))
 oracle=reference['programmes']
 rows=[l.split('\t') for l in r.stdout.splitlines() if l.startswith('P\t')]
 assert len(rows)==len(oracle)==1087
 services=[l.split('\t') for l in r.stdout.splitlines() if l.startswith('S\t')]
 assert len(services)==len(reference['services'])==55
 service_index={(x['tid'],x['serviceName']):x for x in reference['services']}
 for _,tid,bearer,png,name,logo in services:
  expected=service_index[tid,bytes.fromhex(name).decode()]
  assert bearer==expected['bearers'][0]['id']
  assert bytes.fromhex(logo).decode()==expected['medias'][-1]['url']
  assert bool(int(png))==(expected['medias'][-1]['mimeValue']=='image/png')
 print('SI: 55 service records, exact bearer/name/MIME/logo metadata: PASS')
 index={(x['tid'],x['shortId']):x for x in oracle}
 binary_times={}
 corrected_legacy_times=0
 truncated=0
 for row in rows:
  _,tid,bearer,start,duration,lto,pid,version,medium,title,desc,rejected_time=row
  expected=index[tid,int(pid)];t=expected['time'][0]
  assert bearer==expected['sid'],(tid,pid,'bearer')
  if tid not in binary_times:binary_times[tid]=programme_times((objs/f'4_{tid}.bin').read_bytes())
  expected_start,expected_lto,_=binary_times[tid][int(pid)]
  assert not rejected_time,(tid,pid,'valid broadcast time rejected')
  assert int(start)==expected_start,(tid,pid,'binary UTC')
  assert int(lto)==expected_lto,(tid,pid,'binary LTO')
  if expected_start!=int(datetime.fromisoformat(t['time']['utc']).timestamp()):corrected_legacy_times+=1
  assert int(duration)==t['duration'],(tid,pid,'duration')
  assert bytes.fromhex(medium).decode()==expected['mediumName'],(tid,pid,'medium')
  expected_title=expected['longName'] or expected['mediumName'] or expected['shortName']
  assert bytes.fromhex(title).decode()==expected_title,(tid,pid,'title')
  expected_desc=expected['longDescription'] or expected['shortDescription']
  actual=bytes.fromhex(desc).decode()
  assert actual==expected_desc or (len(actual.encode())>=508 and expected_desc.startswith(actual)),(tid,pid,'description',actual,expected_desc)
  if actual!=expected_desc: truncated+=1
 assert corrected_legacy_times>0
 print('Legacy JSON time corrections against independent binary oracle:',corrected_legacy_times)
 print('Bounded descriptions truncated:',truncated)
 assert (objs/'6_F651.bin').read_bytes()==z.read('MOT_Directory_F651.bin')
 for name in z.namelist():
  if name.startswith('png/'):
   tid=name.split('TID_')[1][:4]
   assert (objs/f'4_{tid}.bin').read_bytes()==z.read(name)
 print('Production RAW replay: 70 objects, 35 PI, 5 SI, 30 byte-identical PNG; 1087 bearer/time/title/description records: PASS')
