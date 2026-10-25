"""Synthetic DSRV tests of both functional EPG inputs and isolation.

No serial port, RF capture or original RAW fixture is required.
"""
from pathlib import Path
import ast,json,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'.pio/epg_synthetic_paths';out.mkdir(parents=True,exist_ok=True)
radio=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
# Reuse the existing radio mock shell, but use real DSRV parsing and EPG decode.
tree=ast.parse((ROOT/'tests/test_epg_transport.py').read_text(encoding='utf-8'))
prefix=next(ast.literal_eval(node.value) for node in tree.body if isinstance(node,ast.Assign) and any(isinstance(x,ast.Name) and x.id=='prefix' for x in node.targets))
prefix=prefix.replace('#include "dab_spi_probe.h"','#include "epg_core.h"\n#include "vendor/si468x/Si468x.h"\n#include <string>\n#include <fstream>\n#include "dab_spi_probe.h"')
prefix=prefix.replace('namespace si468x { enum class Result{Ok,Timeout}; struct DsrvHeader{unsigned byteCount=0,dataSource=0;}; }','')
prefix='\n'.join(line for line in prefix.splitlines() if not line.startswith(('static bool epgContinuous=', 'static void discard(')))
a=prefix.index('namespace epg {');b=prefix.index('\nstruct Monitor',a)
prefix=prefix[:a]+r'''
namespace epg {
bool replay=false,enabled=true;
Core syntheticCore;
unsigned payloadCalls=0;
Feed lastFeed=Feed::Accepted;
bool replayActive(){return replay;} bool liveEnabled(){return enabled && !replay;}
void livePadEndpoint(uint32_t,uint32_t){}
void livePayload(const uint8_t* p,size_t n){
 ++payloadCalls;lastFeed=syntheticCore.feed(p,n,clockMs);
 while(syntheticCore.busy())syntheticCore.pump();
}
void livePadPayload(const uint8_t* p,size_t n){livePayload(p,n);}
void livePadData(uint32_t sid,uint32_t cid,const uint8_t* p,size_t n){livePadEndpoint(sid,cid);livePadPayload(p,n);}
}
'''+prefix[b:]
a=prefix.index('struct Chip {');b=prefix.index('} chip;',a)+len('} chip;')
prefix=prefix[:a]+r'''
struct Chip {
 unsigned reads=0,length=0;
 std::vector<uint8_t> wire;
 si468x::Result result=si468x::Result::Ok;
 si468x::Result readCurrentReply(uint8_t* out,uint16_t n){
  ++reads;length=n;assert(n<=wire.size());std::memcpy(out,wire.data(),n);return result;
 }
} chip;
'''+prefix[b:]
# Compile the actual retained-UA ownership query instead of a boolean stub.
a=radio.index('bool DAB::xpadSpiPresent(void) const {');b=radio.index('{',a)+1;depth=1
while depth:
 depth+=(radio[b]=='{')-(radio[b]=='}');b+=1
pad_query=radio[a:b]
a=radio.index('dab_spi::Identity DAB::xpadSpiIdentity(void) const {');b=radio.index('{',a)+1;depth=1
while depth:depth+=(radio[b]=='{')-(radio[b]=='}');b+=1
pad_query=radio[a:b]+'\n'+pad_query
prefix=prefix.replace(' bool ServiceStart=true,dabUaInventoryValid=true;','').replace(' dab_spi::Identity xpadSpiIdentity() const{return xpadConfirmed?dab_spi::Identity::Spi:dab_spi::Identity::NotSpi;}','')
prefix=prefix.replace('bool isFm(){return fm;}','bool isFm() const{return fm;}')
prefix=prefix.replace('bool xpadSpiPresent() const{return xpadConfirmed;}',r'''
 bool ServiceStart=true,dabUaInventoryValid=true;
 struct UaService { uint32_t sid; uint16_t first; uint8_t count; };
 UaService dabUaServices[1]={{0x3201,0,1}};
 uint32_t dabUaComponents[1]={0x01fe0005};
 dab_spi::Identity dabSpiIdentities[1]={dab_spi::Identity::NotSpi};
 uint16_t dabUaComponentCount=1;
 uint8_t dabUaServiceCount=1;
 enum class DabCommand {None,ComponentInfo};DabCommand dabCommand=DabCommand::None;
 dab_spi::Scan dabUaScan;
 dab_spi::Identity xpadSpiIdentity(void) const;
 bool xpadSpiPresent(void) const;
''')
a=radio.index('      if (header.byteCount > 0U)',radio.index('case DabCommand::DsrvHeader:'))
b=radio.index('      dabDsrvPending =',a)
payload=radio[a:b]

