# EPG LIVE – čas, SI-only, archiv a regrese (25. 10. 2026)

Lokální implementace podle CODEX_ZADANI_SI4684_EPG_CAS_SI_ONLY_DST_ARCHIV_2026-10-25.md.
Výchozí i konečný HEAD: `116758e5bd05927f70f6884a332a845e0e60369b`.
Pracovní strom byl na začátku čistý. Nebyl použit cherry-pick, změna historie,
Git indexu ani push. Firmware nebyl nahrán, COM port nebyl otevřen a NVS se neměnilo.

**Výsledek:** hostitelská sada a esp32dev build PASS. Nová verze na přijímači:
**HW NOT TESTED**. Předchozí potvrzené HW výsledky z 24. 10. zůstávají baseline;
nelze je připsat tomuto novému buildu. Testerovo hlášení úspěchu 25. 10. 07:33
je konkrétní dřívější pozorování, nikoli důkaz všech DST hranic této změny.

## Kontrola podkladů a převzetí v2

Oba archivy nalezeny v `_usertesting`, jednotlivé rozdíly porovnány s HEAD.
V1 má čtyři soubory (core cpp/h, runtime, probe), v2 pět včetně UI.
Převzata oprava bitových pozic po ověření normy; priorita PI byla přepracována
na akvizici bez hladovění SI/PNG. Paměťové zvětšení 128/16384 nebylo převzato:
96 záznamů a 8192 B textu už pokryje opravených 68 pořadů Trójky bez LIMIT.
Zkrácena pouze produkční baseline na 500 ms; DEBUG SPI má stále 5 s a původní
30s pozorování i stejný CTS/START/STOP cleanup owner.

Z v2 nebylo převzato implicitní CET +60, odvozování současného offsetu z PI,
mapování historického dne na dnešní čas ani přelepení původního roku v UI.
Soubor `_usertesting/20261024.txt` rozlišuje 11A prázdný Directory a 11B
70 objektů. Nová 27B fixture testuje první případ odděleně od skutečného RAW 11B.

## Čas a hodiny

`epg_time.h` je čisté C++11 rozhraní bez Arduino/TZ: BroadcastClockSample,
ResolvedClock, resolveTime a ověření páru UTC/local. timePoint zachovává
`ltoPresent` v Directory, Programme a cache; skutečné/relativní časy mají
oddělený offset. UTC instants se neposouvají do lokálního času při uložení.
Rezervované bity se ignorují, délky 4/6 + LTO, MJD, HH/MM/SS, znaménko a
půlhodinový rozsah ±14 h se validují. Duration a planned interval zůstávají
UTC `[start,end)`; explicitní actual nepřepisuje planned policy NOW.

TS 102 371 §4.7.4 výslovně definuje nepřítomné LTO jako lokální čas rovný UTC.
Proto je numerická hodnota 0 pro tento PI správná, ale příznak přítomnosti
zůstává oddělený od explicitního LTO +00. Není doplňována česká/polská zóna.

Důležitý nález při kontrole skutečného kódu: rádio posílalo TIME_TYPE=0.
AN649 str. 200 definuje **0 local, 1 UTC**. Lokální čas byl tedy chybně
předán jako UTC. Vendor parser pouze vyjme rok/měsíc/den/HH/MM/SS a žádnou
konverzi nedělá. Vlastní vendor soubor se nezměnil.

Nová sériová fronta drží původní lokální čtení pro hlavní hodiny, přidává
UTC čtení a následnou lokální kontrolu. Offset se určí porovnáním UTC se
**pozdějším** lokálním vzorkem, nejvýše po 2 s a s tolerancí 2 s pro sekundové
rozlišení. To funguje i při přeskočení DST mezi oběma odpověďmi. UTC vzorek
se použije přímo; nejde o odečítání historického PI LTO. Kalendář z UTC je
oddělen od veřejných lokálních radio.Year/Hours, takže hlavní hodiny zůstávají lokální.
Neznámá/nesmyslná/stará odpověď neustanoví spolehlivý čas. Pro NOW je potřeba
UTC i aktuální potvrzený offset. Clock sample od 300 s, ztráta locku/FM nebo
neúspěšná kontrola odstraní důvěryhodnost; po ztrátě locku je potřeba nová generace.

