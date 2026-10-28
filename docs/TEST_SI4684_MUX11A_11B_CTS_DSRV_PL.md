# Test Si4684: MUX 11A/11B, CTS, DSRV i Slideshow

Wersja protokołu testowego: 28.10.2026. Dla Andrzeja.
Nowe oprogramowanie nie zostało jeszcze sprawdzone w tym teście radiowym.
Sam poprawny wynik kompilacji nie oznacza usunięcia przyczyny zawieszeń CTS.

## Cel testu

Oddzielić wpływ pakietowej usługi EPG, dekodowania w EPG Core, okresowego
odświeżania UA, danych X-PAD/Slideshow, diagnostyki UART i zegara SPI.
Nie wyciągamy wniosku o utracie danych w tunerze z jednej niepełnej bitmapy SLS.
Pusty Directory był obserwowany na konkretnym badanym multipleksie 11A;
nie jest cechą wszystkich multipleksów nadających na kanale 11A.

## Wymagany sprzęt i przygotowanie

- ESP32-WROOM-32D bez PSRAM, Si4684, wyświetlacz TFT ILI9341.
- Ta sama antena, jej położenie, zasilacz, przewody i miejsce odbioru dla porównań.
- MUX 11A **216,928 MHz** i 11B **218,640 MHz**; najlepiej PR Trójka w obu próbach.
- Zapisz oznaczenie układu, wersję jego oprogramowania (oczekiwana 6.0.9),
  wersję programu radia, identyfikator kompilacji, Git SHA i SHA-256 otrzymanej binarki.
- Dopisz datę oraz strefę czasową, np. Europe/Warsaw i aktualne przesunięcie UTC.
  Samo „18:29” w pliku nie określa strefy.

Do głównego testu użyj zwykłej kompilacji `esp32dev`, 10 MHz, `detail=0`.
Rozbudowana `esp32dev_diag` ma więcej liczników, lecz potrzebuje dodatkowych
2160 B DRAM. Na tym samym ciasnym układzie może nie uruchomić Core.
Nie używaj jej jako zamiennika głównej próby bez osobnej kontroli pamięci.

Program ma zachować standardową rezerwę **12000 B**. Nie zmniejszaj jej ręcznie
do 8000 B ani nie zmieniaj pojemności buforów. Nie kasuj NVS/presetów.
Wgrywanie innej wersji uzgodnij osobno z autorem; ten protokół nie wymaga
pełnego kasowania pamięci. Obraz `receiver-complete.bin` zastępuje ustawienia NVS.

## Jak uruchomić monitor portu szeregowego

1. Ustaw właściwy port, **115200 bit/s, 8N1**, bez sprzętowej kontroli przepływu.
2. Włącz zapis tekstu UTF-8 do osobnego pliku oraz rzeczywiste znaczniki czasu
   terminala. Loguj od uruchomienia radia do końca próby, bez wycinania środka.
3. Polecenia kończ CR, LF albo CRLF. Wszystkie trzy warianty są obsługiwane;
   pusty drugi znak końca w CRLF jest ignorowany. Bez końca linii polecenie nie ruszy.
4. Czytaj odpowiedź radia, nie tylko lokalne echo wpisanego polecenia.
   Wielkość liter poleceń nie ma znaczenia. Poniższe polecenia nie wymagają ENABLE=1.
5. Nie uruchamiaj równocześnie innego monitora ani programu wysyłającego EPG.
   Główny test ma pracować w LIVE; nie wysyłaj E1/E2 ani EPG TEST INJECT/DEMO.

Przed każdą próbą wykonaj nowy rozruch radia, aby porównać czyste liczniki i tryb.
Nie zmieniaj stacji w mierzonym oknie. Jeśli było to konieczne, zanotuj dokładny
czas i nie traktuj przerwanego obiektu SLS jako dowodu błędu odbioru.

## Polecenia i potwierdzenia