# Reuse only the pure, synthetic MOT/TLV builders from the cache tests.
fixture=(ROOT/'tests/test_epg_cache.cpp').read_text(encoding='utf-8')
fixture=fixture[fixture.index('using Data='):fixture.index('static void directory(')]
suffix=r'''
static Data directoryBytes(const std::vector<Object>& objects){
 Data out(13);out[5]=uint8_t(objects.size());
 for(const auto& o:objects){
  append(out,number(o.tid,2));Data h(7);const size_t n=o.body.size();const bool si=o.tid==0x200;
  const unsigned hs=si?7:15;
  h[0]=uint8_t(n>>20);h[1]=uint8_t(n>>12);h[2]=uint8_t(n>>4);h[3]=uint8_t(n<<4);
  h[4]=uint8_t(hs>>1);h[5]=uint8_t((hs&1)*128+14);h[6]=si?0:1;
  if(!si){h.push_back(0xe7);h.push_back(6);append(h,number(o.bearer,6));}append(out,h);
 }
 Data length=number(out.size(),4);for(unsigned i=0;i<4;++i)out[i]=length[i];return out;
}
static std::vector<Data> groups(){
 Object pi=object(0,1,0,3);
 Data service=tlv(0x29,tlv(0x80,number(pi.bearer,6)));
 append(service,tlv(0x11,tlv(1,str("Synthetic EPG One"))));
 Object si{0x200,0,today,tlv(3,tlv(0x26,tlv(0x28,service)))};
 std::vector<Data> out{group(1,6,0,true,directoryBytes({si,pi})),group(si.tid,4,0,true,si.body)};
 for(size_t pos=0,seg=0;pos<pi.body.size();pos+=80,++seg){
  const size_t n=pi.body.size()-pos<80?pi.body.size()-pos:80;
  out.push_back(group(pi.tid,4,unsigned(seg),pos+n==pi.body.size(),Data(pi.body.begin()+pos,pi.body.begin()+pos+n)));
 }
 return out;
}
static dab_probe::Route inject(DAB& radio,const Data& data,unsigned source,uint32_t sid,uint32_t cid,unsigned ua=7,unsigned state=0,unsigned interrupt=0,unsigned dsc=60){
 chip.wire.assign(24,0);chip.wire[0]=128;chip.wire[7]=uint8_t((source<<6)|dsc);chip.wire[6]=uint8_t(state);chip.wire[4]=uint8_t(interrupt);
 const Data service=number(sid,4),component=number(cid,4);
 for(unsigned i=0;i<4;++i){chip.wire[8+i]=service[3-i];chip.wire[12+i]=component[3-i];}
 chip.wire[16]=uint8_t(ua);chip.wire[17]=uint8_t(ua>>8);
 chip.wire[18]=uint8_t(data.size());chip.wire[19]=uint8_t(data.size()>>8);append(chip.wire,data);
 si468x::DsrvHeader header;assert(si468x::Si468x::parseDsrvHeader(chip.wire.data(),24,header)==si468x::Result::Ok);
 assert(header.dataSource==source && header.rawUatypeOrRfu==ua && header.byteCount==data.size());
 const dab_probe::Endpoint audio(radio.dabActiveServiceId,radio.dabActiveComponentId),sls;
 const auto target=dab_probe::route(sid,cid,uint8_t(source),audio,sls,radio.dabSpiProbe.spi);
 const bool legacy=dab_probe::feedLegacyDecoder(target,radio.dabSpiProbe.spi.valid,true);
 ++clockMs;radio.handle(header,target,legacy);return target;
}
static void reset(){epg::syntheticCore.reset();epg::syntheticCore.select(base+1,today+3660,0);epg::payloadCalls=0;epg::lastFeed=epg::Feed::Accepted;}
int main(int argc,char** argv){
 assert(argc==2);
 DAB radio;radio.dabActiveServiceId=0x3201;radio.dabActiveComponentId=0x01fe0005;
 radio.dabSpiProbe.spi=dab_probe::Endpoint(0xe23153fe,0x01f2c00c);radio.dabSpiProbe.state=dab_probe::State::Observing;
 const auto data=groups();assert(data.size()>3);reset();
 std::ofstream fixtureFile(argv[1],std::ios::binary);assert(fixtureFile.good());
 for(const auto& frame:data){const char length[2]={char(frame.size()),char(frame.size()>>8)};fixtureFile.write(length,2);fixtureFile.write(reinterpret_cast<const char*>(frame.data()),frame.size());}
 fixtureFile.close();
 for(size_t i=0;i<data.size();++i)inject(radio,data[i],0,0xe23153fe,i&1?0xc00c:0x01f2c00c);
 auto& c=epg::syntheticCore;
 assert(c.stats.directories==1 && c.stats.si==1 && c.stats.pi==1 && c.count()==3 && c.current()>=0 && c.next()>=0);
 assert(std::strcmp(c.station()->name,"Synthetic EPG One")==0);
 assert(std::strcmp(c.title(0),"station 1 item 0")==0 && chip.length==24+data.back().size());
 std::puts("MSC synthetic DSRV -> route -> exact payload -> MOT/SI/PI/cache/NOW/NEXT: PASS");
 c.browseDay(1);assert(!c.count());c.browseDay(-1);assert(c.count()==3);
 c.select(base+2,c.now(),0);assert(!c.count());c.select(base+1,c.now(),0);assert(c.count()==3);
 const unsigned calls=epg::payloadCalls,reads=chip.reads;
 assert(inject(radio,data[0],0,0xe23153ff,0xc00c)==dab_probe::Route::Ignored);
 assert(inject(radio,data[0],0,0xe23153fe,0xc00d)==dab_probe::Route::Ignored);
 inject(radio,data[0],3,0xe23153fe,0xc00c);
 inject(radio,Data(4072),0,0xe23153fe,0xc00c);
 radio.dabTuneRequestPending=true;inject(radio,data[0],0,0xe23153fe,0xc00c);radio.dabTuneRequestPending=false;
 epg::replay=true;inject(radio,data[0],0,0xe23153fe,0xc00c);epg::replay=false;
 assert(epg::payloadCalls==calls && chip.reads==reads && c.count()==3);
 chip.result=si468x::Result::Timeout;inject(radio,data[0],0,0xe23153fe,0xc00c);chip.result=si468x::Result::Ok;
 assert(epg::payloadCalls==calls && radio.diagDabCommandErrorCount==1);
 si468x::DsrvHeader h;assert(si468x::Si468x::parseDsrvHeader(chip.wire.data(),23,h)==si468x::Result::MalformedReply);
 std::puts("MSC wrong SID/CID, unknown source, oversize, tune/replay guard, timeout, short header: PASS");
 reset();inject(radio,data[0],0,0xe23153fe,0xc00c);inject(radio,data[1],0,0xe23153fe,0xc00c);
 Data bad=data[2];bad.back()^=1;inject(radio,bad,0,0xe23153fe,0xc00c);
 assert(epg::lastFeed==epg::Feed::Rejected && c.stats.crcBad==1 && !c.count());
 for(size_t i=data.size();i>2;--i)inject(radio,data[i-1],0,0xe23153fe,0xc00c);
 assert(c.count()==3 && c.stats.pi==1);
 for(const auto& frame:data)inject(radio,frame,0,0xe23153fe,0xc00c);
 assert(c.count()==3 && c.stats.pi==1);
 std::puts("MSC damaged CRC, reversed segment completion and duplicate carousel: PASS");
 // The same valid content as PAD must decode with confirmed component UA.
 const uint8_t ua[]={7,0,0,0};assert(dab_spi::isSpi(dab_spi::identify(ua,sizeof(ua),1)));
 reset();radio.dabSpiIdentities[0]=dab_spi::Identity::Spi;const unsigned beforeLegacy=radio.legacy;
 for(size_t i=0;i<data.size();++i){
  const auto target=inject(radio,data[i],1,0x3201,i&1?5:0x01fe0005,7);
  assert(target==dab_probe::Route::Audio);
 }
 assert(epg::payloadCalls==data.size() && c.count()==3 && c.stats.si==1 && c.stats.pi==1 && radio.legacy==beforeLegacy);
 assert(c.current()>=0 && c.next()>=0 && std::strcmp(c.station()->name,"Synthetic EPG One")==0);
 std::puts("X-PAD confirmed SPI, full/short audio CID -> MOT/SI/PI/cache/NOW/NEXT: PASS");
 const unsigned padCalls=epg::payloadCalls;
 const unsigned legacyAfter=radio.legacy;
 inject(radio,data[0],1,0x3201,5,2);
 inject(radio,data[0],2,0x3201,5,7);
 assert(radio.legacy==legacyAfter+2 && epg::payloadCalls==padCalls);
 inject(radio,data[0],1,0x3202,5,7);inject(radio,data[0],1,0xe23153fe,0x01f2c00c,7);
 assert(epg::payloadCalls==padCalls && c.count()==3);
 radio.dabSpiIdentities[0]=dab_spi::Identity::NotSpi;inject(radio,data[0],1,0x3201,5,7);
 assert(epg::payloadCalls==padCalls && radio.legacy==legacyAfter+2);
 radio.dabSpiIdentities[0]=dab_spi::Identity::Spi;radio.dabTuneRequestPending=true;inject(radio,data[0],1,0x3201,5,7);radio.dabTuneRequestPending=false;
 assert(epg::payloadCalls==padCalls);
 epg::replay=true;inject(radio,data[0],1,0x3201,5,7);epg::replay=false;
 assert(epg::payloadCalls==padCalls);
 radio.dabUaInventoryValid=false;inject(radio,data[0],1,0x3201,5,7);radio.dabUaInventoryValid=true;
 radio.dabSpiIdentities[0]=dab_spi::Identity::Unqueried;inject(radio,data[0],1,0x3201,5,7);
 radio.dabSpiIdentities[0]=dab_spi::Identity::Spi;
 inject(radio,data[0],1,0x3201,5,7,4); // Received with errors.
 inject(radio,data[0],1,0x3201,5,7,0,4); // Physical error flag.
 inject(radio,data[0],1,0x3201,5,7,0,0,4); // Not MOT DSCTy.
 assert(epg::payloadCalls==padCalls && radio.legacy==legacyAfter+2);
 const unsigned padReads=chip.reads;inject(radio,Data(4072),1,0x3201,5,7);
 assert(chip.reads==padReads && epg::payloadCalls==padCalls);
 std::puts("SLS/DLS/foreign PAD isolation: PASS (legacy hand-off only, not legacy content decoding)");
}
'''
cpp=out/'paths.cpp';cpp.write_text(prefix+pad_query+'\nvoid DAB::handle(const si468x::DsrvHeader& header,dab_probe::Route target,bool feedLegacy){\n'+payload+'}\n'+fixture+suffix,encoding='utf-8')
compiler=sys.argv[1] if len(sys.argv)>1 else 'C:/msys64/ucrt64/bin/g++.exe'
exe=out/'paths.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(cpp),str(ROOT/'src/epg_core.cpp'),'-o',str(exe)],check=True)
r=subprocess.run([str(exe),str(out/'groups.bin')],capture_output=True,text=True)
print(r.stdout+r.stderr,end='');r.check_returncode()
report={'test_kind':'host synthetic at Si4684 DSRV output boundary','uses_raw_capture':False,'serial_port_opened':False,'dsrv_parser':'actual vendored Si468x.h','routing':'actual route and production payload admission','decoder':'actual EPG core','msc_epg':'PASS','xpad_signalling_route':'PASS','xpad_epg_decode':'PASS','xpad_legacy_content_decode':'NOT_TESTED, only hand-off recorded','rf_or_chip_xpad_extraction':'NOT_TESTED','output':r.stdout}
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Report:',out/'results.json')
