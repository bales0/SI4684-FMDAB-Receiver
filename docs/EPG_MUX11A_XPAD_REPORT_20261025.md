# MUX 11A: packet Directory a EPG přes X-PAD – kontrola a opravy

Lokální kontrola a schválené doplnění, 25. 10. 2026. Hostitelské testy A–F
prošly, stejně jako celá EPG sada a build `esp32dev`. Technicky je souběh
prázdného packet carouselu a plného SPI přes audio X-PAD možný. **Skutečné
X-PAD SPI v MUX 11A zůstává NEOVĚŘENO.** Nový firmware nebyl nahrán;
nové RF/audio/SLS/stack ověření: **HW NOT TESTED**.

## Rozsah a původní stav

Výchozí HEAD: `116758e5bd05927f70f6884a332a845e0e60369b`. Před zahájením již byly
lokálně přítomné schválené UTC/LTO, důvěryhodnost hodin, SI-only, archiv a UI
opravy; zůstaly zachované. NVS, Git index, commity ani GitHub se neměnily.
Externě odstraněné `src/IRReceiver.cpp/.h` nejsou součástí tohoto zásahu ani patch.
Původní časový report a patch v docs zůstávají historickými artefakty.

Původní zdroj už obsahoval DsrvHeader, UA inventuru, packet probe a PAD cestu
směřující do jediného Core. Samotné oddělené mock testy však neověřovaly společně
skutečnou publikaci UA, plnou hlavičkovou větev, převzetí vlastníka a cache.
Samostatná diagnostika obou transportů chyběla. Cílené reprodukce před opravou
selhaly na těchto podmínkách:

- `ServiceStart=false` nesmí odvolat stále potvrzené audio SPI UA.
- Potvrzená audio SPI nesmí zakrýt samostatný packet endpoint.
- Timeout prvního PAD payload read nesmí vymazat platný packet program.
- CRC-validní, ale vadná první PAD MOT obálka nesmí převzít packet cache.
- Packet DSRV serviceState=4 nesmí publikovat prázdný Directory přes platné PI.

## Skutečný zachycený 11A vstup

Nezávislá kontrola `EPG_DSRV_dumps_and_files.zip/log_11A.txt` našla **422 úplných
CRC-validních** `[SPI/RAW]` skupin, chybné CRC=0. Všechny mají packet SID
E231B3FE/CID C00B, type6, TID 8CC2, segment0 a stejné prázdné Directory.
**27 B je délka přenosové skupiny, 16 B délka vlastního Directory**, počet
objektů je 0. Tělo: `0000001000000000dc00000003004101`.

Log má také 44 PAD route řádků. Pět omezeně vypisovaných UA vzorků obsahuje
2× packet source0/raw7 a 3× audio source1/raw2 (Slideshow, SID3802/CID000B).
Žádný zachycený source1/raw7 tu není; rate limiting však neumožňuje dokázat
nepřítomnost X-PAD SPI. Z prázdného packet toku nelze odvodit nepřítomnost EPG
v celém multiplexu. Přesná první 27B skupina, její 16B tělo a sanitizovaný
souhrn s hashi jsou v [fixtures](../tests/fixtures/epg/README.md).
Starší 27B syntetické tělo pro časové testy je od této fixture výslovně odlišeno.

