# EPG Auto Suspend — předávací protokol, 3. 11. 2026

Ověřený výchozí i konečný HEAD: **df0c8bc9eef4c4598742fd462216675d1e885d81**.
Pracovní strom byl před změnou čistý. Implementace je v pracovním stromu;
nevznikl commit ani push. Jde o mitigaci zátěže samostatné paketové SPI EPG
komponenty při validním prázdném MOT Directory, nikoliv potvrzenou opravu
interního firmware Si4684 nebo důkaz, že qmaxRaw=19 způsobuje hang.

## Implementace a vazba na fyzický STOP

Core má nový nealokující Observer::directory: volá jej po CRC/envelope,
sestavení a úplné strukturální validaci Directory, **před** návratem pro shodný
directoryHash_. Platí pro jednu i více skupin. Nedokončená assembly, vadné CRC
ani diagnostické čítače signál nevytvoří. Původní observer.object a jeho
čítače si zachovávají význam; nová cesta je jediným zdrojem potvrzení prázdnosti.

Runtime předává signál politice pouze uvnitř živého packet feedu, když Core
vlastní packet endpoint. PAD ani replay signál neposílají. Token dokončené
skupiny (TID, continuity/repetition) chrání před opakovaným callbackem stejné
skupiny; jiný TID/hash nebo uplynutí 30s okna začínají potvrzování znovu.
Skutečný zachycený karusel 11A mění continuity (ověřeno v archivním log_11A.txt).
Validní neprázdný Directory nebo validní dekódovaný EPG objekt ruší potvrzování
a při rechecku obnovuje ACTIVE bez dalšího fyzického START.

DAB synchronizuje kontext před schedulerem, packet feedem a dokončením
START/STOP. Kontext zahrnuje dabGeneration, dabSwitch.requestId, audio SID/CID,
packet SID/CID, SLS SID/CID a diagnostický režim. Vyžaduje dostupnou jednoznačnou
discovery, lock, stabilní službu, packet owner, odlišné audio a SLS, a při
sledované službě přesnou shodu s vlastníkem Probe. PAD owner, replay, FM,
ladění, změna služby/identity/generace, ambiguity a CTS stall ruší způsobilost.
Tuner reset navíc explicitně resetuje politiku přes liveReset.
Výjimkou je dočasné Pending během UA ověřování již potvrzeně zastavené služby:
při shodném zbytku kontextu zůstává termín cooldownu zachovaný, eligible=0
a START/STOP jsou zakázány. Stejný znovu ověřený endpoint obnoví způsobilost
bez nového časovače; změna endpointu či ambiguity starý kontext zruší.

Po potvrzení prázdnosti politika jednou zavolá existující dabSpiProbe.stop().
Fyzický STOP_DIGITAL_SERVICE stále sestavuje a odesílá výhradně původní
DabCommand::SpiProbeStop v sériovém scheduleru, s uchovaným packet SID/CID.
Audio/SLS START/STOP ani pořadí cleanup při ladění se nemění. Větev produkčního
START při Idle explicitně volá allowStart(), takže neobchází cooldown.

COOLDOWN vzniká pouze po úspěšném asynchronním STOP nebo původně uznávaném
STOP NOT_AVAILABLE (device reason 03). Timeout nepotvrzuje zastavení; Probe
uchovává endpoint, provádí dosavadní omezené cleanup pokusy a CleanupFailed
blokuje START. Stale-generation dokončení smí zachovat dosavadní fyzické
reconciliation, ale nepotvrdí AUTO. CTS klasifikace, cold recovery, DSRV burst=4,
ACK/CTS timeouty, SPI takt, RAW ring a freeze zůstávají beze změny.

## Stavy a časování

Politika je v src/epg_auto_suspend.h, časy jsou v Config. Žádný delay/task,
dynamický buffer ani nový SPI plánovač. Časování používá uint32_t a signed deadline.

| Stav | Podmínka / další krok |
| --- | --- |
| DISABLED | EPG AUTO OFF; původní produkční příjem |
| INELIGIBLE | Neověřený/sdílený/PAD vlastník, nestabilní kontext nebo režim B/C/D |
| ACTIVE | Průběžný příjem; min. 3 validní prázdná Directory, první–poslední >= 2000 ms, okno <= 30000 ms |
| STOP_PENDING | Jediný požadavek STOP; čeká na skutečný výsledek, bez nového START |
| COOLDOWN | Potvrzený STOP; 300000 ms od potvrzení, produkční START blokován |
| RECHECK_START | Jeden produkční Probe; původní baseline 500 ms a asynchronní START |
| RECHECK_ACTIVE | Max. 30000 ms od úspěšného START; prázdnost ukončí dříve, obsah vrací ACTIVE |
| ERROR_BACKOFF | Prokazatelně neúspěšný START; další pokus nejdříve po 300000 ms |

