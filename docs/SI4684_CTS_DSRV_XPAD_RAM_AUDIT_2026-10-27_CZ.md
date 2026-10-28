# Audit SI4684: CTS, DSRV, X-PAD a interní RAM

Zadání z 27. 10. 2026; analýza a lokální úpravy provedeny 28. 10. 2026.
Výchozí i aktuální HEAD při práci: `4a4b2d205a2f8c36b56378ec40dcc2dfa560414d`.
Výchozí pracovní strom byl čistý; upravené zdroje nejsou tímto SHA identifikovány.
Uživatel výslovně schválil kompaktní diagnostiku, UART 2048 B, tichý D,
CTS snapshot, dočasné UA příkazy, tři buildy, testy a tento audit i polský návod.
Nebyl proveden commit/push, flash, změna NVS ani zásah do HW.

## Závěr a hranice důkazu

Prokázaný nedostatek statické interní RAM byl odstraněn: běžný build uvolnil
**4064 B DRAM**, bez snížení 12000B rezervy a bez změny EPG Core či obrazových
bufferů. Režim D je skutečně bez automatických agregovaných výpisů a obsahových
hashů; první CTS timeout dostal nezávislý malý snapshot bez dalšího SPI dotazu.

**Příčina neodpovídajícího CTS na konkrétním polském 11A zatím prokázaná není.**
Neprohlašujeme problém za vyřešený prodloužením timeoutu nebo změnou watchdogu.
Ani jedno nebylo změněno. Nová verze má HW stav **NOT TESTED**; starší uživatelem
potvrzené testy SLS/resetů se na tyto nové změny automaticky nepřenášejí.

## Dostupné podklady a časová osa

Na disku jsou `_usertesting/20261027.txt` a
`_usertesting/Logs_4a4b2d2_memory_and_watchdog.zip`. Nebyly upraveny.
ZIP obsahuje dva logy, 195869 a 299639 B. Předpony času nemají časové pásmo;
níže je zachován jejich původní lokální čas, nikoli domyšlené UTC.

| Pozorování | Výsledek čtení logu |
|---|---|
| První memory log | 8 hlášení MEMORY, need8=14448, free8=23468, largest8=20468; 0 CTS timeoutů, 0 watchdogů |
| Snížená rezerva v druhém pokusu | Údaj testera: experimentálních 8000 B místo 12000 B; není součástí této implementace |
| První selhání | 18:29:28.643 Timeout; následuje UA query failed pro SID 3223/CID 01FE0006 |
| Další terminální selhání | SignalStatus op=5; jiná série obsahuje také DsrvHeader |
| Watchdog 1 | 18:29:37.935, CTS_NO_PROGRESS, failures=8, hostGapUs=8864 |
| Watchdog 2 | 18:31:05.682, failures=8, hostGapUs=9194 |
| Watchdog 3 | 18:31:54, následná fyzická obnova omezená dosavadním rate limitem |
| Druhý log celkem | 24 terminálních CTS prodlev/timeouts, 3 watchdogy; bez důkazu následného restartu ESP32 |
| Příjem 11A | Stovky packetových hlaviček za interval, prázdný CRC-validní Directory; queueMax až 19 |
| PAD vs EPG | PAD obsahuje SLS; pro vybranou audio komponentu je UA 0x002, ne prokázané X-PAD SPI 0x007 |

Reprodukce souhrnu a hashů vstupů:

```powershell
python tests/analyze_cts_watchdog_logs.py _usertesting/Logs_4a4b2d2_memory_and_watchdog.zip --output .pio/cts_audit_baseline/log_analysis.json
```

Nulové dropped/readFail v některých intervalech neprokazují nulovou CPU režii.
Současně 9ms poslední host gap není 9ms CTS timeout a není důkazem příčiny
přibližně sekundového selhání. Stabilní core=0 a nestabilní core=1 nejsou čisté
A/B: liší se fyzickým packetovým příjmem i pamětí a použitou rezervou.

## Audit příkazové a datové cesty

