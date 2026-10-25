# PI priorita a bezpečný SPI start po 500 ms

Schválené lokální doplnění podle zadání z 25. 10. 2026. **T01–T16 PASS
v hostitelských testech; celá sada 26 kroků PASS; build esp32dev PASS.**
Firmware nebyl nahrán. Měření skutečného zrychlení, RF/audio/SLS/FIFO/stack:
**NOT RUN / LIVE RF NOT VERIFIED**.

## Výchozí stav a konečná změna

HEAD `116758e5bd05927f70f6884a332a845e0e60369b`; pracovní strom už obsahoval
schválené časové/SI-only/archivní opravy a packet/X-PAD izolaci. Ty zůstaly
zachované. Nejde o čistý HEAD z původního zadání.

Priorita vhodného PI před SI již byla implementovaná v Core::wanted().
Produkční baseline také už byla 500 ms: původní request vytvořil 5000ms baseline,
načež scheduler dodatečně zavolal productionBaseline(). Log přesto hlásil 5 s
a 30 s pozorování i pro průběžný produkční režim. Nová změna:

- Probe::request přijímá baselineMs přímo; DAB::requestSpiProbe rozlišuje
  produkční požadavek explicitním parametrem. Produkce předává 500 ms, ruční
  DEBUG SPI 5000 ms. Dodatečné přenastavování deadline se odstranilo.
- Log hlásí skutečné mode=LIVE/DEBUG, baseline=500/5000 ms a
  observe=continuous/30s. START/STOP FSM, cleanup, ochrany locku/kontextu a
  priorita ostatní práce scheduleru zůstávají zachované.
- Core::wanted má navíc podmínku nenulové selected_ identity pro přednost PI.
  Test prokázal, že dříve selected_=0 a PI bez beareru mohlo chybně získat
  přednost před SI. Neodvozuje se žádná cizí identita.
- Přibyla malá DEBUG časová osa pro PACKET/XPAD s milníky t0–t8 a N/A pro
  nezachycené kroky, bez dalšího Core/bufferu nebo hardware read.

Focus zůstává první. PI jiného beareru, konflikt nebo nevhodný datový interval
neblokuje SI. SI-only cesta zahrnuje logo i s count()==0. Historická data se
nepřepisují na rok 2026. Žádná změna časového parseru, I²S, DSP, GPIO, tuner
knihovny, PNG/Adam7 implementace či flash layoutu není součástí tohoto doplnění.

Při ověřování se ukázalo, že dotaz wanted() má vlastní stav a jeho dodatečné
volání v testu mění rozhodování. Širší pokus o držení metadata cíle zhoršil
regresi skutečného RAW carouselu, proto se do konečného řešení nepřevzal.
Zachovaný algoritmus znovu načte 68 pořadů Trójky s NOW/NEXT během původních
dvou testovacích průchodů. Test nedokončeného PI nyní ověřuje skutečné přijetí
SI po časovém limitu, bez vloženého dotazu měnícího průběh. Žádná deklarace
nového plošného zrychlení SI/PI z tohoto experimentu nevzniká.

## Výsledky T01–T16

| Test | Výsledek a skutečný rozsah |
|---|---|
| T01 | PASS: přesně vybraný PI má přednost před SI; explicitní focus ji přebije. |
| T02 | PASS: kompletní PI → SI → vlastní Adam7 PNG; SI nesmaže 3 pořady ani NOW. |
| T03 | PASS: SI-only načte SI a vlastní PNG bez PI; navíc skutečný Kraków/OFF SI-only v live testu. |
| T04 | PASS: PI cizího beareru nemá přednost před SI. |
| T05 | PASS: nulová/neznámá/konfliktní identita → SI; očekávané selhání nulové identity na předchozím Core reprodukováno. |
| T06 | PASS: historický interval nemá přednost před SI/PNG, žádné přepsání roku; skutečný historický RAW nemá živé NOW. |
| T07 | PASS: 19 prázdných packet Directory nepoškodí PAD 3 pořady, selected, NOW/NEXT ani Ready. |
| T08 | PASS: packet a PAD používají shodný Directory TID8CC2, data se neslepí. |
| T09 | PASS: packet↔PAD, rozpracované segmenty, potvrzená negativní UA; neúplná/failed UA ani duplicitní signál PAD nemažou. |
| T10 | PASS: skutečný scheduler predicate drží produkční baseline v 499 ms, v 500 ms žádá START. |
| T11 | PASS: manual drží 4999 ms, v 5000 ms žádá START; test ověřuje skutečný text obou režimů. |
| T12 | PASS: lock/tune/service/request změna během baseline končí bez START; zachované context/SLS guardy. |
| T13 | PASS: obě baseline přes UINT32_MAX, existující STOP backoff přes rollover. |
| T14 | PASS: nejistý START → konzervativní STOP; bounded retries/CleanupFailed zachované. |
| T15 | PASS: skutečné DSRV větve pro SPI PAD, SLS/DLS a cizí SID/CID; legacy handoff izolován. |
| T16 | PASS: opakovaný carousel ponechá 3 pořady a 551B vlastní logo bez duplikací/resetu. |

Test T01–T06/T16 používá uchovaný v2 syntetický stream s normovým časem,
SI/PI a Adam7 PNG. Hraniční stavy cíle jsou v host harnessu nastavené přes
privátní Entry; vlastní objekty se dekódují produkčním Core. Referenční soubory
se neměnily. Syntetické hodiny používají platný virtuální den fixture 2024;
nejde o tvrzení, že historický program odpovídá dnešnímu RF roku 2026.

