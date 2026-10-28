"""Read-only summary of the supplied 4a4b2d2 ZIP; observations, not causality."""
import argparse, hashlib, json, re, zipfile
from pathlib import Path

def analyze(path):
    result={'source':path.name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'logs':[]}
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if not name.endswith('.txt'):continue
            raw=archive.read(name);lines=raw.decode('utf-8','replace').splitlines()
            def selected(pattern):return [line for line in lines if pattern in line]
            rates=[]
            for line in selected('[DSRV/RATE]'):
                fields={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',line)}
                rates.append(fields)
            result['logs'].append({
                'name':name,'sha256':hashlib.sha256(raw).hexdigest(),
                'memoryErrors':selected('EPG ERROR MEMORY'),
                'terminalTimeouts':selected('[RADIO/CTS] late host service'),
                'watchdogs':selected('[DAB/WATCHDOG]'),
                'esp32ResetLines':selected('[BOOT] reset reason')+selected('rst:0x'),
                'rates':rates,
                'limitations':['Timezone absent from timestamp prefix; preserve original local times.',
                               'Host service gap is not CTS timeout duration.',
                               'BUFF_COUNT maximum is not a verified physical FIFO capacity.',
                               'Changed reserve and packet ownership confound core=0/core=1 comparison.']})
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('zip',type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args();value=analyze(args.zip)
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for log in value['logs']:
        print(f"{log['name']}: memory={len(log['memoryErrors'])} terminal={len(log['terminalTimeouts'])} watchdog={len(log['watchdogs'])}")