Normové transporty potvrzuje [ETSI TS 102 371 V3.3.1](https://www.etsi.org/deliver/etsi_ts/102300_102399/102371/03.03.01_60/ts_102371v030301p.pdf),
§4.1 a §6.1: SPI lze přenášet v packet i PAD a oba mohou používat Directory mode.
[AN649](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/an649.pdf)
popisuje DATA_SRC a UA v DSRV: raw7 na source0 není důkaz X-PAD SPI; source2
zůstává DLS. Nastavení XPAD 0x0097 se touto opravou nemění.

## Co se změnilo

`dab_spi_discovery.h` umí při skutečné packet volbě vybírat pouze packet MOT
komponenty a současně evidovat ostatní potvrzené SPI reklamy. PAD-only bez
packet komponenty je legitimní stav `confirmed/PAD; no packet`.

`si4684.cpp` odděluje surovou identitu potvrzené aktuální UA od okamžité
použitelnosti audia. PAD se odvolá až po platné dokončené negativní UA,
změně služby nebo kontextu. Neaktivní audio, neúplný scan ani chybná odpověď
samy potvrzení nemažou. První PAD převezme Core teprve po úspěšném přesném
read a validaci obálky; nezávislá platná UA publikace může vlastníka zvolit dřív.
Packet i PAD kontrolují service state a physical error před předáním Core.

Sdílený `inspectGroup` v `epg_core.cpp` vyčleňuje dosavadní read-only kontrolu
CRC/MOT obálky. Používá ho Core, první PAD handoff a diagnostika; nejde o druhý
PI/SI parser. Zůstává jediný Core, stejné reassembly buffery, limity i časové
opravy. Skupiny nevlastněné větve do Core nevstupují. Ani shodný TID mezi oběma
toky proto nemíchá data nebo nemaže PAD cache.

Nový `epg_transport_diag.h` přidává omezené saturující čítače obou transportů,
validní skupiny, ověřené Directory, SI/PI/PNG, odmítnutí a skutečné důvody resetů.
`EPG TEST TRANSPORT` vypisuje identity, RADIO, OWNER, obě větve a HEALTH
postupně s UART backpressure. Neprovádí nové dotazy čipu a nepotřebuje Core
alokovat. Podrobnosti jsou v [Serial_protocol.md](../Serial_protocol.md).

Čítače se počítají i bez DEBUG; nový rozsáhlejší výpis vzniká jen na požádání.
SI_ONLY a EMPTY systémové texty všech osmi jazyků popisují vybraný carousel,
aby netvrdily nepřítomnost programu v celém MUX. Stavy, barvy EPG a časová
politika z předchozí opravy zůstávají zachované.

## Integrované testy A–F

`tests/test_epg_coexistence.py` kompiluje skutečný celý case DsrvHeader,
skutečné metody dotazu/publikace UA a discovery spolu s runtime/Core a vendor
parserem. Mockuje SPI transakce, UART, hodiny, heap a kreslení. Menu používá
skutečné EpgSuspend/EpgResume, kreslení je stub. Legacy SLS/DLS se kontroluje
na předání původní cestě; jejich obsah se v této sadě nedekóduje.

| Scénář | Výsledek |
|---|---|
| A: prázdný packet před/během/po plném PAD; shodný TID8CC2; menu | 16 packet empty, 15 blokovaných pro jiného vlastníka, PAD SI1/PI1, 3 pořady; právě 1 očekávaný handoff reset. |
| B: pouze packet empty, PAD UA neověřená | 4 empty, žádná PAD Directory, 0 pořadů; stav Empty pouze pro packet, 0 resetů během scénáře. |
| B2/E: packet program + SLS/DLS, vadný packet state, první PAD read/CRC/obálka | Cache 3 pořadů zachována, 0 resetů. |
| B3: plný PAD bez packet komponenty | PAD-only, 3 pořady; 1 očekávané PAD-acquire. |
| C: PAD SI + vlastní Adam7 PNG, bez PI; packet empty souběžně | SI1/PNG1/PI0, SI_ONLY, packet empty8 nemění Core, 0 resetů během scénáře. |
| D: částečný packet → PAD, změna pořadí, UA unknown/failed/duplicate, pozdní packet callback, potvrzená negativní UA | Částečná data se nemíchají; cache přežije přechodné stavy, negativní UA uvolní PAD; 2 očekávané resety. |
| D: změna služby/SID/CID, frekvence a FM | Staré skupiny odmítnuté, izolace nového kontextu; 4 očekávané resety. |
| E: SID/CID/generation, dscty/state/physical/length/inactive/read/CRC/source2 | Žádný chybný legacy fallback ani poškození cache; ztráta důvěry hodin změní Ready bez resetu, nová generace ho obnoví. |
| F: signalled PAD bez dat, packet empty opakovaně | Packet empty11, blocked10; PAD WAIT_DIR i po 300s, nikoli falešné Empty. |
| F: TEST/STOP a diagnostika | LIVE izolováno od TEST, správné resety, UART 0/511 čeká, 512 vypisuje; END/cancel, mezní délky řádků a saturace ověřené. |

Všech 10 dílčích scénářů prošlo, **neočekávané resety=0**. `resets` v JSON
je rozdíl za scénář, zatímco pole resetReasons obsahuje kumulativní totals;
počáteční alokace/binding je proto vidět v totals, nikoli v rozdílu.
Syntetický snapshot v JSON používá produkční LIVE větev s mock hodinami;
není to skutečný příjem 11A.

Celá hostitelská sada: **25 kroků PASS** (včetně kompilací), CRC/cache/UART,
discovery/inventory, time/clock, runtime/UI, RAW/oracle, syntetické DSRV,
PC protokol, storage a PNG/Adam7. Skutečný RAW zachovává 70 objektů a
1 087 pořadů. Nejde o opakování nového testu na desce.

Reprodukce z kořene projektu:

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tests/run_epg_tests.py --input tests/fixtures/epg/device-monitor-261023-160924.log --reference tests/fixtures/epg/SI4684_MUX11B_EPG_kompletni_dekodovani.zip
python tests/epg/synthetic_logo_test.py --verify-fixtures
pio run -e esp32dev
```

Strojové výsledky v [JSON](EPG_MUX11A_XPAD_results_20261025.json) obsahují
jednotlivé scénáře, čítače, syntetický snapshot, capture analýzu a firmware hash.

## Paměť, build a limity důkazu

ESP32 target: registry **516 B** (ověřeno target compiler/nm), Core **14 232 B**,
pevné buffery **36 864 B**, celkem Core **51 096 B**, beze změny proti časové
opravě. Žádný druhý Core, MOT buffer ani framebuffer. Zachovány limity
96 pořadů a 8 192 B textu; aktuální profil má Directory 4 096 B a objekty
12 288 B, tedy nepodporuje automaticky všechna normová maxima 8K/16K.

Host heap mock navíc rezervuje 1 024 B DRAM pro konzervativní tlak nové
statické diagnostiky. Při zátěži zůstává free8/largest8=12 808 B,
free32/largest32=17 416 B, žádná další Core alokace. Tato čísla **nejsou**
měřením skutečné desky. UART CTS/overflow/audio/SLS ze syntetického snapshotu
jsou rovněž mock hodnoty; nelze z nich vyvodit nepřerušované audio.

Finální build `esp32dev`: RAM **67 128 / 327 680 B**, flash
**3 551 797 / 4 128 768 B**, firmware.bin **3 552 416 B**.
SHA-256: `0c51a051c3db86ceeadfbfe41ca09224c4fe3d2166c507883e2b80ab38fb93a2`.
Build vytvořil obrazy, žádný se neflashoval; NVS nebylo měněno.

Diagnostika nepřidává hardware reads; znovu kontroluje omezené CRC/obálky,
takže její CPU náklad a dopad na FIFO/audio je nutné ještě změřit na RF.
Nemá druhé úložiště pro nevlastněný tok: u něj lze přímo ověřit úplné prázdné
jednosegmentové Directory nebo známé opakování, nikoli libovolný nový
fragmentovaný/nenulový Directory. Čtyři fingerprinty mají příznak historyLimited.
Signalled PAD bez Directory má diagnostický timeout; automatický návrat na
packet tímto timeoutem se nezavádí.

Průběžné volání `availableForWrite` brání novému výpisu při backpressure;
čtení čítačů není atomický snapshot. Při resetu/změně epoch se výpis zruší.
Skutečnou stabilitu FIFO, stack rezervu, CTS, zvuk, DLS, slideshow a aktuální
X-PAD na jednotlivých stanicích ověří [český RF postup](EPG_MUX11A_XPAD_RF_TEST_CZ.md).

## Patch a konečný závěr

`patches/EPG_MUX11A_XPAD_20261025.patch` je přírůstkový patch nad předchozím
lokálním stavem zachyceným v `EPG_live_20261025.patch`. Neobsahuje externí
odstranění IRReceiver ani přepis historického patch. Git index se nepoužívá.

Technicky možné: **ANO**. Odolnost implementace proti prázdnému packet toku:
**PASS podle provedených hostitelských testů**. Skutečné SPI přes X-PAD na 11A:
**NEOVĚŘENO**; zachycený log dokazuje pouze prázdný packet carousel.