`Si468x::service()` při deadline stále čte normální status a vrátí Timeout,
pokud CTS není potvrzeno. `lastServiceGapUs` je poslední interval service,
`lastDeadlineLatenessUs` pozdní obsluha deadline, nikoli celá délka transakce.
`readCurrentReply()` čte zachovaný reply, neposílá druhý GET ani nový ACK.
DSRV GET zůstává serializovaný, ACK=1 a STATUS_ONLY=0.

`dab_health::Dsrv` zachovává rising sequence, epoch, buffersRemaining a jednu
omezenou verification transakci. Prázdná verification ukončí sticky-high drain.
Burst=4, low-priority pravidla, timeouty, párování času i watchdog 8 chyb/5 s
jsou beze změny. Recovery dál používá dosavadní obnovu Si4684/TFT a 60s omezení;
není přidán restart ESP32 ani paralelní čipová úloha na druhém jádře.

V packetové větvi se původně pro každou skupinu vytvářel `Registry` o 516 B.
Ten byl nahrazen `emptyDirectoryGroup()`: rychlé odfiltrování jiných typů,
validace envelope/CRC a úplného prázdného Directory bez dalšího registry a CRC32.
Core dál provádí vlastní validaci. Identický Directory v Core podle hashe
nevolá clearCache a nespouští žádné zvláštní hledání X-PAD.

MODE C vypíná pouze packetový Core feed. Nadále se čtou/ACKují packetové bloky,
provádí diagnostická CRC kontrola prázdného Directory i obsahový hash.
MODE D zachovává normální Core feed, ale vynechává hashování, konstrukci loss
bitmap, automatické reporty a jejich 30s heap/stack měření. Základní saturující
čítače pokračují. Výslovné STATUS naopak záměrně vypíše jeden omezený report.

`epg_ingress::pad`, ověřená UA komponenty, source=1 a vlastnictví audio kontextu
stále musí souhlasit pro PAD SPI. Raw UATYPE/RFU samo o sobě není potvrzení UA.
Slideshow UA 0x002 zůstává na původním SLS dekodéru. Packet source=0 a jeho
prázdný Directory nepřidávají vedlejší X-PAD parser. Potvrzený PAD owner dál
izoluje packetové MOT TID. Polské logy neprokazují aktivní PAD SPI pro Trójku.

## STATUS3 a dokumentace čipu

Byla přečtena místní `Specification/AN649 Programming Guide Rev 1.9.pdf`,
strany 170–171: DAB GET_DATA má DSPERR na bitu 4, REPOFERR 3, CMDOFERR 2,
ARBERR 1, ERRNR 0; BUFF_COUNT je počet zbývajících bufferů podle této revize.
Starší příručka uvádí nejvýše 8 bloků, což nevysvětluje pozorované číslo 19.
Neděláme z něj odhad fyzické velikosti FIFO ve FW 6.0.9.

Doplněna byla kontrola [oficiální AN649 rev. 2.0](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/an649.pdf),
sekce DAB GET_DIGITAL_SERVICE_DATA a 7.7. Tabulka verzí v příručkách není
samostatné potvrzení všech rozšíření FW 6.0.9; odpovídající release notes nejsou
přiloženy. Interpretace raw UATYPE/RFU a případných nových bitů zůstává tímto
omezená. Vendor knihovna nebyla změněna a diagnostické bity neřídí novou recovery.

Nový snapshot má raw STATUS0–3, statusKnown/age a sticky OR spodních pěti bitů
pro aktuální kontext. Zachytí první operaci v souvislé sérii, příkaz a prefix
nejvýše 4 argumentů, skutečné elapsedMs, deadline lateness a poslední service gap.
Doplňuje intervalové packet/PAD/overflow/queue údaje, obě heap capability skupiny,
largest blocks, stack HWM, Core/EPG stav a epoch/reset count. Vše ze stávajících
pozorování; žádná alokace, další SPI příkaz ani čekání na volný UART.

## RAM, flash a zásobník před/po

