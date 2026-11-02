# Audit DSRV RAW/QUEUE a CTS — 2. 11. 2026

Lokální implementace a host ověření jsou dokončeny. Fyzické RF testy ani měření RAM tohoto buildu na přijímači nebyly provedeny. Firmware nebyl nahrán. Kandidát vyžaduje kontrolu paměti a `core=1` podle [polského návodu](TEST_SI4684_DSRV_QUEUE_INSTRUKCJA_PL.md), než začne vlastní RF měření.

## Výchozí stav a audit

Před editací byl pracovní strom čistý, HEAD `d048c72224d36c34ee43b025300c5d6b180efa7a`. Blob `src/si4684.cpp` byl `f0b1063a68bfea9e60c87e635c4588dc35f05a30`; proti d048c72 nebyl výchozí rozdíl. Nebyl nalezen další AGENTS.md. Nové změny nejsou commitnuté, takže samotné HEAD neidentifikuje tento kandidát: je třeba uvádět i lokální změny a SHA-256 aplikace.

Prohlédnuty byly `src/si4684.cpp/.h`, `src/vendor/si468x/Si468x.h`, `src/dab_loss_diag.h`, `src/dab_receive_health.h`, `src/dab_cts_evidence.h`, `src/dab_scheduler_policy.h`, `src/serial_monitor.h`, `src/comms.cpp`, kořenový sketch, `platformio.ini`, EPG runtime a související host testy. Skutečný parser je v `src/comms.cpp`, volaný přes `Communication()` z hlavní smyčky. Po něm se obsluhuje EPG a odložená diagnostika. UART má původní frontu 2048 B a obsluhu nejvýše 64 B na volání; příkazy přijímají CR, LF i CRLF. `DEBUG` přepíná stav, není to příkaz `DEBUG OFF`.

Lokálně dostupné a přečtené vstupy:

- `_usertesting/20261029/Dla_Alesa_d048c72_testy_11A_11B.zip` a sousední `test.txt`;
- `_usertesting/20261026/EPG_test_e2ea822_11A_11B.zip`;
- `_usertesting/Logs_4a4b2d2_memory_and_watchdog.zip`;
- `Specification/AN649 Programming Guide Rev 1.9.pdf`, zejména strany 43–44 a kapitola 7.7 kolem stran 401–404;
- pracovní předloha `Specification/TEST_SI4684_DSRV_QUEUE_INSTRUKCJA_PL.md`, ponechaná beze změny.

AN649 je Rev. 1.9, únor 2015. Omezení na osm čekajících bloků v této verzi dokumentu není důkazem kapacity konkrétního FW 6.0.9. Aktuální API i fyzický SPI framing byly porovnány s dokumentem.

## Co skutečně dokazují dodané logy

ZIP d048c72 má SHA-256 `19329ea3d15f160e301873e2659ddb5298e6002820ab78222000ff6d1d5bd3b1`. Přehrané výpisy CTS se odlišují podle stejného `atMs`, nepočítají se jako nový timeout.

| Log | Unikátní CTS-FIRST | WATCHDOG v dostupném souboru |
|---|---:|---:|
| 11A A | 4 | 3 |
| 11A B | 0 | 0 |
| 11A C | 2 | 2 |
| 11A D | 4 | 4 |
| 11A 5 MHz | 1 | 1 |
| 11A UA PAUSE | 2 | 2 |
| 11B A | 0 | 0 |

Důležitá odchylka od souhrnu: dostupný C log má 43722 B, končí v 16:41:09.798 a neobsahuje třetí unikátní CTS-FIRST. První `16:39:01.984 / atMs=41798` předchází ACK C `16:39:05.574`; výpis v `16:39:29.443` je jeho opakování. Teprve `16:39:36.619 / atMs=76419` dokládá selhání po přijetí C. Dostupný podklad tedy potvrzuje jeden takový případ v C, ne tři. Watchdogy jsou v 16:39:11.359 a 16:39:43.703. Časy jsou převzaty z logu, údaje souhrnu o Europe/Warsaw nejsou odvozeny ze samotných prefixů.

11A má pozorované `queueMax=19`, vyšší packetovou zátěž a opakované prázdné Directory; 11B má maximum 2–3 a stabilní běh s 1912 validními packet skupinami. B zůstal 15 min 22 s bez CTS, s packet=0 a PAD zachovaným. D selhává i při UARTdrop=0. Test 5 MHz ani UA PAUSE selhání neodstranil. V typických cca 30s intervalech bylo na 11A 434–533 startů DSRV proti 109–113 na 11B, packet 379–479 proti 53–57, PAD obdobně 53–55 proti 55–56. Řádky s jiným `ms` se nesmějí srovnávat jako přesně 30s vzorky.