Recheck bez úplného Directory/obsahu končí INCONCLUSIVE, bezpečným STOP a po
potvrzení 300000ms backoffem. Nezaměňuje se za EMPTY. Nejistý START přechází
přes konzervativní STOP; nejistý STOP zůstává STOP_PENDING, případně Probe
CleanupFailed. Limit rechecku je nezávislý na epgContinuous.

AUTO je standardně **ON**, jen v RAM. Příkazy: EPG AUTO STATUS, EPG AUTO ON,
EPG AUTO OFF. ACK obsahuje enabled a atMs. STATUS nepožaduje START ani STOP.
OFF během již odeslaného cleanupu nechá fyzický cleanup doběhnout a vrací
původní produkční chování. ON znovu potvrdí prázdnost v aktuálním kontextu.

| DSRV režim | AUTO |
| --- | --- |
| A / mode=0 | Použije aktuální ON/OFF; A sám nepřepisuje kill-switch |
| B / mode=1 | Ruční STOP má prioritu, timer nikdy službu nespustí; lze připnout i potvrzený AUTO cooldown |
| C / mode=2 | Ineligible; fyzický příjem pokračuje, EPG parser bypass zůstává platným experimentem |
| D / mode=3 | Ineligible; zachované původní quiet měření bez nových přechodových logů AUTO |

Změna režimu či kontextu ruší předchozí AUTO potvrzování/timer. Návrat do A
při ON začíná aktuálním ACTIVE. DEBUG SPI má dál původní 5s baseline/30s okno
a explicitně ruší AUTO kontext; DEBUG SPI STOP zůstává ručním pozastavením.
Otevření EPG obrazovky cooldown neobchází. Stejný liveEndpoint při rechecku
neresetuje Core, cache ani directoryHash_. Politika během cooldownu Core
nemaže. Původní reakce Core na skutečně změněný obsah Directory se nemění.

STATUS obsahuje enabled/eligible/state/mode, SID/CID, generaci/request,
odlišnost audio/SLS, packet/PAD owner a core; emptyValid/nonEmptyValid,
emptyRepeat/TID/hash/první a poslední čas; startCount/stopCount,
stopConfirmedCount/stopError, recheckCount/result/termín/zbývající čas,
reason/cooldown/CTS/overflow/UARTdrop. Queue build přidává skutečné qmaxRaw.
Úplné RAW evidence jsou nadále přes DSRV STATUS. Čítače AUTO jsou souhrny za
běh ESP32, potvrzovací okno je kontextové. nonEmptyValid zahrnuje validní
neprázdné Directory i dekódované objekty. stopCount počítá AUTO cykly,
nikoliv jednotlivé cleanup retry; startCount úspěšné způsobilé produkční START.
Přechodové logy jdou jednou při změně stavu přes neblokující diagSerial;
STOP_PENDING není označován za pozastavenou službu.

## Reálné ověření

Spuštěno na Windows s Pythonem PlatformIO a g++ C:/msys64/ucrt64/bin/g++.exe:

~~~text
python tests/run_epg_tests.py
compile EPG auto suspend: PASS
EPG auto suspend: PASS
CTS snapshot/UA/UART/modes: PASS
CTS RAW freeze/CLEAR integration: PASS
RAW queue/actual DSRV/Core coexistence: PASS
transport: PASS
coexistence: PASS
pc: PASS
All EPG host checks passed.
~~~

Celkem **43 kroků**, bez selhání, včetně RAW/oracle, cache, scheduleru,
X-PAD, runtime/UI, syntetických cest/UART, storage a PNG Adam7.
Úplné výstupy: .pio/epg_acceptance/results.log a results.json;
finální souhrnný běh .pio/auto_host_final.log.
Po rozšíření negativních scénářů byly navíc samostatně úspěšně spuštěny:

~~~text
python tests/test_epg_transport.py C:/msys64/ucrt64/bin/g++.exe
python tests/test_epg_coexistence.py C:/msys64/ucrt64/bin/g++.exe --queue
AUTO actual scheduler/START/STOP completion: ... PASS
AUTO actual DSRV/Core owner, CRC/foreign SID/empty reply rejection, ... PASS
~~~

