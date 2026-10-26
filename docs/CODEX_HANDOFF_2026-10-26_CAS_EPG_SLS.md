# Předání pro nový chat — čas DAB/EPG a barvy slideshow

Datum: 26. 10. 2026 (Europe/Prague).
Projekt: `C:\Dokumenty\GitHub\SI4684-FMDAB-Receiver`.

## Nejdůležitější stav

**Úkol má ještě nedokončené závěrečné ověření a dokumentaci.**

**Aktualizace od uživatele po vytvoření předání:** „poslední úprava je nahraná a otestovaná - ztráty SLS a resety“.
Poslední verze je tedy podle uživatele již v rádiu a prošla jeho HW testem ztrát SLS a resetů.
Tuto informaci respektovat, nehlásit plošně HW NOT TESTED ani navrhovat opětovné nahrání téže verze.
Není přiložen nový log, přesný hash binárky ani výsledky všech C01–C16 na HW; ty nezaměňovat s potvrzeným SLS/reset testem.
Uživatel nyní požádal o tento předávací soubor. Pokračovat z existujících změn, nezačínat znovu.

Při poslední kontrole byl HEAD `e24bc2ac84349dc6f1a9f07ae59b68b4d6a4fd9c`,
commit „backup, both load of SI/PI, time stability, SLS loading timeout finetune, watchdog for SI4684 halt“.
Před vytvořením tohoto souboru byl working tree čistý. Tento commit vznikl mimo naše příkazy;
asistent žádný commit ani push neprovedl. Obsahuje předchozí opravy i aktuálně rozpracovanou práci.
Při začátku této práce byl HEAD ještě `e2ea822378e232c70906dfe4ba2dd2c6005d42d1`
a řada starších schválených změn nebyla commitnutá. **Nevracet projekt na starý HEAD.**

## Zadání a souhlas

Aktivní zadání:
`Specification/CODEX_ZADANI_SI4684_KONTROLA_AKTUALIZACE_CASU_EPG_2026-10-26.md`.
Uživatel: „ověř a proveď změny dle zadání a uprav barvy ikony slideshow dle epg logiky“.

Trvalé preference: před změnami vyžádat souhlas, bez změn na GitHubu;
nevytvářet samostatný report, RF návod anglicky, případně aktualizovat Serial_protocol.md.
Tento předávací dokument je nyní výslovně vyžádaná výjimka.

Uživatel výslovně schválil celý následující rozsah:

- Publikace EPG času až po platné sekvenci LOCAL → UTC → LOCAL.
- Zachování posledního potvrzeného času do 300 s, oddělené generace a ochrana EPG kontextu.
- Validace větších skoků času, testy C01–C16, diagnostika, Serial_protocol.md, anglický RF návod a build.
- Slideshow: červená čekání/načítání, cyan připravený obrázek, šedá signalizovaná, ale nepoužitelná data;
  bez signalizace a ve FM skrytá. Ukazatel načítání červený.
- **Bez flashování, změn NVS, GitHubu a samostatného reportu.**

Tento souhlas platí pro dokončení uvedených úprav a testů. Nežádat o stejný souhlas znovu.
Pro případné další nahrání firmware je nutný nový výslovný souhlas.
Pozdější zpráva uživatele potvrzuje, že poslední verzi již nahrál a otestoval pro SLS a resety; nejde o pokyn k dalšímu uploadu.

## Prokázaný mechanismus problému

Původní `DAB::finishDabCommand()` při Time a TimeUtc přepisoval jediný veřejný
`dabEpgClock` neúplným vzorkem. Generace rostla už prvním LOCAL krokem.
`epg::poll()` tedy během normální obnovy viděl nedůvěryhodné hodiny a zakázal NOW/NEXT.

`epg::liveContext()` navíc při neplatném čase volal `Core::select(...,0,0)`.
To mohlo změnit offset/denní kontext; `Core::activateContext()` resetuje rozpracované body.
**Zákaz NOW/NEXT, změna kontextu s resetem rozpracovaného MOT a smazání celé cache jsou různé věci.**
Nebylo prokázáno, že samotný refresh vždy přímo mazal celou cache.
Nové integrační testy přímo kontrolují count, bearer, den, revision, PI/SI/PNG, logo a masky fragmentů.

## Již implementované změny

### `src/dab_clock_refresh.h` — nový malý stavový automat bez alokací

