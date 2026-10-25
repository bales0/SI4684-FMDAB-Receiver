"""Word-only storage and the actual PNGdec EPG reader against RAW logos (host)."""
from pathlib import Path
import subprocess,sys,os,struct
ROOT=Path(__file__).resolve().parents[1]
compiler=sys.argv[1] if len(sys.argv)>1 else 'g++'
cc=compiler.replace('g++','gcc')
out=ROOT/'.pio/epg_storage_test';out.mkdir(parents=True,exist_ok=True)
lib=ROOT/'.pio/libdeps/esp32dev/PNGdec/src'
assert lib.exists(), 'Run pio run -e esp32dev to install the project PNGdec dependency'
source=(ROOT/'src/slideshow.cpp').read_text(encoding='utf-8')
source=source[source.index('struct Adam7DrawContext'):source.index('bool ShowSlideShow(')]+source[source.index('struct EpgLogoFile'):]
# Independent platform PNG codec supplies the two real Adam7 logo references.
if os.name=='nt':
 script=out/'oracle.ps1'
 script.write_text(r"""param([string]$inputDir,[string]$outputDir)
Add-Type -AssemblyName System.Drawing
foreach($name in @('4_B385','4_B389')) {
 $bitmap=New-Object System.Drawing.Bitmap (Join-Path $inputDir ($name+'.bin'))
 $bytes=New-Object 'System.Collections.Generic.List[byte]'
 for($y=0;$y -lt $bitmap.Height;$y++) { for($x=0;$x -lt $bitmap.Width;$x++) {
  $color=$bitmap.GetPixel($x,$y)
  $bytes.Add($color.R);$bytes.Add($color.G);$bytes.Add($color.B);$bytes.Add($color.A)
 } }
 [System.IO.File]::WriteAllBytes((Join-Path $outputDir ($name+'.rgba')),$bytes.ToArray())
 $bitmap.Dispose()
}
""",encoding='utf-8')
 code=script.read_text(encoding='utf-8').split('\n',1)[1]
 code="$inputDir='"+str(ROOT/'.pio/epg_test/objects').replace("'","''")+"'\n$outputDir='"+str(out).replace("'","''")+"'\n"+code
 subprocess.run(['powershell','-NoProfile','-Command',code],check=True)
else:
 from PIL import Image
 for name in ('4_B385','4_B389'):
  (out/(name+'.rgba')).write_bytes(Image.open(ROOT/'.pio/epg_test/objects'/(name+'.bin')).convert('RGBA').tobytes())
for name in ('4_B385','4_B389'):
 rgba=(out/(name+'.rgba')).read_bytes();pixels=[]
 for i in range(0,len(rgba),4):
  r,g,b,a=rgba[i:i+4];r=(r*a+255*(255-a)+127)//255;g=(g*a+255*(255-a)+127)//255;b=(b*a+255*(255-a)+127)//255
  pixels.append((r>>3)<<11|(g>>2)<<5|(b>>3))
 (out/(name+'.rgb565')).write_bytes(struct.pack('<'+'H'*len(pixels),*pixels))
