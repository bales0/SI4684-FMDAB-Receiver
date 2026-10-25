"""Actual EPG renderer/navigation and physical button handlers on a recording TFT."""
from pathlib import Path
import re,struct,subprocess,sys,json
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'.pio/epg_ui_test';out.mkdir(parents=True,exist_ok=True)
source=(ROOT/'src/epg_ui.cpp').read_text(encoding='utf-8')
source='\n'.join(l for l in source.splitlines() if not l.startswith('#include'))
ino=(ROOT/'SI4684-DAB-Receiver.ino').read_text(encoding='utf-8')
gui=(ROOT/'src/gui.cpp').read_text(encoding='utf-8')
assets='\n'.join(line for line in gui.splitlines() if line.startswith('const uint16_t* EpgBackground(') or line.startswith('const uint16_t* EpgSelector('))
def function(signature):
 start=ino.index(signature+' {');pos=ino.index('{',start)+1;depth=1
 while depth:
  depth+=(ino[pos]=='{')-(ino[pos]=='}');pos+=1
 return ino[start:pos]
# Use actual font character metadata without retaining bitmap pixels in the mock.
font=(ROOT/'src/font.h').read_text(encoding='utf-8').split('const uint8_t FONT16[] PROGMEM = {',1)[1].split('};',1)[0]
b=bytes(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})',font));count=struct.unpack_from('>I',b)[0]
meta=b[:24+count*28]
translations=(ROOT/'src/language.h').read_text(encoding='utf-8').split('// EPG UI strings follow',1)[1].rsplit('#endif',1)[0]
translations='// EPG UI strings follow'+translations
messages=re.findall(r'"([^"\n]*)"',translations)
assert len(messages)==8*15
glyphs={struct.unpack_from('>7i',b,24+i*28)[0] for i in range(count)}
missing={ch for message in messages for ch in message if ord(ch)>=127 and ord(ch) not in glyphs}
assert not missing, repr(missing)
heading_font=(ROOT/'src/font.h').read_text(encoding='utf-8').split('const uint8_t FONT28[] PROGMEM = {',1)[1].split('};',1)[0]
heading_bytes=bytes(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})',heading_font))
heading_glyphs={32}|{struct.unpack_from('>7i',heading_bytes,24+i*28)[0] for i in range(struct.unpack_from('>I',heading_bytes)[0])}
assert all(ord(ch) in heading_glyphs for lang in range(8) for value in messages[lang*15:lang*15+3] for ch in value)

