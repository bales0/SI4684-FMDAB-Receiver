# Si4684 — test DSRV RAW/QUEUE dla Andrzeja

Wersja instrukcji: 2 listopada 2026. Odbiornik ESP32-WROOM-32D bez PSRAM, Si4684 DAB FW 6.0.9, ILI9341. Celem jest zapis oryginalnej odpowiedzi 0x84 i stanu przed pierwszym CTS, a nie zmiana sposobu odbioru. `BUFF_COUNT=19` jest obserwacją, nie diagnozą ani dowodem pojemności FIFO.

Kandydat `esp32dev_queue_diag` przeszedł 41 kroków testów hosta i kompilację. **Nie został sprawdzony na fizycznym odbiorniku. Przed pomiarem RF trzeba potwierdzić pamięć i działający EPG Core.** Wynik symulacji nie zastępuje tej kontroli.

## 1. Przygotowanie i wgranie aplikacji

Zachowaj tę samą antenę, jej położenie i zasilanie. Pierwsza seria ma pracować w POLL, przy SPI radia 10 MHz, bez zmiany GPIO12/MTDI. Zrób zdjęcie System Info/About z rolą GPIO12 i rzeczywistym trybem sterowania. Interfejs ma ustawienia AUTO/INTB/IR; nie ma osobnego polecenia UART ustawiającego POLL. Sam napis AUTO nie dowodzi POLL — sprawdź także wyświetlany tryb pracy. Jeżeli aktualna konfiguracja wymusza INTB, uzgodnij ją z właścicielem projektu przed testem; nie przepinaj przewodów i nie wymuszaj poziomów na GPIO12.

Zbuduj dokładnie:

```text
pio run -e esp32dev_queue_diag
```

Nie używaj `esp32dev_diag`: pełna diagnostyka ma inną cenę pamięci. Zapisz commit, informację o lokalnych zmianach oraz SHA-256 `.pio/build/esp32dev_queue_diag/firmware.bin` (w PowerShell: `Get-FileHash .pio/build/esp32dev_queue_diag/firmware.bin -Algorithm SHA256`). Sam numer commit nie opisuje niezatwierdzonych zmian.

Po potwierdzeniu, że odbiornik ma dotychczasowy, niezmieniony układ partycji z aplikacją pod **0x10000**, wgraj wyłącznie `firmware.bin`. Przykład dla Windows — COM4 zastąp właściwym, wolnym portem:

```text
python -m esptool --chip esp32 --port COM4 --baud 460800 write_flash 0x10000 .pio/build/esp32dev_queue_diag/firmware.bin
```

Jeśli układ partycji jest inny lub nieznany, najpierw wyjaśnij to z właścicielem. Nie używaj `erase_flash`, `receiver-complete.bin`, obrazu całego układu ani zapisu pod 0x0; kompletny obraz zastępuje także NVS. Nie zmieniaj NVS ani eFuse. Samo zbudowanie kompletu plików przez PlatformIO nie oznacza, że należy je wszystkie wgrać.

Otwórz monitor **115200 baud, 8-N-1**, zapis UTF-8 z timestamps dla każdej linii. Polecenia wysyłaj z CR, LF lub CRLF. Zapisuj pełny log od pierwszego bootu, bez filtrowania i bez wycinania powtórzeń. Zamknij drugi monitor zajmujący ten sam port. Nie uruchamiaj syntetycznego EPG ani odtwarzania RAW podczas pomiarów RF.

## 2. Obowiązkowa kontrola pamięci i trybu

Od początku zapisuj także informacje o pamięci przed utworzeniem Core. Po dostrojeniu stabilnej stacji i zakończeniu rozpoznawania usług wyślij:

```text
DSRV STATUS
EPG TEST STATUS
EPG TEST TRANSPORT
```

Poczekaj na `EPG TRANSPORT END`. Jeżeli transport zgłosi `EPG TRANSPORT CANCELLED context changed`, poczekaj na stabilny odbiór i powtórz odczyt. `EPG TEST STATUS` tylko odczytuje stan; EPG powinno pozostać w LIVE.

Sprawdź i zachowaj w logu:

- `[DSRV/MODE] mode=A ... debug=0` po zwykłym bootowaniu;
- `[DSRV/STATUS] spiHz=10000000 detail=0 core=1 ... reserve8=12000`;
- po utworzeniu Core `free8>=12000`, a także `largest8`, `free32`, `largest32`;
- `EPG HEALTH ... core=1 arena=63600 ... stackMinBytes=... UARTdrop=...` oraz zmiany tych wartości podczas testu;
- brak błędu alokacji Core, zachowany dźwięk i działający SLS. `stackMinBytes` podaje bajty, nie słowa.

Koszt nowej statycznej pamięci to 736 B, zwykły build ma przyrost 0 B. Na podstawie starego pomiaru 13068 B przewidywane free8 to tylko 12332 B, czyli 332 B ponad rezerwą. To **nie jest pomiar tej wersji**. Jeśli `core=0`, błąd alokacji albo free8 spada poniżej limitu, przerwij ocenę RF i wyślij log. Nie obniżaj rezerwy 12000 B, nie wyłączaj EPG i nie próbuj ukryć problemu inną konfiguracją.