DEBUG vypisuje omezené `[DAB/CLOCK] type=0|1 raw=...` z původních 7 kalendářních
bajtů odpovědi a `confirm local=... utc=... paired=... offset=... generation=...`.
Existující vlastníctví SPI reply a kooperativní fronta jsou zachované.
AN649 kontrakt i skutečný parser jsou otestované na hostiteli; konkrétní
odpovědi FW 6.0.9 po této změně jsou **HW NOT TESTED**.

## Dostupnost, SI, archiv a UI

Core poskytuje lookup SI **i** Directory podle SID/EID/ECC; pokud API dodá
SCIdS, helper ho umí zkontrolovat. Rádio zatím SCIdS samostatně nepublikuje:
při více různých odpovídajících bearerech se vyhlásí konflikt, ne odhad podle
názvu/CID. SI služba má uložený název, krátký název, PNG příznak a URL/jméno
obrázku. Název/loga stanice bez PI jsou dostupné a nezískají cizí pořady.

Akvizice upřednostňuje relevantní PI, ale dostupné SI/PNG může zpracovat,
pokud ještě nezačala jiná sestava. Rozpracovaná sestava drží jediný body buffer;
po 5 s bez pokroku uvolní prioritu. SI scope není filtrován podle dnešního
roku a načítají se i další SI objekty po prvním. PNG před SI lze získat při
dalším carouselu; žádný druhý obrazový buffer se nepřidává. Focus používaný
při offline inventuře zůstává striktní a nepřijímá jiné objekty.

Oddělené údaje: transportFresh (příchod CRC-validní MOT skupiny), původní
fresh() pro PI cache, scheduleDateValid, serviceHasPI/SI a clockTrusted.
Priority stavů odpovídají následujícímu pořadí:

| Stav | Výsledek |
|---|---|
| IDENTITY_CONFLICT | Nejvyšší priorita konfliktu, nepřiřazovat náhodný PI/obrázek. |
| NO_SPI | SPI není potvrzeno. |
| SPI_PRESENT_WAIT_DIR | Čekání na platný adresář. |
| DIRECTORY_EMPTY | Kompletní Directory má 0 objektů, přítomnost služby zůstává. |
| PENDING_IDENTITY | Čekání na ověřené spárování. |
| SI_ONLY | Metadata existují, Directory nedeklaruje PI pro tuto stanici. |
| TIME_UNKNOWN | PI deklarováno, ale současné hodiny/offset nejsou důvěryhodné. |
| PI_DATE_STALE | Původní PI nekryje LIVE datum nebo jde o výslovný archivní náhled. |
| READY_LIVE | Platné datum, čas, čerstvý PI a NOW nebo NEXT. TEST má označené virtuální hodiny. |
| PI_DECLARED_LOADING | PI čeká na kompletaci/platnou cache nebo její obnovení. |

Green vyžaduje READY_LIVE. SI-only, samotný PNG, prázdný Directory a archiv
nezelenají. Při archivních datech zůstává skutečné UTC z roku 2026;
seznam a detail uvádějí původní den/rok a označení archivu. NOW/NEXT/progress
se netváří jako živý program. UI nerozšiřuje framebuffer a ponechává částečné
překreslování seznamu, selektor, fonty a všech osm stávajících lokalizací.
Metadata bez PI mají samostatný text o absenci rozpisu a vlastní logo.

`EPG ARCHIVE YYYY-MM-DD` je výslovný náhled původního kalendářního dne v LIVE;
`EPG ARCHIVE OFF` ho vypne. Default OFF, trvalé označení ARCHIVE / SIMULATION,
žádné mapování současného času dne, žádný NOW ani přepsání PI/clock/NVS.
Standardní LIVE může historický seznam procházet i bez simulace. TEST DEMO,
E1/E2 a virtuální TIME/STOP zůstávají samostatné. STATUS nově přidává STATE
 a CLOCK, včetně příznaků a zdrojového rozsahu. Podrobná syntaxe je v Serial_protocol.md.