| Polecenie | Działanie i oczekiwane potwierdzenie |
|---|---|
| `DEBUG` | Przełącznik; odpowiedź `[DEBUG] diagnostics ON` albo `OFF`. Nie istnieje polecenie `DEBUG OFF`. |
| `DSRV STATUS` | `[DSRV/MODE]`, `[DSRV/UA]`, `[DSRV/STATUS]` oraz jeden ograniczony raport liczników; nie włącza DEBUG. |
| `DSRV MODE A` | Normalny odbiór i dekodowanie pakietów; DEBUG włączony, szczegółowe SLS wyłączone. |
| `DSRV MODE B` | Żądanie fizycznego STOP osobnej usługi pakietowej; audio/PAD/SLS pozostają. |
| `DSRV MODE C` | Usługa pakietowa nadal pracuje, pakiety są odczytywane i potwierdzane, ale nie trafiają do EPG Core. |
| `DSRV MODE D` | Normalne dane i EPG, bez automatycznych obszernych raportów, haszowania treści i bitmap strat. |
| `DSRV UA PAUSE 900` | Na 900 s wstrzymuje wyłącznie okresowe UA po poprawnym zakończeniu początkowego skanu. |
| `DSRV UA RESUME` | Kończy czasową pauzę UA. |
| `EPG TEST STATUS` | Odczyt stanu Core; sam nie uruchamia trybu TEST. Oczekiwane `mode=LIVE`. |
| `EPG TEST TRANSPORT` | Odczyt ścieżek, radia i pamięci, zakończony `EPG TRANSPORT END`. |
| `DEBUG SPI STOP` | Osobne zakończenie próby packet SPI; to nie jest wybór trybu B. Nie używaj go wewnątrz pomiaru A/C/D. |
| `DEBUG SLSV` | Tylko na osobne zlecenie: dodatkowe szczegółowe segmenty. Zwiększa ilość tekstu, nie używaj w głównej macierzy. |

Przykład akceptacji żądania, jeszcze bez potwierdzenia fizycznego STOP:

```text
DSRV MODE B
[DSRV/MODE] requested=B result=ACCEPTED probe=5
```

Późniejsza odpowiedź potwierdzająca stan bez aktywnej usługi pakietowej:

```text
DSRV STATUS
[DSRV/MODE] mode=B epoch=1 probe=0 debug=1
```

Liczba epoch zależy od wcześniejszych poleceń. Stany probe: 0 bezczynny,
4 odbiór usługi, 5 żądanie zatrzymania, 6 oczekiwanie na STOP,
7 nieudane zakończenie. Po A/C/D daj czas na dojście do probe=4.

`UNSUPPORTED` oznacza, że brakuje bezpiecznego osobnego właściciela usługi,
trwa inny tryb/kontekst albo zakończenie usługi nie jest pewne. To nie błąd
użytkownika. Nie wymuszaj agresywnego STOP audio/SLS. Próbę B oznacz jako
niewykonaną, zamiast zakładać, że usługa została zatrzymana.

STATUS zamraża bieżące okno liczników; może ono mieć mniej niż 30 s.
Nie wysyłaj STATUS co sekundę podczas pomiaru szybkości. Używaj czasu `ms`
w raporcie, a nie założenia, że każdy wiersz obejmuje dokładnie 30 s.

### Kontrola po uruchomieniu każdej próby

Po ustawieniu DAB, właściwego kanału i stacji poczekaj na stabilny odbiór,
pełny skan UA i ustalenie usługi pakietowej. Wyślij kolejno, czekając na odpowiedzi:

```text
EPG TEST STATUS
EPG TEST TRANSPORT
DSRV STATUS
```

Sprawdź `mode=LIVE`, `EPG HEALTH core=1` oraz `reserve8=12000` w DSRV/STATUS.
W głównej kompilacji ma być `spiHz=10000000 detail=0`.
Zapisz free8/largest8/free32/largest32 i stackMinBytes.

Jeżeli wystąpi `EPG ERROR MEMORY`, **nie wykonuj próby CTS pod obciążeniem**,
nie zmieniaj rezerwy. Odeślij log jako nieodpowiedni wynik pamięciowy.
Jeśli SPI lub stacja nie są dostępne mimo poprawnej pamięci, oznacz właściwą
próbę jako niewykonalną. `core=1` samo nie dowodzi obecności danych EPG.

## Główna macierz prób

Każdy wiersz zaczynaj nowym rozruchem. Po kontroli wstępnej ustaw tryb,
poczekaj na stan ustalony i zanotuj początek mierzonego okna.

