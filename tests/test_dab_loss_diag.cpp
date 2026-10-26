#include "../src/dab_loss_diag_output.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <cstring>
struct Output {
 int room=4096;std::string text;
 int availableForWrite(){return room;}
 size_t write(const uint8_t* p,size_t n){text.append(reinterpret_cast<const char*>(p),n);return n;}
 void printf(const char* fmt,...){char b[1200];va_list a;va_start(a,fmt);int n=vsnprintf(b,sizeof b,fmt,a);va_end(a);assert(n>=0 && n<int(sizeof b));text+=b;}
};
int main(){
 using namespace dab_loss;
 static_assert(sizeof(Evidence)<4096,"Diagnostic RAM budget");
 Evidence e;e.started(3);e.cts(false);e.terminal(3,0);
 e.action(3,Busy);e.action(3,StartFail);
 e.started(5);e.cts(true);e.terminal(5,-6);
 assert(e.window.cmd[3][Start]==1 && e.window.cmd[3][Ok]==1 && e.window.cmd[3][Poll]==1);
 assert(e.window.cmd[5][Irq]==1 && e.window.cmd[5][Device]==1 && !e.active);
 e.total.event[Stored]=e.window.event[Stored]=UINT32_MAX;e.event(Stored);
 assert(e.total.event[Stored]==UINT32_MAX && e.window.event[Stored]==UINT32_MAX);
 e.total.cmd[3][Start]=e.window.cmd[3][Start]=UINT32_MAX;e.started(3);
 assert(e.window.cmd[3][Start]==UINT32_MAX);
 e.pending(UINT32_MAX-9);e.drainStart(10,false);assert(e.window.waitMaxMs==20);
 for(unsigned n:{0U,1U,2U,255U})e.header(1,1,12,n,false,true);
 for(unsigned n:e.window.remaining)assert(n==1);
 assert(e.window.queueMax==255);
 uint8_t mask[32];memset(mask,255,sizeof mask);mask[2]&=~1;
 e.drainStart(UINT32_MAX-9,false);e.begin(UINT32_MAX-9);e.event(Overflow);e.event(ReadFail);e.event(CrcBad);
 e.close(10,77,mask,33,true,33,16848,17360,UINT32_MAX-4,"NEW_HEADER");
 assert(e.lossCount==1 && e.losses[0].expected==34 && e.losses[0].missing==1 && e.losses[0].firstMissing==16);
 assert(e.losses[0].ageMs==20 && e.losses[0].idleMs==15 && e.losses[0].readFail==1 && e.losses[0].crcBad==1);
 e.close(11,78,mask,32,false,0,16336,17360,10,"TIMEOUT");
 assert(e.losses[1].expected==33 && !e.losses[1].last && e.losses[1].missing==1);
 e.close(12,79,mask,32,false,0,16336,17360,10,"OTHER");assert(e.total.event[LossLogDrop]==1);
 // All 256 bits are represented. Without LAST, unobserved tail is uncertain.
 e=Evidence{};memset(mask,0,sizeof mask);e.close(1,1,mask,255,true,255,1,100,0,"OTHER");
 assert(e.losses[0].expected==256 && e.losses[0].missing==256 && e.losses[0].mask[31]==255);
 e=Evidence{};uint8_t payload[]={1,2,3};e.started(3);e.payload(1,2,3,payload,3);e.payload(1,2,3,payload,3);
 assert(!e.total.event[Identical]);e.started(3);e.payload(1,2,3,payload,3);assert(e.total.event[Identical]==1);
 e.started(3);e.payload(1,2,4,payload,3);assert(e.total.event[Identical]==1);
 auto& d=evidence();d=Evidence{};Output out;d.started(3);d.cts(false);d.terminal(3,0);
 service(out,30000,false,4,0,14000,13000,2000);assert(out.text.empty() && d.stage==0 && d.report.cmd[3][Start]==1);
 d.started(5);d.cts(true);d.terminal(5,0);
 out.room=0;service(out,30100,true,4,0,14000,13000,2000);assert(out.text.empty());
 out.room=4096;for(unsigned t=30200;t<38000;t+=100)service(out,t,true,4,0,14000,13000,2000);
 assert(out.text.find("ctsPoll=1 ctsIrq=0 cmdDsrvStart=1 cmdDsrvOk=1 dsrvCts=1 nonDsrv=0")!=std::string::npos);
 assert(out.text.find("op=signal delta=0,0,0,0,0,0,0,0,0,0 total=1,1")!=std::string::npos);
 assert(d.window.cmd[5][Start]==1 && d.stage==255);
 d=Evidence{};d.experiment=true;d.mode=Mode::Quiet;memset(mask,255,sizeof mask);mask[2]&=~1;
 d.close(1,15780,mask,33,true,33,16848,17360,0,"NEW_HEADER");
 out.text.clear();service(out,100,false,4,0,14000,13000,2000);
 assert(out.text.find("tid=15780 reason=NEW_HEADER total=34 received=33 missing=1 firstMissing=16")!=std::string::npos);
 assert(out.text.find("missingMask=0000010000")!=std::string::npos);
 std::printf("Diagnostic saturation, interval snapshot/CTS reconciliation, queue histogram, bitmap/LAST uncertainty, wrap, identity transactions and paced quiet output: PASS (%u bytes)\n",unsigned(sizeof(Evidence)));
}