Staré logy neobsahují původní raw24. Číslo 19 proto dosud může znamenat například:

1. rozdíl významu/jednotek/rozsahu pole ve FW 6.0.9 proti starší AN649;
2. chybu framingu, indexování či fyzického SPI přenosu;
3. přepsání nebo záměnu sdíleného bufferu a kontextu odpovědi.

Nové testy prověřují softwarové body 2 a 3, ale neprokazují bezchybný přenos na fyzické desce. Interní závada DSP nebo napájení zůstává také možná. Žádná příčina není vybrána jako prokázaná. `overflow=0`, validní CRC nebo opakovaný prázdný Directory samy neprokazují úplnost přenosu ani opakování stejné fyzické odpovědi. Deklarovaný SHA aplikace testera EFCD265BA43F36AF24C23AD8E63C81D70A032AF1F3E0F837A2C9C4F753B9041E nebyl ověřen, protože původní binární aplikace v podkladech není.

## Zachycení a validita dat

`hostReadReply()` zpracuje fyzický framing byte zvlášť. Logická odpověď začíná `SPIbuffer+1`, nikoli prefixem `SPIbuffer[0]`. Nový hook je v úspěšné větvi `DsrvHeader`, po `parseDsrvHeader(...,24,...)` a kontrole generace, před všemi existujícími full-reply čteními a dispatchi. Neprovádí žádný nový SPI dotaz.

| Logický offset | Obsah |
|---|---|
| 0–3 | původní STATUS0–3, včetně CTS / ERR_CMD |
| 4 / 5 / 6 | INTSRC / přesný BUFF_COUNT / SRV_STATE |
| 7 | původní DATA_SRC / typ; source je horní dvojbit |
| 8–11 / 12–15 | little-endian SID / CID |
| 16–17 | zachované RFU dle AN649; současné API je také zpřístupňuje |
| 18–19 | little-endian BYTE_COUNT |
| 20–23 | zachované původní bajty, API je interpretuje jako segmentová pole |

`parseDsrvHeader` hlídá ukazatel a délku, nemá hlubokou sémantickou validaci. `headers` tedy znamená úspěšný command/aktuální generaci/24B odpověď přijatou API, nikoli automatický důkaz správnosti reserved bitů či skutečné kapacity FIFO. Všech 24 B zůstává beze změny, `BUFF_COUNT` se nemaskuje ani neomezuje na 8. `STATUS3` je raw hex; čítač chyb sleduje jeho dolních pět bitů. Nestandardní hodnoty se reportují, nepřidávají nový důvod recovery.

Timeout/TransportError/DeviceError mají vlastní čítače, nevytvářejí RAW řádek. Stará generace se počítá zvlášť a nečte starý buffer. Při existujícím full read se zachovaly kontroly SID/CID a BYTE_COUNT; nové čítače jen označují původní důvod odmítnutí. Běžná změna statusu či remaining při dalším čtení není označena jako korupce. `replyHeaderChanged` je součet těchto dvou původních sekvenčních větví; jedna odmítnutá odpověď se nepočítá dvakrát. `ReadOk/ReadFail` se neduplikují.

## Konstrukce, freeze a výstup

Čistý header `dab_dsrv_queue_diag.h` neobsahuje závislost na Arduino, SPI, heapu ani UART. Obsahuje 12 posledních hlaviček a první over8 aktuálního capture. Vzorek má 33 B: raw24, čas4, generace4, režim1. Byte metadata se skládají explicitně, bez nezarovnaných word přístupů na Xtensa.

První skutečně zachycený CTS-FIRST zmrazí ring, první over8, kontext, 76B souhrn a stáří posledních událostí ještě před recovery. Další úspěchy, CTS epizody, změny generace i cold recovery Si4684/TFT ho nepřepíší. Živá BOOT/WINDOW statistika pokračuje. Ring může obsahovat více generací; každý řádek má vlastní `gen` a `mode`. Místo velké kopie se banka pouze přestane měnit.

`DSRV STATUS` read-only vyžádá postupný opakovaný výpis, nezmrazí ani nemaže. Nový `DSRV QUEUE CLEAR` výslovně smaže/rearmuje pouze RAW capture; zachová BOOT i WINDOW statistiky, původní CTS evidence a rádio. V běžném buildu odpoví DISABLED. Automatický clear v begin/reset/tune není.