Měřeno target compilerem Xtensa, původní ELF/map uchovány v
`.pio/cts_audit_baseline`; aktuální ELF pro každou variantu v `.pio/build`.
Flash je součet sekcí podle SIZEPROGREGEXP PlatformIO, ne délka binárky.
Běžná varianta má stejné build flags jako baseline; SPI5 přidává pouze
`RADIO_SPI_HZ=5000000UL`, servisní varianta pouze `SI4684_DIAG_DETAIL=1`.

| Metrika [B] | 4a4b2d2 | Běžný 10 MHz | Běžný 5 MHz | Servisní 10 MHz |
|---|---:|---:|---:|---:|
| .dram0.data | 21800 | 19768 | 19768 | 21928 |
| .dram0.bss | 49432 | 47400 | 47400 | 47400 |
| Statická DRAM celkem | 71232 | 67168 | 67168 | 69328 |
| Úspora DRAM proti baseline | — | **4064** | **4064** | 1904 |
| .iram0.text | 74747 | 74747 | 74747 | 74747 |
| .iram0.vectors | 1027 | 1027 | 1027 | 1027 |
| Flash (PlatformIO) | 3570153 | 3572189 | 3572193 | 3574489 |
| Evidence | 3664 | 1528 | 1528 | 3688 |
| Objekt UART | 4136 | 2088 | 2088 | 2088 |
| CTS evidence navíc | 0 | 108 | 108 | 108 |
| finishDabCommand entry frame | 720 | 304 | 304 | 304 |

Normální Evidence používá 4 skupiny operací: other/dsrv/signal/UA, ostatní
operace se započítají do other. Servisní build má všech 22 řádků. Tři banky
zůstávají kvůli souvislému měření při pomalém výstupu, ale jejich matice jsou
kompaktní. Nejde o přesun statistik na zásobník.

Formatter klesl z 1024 na 768 B. Maximalizované testovací řádky reportů mají
nejvýše 525 B; CTS dvojice byla testována na skutečném 2048B UART wrapperu.
Monitor dál odesílá nejvýše 64 B na service volání a nikdy nečeká na FIFO.
Počet discarded bytes nyní zahrnuje i celý odmítnutý konec blokového zápisu.

Core metadata zůstávají **14448 B**, bufferBytes **49152 B**, dohromady
**63600 B na ESP32**. Host ABI má jiné zarovnání; jeho 63920 B není target regresí.
Bez změny zůstávají EPG kapacity, 50KiB SLS buffer, JPEG/PNG dekodéry i původní
12000B DRAM a 4096B 32bit/EXEC rezervy. Capability pooly se nesčítají jako nezávislá RAM.

Pro stejnou dynamickou startup konfiguraci znamená úspora modelově free8
23468 + 4064 = **27532 B**, tedy nad prahem 26448 B. Po metadatech by zbývalo
13084 B. To je **výpočet z původního logu**, nikoli nové HW měření; largest block
ani free32 nelze tímto výpočtem zaručit. Skutečné free8/largest8/free32/largest32,
stack HWM a core=1 po bootu: **NOT RUN**, čeká na přijímač testera.

Servisní build na stejné úzké konfiguraci modelově nestačí: 23468 + 1904 = 25372 B.
Proto není automatickou náhradou běžného buildu pro hlavní RF test a nesmí se
kvůli němu snížit rezerva. Hlavní test používá detail=0, který obsahuje UA/signal/DSRV
samostatně a všechna potřebná souhrnná data. Dodatečná CPU latence na HW: neměřena.

## Testy a rizika

Všech **38 kroků hostitelského runneru PASS**, všechny tři target buildy PASS.

