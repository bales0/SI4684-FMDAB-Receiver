# EPG AUTO — instrukcja testów dla Andrzeja (3.11.2026)

To jest ograniczenie aktywności pustej, osobnej usługi pakietowej SPI EPG.
Nie traktujemy tego jako potwierdzonej naprawy firmware Si4684. Duża kolejka
(qmaxRaw=19) sama nie dowodzi przepełnienia ani przyczyny utraty CTS.

## Przygotowanie i zapis wyników

1. Zapisz SHA kompilacji (git rev-parse HEAD oraz informację o lokalnych zmianach),
   nazwę środowiska PlatformIO i datę/godzinę rozpoczęcia. Baza tej zmiany:
   df0c8bc9eef4c4598742fd462216675d1e885d81. Sprawdź właściwy plik firmware.
2. Zapisz fizyczny tryb POLL/INTB, taktowanie SPI radia (10 MHz dla esp32dev,
   esp32dev_diag i esp32dev_queue_diag; 5 MHz dla esp32dev_spi5), wersję obrazu
   Skyworks DAB (projekt używa 6.0.9), płytkę i wybrany mux/stację.
3. Włącz przechwytywanie UART 115200 do osobnego pliku dla każdego testu.
   Nie wyłączaj automatycznej recovery. Nie zmieniaj eFuse ani okablowania.
4. Po ustabilizowaniu radia wyślij EPG TEST STATUS, EPG TEST TRANSPORT,
   DSRV STATUS i EPG AUTO STATUS. W EPG POOLS zapisz free8 i largest8,
   w EPG RAM — UARTdrop. Potwierdź core=1 w EPG AUTO. Brak Core oznacza,
   że automatyka nie ma warunków do testu.
5. Każdą zmianę trybu zaznacz czasem zegarowym w notatkach i zachowaj ACK
   z czasem atMs dla AUTO oraz [DSRV/MODE] requested=... result=ACCEPTED.
   Tryb musi być zaakceptowany PRZED pierwszym CTS timeoutem, inaczej pomiar
   referencyjny należy powtórzyć. Nie używaj DEBUG SPI podczas tych prób.

Polecenia (osobna linia dla każdego):

~~~text
DSRV MODE A
EPG AUTO ON
EPG AUTO OFF
EPG AUTO STATUS
DSRV STATUS
EPG TEST STATUS
EPG TEST TRANSPORT
~~~

AUTO jest domyślnie ON po restarcie ESP32; ON/OFF nie zapisują NVS.
DSRV MODE A zachowuje ostatnie ON/OFF. B blokuje restart usługi również po
upływie timera. C (bypass parsera) i D (quiet) wyłączają stosowanie AUTO, lecz
nie zmieniają kill-switcha. Powrót do A przy ON ponownie uzbraja automatykę.
Zmiana trybu/kontekstu zeruje potwierdzanie pustki i anuluje poprzedni cooldown.
D nie generuje nowych automatycznych wpisów EPG/AUTO; odpowiedź na jawne
polecenie STATUS nadal jest dozwolona.

## 11B: rzeczywiste EPG — co najmniej 15 minut

Ustaw DSRV MODE A, następnie EPG AUTO ON. Zachowaj ACK i początkowy STATUS.
Sprawdź PI, SI, logo, SLS i audio. Oczekiwane: eligible=1 dla osobnej usługi
pakietowej, state=ACTIVE, rosnące nonEmptyValid, brak wzrostu stopCount AUTO,
brak CTS. Przy właścicielu PAD oczekiwane eligible=0 i brak osobnego STOP.
Co około 5 minut i na końcu zapisz STATUS-y. Otwarcie/zamknięcie ekranu EPG
nie powinno kasować danych ani zmieniać pracy audio/SLS.

## 11A: pomiar referencyjny z aktywnymi pakietami

Ustaw EPG AUTO OFF i DSRV MODE A. Zapisz oba potwierdzenia oraz czas startu.
Usługa pakietowa ma pracować ciągle tak jak wcześniej. Zapisz czas do pierwszego
CTS i wszystkie automatyczne recovery. Zachowaj [DAB/CTS-FIRST],
[DSRV/QUEUE-FROZEN] (wersja queue_diag), CTS count, qmaxRaw, overflow,
free8, UARTdrop oraz stan audio/SLS. Nie czyść RAW evidence przed zapisaniem.
Nie nazywaj recovery restartem ESP32, jeśli nie ma ponownego bootu ESP32.

## 11A: AUTO ON — 20–30 minut

1. Ustaw DSRV MODE A i EPG AUTO ON. Zapisz ACK z atMs, STATUS i czas zegarowy.
2. Oczekuj co najmniej 3 kompletnych, poprawnych pustych Directory tego samego
   TID/hash, z odstępem między pierwszym i ostatnim co najmniej 2000 ms
   (okno do 30000 ms). Segment ani błędny CRC nie są pustym Directory.