## Matice T01–T18

PASS níže znamená **hostitelský test produkčního kódu / mock**, nikoli RF test.

| ID | Stav | Důkaz a hranice |
|---|---|---|
| T01 | PASS | Pevné nezávislé binární vektory 00:00, 00:07, 12:34, 19:00, 22:07, 23:59. |
| T02 | PASS | Short/long, +00/+01/+02/−05:30/+05:30, absence LTO, neplatné HH/MM/SS/délka/LTO, rok/MJD. |
| T03 | PASS | Pevné podzimní UTC/local vektory i skutečné LOCAL/UTC/LOCAL cases přes 01:00Z. |
| T04 | PASS | Jarní skok 01:59:59→03:00:00 přes 01:00Z; stejný parser/fronta. |
| T05 | PASS | RAW Trójka + živé hodiny 25.10.2026: archive/date stale, NOW/NEXT −1. |
| T06 | PASS | RAW TEST + 12.11.2024: 68 položek Trójky, NOW/NEXT; DEMO Jedynka nyní 42 platných položek. |
| T07 | PASS | RAW 37FC: Radio Kraków, vlastní krakow.png, SI_ONLY, žádný PI. |
| T08 | PASS | RAW 37FD: OFF RadioKraków, vlastní off.png, SI_ONLY. |
| T09 | PASS | Samostatně sestavený validní 27B Directory s 0 objekty, bez CRC/transport chyby. |
| T10 | PASS | RAW PI/SI/PNG pro Trójku, 68 položek bez LIMIT; metadata nejsou blokována čekáním na PI. |
| T11 | PASS | Normální i obrácený RAW carousel, otočené segmenty, další rotace po PNG před SI. |
| T12 | PASS | RAW PR24/Chopin se stejným bearer: konflikt, žádné logo/NOW. |
| T13 | PASS / HW NOT TESTED | Runtime STOP/FM/service/mux, starý READY odstraněn; skutečné audio/SLS po změně netestováno. |
| T14 | PASS | Dosavadní CRC/length/duplicate/out-of-order/conflict/timeout regressions, SPI cleanup/router. |
| T15 | HW NOT TESTED | Host allocator/canaries/rollback/IRAM/Adam7/word access PASS; reálné low-water RAM, stack a souběh SLS/EPG na desce NOT RUN. |
| T16 | PASS | Přímá výměna archivního Directory za současný PI bez resetu: READY_LIVE. |
| T17 | PASS | Nejistý/stale clock, chybějící offset, ztráta locku; obnovit až s novou generací. |
| T18 | PASS | SI+PNG bez PI obou Kraków služeb, vlastní metadata a žádný NOW. |

Celá `python tests/run_epg_tests.py` prošla: RAW/oracle, errors/cache/UART,
SPI probe/router, skutečné DAB clock cases, X-PAD, live/runtime/UI/transport,
synthetic MSC/PAD/E2, storage, Adam7 i PC klient. Samostatně prošel offline
hash/regenerační test uložených fixture a rozšířený actual/end-exclusive test.
Logy jsou v `.pio/epg_acceptance/results.log`, results.json a epg_live_20261025.json.
Trvalé souhrnné výsledky: [EPG_live_20261025_results.json](EPG_live_20261025_results.json).

## RAW oracle a zachování původních dat

Z 3080 deklarovaných výpisů se získá 1939 kompletních CRC-validních rámců,
1141 neúplných se nadále zahazuje. Ověřeno 70 objektů = 35 PI + 5 SI + 30 PNG,
55 SI záznamů napříč SI objekty a všech 1087 programových záznamů. Počet
záznamů dekódovaných offline není velikost RAM cache jednoho dne (Trójka 68).

