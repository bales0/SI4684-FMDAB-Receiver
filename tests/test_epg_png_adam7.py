"""Adam7 fixtures: independent encoder, all color/depth/filter cases and hostile input."""
from pathlib import Path
import sys,subprocess,struct,zlib
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'.pio/epg_adam7_test';out.mkdir(parents=True,exist_ok=True)
lib=ROOT/'.pio/libdeps/esp32dev/PNGdec/src'
compiler=sys.argv[1] if len(sys.argv)>1 else 'g++'
def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
passes=[(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]
def paeth(a,b,c):
 p=a+b-c;v=[abs(p-a),abs(p-b),abs(p-c)];return [a,b,c][v.index(min(v))]
def encode(w,h,kind,depth,extra=b'',filter_bad=False):
 channels={0:1,2:3,3:1,4:2,6:4}[kind];maximum=(1<<depth)-1
 palette=[((i*71)%256,(i*127)%256,(i*193)%256) for i in range(min(1<<depth,37))]
 alpha=[(i*97)%256 for i in range(len(palette))]
 samples=[];rgb=[]
 for y in range(h):
  for x in range(w):
   v=[((x*17+y*31+c*43)^(x*y*7))&maximum for c in range(channels)]
   if kind==3:v[0]%=len(palette)
   samples.append(v)
   eight=lambda n:n>>8 if depth==16 else n if depth==8 else n*255//maximum
   if kind==3:r,g,b=palette[v[0]];a=alpha[v[0]]
   elif kind in (0,4):r=g=b=eight(v[0]);a=eight(v[1]) if kind==4 else (0 if v[0]==0 else 255)
   else:r,g,b=map(eight,v[:3]);a=eight(v[3]) if kind==6 else (0 if v[:3]==[0,0,0] else 255)
   r=(r*a+255*(255-a)+127)//255;g=(g*a+255*(255-a)+127)//255;b=(b*a+255*(255-a)+127)//255
   rgb.append((r>>3)<<11|(g>>2)<<5|(b>>3))
 raw=bytearray();rowcount=0
 for x0,y0,dx,dy in passes:
  if x0>=w or y0>=h:continue
  previous=None
  for y in range(y0,h,dy):
   values=[v for x in range(x0,w,dx) for v in samples[y*w+x]]
   if depth>=8:line=bytearray().join(v.to_bytes(depth//8,'big') for v in values)
   else:
    line=bytearray((len(values)*depth+7)//8)
    for i,v in enumerate(values):line[(i*depth)//8]|=v<<(8-depth-(i*depth)%8)
   previous=previous if previous is not None else bytes(len(line));stride=max(1,(channels*depth+7)//8)
   filter=rowcount%5;rowcount+=1;filtered=bytearray()
   for i,v in enumerate(line):
    a=line[i-stride] if i>=stride else 0;b=previous[i];c=previous[i-stride] if i>=stride else 0
    predictor=[0,a,b,(a+b)//2,paeth(a,b,c)][filter];filtered.append((v-predictor)&255)
   raw.append(5 if filter_bad else filter);raw.extend(filtered);previous=line
 header=struct.pack('>IIBBBBB',w,h,depth,kind,0,0,1)
 prefix=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',header)
 if kind==3:prefix+=chunk(b'PLTE',bytes(v for color in palette for v in color))+chunk(b'tRNS',bytes(alpha))
 elif kind==0:prefix+=chunk(b'tRNS',b'\0\0')
 elif kind==2:prefix+=chunk(b'tRNS',b'\0'*6)
 compressed=zlib.compress(bytes(raw)+extra)
 step=1 if w*h<10 else 37
 result=prefix+b''.join(chunk(b'IDAT',compressed[i:i+step]) for i in range(0,len(compressed),step))+chunk(b'IEND',b'')
 return result,struct.pack('<II',w,h)+struct.pack('<'+'H'*len(rgb),*rgb)
files=[]
for kind,depths in [(0,[1,2,4,8,16]),(2,[8,16]),(3,[1,2,4,8]),(4,[8,16]),(6,[8,16])]:
 for depth in depths:
  for w,h in [(1,1),(3,7),(17,13),(320,240)]:
   data,expected=encode(w,h,kind,depth);name=f'{kind}_{depth}_{w}_{h}'
   p=out/(name+'.png');p.write_bytes(data);p.with_suffix('.expected').write_bytes(expected);files.append(p)
base,_=encode(17,13,6,8)
invalid=[base[:-1],base[:32],base[:29]+bytes([base[29]^1])+base[30:],encode(17,13,6,8,extra=b'X')[0],encode(17,13,6,8,filter_bad=True)[0]]
# Bad zlib checksum with correct outer PNG CRC.
def rewrite_idat(data,transform):
 chunks=[];compressed=bytearray();pos=8
 while pos<len(data):
  n=struct.unpack('>I',data[pos:pos+4])[0];tag=data[pos+4:pos+8];payload=data[pos+8:pos+8+n]
  if tag==b'IDAT':compressed.extend(payload)
  else:chunks.append((tag,payload))
  pos+=12+n
 return data[:8]+b''.join(chunk(tag,payload) for tag,payload in chunks[:-1])+chunk(b'IDAT',transform(bytes(compressed)))+chunk(*chunks[-1])
invalid.extend([rewrite_idat(base,lambda b:b[:-1]+bytes([b[-1]^1])),rewrite_idat(base,lambda b:b+b'junk'),rewrite_idat(base,lambda b:zlib.compress(zlib.decompress(b)[:-1]))])
for i,data in enumerate(invalid):p=out/f'invalid_{i}.bad.png';p.write_bytes(data);files.append(p)
source=r"""
#include "png_adam7.h"
#include <vector>
#include <fstream>
#include <iterator>
#include <cassert>
#include <cstring>
#include <cstdio>
struct State { unsigned w,h,pumps=0;std::vector<uint16_t> pixels;std::vector<unsigned> seen; };
static void pump(void* user){++static_cast<State*>(user)->pumps;}
static bool row(void* user,unsigned x,unsigned y,unsigned step,const uint16_t* pixels,unsigned count){
 State& s=*static_cast<State*>(user);assert(y<s.h && count && step && count<=320);
 for(unsigned i=0;i<count;++i){assert(x+i*step<s.w);const size_t at=y*s.w+x+i*step;assert(!s.seen[at]++);s.pixels[at]=pixels[i];}return true;
}
int main(int argc,char** argv){
 const size_t bytes=PngAdam7WorkspaceBytes();assert(bytes<=76800);
 std::vector<uint64_t> arena((bytes+7)/8+2,0xa5a5a5a5a5a5a5a5ULL);
 unsigned valid=0,invalid=0;
 for(int i=1;i<argc;++i){
  std::ifstream f(argv[i],std::ios::binary);std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});assert(f.good() || f.eof());
  std::string name(argv[i]);bool bad=name.find(".bad.")!=std::string::npos;
  PngAdam7Info info;State s{};
  if(!bad){std::ifstream reference(name.substr(0,name.size()-4)+".expected",std::ios::binary);assert(reference.good());
   uint32_t dimensions[2];reference.read(reinterpret_cast<char*>(dimensions),8);s.w=dimensions[0];s.h=dimensions[1];s.pixels.resize(s.w*s.h);s.seen.resize(s.w*s.h);
   std::vector<uint16_t> expected(s.w*s.h);reference.read(reinterpret_cast<char*>(expected.data()),expected.size()*2);
   assert(PngAdam7Decode(data.data(),data.size(),arena.data()+1,bytes,320,240,info,row,&s,pump));assert(s.pixels==expected && s.pumps && info.width==s.w && info.height==s.h);
   for(unsigned count:s.seen)assert(count==1);
   std::vector<uint32_t> word((data.size()+4)/4,0);epg::Buffer storage(word.data(),true);storage.write(1,data.data(),data.size());
   s.pixels.assign(s.w*s.h,0);s.seen.assign(s.w*s.h,0);
   assert(PngAdam7Decode(storage+1,data.size(),arena.data()+1,bytes,320,240,info,row,&s,pump));assert(s.pixels==expected);
   if(!valid || (s.w==320 && s.h==240 && info.type==6 && info.depth==8)) {
    // Reproduce heap blocks whose address misses the decoder's 8-byte boundary.
    std::vector<uint64_t> shifted((bytes+14)/8+2,0x9696969696969696ULL);
    for(unsigned offset=1;offset<8;++offset) {
      s.pixels.assign(s.w*s.h,0);s.seen.assign(s.w*s.h,0);
      void* base=reinterpret_cast<uint8_t*>(shifted.data()+1)+offset;
      assert(PngAdam7Decode(data.data(),data.size(),base,bytes+7,320,240,info,row,&s,pump));
      assert(s.pixels==expected && info.padding==8-offset);
      assert(shifted.front()==0x9696969696969696ULL && shifted.back()==0x9696969696969696ULL);
      assert(!PngAdam7Decode(data.data(),data.size(),base,info.padding?info.padding-1:0,320,240,info));
    }
   }
   assert(!PngAdam7Decode(data.data(),data.size(),arena.data()+1,bytes-1,320,240,info));
   assert(!PngAdam7Decode(data.data(),data.size(),reinterpret_cast<uint8_t*>(arena.data()+1)+1,bytes,320,240,info));++valid;
  }else{assert(!PngAdam7Decode(data.data(),data.size(),arena.data()+1,bytes,320,240,info));++invalid;}
  assert(arena.front()==0xa5a5a5a5a5a5a5a5ULL && arena.back()==0xa5a5a5a5a5a5a5a5ULL);
 }
 std::printf("Adam7: %u color/depth/filter/size fixtures, direct+word input, shifted 1..7-byte workspaces, %u hostile streams, bounded workspace=%zu: PASS\n",valid,invalid,bytes);
}
"""
cpp=out/'adam7.cpp';cpp.write_text(source,encoding='utf-8')
objects=[str(ROOT/'.pio/epg_storage_test'/(name+'.o')) for name in ['adler32','crc32','infback','inffast','inflate','inftrees','zutil']]
exe=out/'adam7.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-D__LINUX__','-I',str(lib),'-I',str(ROOT/'src'),str(cpp),'src/png_adam7.cpp','src/epg_core.cpp',*objects,'-o',str(exe)],cwd=ROOT,check=True)
subprocess.run([str(exe),*map(str,files)],check=True)