assert all(ord(ch) in {struct.unpack_from('>7i',b,24+i*28)[0] for i in range(count)} for ch in 'ąćęłńóśźżĄĆĘŁŃÓŚŹŻ')
# Keep an actual decoded Jedynka description, including its broadcast newline.
programme_rows=[line.split('\t') for line in (ROOT/'.pio/epg_test/programmes.tsv').read_text().splitlines() if line.startswith('P\tF629\t')]
jedynka=next(bytes.fromhex(row[10]).decode() for row in programme_rows if row[6]=='1056061')
assert '\n06:40:00 Po pierwsze  ekonomia' in jedynka and 'od godz. 6.00' in jedynka
fixture='const char* jedynkaDescription='+json.dumps(jedynka,ensure_ascii=False)+';\n'
prefix=r'''
#include "epg_ui.h"
#include "epg_runtime.h"
#include "fm_features.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
static std::string textLog;
struct TextDraw { std::string value; int y; };
static std::vector<TextDraw> drawnText;
static unsigned backgrounds=0, rows=0, scans=0, mainRedraws=0, volumePaints=0, legacyCycles=0, panels=0;
static uint32_t testMillis=100;
uint32_t millis(){return testMillis;}
#define pgm_read_byte(p) (*static_cast<const uint8_t*>(p))
#define TL_DATUM 0
#define PROGMEM
unsigned char language=0;
struct MockTft {
 bool fontLoaded=true, fillBackground=false;
 unsigned fontLoads=0, fontUnloads=0;
 struct Vertical { int x,y,h,color; };
 struct Text { std::string value; int x,y; };
 std::vector<Vertical> verticals;
 std::vector<Text> texts;
 void pushImage(int x,int y,int w,int h,const uint16_t*) { if(x==0 && y==0 && w==320 && h==240) { ++backgrounds; textLog.clear(); } }
 void loadFont(const uint8_t*){fontLoaded=true;++fontLoads;}
 void unloadFont(){fontLoaded=false;++fontUnloads;}
 void setTextDatum(int){} void setTextColor(int,int,bool fill=true){fillBackground=fill;}
 int textWidth(const char* p){int n=0; for(;*p;++p) if((uint8_t(*p)&192)!=128) n+=8; return n;}
 void drawString(const char* p,int x,int y){textLog+=p; textLog+='|';texts.push_back({p,x,y});}
 void pushSprite(int x,int y){assert(x>=0 && y>=0 && x+308<=320 && y+20<=240); ++rows;}
 void drawFastVLine(int x,int y,int h,int color){verticals.push_back({x,y,h,color});}
 void drawRoundRect(int,int,int,int,int,int){}
 void drawLine(int,int,int,int,int){}
 void drawRect(int x,int y,int w,int h,int){assert(x>=0 && y>=0 && x+w<=320 && y+h<=240);}
 void fillRect(int x,int y,int w,int h,int){assert(x>=0 && y>=0 && x+w<=320 && y+h<=240);}
} tft,FullLineSprite;
void tftPrintFixed(int,const char* text,int x,int y,int,int,unsigned){assert(x>=0 && x<320 && y>=0 && y<240);textLog+=text;textLog+='|';if(*text)drawnText.push_back({text,y});}
bool menu=false,tuning=false,seek=false,setvolume=false,memorystore=false;
bool ChannelListView=false,DabGlobalListView=false,SlideShowView=false,ShowServiceInformation=false;
bool slsWaitingView=false,slsWaitingOverlayVisible=false;
int rotary=0,rotary2=0,memoryposstatus=0;
const int MEM_NORMAL=0;
uint32_t tottimer=0;
int PrimaryColor=1,PrimaryColorSmooth=2,SecondaryColor=3,SecondaryColorSmooth=4,ActiveColor=5,BackgroundColor3=6;
char _serviceName[17]="radio";
struct Radio { bool isFm(){return false;} } radio;
namespace epg {
static Core instance;
Core* runtime(){return &instance;} bool replayActive(){return true;} bool ensure(bool){return true;}
}
void DrawNeutralStatusPanel(){++panels;}
const uint16_t configurationbackground[320*240]={};
const uint16_t selector[304*20]={};
bool DrawEpgLogo(epg::Bytes data,size_t size,int,int,int,int){assert(data && size>8 && data[0]==137);return true;}
void BuildDisplay(){++mainRedraws;EpgResume();if(EpgVisible())EpgDraw(true);}
void ShowVolume(){++volumePaints;} void BuildChannelList(){} void ShowServiceInfo(){}
void SlideShowButtonPress(){if(EpgVisible())EpgBack();}
#define MODEBUTTON 1
#define ROTARY_BUTTON 2
#define SLBUTTON 3
#define LOW 0
int pins[4]={1,1,1,1}; int digitalRead(unsigned pin){return pins[pin];}
enum class FmScanState { Idle }; enum class DabFullScanState { Idle };
FmScanState fmScanState=FmScanState::Idle; DabFullScanState dabFullScanState=DabFullScanState::Idle;
bool dabSeekStarted=false,fmSeekStarted=false;
const unsigned RADIO_MODE_FM=1; unsigned radioMode=0;
void CancelFmFullScan(){} void CancelDabFullScan(){} void CancelDabAuto(){}
void CaptureSettingsSnapshot(){} void BuildMenu(){EpgSuspend();}
void ExitSettingsMenu(){menu=false;BuildDisplay();}
void CycleTuneMode(){if(EpgVisible())EpgBack();else ++legacyCycles;}
void StartFmFullScan(){++scans;} void StartDabFullScan(){++scans;}
void ButtonPress(){if(EpgVisible()&&!menu)EpgClick();}
'''
font_code=translations+'\n'+'static const uint8_t fontMeta[]={'+','.join(map(str,meta))+'};\nconst uint8_t* EpgFont16(){return fontMeta;}\n'
actual_panel=function('void DrawEpgWaitingPanel(const char* text)')
actual_handlers='\n'.join(function(s) for s in ['void ModeButtonPress(void)','void Rotary1ButtonProcess(void)','void closeVolume(void)','void Button2Press(void)'])
suffix=r'''
int main(int argc,char** argv){
 // Actual multiline layout must not send control glyphs to the TFT or lose
 // either the listener advisory time or the schedule lines in the source.
 char safe[640];fallback=false;
 displayText("a\nb\r\nc\rd\te",safe,sizeof(safe),true);
 assert(std::strcmp(safe,"a\nb\nc\nd e")==0 && !fallback);
 displayText("a\nb\r\nc\rd\te",safe,sizeof(safe));
 assert(std::strcmp(safe,"a b c d e")==0 && !fallback);
 drawnText.clear();assert(wrapped("Jedynka\n\n06:40:00 Program",0,6,80,PrimaryColor)==3);
 assert(drawnText.size()==2 && drawnText[0].value=="Jedynka" && drawnText[0].y==80 && drawnText[1].y==120);
 drawnText.clear();fallback=false;const unsigned total=wrapped(jedynkaDescription,0,8,0,PrimaryColor);
 assert(total>1 && !fallback);
 bool advisory=false,schedule=false;
 for(const auto& row:drawnText){
   assert(row.value.find('?')==std::string::npos && row.value.find('\n')==std::string::npos && row.value.find('\r')==std::string::npos);
   advisory|=row.value.find("6.00")!=std::string::npos;
   schedule|=row.value=="06:40:00 Po pierwsze  ekonomia";
 }
 assert(advisory && schedule);
 const auto originalRows=drawnText;
 drawnText.clear();assert(wrapped(jedynkaDescription,1,2,100,PrimaryColor)==total);
 assert(drawnText.size()==2 && drawnText[0].value==originalRows[1].value && drawnText[0].y==100 && drawnText[1].value==originalRows[2].value && drawnText[1].y==120);
 drawnText.clear();wrapped("Pytanie? godz. 6.00\n07:15:00 Rozmowa dnia",0,6,0,PrimaryColor);
 assert(drawnText.size()==2 && drawnText[0].value=="Pytanie? godz. 6.00" && drawnText[1].value=="07:15:00 Rozmowa dnia");
 fallback=false;displayText("\xF0\x9F\x98\x80\n",safe,sizeof(safe),true);
 assert(fallback && std::strcmp(safe,"?\n")==0); // Unsupported glyphs still get a real fallback.
 char small[4];displayText("\xC4\x85\n\xC4\x99",small,sizeof(small),true);
 assert(std::strcmp(small,"\xC4\x85\n")==0); // UTF-8 truncation remains complete.
 assert(argc==2);std::ifstream f(argv[1],std::ios::binary);assert(f.good());
 auto* c=epg::runtime();c->select(0x40e231533233ULL,epg::epoch(2024,11,12,3,30),60);
 while(true){uint8_t h[10];f.read(reinterpret_cast<char*>(h),10);if(!f.gcount())break;assert(f.gcount()==10);
  unsigned n=h[8]|(unsigned(h[9])<<8);std::vector<uint8_t> b(n);f.read(reinterpret_cast<char*>(b.data()),n);
  c->feed(b.data(),n,testMillis++);while(c->busy())c->pump();}
 assert(c->count() && c->current()>=0); EpgOpen();
 assert(EpgVisible() && textLog.find("EPG NOW / NEXT")!=std::string::npos);
 unsigned previous=backgrounds;EpgDraw();assert(backgrounds==previous);
 assert(EpgBackground(true)==configurationbackground && EpgBackground(false)==configurationbackground);
 EpgClick();testMillis+=200;EpgDraw();assert(rows==9 && textLog.find("EPG PROGRAM")!=std::string::npos);
 assert(FullLineSprite.fontLoaded && !FullLineSprite.fillBackground);
 assert(tft.verticals.empty() && FullLineSprite.verticals.empty() && FullLineSprite.texts.size()==18);
 for(unsigned i=0;i<9;++i){
   const auto& clock=FullLineSprite.texts[2*i];const auto& title=FullLineSprite.texts[2*i+1];
   assert(clock.value.size()==5 && clock.value[2]==':' && clock.x==8 && clock.y==3);
   assert(clock.x+FullLineSprite.textWidth(clock.value.c_str())<=48);
   assert(title.x==64 && title.y==3 && title.x+FullLineSprite.textWidth(title.value.c_str())<=296);
 }
 // Actual navigation on the same page transfers only two complete row sprites.
 unsigned oldRows=rows;previous=backgrounds;
 EpgMove(-1);testMillis+=200;EpgDraw();
 assert(rows==oldRows+2 && backgrounds==previous);
 assert(tft.verticals.empty() && FullLineSprite.verticals.empty());
 oldRows=rows;EpgMove(0);testMillis+=200;EpgDraw();assert(rows==oldRows && backgrounds==previous);
 // Move to an adjacent group of nine: replace all rows, never the background.
 EpgMove(9);testMillis+=200;EpgDraw();assert(rows==oldRows+9 && backgrounds==previous);
 oldRows=rows;++c->revision;testMillis+=200;EpgDraw();assert(rows==oldRows+9 && backgrounds==previous);
 EpgClick();testMillis+=200;EpgDraw();assert(textLog.find("EPG DETAIL")!=std::string::npos);
 for(unsigned i=0;i<100;++i) { EpgMove(1); }
 testMillis+=200;EpgDraw(); // bounded text scrolling
 Button2Press();assert(setvolume && volumePaints==1);previous=backgrounds;
 EpgInvalidate();EpgDraw(true);assert(backgrounds==previous);Button2Press();assert(!setvolume && EpgVisible() && backgrounds>previous);
 // Actual rotary press tracker: a long press inside EPG must not start scan.
 pins[ROTARY_BUTTON]=LOW;Rotary1ButtonProcess();testMillis+=1001;Rotary1ButtonProcess();pins[ROTARY_BUTTON]=1;Rotary1ButtonProcess();assert(scans==0);
 // MODE long keeps the page and enters settings, release cannot cycle tune mode.
 pins[MODEBUTTON]=LOW;ModeButtonPress();testMillis+=1001;ModeButtonPress();assert(menu&&!EpgVisible());
 pins[MODEBUTTON]=1;ModeButtonPress();ExitSettingsMenu();assert(EpgVisible() && textLog.find("EPG DETAIL")!=std::string::npos && legacyCycles==0);
 // MODE short goes back one level and never tunes.
 pins[MODEBUTTON]=LOW;ModeButtonPress();testMillis+=50;pins[MODEBUTTON]=1;ModeButtonPress();testMillis+=200;EpgDraw();assert(textLog.find("EPG PROGRAM")!=std::string::npos && legacyCycles==0);
 const uint32_t now=c->now();EpgMove(1,true);assert(c->dayOffset()==1 && c->now()==now);
 previous=backgrounds;testMillis+=200;EpgDraw();
 assert(backgrounds==previous && textLog.find(epgText[language][EPG_WAIT_PROGRAM])!=std::string::npos);
 // Return to the loaded day without feeding any data: remove the waiting
 // panel immediately and display cached rows. The detector state is irrelevant.
 const uint32_t beforeFrames=c->stats.frames;
 unsigned beforePanels=panels;oldRows=rows;
 EpgMove(-1,true);testMillis+=200;EpgDraw();
 assert(c->count()==68 && c->stats.frames==beforeFrames && panels==beforePanels && rows==oldRows+9 && backgrounds==previous);
 const uint64_t loadedStation=c->selected();
 c->select(0x40e2315337fcULL,c->now(),c->offset());testMillis+=200;EpgDraw();
 assert(c->count()==0 && panels==beforePanels+1 && !c->logo());
 beforePanels=panels;oldRows=rows;
 c->select(loadedStation,c->now(),c->offset());testMillis+=200;EpgDraw();
 assert(c->count()==68 && c->stats.frames==beforeFrames && panels==beforePanels && rows==oldRows+9);
 EpgBack();assert(c->dayOffset()==0);EpgBack();assert(!EpgVisible() && mainRedraws>0);
 // The shared main-screen sprite must retain its startup FONT16, including
 // across EPG pages, settings suspension and the final return to radio text.
 assert(FullLineSprite.fontLoaded && FullLineSprite.fontLoads==0 && FullLineSprite.fontUnloads==0);
 FullLineSprite.setTextColor(PrimaryColor,PrimaryColorSmooth,false);
 FullLineSprite.drawString("Station infotext",0,0);
 assert(FullLineSprite.fontLoaded && !FullLineSprite.fillBackground);
 // Every configured language is rendered from the production table. Empty
 // data uses the same neutral panel as slideshow waiting, with an EPG symbol.
 c->reset();
 for(language=0;language<8;++language) {
   EpgOpen();assert(panels>0);
   assert(textLog.find(epgText[language][EPG_OVERVIEW])!=std::string::npos);
   assert(textLog.find(epgText[language][EPG_WAIT_PROGRAM])!=std::string::npos);
   EpgClick();testMillis+=200;EpgDraw();
   assert(textLog.find(epgText[language][EPG_PROGRAM])!=std::string::npos);
   const unsigned oldPanels=panels, oldBackgrounds=backgrounds;
   EpgMove(1);testMillis+=200;EpgDraw();
   assert(panels==oldPanels && backgrounds==oldBackgrounds);
   EpgBack();EpgBack();
 }
 // Data arriving while the empty list is visible replaces the waiting panel
 // with the rows without erasing the full screen or reopening the view.
 language=0;EpgOpen();EpgClick();testMillis+=200;EpgDraw();
 previous=backgrounds;const unsigned beforeRows=rows, dataPanels=panels;
 c->select(0x40e231533233ULL,epg::epoch(2024,11,12,3,30),60);
 f.clear();f.seekg(0);
 while(true){uint8_t h[10];f.read(reinterpret_cast<char*>(h),10);if(!f.gcount())break;assert(f.gcount()==10);
   unsigned n=h[8]|(unsigned(h[9])<<8);std::vector<uint8_t> b(n);f.read(reinterpret_cast<char*>(b.data()),n);
   c->feed(b.data(),n,testMillis++);while(c->busy())c->pump();}
 testMillis+=200;EpgDraw();assert(c->count() && backgrounds==previous && rows==beforeRows+9 && panels==dataPanels);
 EpgBack();EpgBack();
 std::puts("Actual EPG UI: two-column background/text geometry, Jedynka multiline/advisory times/scroll, partial list redraw, waiting/data transition, 8 languages, UTF8 fonts, volume/day/navigation: PASS");
}
'''
cpp=out/'ui.cpp';cpp.write_text(prefix+assets+'\n'+font_code+fixture+actual_panel+'\n'+source+'\n'+actual_handlers+suffix,encoding='utf-8')
exe=out/'ui.exe'
subprocess.run([(sys.argv[1] if len(sys.argv)>1 else 'g++'),'-std=c++11','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'src'),str(cpp),'src/epg_core.cpp','-o',str(exe)],cwd=ROOT,check=True)
subprocess.run([str(exe),str(ROOT/'.pio/epg_test/frames.bin')],check=True)
# Production wire/control invariants complement the executable recording UI.
radio=(ROOT/'src/si4684.cpp').read_text(encoding='utf-8')
assert 'header.dataSource == 0U && header.byteCount <= sizeof(SPIbuffer) - 25U' in radio
assert 'SPIbuffer + 25, header.byteCount' in radio
assert radio.count('si468x::writeLe32(args + 7, dabSpiProbe.spi.cid);')==2
assert 'epg::replayActive()' in radio and 'epgContinuous && sameProbeContext' in radio
assert 'radio.Update(!tuning);' in ino and ino.index('radio.Update(!tuning);')<ino.index('if (EpgVisible() && !menu)')
assert 'slTracker.update(digitalRead(SLBUTTON) == LOW, millis(), 1000UL)' in ino
assert 'digitalRead(ROTARY_BUTTON) == LOW && digitalRead(SLBUTTON) == LOW' in ino # boot combo unchanged
print('EPG transport/generation/legacy ownership and boot/SL wiring: PASS')