## 3. Kolejność osobnych pomiarów

Każdy wiersz to **nowy boot ESP32 i osobny plik logu**. Nie przełączaj trybu w środku ocenianej serii. Między seriami można wybrać odpowiedni mux/stację, lecz zachowaj pełny log i jednoznacznie oznacz początek stabilnego pomiaru.

| Seria | Mux / tryb | Czas |
|---|---|---|
| 1 | 11A / PR Trójka / domyślny A / POLL / 10 MHz | 5 min lub do pierwszego CTS, potem zapis recovery i całego dumpu |
| 2 | 11B / A / te same ustawienia | 5–10 min |
| 3 | 11A / PR Trójka / C | 5 min lub do pierwszego CTS, potem recovery i dump |
| 4, opcjonalna | 11A / B, tylko jeśli STOP jest bezpiecznie zaakceptowany | 10–15 min |
| 5, osobna cicha seria | 11A / D | 5 min lub do pierwszego CTS, potem recovery i dump |

A po bootowaniu jest domyślny. **Nie wysyłaj niepotrzebnie `DSRV MODE A`**, ponieważ A/B/C włączają DEBUG. Jeśli jednak trzeba ustawić A, sprawdź odpowiedź, potem wyłącz DEBUG poleceniem `DEBUG` i potwierdź stan przez `DSRV STATUS`.

Dla C, już po rozpoznaniu usług:

```text
DSRV MODE C
DEBUG
DSRV STATUS
```

Oczekuj `requested=C result=ACCEPTED`, następnie `[DEBUG] diagnostics OFF` i `mode=C ... debug=0`, przy aktywnej usłudze packet (zwykle `probe=4`). `DEBUG` jest przełącznikiem: nie wysyłaj go ponownie, jeśli debug już jest OFF. Nie istnieje polecenie `DEBUG OFF`. C nadal czyta pakiety, ale pomija parser EPG. **Oceniany CTS musi nastąpić po ACK C.** Jeśli najwcześniejszy snapshot powstał jeszcze w A, zachowaj ten log i powtórz C po nowym bootowaniu. Nie czyść śladu tylko po to, żeby przypisać starszy błąd do C.

B, tylko gdy zgłoszono bezpieczne, odrębne ownership usługi:

```text
DSRV MODE B
DEBUG
DSRV STATUS
```

Potwierdź ACCEPTED, DEBUG OFF, mode=B i rzeczywisty STOP (zwykle `probe=0`, kolejne packet=0, PAD i audio nadal działają). UNSUPPORTED nie oznacza zgody na ręczne wymuszenie STOP; wyślij log i pomiń serię B.

W osobnej serii D wyślij `DSRV MODE D`, następnie `DSRV STATUS`. D sam wyłącza DEBUG. Nowy krótki `[DSRV/QUEUE]` co około 30 s i dowody CTS/RAW nadal mają być dostępne. Pozostałe zwykłe szczegółowe logowanie D pozostaje wyłączone.

Nie zmieniaj podczas pomiarów SPI clock, burst=4, parsera, ACK, timeoutów ani watchdog. Nie włączaj `DEBUG SPI`, SLSV/VERBOSE ani UA PAUSE w tej macierzy.

## 4. Co robić po CTS

Nie odłączaj zasilania od razu: pozwól na automatyczne recovery Si4684/TFT i dokończenie UART, o ile nie ma zagrożenia elektrycznego. ESP32 nie powinien być resetowany przed zapisaniem dowodów. Najpierw mogą pojawić się `[DAB/CTS-FIRST]`, `[DAB/CTS-STATE]` i później `[DAB/WATCHDOG]`; nowy reporter ustępuje im pierwszeństwa.

Następnie poczekaj na:

- `[DSRV/QUEUE-FROZEN] reason=CTS_FIRST` z czasem, generacją, trybem i liczbą próbek;
- `QUEUE-COUNTS`, `QUEUE-FLAGS`, `QUEUE-ERRORS` ze `scope=FROZEN`;
- maksymalnie 12 uporządkowanych `[DSRV/QUEUE-RAW]`;
- `[DSRV/QUEUE-FIRST-OVER8]`, jeśli taki header wystąpił;
- podsumowania `scope=BOOT` i `[DSRV/QUEUE-TIMES]`.

Wyślij `DSRV STATUS`, jeśli chcesz odczytać zachowany ślad ponownie. Polecenie jest read-only: nie czyści i nie aktualizuje zamrożonego obrazu. Ten sam `atMs` w ponownym CTS-FIRST nie jest nowym błędem. Po późniejszej recovery stary RAW nadal opisuje **pierwszy** przechwycony CTS; nie należy łączyć go z inną, późniejszą serią CTS.

Nie używaj CLEAR w środku błędu. Tylko po zapisaniu dowodów i na wyraźne potrzeby nowego przechwycenia można ręcznie wysłać:

```text
DSRV QUEUE CLEAR
```