Reporter používá existující `diagSerial`, ne další Serial cestu. Přednost má původní čekající CTS-FIRST/STATE/RECOVERY, pak nejvýše jeden nový řádek za 100 ms s požadavkem 512 B volného místa. Formátování má 448B stack buffer; maximální hostem ověřený řádek má 354 B. Nedostatek místa nezmění kurzor ani snapshot, zvyšuje `suppressed`. Nevejití formátu/krátký write zvyšuje `reportDrop`. Hot path neformátuje, nealokuje, nehashuje, nečeká a neprovádí heap dotazy.

Ve všech režimech včetně D a DEBUG OFF je v queue buildu krátký WINDOW souhrn nejdříve po 30 s a jednorázové upozornění na první over8; kompletní hex je odložený k freeze nebo na vyžádání. Zbývající tiché chování D je zachované. Health se v queue buildu cacheuje jednou za 30 s i v D. Podrobné BOOT/FROZEN řádky se neprodukují trvale.

Čítač hlaviček je saturující 32bit, dílčí čítače a histogram saturující 16bit. `sat=1` znamená dosažení limitu: hodnoty na limitu jsou dolní meze. BOOT je živý souhrn při odeslání řádku; FROZEN je neměnný BOOT souhrn v okamžiku CTS; WINDOW je skutečný interval `ms` od předchozího úspěšného souhrnu, může překročit 30 s. STATUS toto okno neresetuje. `mixed=1` označuje změnu generace/režimu v okně; neodvozuje se z něj kapacita čipu. `packetMax/padMax` jsou raw DATA_SRC 0/1, nejsou potvrzením EPG ownership.

První over8 RAW je pro aktuální capture. BOOT first/last časy a frozen ages mohou po ručním clear pocházet ze staršího capture/generace; mají vlastní known příznaky. `seq=0` je vyhrazen samostatnému FIRST-OVER8, ring má sekvenci z bootového počtu hlaviček. 16bit reportovací čítače/captureId také saturují. Intervalové odečty jsou uint32_t bezpečné přes wrap millis; delší interval než celý oběh se takto nedá rozlišit.

## Paměť a buildy

| Varianta | .data B | .bss B | DRAM celkem B | Přírůstek B | IRAM text B | PIO flash B |
|---|---:|---:|---:|---:|---:|---:|
| místní baseline d048c72 | 19768 | 47400 | 67168 | 0 | 74747 | 3572189 |
| esp32dev po úpravě | 19768 | 47400 | 67168 | 0 | 74747 | 3572337 |
| esp32dev_queue_diag | 20504 | 47400 | 67904 | 736 | 74747 | 3578485 |

`sizeof(Evidence)=732 B`, zbylé 4 B jsou zarovnání linkeru. Statický hard limit 768 B je splněn, preferovaný cíl 600 B splněn není: zachovaných 12 úplných vzorků, extra first-over8 a tři oddělené sady metrik včetně immutable freeze vyžadují uvedených 732 B. Strop je vynucen static_assert. Normální build nemá symbol dsrvQueue, rozdíl jeho flash je 148 B kvůli potvrzované DISABLED větvi nového ručního příkazu. IRAM ani EPG/SLS kapacity nejsou zmenšeny. Žádný nový heap/payload buffer nevznikl.

Core zůstává 63600 B na targetu (metadata14448 + buffery49152), rezerva 12000 B DRAM a 4096 B executable32. Ze starého fyzického `free8=13068` vychází pouze odhad `13068−736=12332`, tedy rezerva navíc 332 B. **To není měření tohoto buildu.** `largest8`, `free32/largest32`, `stackMinBytes`, skutečné `core=1` a free8 před/po alokaci musí ověřit tester. Pokud Core nevznikne nebo limity nevyjdou, RF běh se nemá hodnotit a rezervy se nesnižují.

Oba příkazy `pio run -e esp32dev` a `pio run -e esp32dev_queue_diag` prošly. Queue prostředí dědí běžné esp32dev a přidává pouze SI4684_DSRV_QUEUE_DIAG=1, nikoli full-detail variantu. Radio SPI10 MHz, TFT SPI27 MHz a partition layout zůstaly stejné. Build skript vytvořil také původní kompletní image, ale žádná image nebyla flashována. Návod používá jen aplikaci na 0x10000.

SHA-256 aktuálně sestavených firmware.bin (konkrétní lokální soubory; jiný čas sestavení může změnit hash):

- baseline: `2a5319638c6b25a56de1417d5097c4bfb7dfaee5231c28d18b08ae1d771c6998`
- esp32dev: `40d6dfb513eb13f02670ebe24f1872741d93d5b2cee7c429945a3703ac805742`
- esp32dev_queue_diag: `03a578e06fd94bb8bd61edaa97a0b12a4fae182314215356ddf9c762111f7290`