| Próba / nazwa pliku | MUX | Ustawienie | Czas i kontrola |
|---|---|---|---|
| `20261028_11A_A_normal_10MHz.txt` | 11A | MODE A | 10–15 min, co najmniej 20 zamkniętych cykli TID SLS; wcześniej zakończ przy powtarzającym się recovery |
| `20261028_11A_B_no_packet_10MHz.txt` | 11A | MODE B | Tyle samo; najpierw potwierdź probe=0, packet blisko zera i działający PAD/SLS |
| `20261028_11A_C_no_core_parse_10MHz.txt` | 11A | MODE C | Tyle samo; probe=4, pakiety nadal odczytywane, dekodowanie packet Core pominięte |
| `20261028_11A_D_quiet_10MHz.txt` | 11A | MODE D | 15 min; nie włączaj DEBUG, zanotuj zewnętrzne objawy i raport na końcu |
| `20261028_11B_A_reference_10MHz.txt` | 11B | MODE A | Co najmniej 10–15 min; PI/SI/PNG i SLS powinny działać |
| `20261028_11A_UA_refresh_off_10MHz.txt` | 11A | MODE A + UA PAUSE 900 | Opcjonalnie 10 min wewnątrz potwierdzonej pauzy |
| `20261028_11A_SPI_5MHz_A.txt` | 11A | Osobna kompilacja 5 MHz, MODE A | Opcjonalnie ten sam czas, zasilanie i antena jak dla 10 MHz |

Datę w nazwie zastąp faktyczną datą pomiaru. Nie licz powtórzeń tego samego
ukończonego TID jako kolejnych nowych obrazów. Gdy stacja zmienia obiekty wolno,
20 cykli może wymagać więcej niż 15 min; zapisz rzeczywistą liczbę. W D, bez
szczegółowych komunikatów TID, wpisz „niezmierzona”, zamiast wymyślać liczbę.

## Test A – normalny odbiór MUX 11A

1. Po nowym rozruchu ustaw 11A i wybraną stację, wykonaj kontrolę pamięci.
2. Wyślij `DSRV MODE A`; oczekuj `requested=A result=ACCEPTED`.
3. Poczekaj na probe=4 i pakiety, zapisz początek okna.
4. Obserwuj dźwięk, tekst stacji, SLS, `[DSRV/RATE]`, błędy CTS i recovery.
5. Na końcu wyślij `DSRV STATUS` i `EPG TEST TRANSPORT`, poczekaj na końce raportów.

Pusty Directory i brak PI na tym 11A mogą być poprawną treścią multipleksu.
Nie są samodzielnym dowodem zawieszenia parsera.

## Test B – bez pakietowej usługi EPG

1. Nowy rozruch i ta sama kontrola; przed B musi istnieć osobna usługa packet.
2. `DSRV MODE B`; oczekuj ACCEPTED, ale nie zaczynaj pomiaru tylko na tej podstawie.
3. Po kilku sekundach `DSRV STATUS`; sprawdź probe=0. W razie oczekiwania
   daj czas na zakończenie, nie wysyłaj ręcznych komend STOP do innych usług.
4. W ustalonym oknie packet powinien być bliski zeru, PAD/SLS i dźwięk muszą działać.
5. Brak bezpiecznego STOP, UNSUPPORTED lub probe=7: próba niewykonana, odeślij log.

Nie oczekuj nowych packet PI w tym trybie. Zachowana wcześniej cache nie
jest dowodem, że packet nadal płynie. Następną próbę zacznij nowym rozruchem.

## Test C – odbiór pakietów bez dekodowania w EPG Core

Po nowym rozruchu i kontroli wyślij `DSRV MODE C`, oczekuj ACCEPTED i probe=4.
Pakiety mają nadal zwiększać liczniki, a readOk/start GET powinny postępować.
Packet feed do Core jest pomijany; `parseSkipped` rośnie.
PAD/SLS nadal działa; ewentualne prawidłowo potwierdzone PAD SPI ma własną ścieżkę.

To nie wyłącza każdej analizy: pozostaje diagnostyczna kontrola CRC kompletnego
pustego Directory oraz haszowanie odczytanej treści. Nie opisuj C jako
„bez wszystkich parserów”. Zakończ raportami tak jak A.

## Test D – diagnostyka wyciszona

Po nowym rozruchu i kontroli wyślij `DSRV MODE D`, oczekuj ACCEPTED.
Odbiór packet/Core jest normalny, lecz rozbudowane automatyczne DSRV/SLS raporty,
hasze treści i budowanie bitmap strat są wyłączone. **Nie wpisuj DEBUG ani DEBUG SLSV.**

Przez 15 min obserwuj dźwięk, logo i ekran. Przy zaniku wpisz czas w notatce
terminala lub osobnym pliku; można zrobić zdjęcie, nie zmieniając stacji.
Krytyczny `[DAB/CTS-FIRST]`/`[DAB/CTS-RECOVERY]` i `[DAB/WATCHDOG]` pozostaje dostępny,
choć przy pełnym UART zapis snapshotu może pojawić się z opóźnieniem.

Na końcu wyślij **jeden** `DSRV STATUS`, poczekaj na ograniczony raport,
potem `EPG TEST TRANSPORT`. Te jawne odpowiedzi nie są usterką trybu cichego.
Nie oczekuj w D automatycznych `[DSRV/RATE]` co 30 s. Drobne liczniki nadal rosną.

