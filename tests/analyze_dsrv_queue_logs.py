"""Read-only d048c72 evidence; deduplicate CTS replay and order it against mode ACKs."""
import argparse,hashlib,json,re,zipfile
from pathlib import Path

def analyze(path):
    output={'zip':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'logs':[]}
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if not name.endswith('.txt') or not re.search(r'11[AB]_',name):continue
            data=archive.read(name);lines=data.decode('utf-8','replace').splitlines()
            mode='A';first={};acks=[];watchdogs=[];rates=[]
            for line in lines:
                timestamp=re.search(r'\d\d:\d\d:\d\d\.\d{3}',line)
                at=timestamp[0] if timestamp else None
                ack=re.search(r'\[DSRV/MODE\] requested=([ABCD]) result=ACCEPTED',line)
                if ack:mode=ack[1];acks.append({'at':at,'mode':mode})
                state=re.search(r'\[DSRV/MODE\] mode=([ABCD])',line)
                if state:mode=state[1]
                if '[DAB/CTS-FIRST]' in line:
                    key=re.search(r'\batMs=(\d+)',line)
                    if key and key[1] not in first:first[key[1]]={'at':at,'modeAtFirstRecord':mode,'raw':line}
                if '[DAB/WATCHDOG]' in line:watchdogs.append(line)
                if '[DSRV/RATE]' in line:
                    row={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',line)}
                    m=re.search(r'\bmode=([ABCD])',line)
                    row.update({'at':at,'mode':m[1] if m else None});rates.append(row)
            output['logs'].append({'file':name,'sha256':hashlib.sha256(data).hexdigest(),
                                  'modeAcks':acks,'uniqueFirst':list(first.values()),
                                  'watchdogs':watchdogs,'rates':rates})
    return output

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('zip',type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args();result=analyze(args.zip)
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for log in result['logs']:
        print(f"{log['file']}: unique CTS-FIRST={len(log['uniqueFirst'])}, watchdog={len(log['watchdogs'])}")
        if '11A_C' in log['file']:
            print('MODE C ACK:',[r for r in log['modeAcks'] if r['mode']=='C'])
            print('First records:',[(r['at'],r['modeAtFirstRecord']) for r in log['uniqueFirst']])