W queue buildu odpowiedź to `[DSRV/QUEUE-CLEAR] result=OK counters=BOOT_RETAINED`. Polecenie uzbraja nowy RAW capture i usuwa wcześniejszy obraz ring/first-over8, ale zachowuje liczniki BOOT i WINDOW, nie restartuje radia, nie czyści starej CTS evidence i nie zapisuje NVS. W zwykłym esp32dev wynik to DISABLED. Najprostszy podział pomiarów pozostaje: nowy boot na każdą serię.

## 5. Jak czytać dane

`raw24` to dokładnie **48 znaków hex**, 24 oryginalne bajty logicznej odpowiedzi od STATUS0. Fizyczny framing SPI nie wchodzi do tych 24 B. `read=HEADER` oznacza pierwszą odpowiedź istniejącego GET_DATA, przed full read. Nie są dodawane żadne kontrolne odczyty SPI. `buffRaw` to niezmieniony bajt r[5], nie liczba ograniczona do 8. CTS/ERR_CMD oraz STATUS3 także są zapisane. Nieudany command nie tworzy nowej RAW próbki ze starego bufora.

`INTSRC overflow` (`intsrcOvf`, bit02 w r[4]) i `SRV_STATE=2` (`state2`, r[6]) to dwa osobne wskaźniki. Mogą wystąpić razem albo niezależnie. `over8NoOvf`, `state2NoOvf`, `over8Ovf`, `over8State2` opisują kombinacje; stan2 nie jest dopisywany do starego licznika INTSRC overflow.

Histogram ma kolejno przedziały **0, 1, 2–3, 4–7, 8, 9–15, 16–31, 32–255**. `packetMax/padMax` oznaczają raw DATA_SRC0/1, nie potwierdzają rozpoznania EPG. `replyHeaderChanged` sumuje istniejące odmowy SID/CID i BYTE_COUNT przy potrzebnym full read, a zmiana zwykłego statusu nie jest błędem. `readFailBoot` jest starym licznikiem, bez podwójnego naliczania.

- `scope=WINDOW`: rzeczywisty okres `ms` od poprzedniego wysłanego podsumowania, zwykle co najmniej 30 s. `mixed=1` oznacza zmianę generacji/trybu; maksimum z takiego okna nie jest pojemnością układu.
- `scope=BOOT`: narastające dane od bootu, odczytane przy wysyłaniu danej linii; mogą nadal rosnąć po recovery.
- `scope=FROZEN`: niezmienione liczniki BOOT z chwili pierwszego CTS. Ring nie jest nadpisywany po tej chwili.

`headers` ma 32 bity, szczegółowe liczniki i koszyki histogramu 16 bitów i saturują. `sat=1` sygnalizuje osiągnięty limit; wartość 65535 jest wtedy dolną granicą, nie dokładnym wynikiem. `suppressed` oznacza odłożenie wyjścia z powodu pełnej kolejki UART, `reportDrop` problem długości/zapisu nowej linii; `serialDrop` i `queued` dotyczą UART, nie utraty RF.

`capture` zmienia się po ręcznym CLEAR. `seq=0` w FIRST-OVER8 oznacza osobną zachowaną próbkę; pozostałe `seq` porządkują ring. FIRST-OVER8 to pierwsza taka próbka bieżącego capture, natomiast `QUEUE-TIMES scope=BOOT` zachowuje pierwsze/ostatnie czasy całego bootu. Po CLEAR mogą odnosić się do starszej generacji. Pola known wskazują, czy czas/age ma sens; `atMs=0` może być prawidłowym czasem. Metadane próbek należy zawsze czytać razem z gen/mode, także gdy ring obejmuje zmianę kontekstu.

## 6. Co odesłać

Dla każdej serii wyślij pełny, surowy TXT UTF-8 z timestamps od bootu do końca, SHA-256 aplikacji, commit i opis lokalnych zmian, zdjęcie System Info z GPIO12 i trybem sterowania, mux/częstotliwość/stację/SID/CID, czas pomiaru, RSSI/CNR/FIC/lock, moment pierwszego błędu i liczbę recovery. Napisz, czy resetował się ESP32 (nowy komunikat startowy), czy tylko Si4684 i TFT przez wspólny RST GPIO17, oraz czy były przerwy dźwięku lub problemy SLS. Nie zastępuj logu samym podsumowaniem.

Eksperyment POLL kontra INTB będzie oddzielnym, późniejszym testem, dopiero po ocenie tej serii i uzgodnieniu bezpiecznego połączenia. GPIO12 jest również MTDI; nie zmieniaj eFuse, poziomów startowych ani zasilania w czasie pracy. Nie wyłączaj watchdog. Jeżeli instrumentacja zmienia objawy, potrzebne będzie osobne porównanie z baseline, a nie jednoczesna zmiana kilku parametrów.

Szczegóły implementacji i jawne ograniczenia: [czeski protokół](SI4684_DSRV_QUEUE_RAW_CTS_AUDIT_2026-11-02_CZ.md). Opis wszystkich pól UART: [Serial_protocol.md](../Serial_protocol.md).