prefix=r"""
#include "epg_core.h"
#include "png_adam7.h"
#include "PNGdec.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <cstdio>
struct Monitor { int availableForWrite(){return 4096;} void printf(const char*,...){} } diagSerial;
static PNG decoder;
alignas(8) static uint8_t decoderWorkspace[76800];
static const size_t SLS_DECODER_WORKSPACE_BYTES=sizeof(decoderWorkspace);
static std::vector<uint16_t> frame(7000,0);
static uint32_t hashValue=2166136261u;
static unsigned lines=0, pumps=0;
static void pixels(const uint16_t* p,int count) { for(int i=0;i<count;++i) { hashValue=(hashValue^p[i])*16777619u; } ++lines; }
struct Radio { void Update(bool schedule) { assert(!schedule); ++pumps; } } radio;
void yield(){}
struct Tft { void startWrite(){} void endWrite(){} void pushImage(int x,int y,int w,int h,const uint16_t* p) { assert(x>=12 && x+w<=112 && y>=34 && y<104 && w<=100 && h==1);for(int i=0;i<w;++i) frame[(y-34)*100+x-12+i]=p[i];pixels(p,w); } } tft;
PNG* acquirePngDecoder(bool& shared){shared=true;return &decoder;}
void releasePngDecoder(PNG*,bool){}
"""
suffix=r"""
int main(int argc,char** argv) {
 static_assert(!std::is_convertible<epg::Bytes,const uint8_t*>::value,"no raw byte access to word storage");
 assert(argc>=2);
 std::vector<uint32_t> words(256,0); epg::Buffer buffer(words.data(),true);
 std::vector<uint8_t> expected(1024,0),copy(1024);
 for(unsigned i=0;i<1024;++i){ const uint8_t v=uint8_t(i*73+11);buffer.set(i,v);expected[i]=v; }
 buffer.copyTo(copy.data(),copy.size());assert(copy==expected);
 for(unsigned destination:{0u,1u,2u,3u,7u,31u,511u}) for(unsigned source:{0u,1u,2u,3u,9u,31u,512u}) {
  buffer.write(0,expected.data(),expected.size());
  auto moved=expected;std::memmove(moved.data()+destination,moved.data()+source,127);
  buffer.move(destination,source,127);buffer.copyTo(copy.data(),copy.size());assert(copy==moved);
 }
 buffer.write(0,expected.data(),expected.size());
 assert(epg::crc32(buffer,1024)==epg::crc32(expected.data(),1024));
 assert(epg::crc16(buffer+3,1017)==epg::crc16(expected.data()+3,1017));
 uint8_t shortRead[7];(buffer+1017).copyTo(shortRead,7);assert(!std::memcmp(shortRead,expected.data()+1017,7));
 unsigned images=0,unsupported=0;
 for(int file=1;file<argc;++file) {
  std::ifstream f(argv[file],std::ios::binary); assert(f.good());
  std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
  if(png.size()<8 || png[0]!=137 || png[1]!=80) continue;
  assert(png.size()<=epg::kObjectBytes);
  std::vector<uint32_t> storage((png.size()+3)/4,0);epg::Buffer wordPng(storage.data(),true);wordPng.write(0,png.data(),png.size());
  hashValue=2166136261u;lines=0;
  int rc=decoder.openRAM(png.data(),int(png.size()),+[](PNGDRAW* draw) {
   uint16_t line[320];decoder.getLineAsRGB565(draw,line,PNG_RGB565_LITTLE_ENDIAN,0xFFFFFFFF);pixels(line,draw->iWidth);return 1;
  });
  if(rc==PNG_UNSUPPORTED_FEATURE) {
    decoder.close();
    std::string name(argv[file]);name=name.substr(name.find_last_of("/\\")+1);name=name.substr(0,name.size()-4);
    std::ifstream oracle(".pio/epg_storage_test/"+name+".rgb565",std::ios::binary);assert(oracle.good());
    std::vector<uint16_t> expectedPixels(7000);oracle.read(reinterpret_cast<char*>(expectedPixels.data()),14000);assert(oracle.gcount()==14000);
    frame.assign(7000,0);assert(DrawEpgLogo(wordPng,png.size(),12,34,100,70) && !epgLogoFile);
    assert(frame==expectedPixels);++unsupported;continue;
  }
  assert(rc==PNG_SUCCESS);
  const int height=decoder.getHeight();assert(decoder.getWidth()<=100 && height<=70);
  assert(decoder.decode(nullptr,PNG_CHECK_CRC)==PNG_SUCCESS);decoder.close();
  const uint32_t expectedHash=hashValue;assert(lines==unsigned(height));
  hashValue=2166136261u;lines=0;
  assert(DrawEpgLogo(wordPng,png.size(),12,34,100,70));
  assert(hashValue==expectedHash && lines==unsigned(height) && !epgLogoFile);
  assert(!DrawEpgLogo(wordPng,png.size(),12,34,1,1) && !epgLogoFile);
  ++images;
 }
 assert(images+unsupported==30 && images>0 && pumps>0);
 std::printf("Word-only offsets/overlap/CRC and actual PNGdec callbacks: %u RAW logos pixel-identical to byte input, %u Adam7 logos identical to independent OS codec, bounds/cleanup: PASS\n",images,unsupported);
}
"""
cpp=out/'storage.cpp';cpp.write_text(prefix+source+suffix,encoding='utf-8')
objects=[]
for name in ['adler32','crc32','infback','inffast','inflate','inftrees','zutil']:
 obj=out/(name+'.o');objects.append(str(obj))
 subprocess.run([cc,'-std=c99','-Duint=unsigned','-O2','-I',str(lib),'-c',str(lib/(name+'.c')),'-o',str(obj)],check=True)
obj=out/'png.o';objects.append(str(obj))
subprocess.run([compiler,'-std=c++11','-O2','-D__LINUX__','-DPNG_MAX_BUFFERED_PIXELS=2600','-I',str(lib),'-c',str(lib/'PNGdec.cpp'),'-o',str(obj)],check=True)
exe=out/'storage.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-D__LINUX__','-DPNG_MAX_BUFFERED_PIXELS=2600','-I',str(lib),'-I',str(ROOT/'src'),str(cpp),'src/epg_core.cpp','src/png_adam7.cpp',*objects,'-o',str(exe)],cwd=ROOT,check=True)
subprocess.run([str(exe),*map(str,sorted((ROOT/'.pio/epg_test/objects').glob('4_*.bin')))],check=True)
