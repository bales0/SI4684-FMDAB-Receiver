# Test EPG LIVE dla Andrzeja – 25.10.2026

Nowe poprawki przeszły testy na komputerze i build, ale **nie były jeszcze
wgrane ani sprawdzone przez RF**. Ten dokument nie potwierdza testu HW.
Oczekiwany czas i offset pochodzą z DAB_GET_TIME LOCAL/UTC/LOCAL;
EPG z 2024 nie jest automatycznie przenoszone do 2026.

1. Po uzgodnionym wgraniu aplikacji na 0x10000 zachować NVS. Nie używać
   receiver-complete.bin na 0x0. Sam plik testowy nie zmienia ustawień.
2. Zamknąć skrypt replay/syntetyczny. Otworzyć jeden monitor 115200 8-N-1.
   Wysłać `EPG TEST STATUS`; jeżeli mode=TEST, jednorazowo `EPG TEST STOP`.
   Nie wysyłać STOP między pomiarami LIVE, bo kasuje cache.
3. Wybrać MAN i stabilny sygnał. Zakończyć strojenie przed rozpoczęciem logu.
   `DEBUG` włącza/wyłącza diagnostykę – sprawdzić odpowiedź ON; gdy wyjdzie
   OFF, wysłać jeszcze raz. Nie włączać DEBUG SLSV/VERBOSE ani długich HEX.
4. Na MUX 11B wybrać PR Trójka i otworzyć EPG długim SL. Zapisać STATUS
   przed Directory i po 2–3 pełnych obrotach carouselu. Gdy PI nadal ma rok
   2024, oczekiwane PI_DATE_STALE / Archive z datą programu 2024, brak
   bieżącego NOW/NEXT/progress i brak zielonego Ready. Samo świeże odebranie
   starego PI nie oznacza aktualnego programu. Jeśli nadawca wyśle obecny
   rozkład i potwierdzi się clock, oczekiwane READY_LIVE i właściwy NOW.
5. Na tym samym MUX wybrać Radio Kraków (37FC), potem OFF RadioKraków
   (37FD). Oczekiwane SI_ONLY, nazwa + właściwe logo, komunikat o braku
   ramówki, bez cudzego programu i bez zielonego Ready. Otworzyć listę/detail:
   nie powinny udawać, że trwają nieskończone pobieranie brakującego PI.
6. Sprawdzić PR24/Chopin, jeśli oba nadal mają ten sam bearer. Oczekiwane
   IDENTITY_CONFLICT; nie wolno wybierać przypadkowego logo lub rozkładu.
7. Na MUX 11A: jeśli nadal nadawany jest 27-bajtowy pusty Directory,
   oczekiwane DIRECTORY_EMPTY, obiekty=0 i cyan Present. Nie przenosić tej
   obserwacji do RAW z 11B. Gdy nadawca doda obiekty, stan ma się sam zmienić.
8. Test archiwum, opcjonalnie po załadowaniu LIVE danych:

```text
EPG ARCHIVE 2024-11-12
EPG TEST STATUS
EPG ARCHIVE OFF
```

Oczekiwane ARCHIVE / SIMULATION z oryginalnym rokiem 2024; brak NOW/progress.
UTC DAB i oryginalne PI nie mogą zostać przepisane. To nie jest mapowanie
aktualnej godziny do starego programu. Wirtualny NOW testuje się osobno przez
DEMO/replay z oznaczeniem TEST i `EPG TEST TIME`, nie jako dowód LIVE.

9. Przełączyć stację, MUX, DAB/FM, wrócić. Po utracie locku nie może pozostać
   stare Ready; po powrocie potrzebna jest nowa generacja poprawnego clock.
   Sprawdzić audio analogowe/I²S, DLS i slideshow (JPEG/PNG/Adam7), równocześnie
   zbierając status przed/po. Bez zgody nie restartować ani nie zmieniać NVS.
10. Włączyć DEBUG tylko do krótkiego zapisu i zebrać:

```text
EPG TEST STATUS
```

Przesłać pełne linie EPG DATA/RX/RAM/LOGO/POOLS/STATE/CLOCK oraz
[DAB/CLOCK] type=0/type=1 i confirm paired/offset. W załączniku opisać:
aktualną datę i godzinę lokalną + znane UTC, rzeczywisty LTO z nadawania,
MUX, SID, PI/SI/PNG, treść statusu na TFT i wygląd logo.
Jeżeli TIME_UNKNOWN, nie zgadywać CET +60 z dawnego PI: potrzebne surowe
clock odpowiedzi. Poprawny typ 1 na zainstalowanym FW trzeba potwierdzić.

W dłuższym pomiarze sprawdzić brak narastającego ubytku RAM (free8/free32,
largest/min), CTS+=0 i overflow+=0; UARTdrop liczy utracone logi, nie musi
znaczyć utraty DSRV. Niedokończone/długie HEX nie są wiarygodnym pomiarem
całego strumienia. Proszę potwierdzić słuchowo brak przerw audio i wizualnie
slideshow – sam `audio=1` tego nie dowodzi.

Raport PC i zakres zmian: [EPG_LIVE_TIME_REPORT_20261025.md](EPG_LIVE_TIME_REPORT_20261025.md).