| Oblast | Důkaz | Výsledek |
|---|---|---|
| Saturace, kompaktní/plné banky, maxima formátů | test_dab_loss_diag.cpp pro obě konfigurace | PASS |
| První CTS/STATUS3, stale generation, wrap, plný UART, žádné extra SPI | test_dab_cts_ua.py, skutečné completion/recovery funkce | PASS |
| UA sken, mezní hodnoty, doba/resume, nový kontext, replay | stejný test skutečného parseru + vazby periodického gate | PASS |
| MODE B ACCEPTED vs fyzická Probe completion; D tichý i s experiment flag | nové CTS/UA a loss testy | PASS |
| MODE C plné čtení a SLS handoff, bez Core; návrat A PI/SI | rozšířený test_epg_coexistence.py | PASS |
| SLS segment 0/LAST, vnitřní díra vs switch/context změna | loss a receive_health | PASS |
| Watchdog/DSRV/IRQ/ACK/generace, UART pacing UA a fairness | receive_health, packet_runtime, scheduler | PASS |
| Identický prázdný Directory, CRC, PAD/packet izolace | runtime, coexistence | PASS |
| Skutečný RAW/oracle, PI/SI/logo, čas/DST/archive, Core cache | kompletní EPG sada | PASS |
| PNG včetně Adam7, syntetika, PC klient, úložiště | kompletní EPG sada | PASS |
| Fyzické audio/TFT, GPIO reset, 11A/11B při packet zátěži | nový firmware nebyl flashnut | NOT RUN |
| 5 MHz proti 10 MHz na stejném RF signálu | pouze obě kompilace | HW NOT RUN |

Hlavní riziko je menší tolerance UART špiček; testy potvrzují neblokování a
správné ztrátové počítání, nikoli zaručené doručení každého verbose řádku.
Snapshot je jediný poslední první timeout série, nikoli nekonečná historie.
SLS credibleInternal není důkaz konkrétní RF ztráty: vyžaduje segment nula/LAST,
stabilní kontext a žádné explicitní přerušení. exact říká pouze známý konec bitmapy.

Nevytváří se automatická pauza fyzické packet služby po prázdném Directory.
A/B takové budoucí strategie lze navrhnout až podle výsledku B/C/D/UA a po
samostatném schválení. Není přidána druhá FreeRTOS úloha ani změna SPI/CTS timeoutů.

## Reprodukce a předání

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tests/run_epg_tests.py --compiler C:/msys64/ucrt64/bin/g++.exe --input tests/fixtures/epg/device-monitor-261023-160924.log --reference tests/fixtures/epg/SI4684_MUX11B_EPG_kompletni_dekodovani.zip
pio run -e esp32dev -e esp32dev_spi5 -e esp32dev_diag
```

Měření a report testů jsou v `.pio/cts_audit_baseline/memory.json`, `build_after.log`,
`log_analysis.json` a `.pio/epg_acceptance/results.json/.log`; `.pio` není trvalý zdroj vstupů.
Ověřovací firmware není automaticky nasazen. SHA-256 binárek z tohoto buildu:

| Varianta | SHA-256 firmware.bin |
|---|---|
| esp32dev | 2a5319638c6b25a56de1417d5097c4bfb7dfaee5231c28d18b08ae1d771c6998 |
| esp32dev_spi5 | a9a428ff5ece521b40f2c26985b8602ea15e754ed1b5c42c16d13c2b4c73ff75 |
| esp32dev_diag | 5be809b246f5cae95d20a801c091453f184ded65169ae2ff4be3836ee26fe74b |

Změněné produkční soubory: dab_loss_diag.h, dab_loss_diag_output.h,
dab_cts_evidence.h (nový), serial_monitor.h, epg_transport_diag.h,
dab_spi_discovery.h, si4684.cpp/.h, comms.cpp, hlavní sketch a platformio.ini.
Změněné testy, Serial_protocol.md a README dokumentují skutečnou výslednou sémantiku.
Vendor, Core, hodiny, GUI, JPEG/PNG dekodéry, NVS i fixture vstupy jsou beze změny.

Externímu testerovi je určen samostatný [polský protokol](TEST_SI4684_MUX11A_11B_CTS_DSRV_PL.md).
Nejprve ověřit standardní core=1 a rezervu; potom nový boot pro každý A/B/C/D,
11B referenci a případně UA pause/5MHz. Porovnávat první timeout a STATUS3,
nikoli jen osmou chybu, a neprovádět závěr z neúplné SLS bitmapy.