3. W logu rozróżnij state=STOP_PENDING / reason=empty-confirmed-stop-requested
   od state=COOLDOWN / reason=stop-confirmed. Dopiero COOLDOWN i wzrost
   stopConfirmedCount oznaczają potwierdzony STOP. stopError lub STOP_PENDING
   bez potwierdzenia nie upoważniają do stwierdzenia, że usługa jest wyłączona.
4. W COOLDOWN remainingMs maleje od około 300000 ms. Nie powinien rosnąć
   licznik odbieranych produkcyjnych pakietów pustej SPI EPG komponenty.
   Porównuj delta packetGroups / packetEmpty w EPG TEST TRANSPORT.
   Audio PAD, DLS i SLS mogą nadal przychodzić — to nie oznacza wznowienia EPG.
   Pojedyncze dane wcześniej zakolejkowane po STOP trzeba odróżnić od stałego ruchu.
5. Termin liczymy od POTWIERDZENIA STOP. Po 5 minutach oczekuj jednej kontroli:
   RECHECK_START, potem RECHECK_ACTIVE / reason=recheck-started; recheckCount +1.
   Kolejna potwierdzona pustka powinna szybko wywołać jeden STOP i nowy COOLDOWN.
6. Brak kompletnego Directory przez 30 s po udanym START daje INCONCLUSIVE,
   następnie bezpieczny STOP i 5-minutowy backoff. To nie jest wynik EMPTY.
   Nieudany START daje START_ERROR; niepewny STOP pozostaje bez cooldownu
   pod kontrolą istniejącego cleanup/recovery.
7. Porównaj z OFF czas do CTS, liczbę CTS, qmaxRaw, overflow, audio/SLS i UARTdrop.
   Zapisz co najmniej 3 pełne cykle kontroli. Zmiana muxu, locku, PAD ownership
   lub rzeczywista zmiana discovery może legalnie unieważnić stary timer.
   Zwykły okresowy UA refresh (około 60 s) z tymczasowym Pending zachowuje
   termin cooldownu, lecz do ponownego potwierdzenia endpointu blokuje akcje
   (eligible=0). Zanotuj zmianę terminu, jeśli endpoint rzeczywiście się zmienił.

## 11A: C oraz B

C: ustaw DSRV MODE C, zachowaj ACK, sprawdź EPG AUTO STATUS (eligible=0).
Pakiety nadal są fizycznie odczytywane, parser EPG jest pomijany, AUTO nie
zatrzymuje usługi. To porównanie izoluje parser, nie fizyczny odbiór.

B: najlepiej zacznij w A z aktywną osobną usługą pakietową. Wyślij DSRV MODE B
przed pierwszym CTS i zachowaj ACCEPTED. B jest też dozwolone w potwierdzonym
AUTO COOLDOWN — wtedy nie wymaga ponownego fizycznego STOP. Testuj 15 minut;
żaden recheck nie może uruchomić usługi. Zapisz faktyczny wynik STOP oraz
probe=0 po jego zakończeniu. AUTO pozostaje INELIGIBLE; jego cooldown nie jest
potwierdzeniem ręcznego B. Powrót: DSRV MODE A (i EPG AUTO ON, jeśli wcześniej OFF).
Jeśli B odpowie UNSUPPORTED, nie deklaruj zatrzymania usługi.

## Powrót rzeczywistych danych / zmiana muxu

Host-testy obejmują niepuste Directory oraz poprawny obiekt EPG w rechecku:
wynik NONEMPTY, state=ACTIVE, brak dodatkowego START i brak limitu 30 s.
Jeśli emisja na 11A rzeczywiście zmieni się podczas kontroli, sprawdź to samo
na radiu. Przejście z 11A do 11B jest osobnym testem zmiany kontekstu:
stary timer/identyfikator nie może zatrzymać nowej usługi; 11B odbiera PI/SI
ciągle. Samo przełączenie muxu nie dowodzi pojawienia się danych w tym samym rechecku.

## Interpretacja i komplet logów

Doślij pełne logi z nagłówkiem: SHA i lokalne zmiany, build, czas aktywacji,
POLL/INTB, radio SPI, DAB image, mux/stacja, core/free8/UARTdrop, początkowe
oraz końcowe CTS/qmaxRaw/overflow i obserwacja audio/SLS. Przy awarii zachowaj
pierwszy CTS oraz zamrożony RAW ring. DSRV QUEUE CLEAR używaj dopiero po
zapisaniu evidence, jeżeli zaczynasz nową próbę; liczniki BOOT pozostają.

Restart ESP32 rozpoznaj po ponownym boot logu i utracie ustawienia AUTO OFF
(ON jest domyślne). Si4684/TFT cold recovery może wystąpić bez restartu ESP32;
ma własne komunikaty recovery i nowy kontekst tunera. Nie utożsamiaj obu zdarzeń.
Testy host i kompilacja nie dowodzą stabilności RF ani usunięcia wewnętrznej
przyczyny zawieszania CTS — do tego niezbędne są pomiary na fizycznym 11A.