Původní referenční JSON má stejnou chybu bitu hodin jako starý parser;
jeho časy tedy nemohou být správnou časovou referencí. ZIP nebyl změněn.
`tests/epg_time_oracle.py` nezávisle čte z původních binárních objektů UTC/LTO
podle normy. Všechny ostatní reference, názvy, popisy, identity, duration,
MOT Directory a PNG byte-for-byte zůstávají porovnávané proti původnímu ZIP.
Tři časy dříve mylně odmítané jako minute=60 jsou po opravě validní.
Nové syntetické streamy mají `_v2.bin`; původní v1 fixture i staré HW reporty
zůstaly zachované včetně hashů. Nízké-levels UART formát E1/E2 se nezměnil.

## Paměť, build a omezení důkazů

ESP32 layout byl ověřen xtensa kompilátorem: Core 14232 B DRAM + 36864 B
word-only IRAM bufferů = **51096 B**, o 296 B více než původních 50800.
CachedProgramme zůstává 32 B; nové flags využívají bitová pole. Žádné nové
image/frame buffery, malloc/free na paketu, PSRAM nebo flash cache.
Zůstává kMaxProgrammes=96, kTextBytes=8192. Guard 12 KiB DRAM a 4 KiB IRAM
po alokaci zůstává; allocator rollback, reuse i canaries se otestovaly.

Build esp32dev PASS: statická RAM **66584 B**, flash **3545485 / 4128768 B**,
firmware.bin **3546112 B**, verze v2.5b.
SHA256 `20aa1ba00ecbd921c9a3cc12ee7c3ad06f2abc21d1dd2259e36c428068819ed8`.
Host mock po alokaci: free8/largest8=13832, free32/largest32=17416 B;
jsou to **simulované** hodnoty. Nové skutečné HW free8/free32/largest/minheap/
stack jsou **NOT RUN / HW NOT TESTED** a nesmějí se zaměnit s těmito čísly.

I²S konstanta `0x1870` a soubory i2s_audio.h, slideshow.cpp, png_adam7.cpp,
vendor/si468x/Si468x.h, nvs_storage.cpp i epg_uart.cpp jsou obsahově beze změn
vůči HEAD. UART výsledky a obrazové regresní testy prošly. Zachování fyzického
audia, CTS/overflow a chování FIFO po nové dvojici clock dotazů musí ověřit HW.

Během práce se v pracovním stromu objevilo odstranění src/IRReceiver.cpp/.h
mimo tento EPG zásah. Nebyly obnoveny ani zahrnuty do EPG patche; uvedený build
je z tohoto aktuálního pracovního stromu. Původní build HEAD před tím narazil
na chybějící IRremoteESP8266; byla instalována lokálně do .pio/libdeps verze
2.9.0 přes `pio pkg install --library crankyoldgit/IRremoteESP8266@2.9.0 --no-save`.
platformio.ini se neměnilo. Po odstranění nepoužívaných IRReceiver souborů
aktuální LDF tuto knihovnu nevyužívá. Jde o okolnost reprodukce buildu.

## Soubory a diff

Produkce: epg_core.cpp/.h, nové epg_time.h, epg_runtime.cpp/.h,
epg_ui.cpp, language.h, si4684.cpp/.h a malý productionBaseline v dab_spi_probe.h.
Testy: nezávislý time oracle, test_dab_clock.py, test_epg_live.py, pevné JSON
vektory, probe/regresní testy a nový v2 stream. Upravené starší testy opravují
syntetické encodery a očekávané počty po opravě času, neobcházejí kontrolu UTC.
Dokumentace: Serial_protocol.md, tests README/fixture manifest a tento report.

Konkrétní lokální diff (včetně nových souborů):
[EPG_live_20261025.patch](patches/EPG_live_20261025.patch).
Návod pro skutečné HW ověření: [EPG_ANDRZEJ_20261025.md](EPG_ANDRZEJ_20261025.md).

Normové zdroje: [ETSI TS 102 371 V3.2.1 §4.7.4](https://www.etsi.org/deliver/etsi_ts/102300_102399/102371/03.02.01_60/ts_102371v030201p.pdf)
a [Skyworks AN649 Rev.2, DAB_GET_TIME str.199–200](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/an649.pdf).