Logy: .pio/auto_transport_review.log a .pio/auto_coex_review.log.
Testy pokrývají 1/2/3 Directory, minimální odstup a vypršení okna, duplicity,
CRC/neúplné segmenty, nečinnost, cizí SID, PAD/replay/generaci/lock/ambiguity,
shodu audio/SLS, neprázdný obsah, 5min gate, recheck limit, chyby START/STOP,
NOT_AVAILABLE, stale dokončení, millis wrap, ruční B a tři neúspěšné cleanup
pokusy bez následného START. Kompilují skutečné scheduler/START/STOP výsledkové
bloky, celý DSRV case a skutečný runtime/Core; chip I/O je simulované.
Existující CTS testy nadále ověřují freeze, UART headroom a recovery evidence.

Reálně spuštěno pro změnu i samostatný git archive výchozího HEAD:

~~~text
pio run -e esp32dev -e esp32dev_queue_diag -e esp32dev_spi5 -e esp32dev_diag
esp32dev             SUCCESS
esp32dev_spi5        SUCCESS
esp32dev_diag        SUCCESS
esp32dev_queue_diag  SUCCESS
4 succeeded
~~~

První pokus narazil na PermissionError při zápisu platforms.lock/packages.lock
v uživatelském .platformio. Build byl úspěšně dokončen s kopiemi již dostupných
balíčků v .pio/auto_core a nastavenými PLATFORMIO_CORE_DIR a
PLATFORMIO_PACKAGES_DIR; žádná změna platformio.ini. Platform espressif32 6.9.0,
Arduino 3.20017.241212+sha.dcc1105b, Xtensa 8.4.0+2021r2-patch5.
Výchozí zdroje: .pio/auto_baseline_source. Logy:
.pio/auto_baseline_local.log a .pio/auto_build_final.log.
Známé varování TFT_eSPI o nedefinovaném TOUCH_CS se vyskytlo i v baseline.

Paměť v bajtech podle skutečných linker/PlatformIO výstupů:

| Prostředí | RAM před | RAM po | Rozdíl | Flash před | Flash po | Rozdíl |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| esp32dev | 67168 | 67296 | +128 | 3572753 | 3575697 | +2944 |
| esp32dev_spi5 | 67168 | 67296 | +128 | 3572757 | 3575717 | +2960 |
| esp32dev_diag | 69328 | 69456 | +128 | 3575081 | 3577953 | +2872 |
| esp32dev_queue_diag | 67904 | 68032 | +128 | 3578901 | 3581857 | +2956 |

Statická RAM tedy **+128 B** ve všech buildech. Samotná politika má na ESP32
124 B (ověřeno xtensa nm), na hostu 128 B. Observer přidává jeden callback do
Core (+4 B na ESP32 při jeho alokaci); žádný nový heapový buffer. Reálný
host allocator test s diagnostickým tlakem zachoval rezervu >= 12000 B a počet
alokací během příjmu/recheck simulací. Skutečné free8/fragmentaci na desce
musí potvrdit RF měření. Obrazy firmware.bin a receiver-complete.bin byly
vygenerovány obvyklým build skriptem; na zařízení se nic neflashovalo.

Whitespace kontrola prošla: git -c core.whitespace=cr-at-eol diff --check
(repozitář má původní CRLF zdroje).

## Změněné soubory a předání

- src/epg_auto_suspend.h — nová pevná politika a kontext.
- src/epg_core.h/.cpp — plně validovaný Directory callback, včetně shodného hash.
- src/epg_runtime.h/.cpp — packet owner, předání validací a reset při tuneru.
- src/si4684.h/.cpp — scheduler gate, asynchronní výsledky, identity, AUTO příkazy.
- src/comms.cpp — routing AUTO před EPG TEST parserem.
- tests/test_epg_auto_suspend.cpp — nové unit/Core testy.
- tests/test_epg_transport.py — skutečný scheduler a START/STOP completion scénáře.
- tests/test_epg_coexistence.py — skutečný DSRV/runtime/Core/owner a AUTO příkazy.
- tests/test_dab_cts_ua.py a tests/run_epg_tests.py — závislost test harnessu a registrace.
- docs/EPG_AUTO_TEST_PL.md — praktický polský RF návod.
- docs/EPG_AUTO_SUSPEND.md — tento předávací protokol.

**Bez fyzického Si4684 a multiplexu 11A nebylo ověřeno:** skutečné potvrzení
STOP a zánik produkčního packet provozu na RF, 20–30 minut stability a více
pětiminutových cyklů, CTS frekvence vůči OFF/C/B, reálný poslech/SLS/TFT,
free8 na WROOM a obnovení obsahu v téže službě během vysílaného rechecku.
Archivní data a host testy tyto fyzické výsledky nenahrazují. Postup je v
[polském návodu](EPG_AUTO_TEST_PL.md).