T07–T09 používají plný skutečný DsrvHeader case, skutečnou publikaci UA,
discovery a runtime/Core. Přibyl třikrát packet STOP/START s 500ms baseline
při stále aktivním PAD a callbackem stejného endpointu. Nečekané resety=0,
1 očekávaný packet→PAD handoff. Chip I/O a jeho odpovědi jsou mockované;
nejde o skutečné RF START/STOP na desce. SLS/DLS se ověřuje na předání původní
cestě; nové fyzické dekódování slideshow/audio není tímto testem dokázané.

Celá sada obsahuje 26 kroků včetně kompilací, RAW/oracle, cache/UART, clock,
UI, probe/discovery/inventory, syntetických cest, storage a PNG/Adam7.
Skutečný RAW zachovává 70 objektů a 1 087 pořadů. Výsledky a mapování
jsou v [JSON](EPG_PI_PRIORITY_results_20261025.json).

## Časová diagnostika

`EPG ACQ` se objeví pouze při DEBUG ON, maximálně jedna změněná větev za
250 ms a při alespoň 320 B UART queue headroom. Body se zachytí jen jednou
v kontextu; při DEBUG OFF se stopa zahodí. Žádné průběžné HEX výpisy.
Podrobná definice je v [Serial_protocol.md](../Serial_protocol.md):

| Bod | Událost |
|---|---|
| t0 | Audio kontext dosáhne Ready/AudioOnly po ustálení. |
| t1 | Validovaný endpoint. |
| t2 | Packet probe/baseline vytvořen. |
| t3 | Packet START požadován schedulerem. |
| t4 | START potvrzen úspěšnou odpovědí. |
| t5 | Kompletní validní owner Directory. |
| t6 | Dokončené PI vybrané stanice poskytuje cache požadovaného dne bez konfliktu. |
| t7 | Přehled EPG skutečně vykreslí platné LIVE NOW nebo NEXT. |
| t8 | SI a vlastní PNG vybrané stanice jsou dostupné společně. |

Čísla jsou ms od prvního zachyceného bodu dané větve, ne absolutní millis.
Rozdíl známých hodnot určuje interval. N/A je nezachycený nebo nepoužitelný
bod; u XPAD zůstává t2/t3/t4 N/A. T7 vyžaduje otevřený přehled, jinak
netvrdí skutečné zobrazení. UI test ověřuje přímo tento kreslicí hook.
TEST/replay nepředstírá živé měření. Pozdní DEBUG nic nedopočítává zpětně.
Host testy ověřují rollover, první zápis, context/path izolaci, chybějící body,
UART headroom 319/320 a neopakování stejného výpisu.

## Latence a paměť

Proti starému 5000ms produkčnímu čekání: **EXPECTED 4500 ms less startup
baseline**. Předchozí lokální firmware však už používal 500 ms, takže tato
úprava sama o sobě nepřináší dalších 4,5 s. Čas do DIR/PI/UI závisí také na
pořadí/periodě carouselu a vytížení scheduleru. HW měření před/po nebylo
provedeno, proto není uvedeno vymyšlené celkové zrychlení.

Finální build: RAM **67 248 / 327 680 B**, flash **3 553 337 / 4 128 768 B**.
Proti předchozímu lokálnímu transportnímu buildu je statická RAM +120 B a
flash +1 540 B. Target compiler/nm ověřil sizeof Trace=**124 B**, Core zůstává
**14 232 B** + **36 864 B** bufferů = **51 096 B**. Rozdíl linker RAM a samotné
sizeof souvisí s rozložením statických symbolů; nevznikla nová heap alokace.
Maximální nový lokální UART formátovací buffer má 320 B na stacku jen při výpisu.
Skutečná stack rezerva a FIFO/audio dopad zatím HW NOT TESTED.

firmware.bin: **3 553 952 B**, SHA-256
`7c5dbe6c7f8734dd7a3595990560af7937254cff4c926a310aa186fb10e425cc`.
NVS, flash zařízení, Git index ani GitHub se neměnily. Build pouze vytvořil obrazy.
Externí odstranění IRReceiver se nezahrnuje do přírůstkového patch.

## Opakování a RF ověření

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tests/run_epg_tests.py --input tests/fixtures/epg/device-monitor-261023-160924.log --reference tests/fixtures/epg/SI4684_MUX11B_EPG_kompletni_dekodovani.zip
pio run -e esp32dev
```

Pro živé porovnání po samostatně schváleném nahrání zapni DEBUG před volbou
stanice. Zachyť `EPG ACQ`, skutečný baseline log, STATUS a TRANSPORT, otevři
EPG přehled a sleduj t7. Měř alespoň několik celých carouselů na stejném MUX,
stanici a stabilním signálu. Sleduj CTS/overflow/UARTdrop a sluchem audio,
vizuálně DLS/slideshow; porovnávej režimy bez změny signálu či datového zdroje.

Na 11B vyber stanici s deklarovaným vhodným PI a samostatně SI-only Kraków.
Při historickém programu musí zůstat archiv/stale; jeho stáří se nesmí maskovat.
Na 11A použij [samostatný český RF postup](EPG_MUX11A_XPAD_RF_TEST_CZ.md).
422 validních prázdných packet skupin z původního logu dokazuje pouze prázdný
packet carousel, nikoli nepřítomnost SPI přes audio X-PAD. Skutečný X-PAD na
11A zůstává NEOVĚŘENO. Nový firmware se v rámci této práce nenahrával.

Přírůstkový [patch](patches/EPG_PI_PRIORITY_20261025.patch) platí nad lokálním
stavem po předchozí transportní opravě; neobsahuje znovu časové/SI/archivní
změny ani historické patche. Výchozí stav je lokálně zachycený pro kontrolu diffu.