`epg::ClockRefresh` drží kandidáta a případný skok pro nezávislé potvrzení.
Publikovaný `BroadcastClockSample` zůstává oddělený v DAB.

- Stavy Idle / AwaitLocal / AwaitUtc / AwaitConfirm / Failed.
- Důvody odmítnutí parse, order, pair-window, offset-mismatch, jump, timeout, lock-loss, tune-changed.
- Párování používá stávající `pairedOffsetAfterUtc()`, limit 2 s a normové půlhodinové offsety.
- Limit celé aktivní sekvence 10 s; chyby spouštějí backoff 5/10/20/30 s.
- Publikace vzorku i nové generace až po dokončeném potvrzení.
- Odchylka UTC přes 5 s vyžaduje druhou úplnou sekvenci; shoda do 3 s,
  odstup alespoň 1 s a maximálně 120 s. DST mění offset, nikoli UTC.
- Při počáteční akvizici / již prošlém starém vzorku není přijímač trvale uzamčen na starém čase.
- Kontext ladění a RF lock ruší kandidáta; potvrzená generace je nezávislá na generaci lokálního UI času.
- Metody se jmenují `acceptLocal` a `acceptUtc`: jméno `local` kolidovalo s makrem při target buildu.

### `src/si4684.cpp`, `src/si4684.h`

- `broadcastClock()` vrací jen potvrzený `dabEpgClock`; přidán read-only `clockRefresh()`.
- `maintainDabClock()` se volá z Update a scheduleru; sleduje generaci, lock a deadline.
- Skutečné větve Time/TimeUtc/TimeLocalConfirm používají kandidáta; validují i `epg::epoch()`.
- Lokální Year/Months/Days/Hours/Minutes/Seconds zůstávají lokální pro hlavní obrazovku.
- Chybová cesta časových příkazů v finishDabCommand ukončuje kandidáta a nesype dlouhé opakované výpisy.
- Retry využívá existující kooperativní low-priority scheduler; žádné delay ani blokující CTS čekání.
- Nový `slideshowSignalled()` a `slideshowGeneration()` pro UI.
- SLS evidence se uchovává pro aktuální SID/CID při rutinním UA rescan,
  ale nesmí se přenést na jinou službu. Podporuje PAD i packet SLS.

### `src/epg_runtime.cpp`

- `liveContext()` při chybějícím platném čase nemění den/offset stejné služby.
  Jinou identitu služby řeší odděleně, se zachováním bezpečného offsetu.
- RF reacquisition gate požaduje novou **validní, potvrzenou** generaci, ne první LOCAL.
- `EPG TEST STATUS` doplněn o řádek `EPG CLOCK_REFRESH`:
  acceptedGeneration, candidateState, lastAcceptedAgeMs, candidateAgeMs,
  offset, clockTrusted, refreshAttempt, ok, reject, timeout, reason (včetně stale).
- Čas TEST/TIME nepřepisuje živé hodiny.

### `src/gui.cpp`, `SI4684-DAB-Receiver.ino`

- `ShowSlideShowIcon()` teď skryje ikonu bez signalizace / ve FM, čekání červeně,
  připravený obrázek cyan, 120 s bez pokroku šedě; jde pouze o UI časovač.
- Obnovení pozadí jen v oblasti ikony, kreslí jen při změně nebo vynuceném redraw.
- Loading popup a ukazatel/progress runner jsou červené.
- Průběžný loading renderer již nepřekresluje stavovou ikonu přes výsledek hlavního rendereru.
- Dosavadní fyzické SLS timeouty a sběr dat nebyly kvůli UI barvám změněny.

## Testy — přesně známý stav

### PASS v tomto rozpracovaném úkolu

1. `tests/test_dab_clock.py` byl přepsán; používá nový `tests/dab_clock_harness.py`.
   Harness vyřezává skutečné produkční completion/error/scheduler větve a maintenance,
   mockuje jen čipové odpovědi a monotónní čas.
   PASS: C01/C04, C02/C03/C15 (11 minut), C05/C06, C07, C08, C10, C11, C14.