## Test referencyjny MUX 11B

Nowy rozruch, 11B i najlepiej ta sama stacja, kontrola pamięci, `DSRV MODE A`.
Sprawdź PI, SI, własne logo PNG i działanie EPG oraz audio/SLS przez 10–15 min.
W razie aktualnego programu oczekuj READY_LIVE i sensownego NOW/NEXT.
Jeżeli nadawca wysyła historyczne PI, PI_DATE_STALE jest poprawną ochroną daty;
nie przesuwaj ręcznie czasu LIVE, aby uzyskać zielony wynik.

Brak usługi lub PI na danej stacji oznacz jako niepotwierdzone/niewykonalne.
Zapisz `EPG TEST STATUS`, `EPG TEST TRANSPORT`, `DSRV STATUS` i zdjęcie EPG.

## Testy opcjonalne: odświeżanie UA i SPI 5 MHz

### Czasowe wstrzymanie okresowych zapytań UA

1. Nowy rozruch, 11A, ta sama stacja, MODE A; poczekaj na pełny poprawny skan.
2. Wyślij `DSRV UA PAUSE 900`. Oczekuj dokładnie:

```text
[DSRV/UA] requested=PAUSE seconds=900 result=ACCEPTED
```

3. `DSRV STATUS` ma pokazać paused=1, dodatnie remainingMs, complete=1 i failed=0.
4. Mierz 10 min bez przestrajania. Pauza dotyczy tylko okresowych zapytań;
   początkowy skan i faktyczna zmiana listy/stacji/kontekstu mają pierwszeństwo.
5. Na końcu sprawdź, czy pauza nie wygasła wcześniej, i wyślij `DSRV UA RESUME`.

Dostępny zakres to 30–900 s. UNSUPPORTED oznacza m.in. niepełny/błędny skan,
FM/TEST, przełączanie stacji lub złą liczbę sekund. Nie omijaj tego ograniczenia.
Pauza sama wygaśnie; zmiana kontekstu może ją wcześniej anulować. Wszystko jest w RAM.

### Oddzielna kompilacja SPI 5 MHz

Uzgodnij z autorem osobną binarkę `esp32dev_spi5`. Po nowym rozruchu potwierdź
`spiHz=5000000 detail=0` w DSRV/STATUS i zapisz SHA-256 tej wersji.
Powtórz A na 11A w tych samych warunkach; poza zegarem radia nie zmieniaj parametrów.
TFT pozostaje na 27 MHz. Wolniejszy SPI może wydłużyć przesyłanie bloków i zwiększyć
kolejkę, dlatego sam brak błędu w krótkiej próbie nie dowodzi naprawy.

## Jak rozpoznać błąd i co oznaczają liczniki

- `rst:` i `[BOOT] reset reason` dokumentują rozruch ESP32; porównaj z początkiem logu.
  Sam `[DAB/WATCHDOG]` i odtworzenie TFT/Si4684 mogą wystąpić bez restartu ESP32.
- `lastServiceGapUs=9000` to ostatnia przerwa między obsługami hosta, **nie**
  9ms timeout CTS. Sprawdź result, elapsedMs i deadlineLatenessUs.
- Pierwszy błąd serii jest zapisany jako CTS-FIRST; przy recovery ten sam snapshot
  może wrócić jako CTS-RECOVERY. Nie licz powtórzonego atMs jako kolejnego pierwszego błędu.
- Status ma cztery surowe bajty. statusKnown=0 oznacza brak ważnej obserwacji
  w kontekście; nie interpretuj wtedy flag. sticky3 sumuje zaobserwowane bity błędów.
- DSPERR/REPOFERR/CMDOFERR/ARBERR/ERRNR są wskaźnikami diagnostycznymi.
  Same logi starego programu nie miały tych danych i nie wskazują pewnej przyczyny CTS.
- queueMax/BUFF_COUNT to zgłoszona liczba, nie potwierdzona fizyczna pojemność FIFO.
  Wartość 19 trzeba zachować w logu, nie przeliczać automatycznie na zajętość kolejki.
- `packet` i `pad` liczą nagłówki; PAD nie oznacza automatycznie PAD EPG.
  UA 0x002 to Slideshow; SPI 0x007 wymaga potwierdzenia komponentu i kontekstu.
- W zwykłej kompilacji CMD ma wiersze other/dsrv/signal/UA. other sumuje pozostałe
  operacje; nie interpretuj go jako jednej konkretnej komendy tunera.