## Provedená ověření

Celý `tests/run_epg_tests.py` dokončil **41/41 kroků PASS**, včetně oracle replay dodaného RAW proti zachované ZIP referenci, EPG času/cache/UI/transportů, syntetických streamů, storage, Adam7 i PC UART simulace. Nové testy ověřují:

- fyzický dummy bajt a všech 24 různých offsetů reálného hostReadReply; raw hodnoty 0,1,7,8,9,19,255 bez clampu;
- histogram, saturaci, INTSRC overflow/PHY/ready, oddělené stavy1/2/4 a kombinace, CTS/ERR_CMD/STATUS3;
- neúspěšné, krátké a stale odpovědi bez falešné RAW hlavičky;
- skutečné full-read přepsání bufferu až po capture; povolené změny statusu, odmítnutí stabilních SID/CID/count bez dalšího commandu;
- wrap ring/millis, první over8, skutečný první CTS hook, immutable stopu přes další úspěch/recovery/generaci;
- CTS prioritu, UART backpressure, 12 řádků ve správném pořadí, přesný převod raw24 na 48 hex znaků, délky a ruční STATUS/CLEAR;
- D/DEBUG OFF, smíšená okna i bez nového headeru po změně kontextu;
- skutečný DSRV dispatch/Core při dodatečném 732B DRAM tlaku, reserve12000 a oba transporty. Jde o host model, ne fyzický volný heap.

Porovnání 14 klíčových produkčních funkcí proti HEAD po odstranění čistě podmíněných queue bloků a whitespace bylo shodné: hostWriteCommand, hostReadReply, statusChanged, scheduleNextDabCommand, Update, getServiceData, readDsrvPayload, finishDabCommand, observeDabTerminal, requestDabRecovery, setDsrvDiagnosticMode, begin, onHardwareResetComplete, setFreq. Vendor API, scheduler policy, receive health, původní loss/CTS helpers, Core/parser, čas, GUI a fixtures nebyly změněny. Burst4, ACK01, timeouty, idle poll a watchdog prahy zůstaly zachovány. Změna hlavní smyčky pouze připojuje odloženou diagnostiku a její 30s health cache.

Reprodukce v kořeni projektu:

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tests/run_epg_tests.py --compiler C:/msys64/ucrt64/bin/g++.exe --input tests/fixtures/epg/device-monitor-261023-160924.log --reference tests/fixtures/epg/SI4684_MUX11B_EPG_kompletni_dekodovani.zip
python tests/analyze_dsrv_queue_logs.py _usertesting/20261029/Dla_Alesa_d048c72_testy_11A_11B.zip --output .pio/queue_audit_20261102/log_analysis.json
pio run -e esp32dev -e esp32dev_queue_diag
```

Lokální technické výstupy jsou `.pio/epg_acceptance/results.json` a `results.log`, `.pio/queue_audit_20261102/memory.json`, `log_analysis.json`, `unchanged_functions.json` a `build_after.log`. .pio je pracovní výstup, nikoli povinná součást repozitáře. Tento dokument zachovává hlavní skutečné výsledky.

## Změněné soubory a stav HW

- `src/dab_dsrv_queue_diag.h`, `src/dab_dsrv_queue_output.h`: pevná evidence a oddělený reporter.
- `src/si4684.cpp/.h`, `SI4684-DAB-Receiver.ino`: podmíněné hooky, freeze, health a odložený výstup.
- `src/comms.cpp`: ruční CLEAR s odpovědí, dosavadní příkazy zachovány.
- `platformio.ini`: izolované queue prostředí.
- `tests/test_dab_dsrv_queue.py`, `tests/test_dab_cts_ua.py`, `tests/test_epg_coexistence.py`, `tests/run_epg_tests.py`: nové a integrační regrese, beze změny fixtures.
- `tests/analyze_dsrv_queue_logs.py`: read-only analýza skutečných logů s deduplikací CTS replay.
- `Serial_protocol.md`, `tests/README.md`, tento český protokol a polský návod: skutečné příkazy, výsledky a fyzická akceptace.

Fyzický příjem 11A/11B, zvuk, SLS, GPIO12/POLL, RAM/stack a stopa raw19 na této image: **NEPROVEDENO**. Nový RAW dump v dokumentaci není vydáván za měření z rádia. Zachování audia/SLS na HW musí potvrdit následný test. Žádný commit/push, flash, NVS, GPIO12 ani eFuse nebyl proveden nebo změněn. Až fyzická data rozhodnou o další samostatné hypotéze či experimentu; v této změně není skryta úprava příjmu.
