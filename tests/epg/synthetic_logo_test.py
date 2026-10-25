"""Preserved synthetic logo test; --verify-fixtures is offline, otherwise opens UART.
After the four cases, PAD Adam7 repeats until Ctrl+C. Does not flash firmware.
"""
from pathlib import Path
import sys,struct,zlib,binascii,time,json
ROOT=Path(__file__).resolve().parents[2]
FIXTURES=ROOT/'tests/fixtures/epg'
sys.path.insert(0,str(ROOT/'tools'))
from epg_synthetic import Client,groups,wire
out=ROOT/'.pio/epg_acceptance'
original=groups(FIXTURES/'synthetic_programme_groups.bin')
def tlv(t,b):return bytes([t])+ (bytes([len(b)]) if len(b)<254 else b'\xfe'+struct.pack('>H',len(b)))+b
def crc16(b):
 c=0xffff
 for v in b:
  c^=v<<8
  for _ in range(8):c=((c<<1)^ (0x1021 if c&0x8000 else 0))&65535
 return c^65535
def group(tid,kind,seg,b,last=True):
 b=bytes([0x70|kind,0,(seg>>8)|(128 if last else 0),seg&255,0x12,tid>>8,tid&255])+struct.pack('>H',len(b))+b
 return b+struct.pack('>H',crc16(b))
letters=['11110111101111','10000100101000','11100111101011','10000100001001','11110100001111']
def pixel(x,y):
 if x<3 or x>=93 or y<3 or y>=61:return bytes([0,220,240])
 if 6<=x<18 and 6<=y<18:return bytes([255,210,0])
 if 78<=x<90 and 46<=y<58:return bytes([255,0,150])
 if 13<=x<83 and 20<=y<45 and letters[(y-20)//5][(x-13)//5]=='1':return b'\xff\xff\xff'
 return bytes([12,24,72])
def png(interlace):
 def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',binascii.crc32(t+b))
 passes=[(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)] if interlace else [(0,0,1,1)]
 raw=b''.join(b'\0'+b''.join(pixel(x,y) for x in range(xs,96,dx)) for xs,ys,dx,dy in passes for y in range(ys,64,dy))
 return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',96,64,8,2,0,0,interlace))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')
def fixture(image):
 name=b'synthetic.png'
 service=tlv(0x29,tlv(0x80,bytes.fromhex('40e231533201')))+tlv(0x11,tlv(1,b'Synthetic EPG One'))+tlv(0x2b,tlv(0x80,b'image/png')+tlv(0x82,name))
 si=tlv(3,tlv(0x26,tlv(0x28,service)))
 pi=b''.join(g[9:-2] for g in original[2:])
 objects=[(0x200,7,0,si,b''),(0x100,7,1,pi,b'\xe7\x06'+bytes.fromhex('40e231533201')),(0x300,2,3,image,b'\xcc'+bytes([len(name)+1])+b'\0'+name)]
 directory=bytearray(13);directory[5]=3
 for tid,typ,sub,body,ext in objects:
  n=len(body);hs=7+len(ext)
  h=bytes([n>>20,(n>>12)&255,(n>>4)&255,((n&15)<<4)|(hs>>9),(hs>>1)&255,((hs&1)<<7)|(typ<<1)|(sub>>8),sub&255])
  directory+=struct.pack('>H',tid)+h+ext
 directory[:4]=struct.pack('>I',len(directory))
 result=[group(1,6,0,directory)]
 for tid,typ,sub,body,ext in objects:
  for i,pos in enumerate(range(0,len(body),80)):
   result.append(group(tid,4,i,body[pos:pos+80],pos+80>=len(body)))
 return result
def main():
 import argparse,hashlib
 parser=argparse.ArgumentParser(description='Synthetic station logo: MSC/PAD and normal/Adam7 PNG. Uses preserved fixtures, not .pio inputs.')
 parser.add_argument('--port',default='COM4')
 parser.add_argument('--verify-fixtures',action='store_true',help='Offline integrity and generator check; does not open UART')
 args=parser.parse_args()
 manifest=json.loads((FIXTURES/'manifest.json').read_text(encoding='utf-8'))
 for name,entry in manifest['files'].items():
  content=(FIXTURES/name).read_bytes()
  assert len(content)==entry['bytes'] and hashlib.sha256(content).hexdigest()==entry['sha256'],name
 for interlace in (0,1):
  image=png(interlace)
  assert image==(FIXTURES/f'synthetic_logo_{interlace}.png').read_bytes()
  assert fixture(image)==groups(FIXTURES/f'synthetic_logo_{interlace}_groups.bin')
  for frame in fixture(image):assert crc16(frame[:-2])==int.from_bytes(frame[-2:],'big')
 print('Preserved EPG fixture hashes, generated PNG/streams and MSC CRC: PASS',flush=True)
 if args.verify_fixtures:return
 import serial
 out.mkdir(parents=True,exist_ok=True)
 port=serial.Serial(port=None,baudrate=115200,timeout=.1,write_timeout=2);port.dtr=False;port.rts=False;port.port=args.port;port.open()
 with (out/'hardware_synthetic_logo.log').open('w',encoding='utf-8') as log:
  client=Client(port,log);results=[]
  try:
   for interlace in (0,1):
    image=png(interlace);(out/f'synthetic_logo_{interlace}.png').write_bytes(image);data=fixture(image)
    for path in ('MSC','PAD'):
     start=len(client.lines);client.begin(path)
     for g in data:client.send(wire(g,path))
     time.sleep(.5);s=client.status()
     assert s['cached']==3 and s['PNG']==1 and s['bytes']==len(image) and s['interlace']==interlace and s['crcBad']==s['invalid']==0,s
     # Allow UI validation/drawing to complete, then drain diagnostic messages.
     time.sleep(1);client.status()
     drawing=[line for line in client.lines[start:] if '[EPG/LOGO]' in line]
     if interlace:assert any('draw=1' in line for line in drawing),drawing
     results.append(dict(path=path,interlace=interlace,bytes=len(image),pass_=True,drawing=drawing))
     print('LOGO PASS',path,'Adam7' if interlace else 'non-interlaced',len(image),flush=True)
   (out/'hardware_synthetic_logo.json').write_text(json.dumps(dict(scope='MCU synthetic DSRV, not RF',results=results,visual_confirmation='pending'),indent=2)+'\n',encoding='utf-8')
   print('LOGO SUITE PASS; PAD Adam7 loop active',flush=True)
   while True:
    for g in data:client.send(wire(g));time.sleep(.05)
    client.status();time.sleep(2)
  except KeyboardInterrupt:pass
  finally:
   port.write(b'EPG TEST STOP\n');port.close()

if __name__=='__main__':
 sys.stdout.reconfigure(encoding='utf-8',errors='backslashreplace')
 main()