- `[SLS/LOSS] firstMissing=0` po zmianie stacji często oznacza wejście w środek karuzeli MOT.
  Brak LAST nie pozwala zgadywać całkowitej długości sekwencji.
- `exact=1` oznacza znany koniec bitmapy. `credibleInternal=1` wymaga segmentu 0 i LAST,
  wewnętrznych dziur oraz nieprzerwanego kontekstu; nadal nie wskazuje warstwy odpowiedzialnej.
- UARTdrop dotyczy utraconych bajtów tekstu diagnostycznego, nie pakietów RF.
  Nie sumuj free8 i free32 jako dwóch niezależnych obszarów RAM.
- Zapisz realne RSSI/CNR/FIC i lock. Nie przypisuj przerw po przestrajaniu stabilnemu TID.

Po powtarzających się recovery zakończ próbę i odeślij log. Nie prowokuj
nieskończonych resetów. Przed następnym wariantem wykonaj nowy rozruch.
Nie zmieniaj rezerwy, timeoutów, NVS ani przypisania audio/SLS.

## Tabela wyników do wypełnienia

Dla każdej próby skopiuj formularz:

| Pole | Wynik |
|---|---|
| Nazwa próby / plik logu | do uzupełnienia |
| Wersja programu, Git SHA, SHA-256 binarki | do uzupełnienia |
| Układ / oprogramowanie Si4684 / SPI Hz / detail | do uzupełnienia |
| Data, strefa czasowa, początek i koniec | do uzupełnienia |
| MUX, częstotliwość, stacja, SID/CID | do uzupełnienia |
| Średnie RSSI / CNR / FIC i lock | do uzupełnienia |
| EPG Core / stan PI/SI/PNG / NOW/NEXT | do uzupełnienia |
| free8 / largest8 / free32 / largest32 przed i po | do uzupełnienia |
| stackMinBytes / minimalny heap | do uzupełnienia |
| queueMax / packet i PAD na rzeczywiste ms | do uzupełnienia |
| overflow / readFail / UARTdrop | do uzupełnienia |
| Ukończone nowe SLS TID / wiarygodne wewnętrzne braki | do uzupełnienia lub niezmierzone |
| Pierwszy CTS, atMs, op, elapsedMs, status0–3, sticky3 | do uzupełnienia lub brak |
| Liczba watchdog cold recovery / restartów ESP32 i powód | do uzupełnienia |
| Zdjęcie TFT / dźwięk / inne uwagi | do uzupełnienia |

Skrót zbiorczy — **przykład sposobu zapisu, nie wynik nowego testu**:

| Próba | Core | Packet/PAD | CTS / recovery | SLS | Ocena |
|---|---|---|---|---|---|
| 11A_A_normal | 1 | według logu, okno 30000 ms | niezmierzone | niezmierzone | niepotwierdzone |
| 11A_B_no_packet | do uzupełnienia | wymagane probe=0 i packet blisko zera | do uzupełnienia | do uzupełnienia | niewykonana |
| 11B_A_reference | do uzupełnienia | do uzupełnienia | do uzupełnienia | PI/SI/PNG do sprawdzenia | niepotwierdzone |

Kryteria: Core działa z rezerwą 12000 B, 11B zachowuje PI/SI/PNG/SLS;
11A przy aktywnym packet nie traci CTS w dostatecznie długiej próbie;
B rzeczywiście odłącza packet bez utraty audio/SLS, C oddziela Core,
D nie zalewa UART, a pierwszy błąd zachowuje STATUS3. Brak wiarygodnych
wewnętrznych dziur trzeba potwierdzić w stabilnych pełnych cyklach TID.
Bez tych dowodów pisz „niepotwierdzone”, nie „naprawione”.

## Jakie pliki odesłać

- Osobny, nieprzerwany `.txt` ze znacznikami czasu dla każdej próby, od rozruchu do końca.
- Tabelę wyników `.md` lub `.txt`, także dla prób niewykonalnych i przyczyny.
- Zdjęcia/zrzuty TFT: stan EPG, logo, objaw zawieszenia lub brakującego SLS.
- Informację o binarce i jej SHA-256; opcjonalnie jeden ZIP, np.
  `Andrzej_20261028_SI4684_11A_11B.zip`, zawierający powyższe pliki.

Nie wysyłaj samego końcowego wiersza watchdog: potrzebne jest pierwszy błąd,
kontekst przed nim oraz zapis dalszej próby. Jeśli części danych nie ma,
napisz, że nie zostały zebrane; nie uzupełniaj ich przypuszczeniami.