2. `tests/test_epg_runtime.py` byl rozšířen propojením stejného skutečného driver harnessu
   s reálným `epg::poll()` a Core, PI/SI/Adam7 PNG.
   Poslední spuštěný proces měl ID 86002; session už není dostupná.
   Při tvorbě tohoto předání byl přímo znovu spuštěn jeho poslední vytvořený runtime.exe:
   **exit 0, všechny níže uvedené testy PASS**:
   - >10 minut platného READY, stejný bearer/den/revision/PI/SI/PNG/logo i rozpracovaný MOT;
   - failed refresh a 300s TTL zakážou NOW/NEXT, zachovají cache a rozpracovaný MOT;
   - C09 pending LOCAL nemění den, potvrzená extrapolace legitimně překročí půlnoc;
   - C10 návrat locku vyžaduje dokončenou novou generaci;
   - C16 TEST/TIME/FM/LIVE izolace;
   - původní UART/DEMO/indikátor/ownership/heap rollback testy.
3. `tests/test_epg_coexistence.py`: A–F PASS po doplnění mock API a SLS polí.
4. `tests/test_epg_live.py` PASS před posledním doplněním explicitních kontrol zachování metrik.
   **Nejnovější doplněnou verzi ještě znovu spustit.**
5. Target build prošel po změnách produkčních zdrojů:
   RAM **67544 B**, flash **3558373 B**, firmware.bin **3558992 B**.
   Před tímto úkolem RAM 67432 B, flash 3556665 B; rozdíl +112 B RAM / +1708 B flash.
   Core na ESP32 zůstává **63600 B**. Host Core má odlišné zarovnání a hlásí 63920 B — nejde o target regresi.
   Podrobné porovnání DRAM vs IRAM a závěrečný build ještě doplnit.

### Co ještě není ověřené / hotové

- Kompletní regresní sada **nebyla spuštěna znovu pro tento clock/SLS úkol**.
  `.pio/epg_acceptance/results.json` s 31 PASS patří předchozímu dokončenému úkolu!
- `tests/test_dab_packet_runtime.py` byl právě rozšířen o skutečný SLS accessor a nové assertions;
  tato nejnovější verze zatím nebyla spuštěná.
- Nové skutečné kreslicí testy slideshow stavů/barvy/obnovy pozadí ještě nejsou napsané.
- Dokumentace nového clock automatu, CLOCK_REFRESH a změněných slideshow barev ještě není aktualizovaná.
- Matice C01–C16 s přesným mapováním na testy zbývá doplnit např. do stávajícího tests/README.md,
  bez vytváření samostatného reportu. C12/C13 už pokrývá live/runtime sada, ověřit a označit.
- Podle nového potvrzení uživatele je poslední úprava **nahraná a HW otestovaná pro ztráty SLS a resety**.
  Kompletní fyzická matice časových scénářů C01–C16 zatím není doložená.
  Další nahrávání není tímto hlášením požadováno ani automaticky schváleno.

## Důležité detaily testovacího prostředí

- Python default encoding je cp1250; pro soubory/logy používat explicitní UTF-8.
- `C:/msys64/ucrt64/bin/g++.exe`.
- PlatformIO `C:/Users/alesb/.platformio/penv/Scripts/pio.exe`.
- Target toolchain `C:/Users/alesb/.platformio/packages/toolchain-xtensa-esp32/bin/`.
- Exec volání vyžadovala `sandbox_permissions: require_escalated` (jinak chyba spawn 740).
- Nevytvářet `__pycache__`: `$env:PYTHONDONTWRITEBYTECODE='1'`.
- Zachovat CRLF hlavně v src/si4684.cpp, src/language.h; hromadná normalizace dělá obří diff.
- Neměnit fixture vstupy, vendor kód, kodek, fonty ani ostatní grafiku.
- Nespouštět zbytečně stejné testy po úspěchu, pokud se jejich vstupy nezměnily.

Příkazy pro dokončení (z kořene projektu):

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tests/test_dab_clock.py C:/msys64/ucrt64/bin/g++.exe
python tests/test_epg_live.py C:/msys64/ucrt64/bin/g++.exe
python tests/test_epg_runtime.py C:/msys64/ucrt64/bin/g++.exe
python tests/test_dab_packet_runtime.py C:/msys64/ucrt64/bin/g++.exe
python tests/run_epg_tests.py --compiler C:/msys64/ucrt64/bin/g++.exe --input tests/fixtures/epg/device-monitor-261023-160924.log --reference tests/fixtures/epg/SI4684_MUX11B_EPG_kompletni_dekodovani.zip
& C:/Users/alesb/.platformio/penv/Scripts/pio.exe run -e esp32dev
git -c core.whitespace=cr-at-eol diff --check
```

## Doporučené další kroky

1. Přečíst zadání a ověřit aktuální HEAD/status; respektovat již existující změny a schválení.
2. Audit ClockRefresh/driver/runtime: pořadí odpovědí, generace, chyby, retry, TTL a skoky.
3. Dopsat test rendereru slideshow (skrytá/červená/cyan/šedá, minimální redraw, progress, millis wrap).
   Zvážit zejména přechod po skutečném abortu SLS: reset počítadla bajtů na nulu teď restartuje
   120s UI čekání; zkontrolovat, zda má být v tomto případě ihned šedá. Nezaměnit s novým TID.
4. Spustit nejnovější cílené testy a celou sadu, opravit skutečné závady v autorizovaném rozsahu.
5. Aktualizovat Serial_protocol.md (pending ≠ invalid; nevalidní čas ≠ smazaná cache),
   tests/README.md a existující anglický `docs/SI4684_RX_RF_TEST_EN.md`.
6. Závěrečný build, paměťové porovnání a kontrola diffu. Žádný commit/push/flash/NVS.
7. Předat stručný výsledek a stav fyzických testů; pro upload si vyžádat souhlas, pokud o něj uživatel požádá.

## Předchozí dokončená práce — nevracet zpět

Před tímto clock úkolem byly implementovány a na HW otestovány:

- Dvě RX assembly pro EPG MOT, PI priorita, příjem MSC i X-PAD, streamy a loga včetně Adam7.
- DSRV/CTS/watchdog diagnostika, validace SLS duplicit a single MOT buffer.
- EPG indikátor: bez SPI skrytý, čekání červené, použitelná aktuální SI/PI cyan,
  nepoužitelná/prázdná data a 120s čekání šedé; bez destruktivních resetů.
- ROT1 na NOW/NEXT přehledu listuje NEXT/další, opačně zpět, klik otevře seznam na výběru;
  neovlivňuje skutečné hodiny/NOW, při listování překresluje jen programovou oblast.
- Předchozí slideshow měla cyan loading / green ready / grey absent; aktuální úkol ji mění podle EPG.
- Stávající lokalizace, fonty, menu a I2S se mají zachovat.

Dřívější HW test provedený asistentem před aktuálním clock úkolem (novější uživatelské potvrzení je nahoře):

- Firmware s EPG indikátorem a ROT1 byl nahrán **jen do app oddílu 0x10000**, kontrola hashe OK,
  bez změny NVS. COM4 CH340, Si4684 FW 6.0.9, ESP32-D0WD-V3 rev3.1.
- Uživatel potvrdil červená → šedá, zvuk bez výpadků, cyan po načtení, ROT1 i logo v pořádku.
- 9 syntetických MSC/PAD scénářů, standard/Adam7 PNG × oba transporty, 125s zachování partial,
  opakovaná přesná 11A empty Directory prošly.
- Historický RAW Trójky: 68 pořadů + SI + vlastní PNG + NOW/NEXT.
- Jeden RAW pokus s DEBUG zaplavil UART a selhal; opakování po restartu s podrobnou
  diagnostikou vypnutou prošlo, UARTdrop/CRC/CTS/overflow/recovery = 0.
- Na konci tehdejšího testu LIVE, DEBUG OFF, COM4 uvolněn. Asistent nyní nemá spuštěný monitor ani sender; aktuální použití portu uživatelem neověřoval.
- Místní RF: 11B, 218640 kHz, EID 2064, SID 2FD9, signalizuje slideshow, **nikoli SPI**.
  Skutečný RF příjem EPG tím není prokázaný. Syntetika není test RF odbalení X-PAD v čipu.

Užitečné dočasné artefakty (ignorované `.pio`, mohou zmizet):

- `.pio/epg_acceptance/clock_before/` — kopie zdrojů před aktuálním clock úkolem.
- `.pio/dab_clock/clock.cpp`, `clock.exe` — driver harness.
- `.pio/epg_runtime_test/runtime.cpp`, `runtime.exe`, `lines.txt` — runtime integrace.
- `.pio/epg_acceptance/hw_indicator_test.py/.log` — předchozí HW test indikátoru.
- `.pio/epg_acceptance/hw_indicator_raw_quiet.py/.log` — úspěšný předchozí HW RAW.
- Testovací vstupy jsou trvale v `tests/fixtures/epg`, nepřepisovat je.
