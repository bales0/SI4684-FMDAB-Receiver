# MUX 11A: živý RF test X-PAD a samostatného packet SPI

Tento postup je pro nový lokální firmware s příkazem `EPG TEST TRANSPORT`.
**Nový RF test zatím nebyl proveden.** Syntetický PASS neprokazuje obsah MUX 11A.
Prázdný packet Directory nevylučuje SPI přes X-PAD jiné nebo téže audio stanice.

## Příprava a příkazy

Použij jeden sériový monitor: 115200 Bd, 8-N-1, zakončení LF/CRLF. Zavři PC
replay/syntetický odesílač. Do uloženého protokolu nevkládej účet, lokální
cesty ani konfiguraci sériového portu. Tento postup neprovádí flash ani zápis NVS;
nahrání nové aplikace je samostatný krok vyžadující souhlas.

```text
EPG TEST HELP
EPG TEST STATUS
```

Ověř, že HELP obsahuje TRANSPORT a STATUS vrací `mode=LIVE`. Jen pokud běží
TEST, zastav odesílač a jednorázově pošli `EPG TEST STOP`. STOP maže EPG cache;
neposílej ho mezi měřeními LIVE. `EPG TEST TRANSPORT` funguje i bez ENABLE/DEBUG
 a nealokuje druhý Core ani nový MOT buffer.

Přepni rádio jeho ovládáním do DAB a MAN, zajisti stabilní příjem. Pokud chceš
ladit přes UART, běžné příkazy jsou:

```text
ENABLE=1
TUNE=24
```

Index 24 v tabulce `:FREQ` odpovídá 11A / 216928 kHz; ověř aktuální tabulku.
Po dokončení ladění vyber audio službu ovladačem rádia nebo příkazem
`SERVICE=<index>` podle aktuálního seznamu `$L` (za `<index>` dosadit skutečné
číslo). SID není index SERVICE. FM ladění tímto protokolem není podporováno.
Během jednoho měřicího okna stanici neměň.

`DEBUG` je přepínač: ověř odpověď `[DEBUG] diagnostics ON`; pokud právě vypnul
logování, pošli ho znovu. Nepoužívej `DEBUG SLSV`/`DEBUG VERBOSE` ani dlouhé
HEX dumpy. `DEBUG SPI` spouští jinou konečnou sondu a mění packet lifecycle;
pro tento test není potřeba, produkční příjem běží automaticky.

## Měření jedné audio služby

1. Počkej na lock a platný UA scan. Pošli:

```text
EPG TEST TRANSPORT
```

Čekej na `EPG TRANSPORT END` (obvykle asi 1,3 s). Při nedostatku místa na UART
výpis počká. `pending` znamená, že předchozí požadavek ještě běží.
`EPG TRANSPORT CANCELLED context changed` znamená reset/změnu kontextu během
výpisu: nevyhodnocuj částečnou sadu a zopakuj příkaz po ustálení rádia.

2. Zapiš `EPG RADIO`: DAB režim, index/frekvenci, EID/ECC, lock, tune/switching,
CTS, overflow a errors. `requestedIndex` je požadavek tuneru, proto při
`tune=1` ještě netvrď, že cílový MUX už je přijímaný. `channelIndex` v OWNER
je UI index. Sleduj zejména stabilní stav bez probíhajícího přeladění.

3. `EPG AUDIO` popisuje **aktuální** audio SID a plný CID. `ua=confirmed` znamená
ověřené SPI 0x007; `unknown`, `failed` a `not-signalled` rozlišují ostatní případy.
`identity=5` obsahuje zároveň SPI i Slideshow, `identity=4` je jen Slideshow,
`identity=2` SPI, `identity=0` zatím neověřeno, `identity=3` chyba odpovědi,
`identity=1` platná UA bez SPI/Slideshow. Potřebné jsou také inventory=1,
complete=1, gen/published a failed. `usable=0` není sám o sobě důkaz absence UA.
Pokud chceš celý seznam UA včetně 0x002/0x007, při DEBUG zachyť `[DAB/UA]` /
`[DAB/COMP]`; přepnutí DEBUG neprovádí novou hardwarovou UA transakci.

4. `EPG PACKET` odděluje vybraný packet endpoint od sledovaného fyzického
kontextu. `selected=1` není důkaz přijatých dat; `tracked` s `probe=4` značí
kontext ve fázi Observing. `confirmed` počítá SPI reklamy i mimo packet transport,
proto ho nečti jako počet packet endpointů. Stav `confirmed/PAD; no packet`
je legitimní. Neověřený packet MOT nebo neúplný scan zůstává neověřený.

5. Porovnej oba řádky `EPG TRANSPORT` a `EPG MOTPATH`:

- PACKET: source=0, raw SID/CID, počet type6, `dirs`, `empty`, `dirBytes`,
  objects a TID. Přijaté prázdné Directory mají být explicitně v PACKET řádku.
- PAD: source=1 / UA=0007 kandidáti, admitted, validní skupiny, DIR/SI/PI/PNG.
  Raw UATYPE při source=0 nebo source=2 není důkaz X-PAD SPI.
- `headers=a/b/c/d` počítá DATA_SRC 0/1/2/3; `last` je poslední SPI kandidát
  dané větve, nebo poslední hlavička, pokud zatím kandidát nebyl. DLS/SLS se
  nepovažuje za SPI jen proto, že běží v PAD.
- Plné command CID a krátké DSRV CID se mohou lišit: ukládej obě původní čísla,
  neopravuj je globálním maskováním. Router povoluje jen specifické ověřené varianty.
- `dirs` označuje ověřené kompletní Directory. U nevlastněné větve lze bez
  druhého bufferu přímo ověřit celé jednosegmentové prázdné Directory nebo
  opakování dříve ověřeného obsahu. Nový nenulový/fragmentovaný nevlastněný
  Directory nelze z těchto čítačů automaticky považovat za dekódovaný.

6. `EPG REJECT` ukazuje ua/generation/sid/cid/inactive/dscty/state/physical/
length/read/replay/owner/data/no-endpoint/busy. `owner` u packet větve je při
aktivním PAD očekávané oddělení toků. `data` není nutně výpadek RF: znamená
odmítnutí Core či první vadnou obálku, CRC má samostatný čítač. `reason=waiting-data`,
`incomplete-directory` a po pěti minutách `directory-timeout` nejsou
`empty-directory`. Timeout sám nepřepíná na jiný transport.

7. Pošli také `EPG TEST STATUS`. Jeho STATE/DATA/LOGO popisují pouze právě
vlastněný Core, nikoli všechny transporty najednou. Rozliš SI_ONLY,
DIRECTORY_EMPTY, PI_DATE_STALE, TIME_UNKNOWN, WAIT_DIR a READY_LIVE. Logo/SI
bez PI je platná služba. Historické PI z 2024 se ani po čerstvém přijetí nesmí
vydávat za současný NOW/NEXT z roku 2026.

8. Sleduj alespoň **5 minut**, případně déle podle carouselu. Zopakuj TRANSPORT
 a STATUS přibližně každých 30 s; zaznamenej skutečnou délku okna a počet opakování.
UA se za vhodných podmínek znovu ověřuje po 60 s. Porovnávej rozdíly čítačů
při stejném SID/CID a epoch; po změně endpointu/ladění začíná jiná instance.
Čítače jsou kumulativní, výpis je postupný, nikoli atomický snímek.

Během stabilního PAD nesmějí přírůstky PACKET empty zvyšovat `EPG RESETS`,
mazat PAD metadata nebo snižovat indikátor jen kvůli prázdnému packet toku.
Ready může zmizet z jiného doloženého důvodu: ztráta hodin/locku, stáří PI,
změna identity apod. `stateChanges` počítá logické stavy, nikoli každé kreslení TFT.

9. Zkontroluj sluchem audio a vizuálně slideshow/DLS. `audio=1` a `SLS=1`
samy nedokazují zvuk bez přerušení ani správné dekódování obrázku. HEALTH:
free8/largest8/free32/largest32, min, UARTdrop, core, arena, cached/limited,
timeout/busy/invalid/CRC. RADIO: CTS a overflow. Požadujeme bez progresivního
úbytku RAM, bez nových CTS/overflow problémů; UARTdrop počítá ztracené logy,
nelze ho zaměnit za ztrátu DSRV paketů.

10. Až po skončení okna vyber další audio stanici na stejném MUX a opakuj.
X-PAD se může lišit podle audio služby. Není potřeba přelaďovat na ostatní
stanice kvůli jejich programům uvnitř právě přijímaného carouselu. Potom proveď
kladnou kontrolu na MUX 11B (index 25 / 218640 kHz) a rozliš aktuální program
od historického SI/PI. Na konci vypni DEBUG s potvrzením OFF a případně pošli
`ENABLE=0`. Tím nevypínáš živý EPG příjem; nový TRANSPORT výpis není periodický.

## Rozhodovací matice

| Pozorovaný packet Directory | Pozorování aktuálního audio PAD | Dovolený závěr |
|---|---|---|
| validní, 0 objektů | UA=7, vlastní validní nenulový DIR + SI/PI | Prázdný packet a SPI přes X-PAD současně prokázány na této stanici. |
| validní, 0 objektů | UA=7, DIR + SI/PNG bez PI | X-PAD SPI metadata prokázána, rozpis pro stanici není deklarován. |
| validní, 0 objektů | UA=7, nic kompletního nebo chyby | X-PAD signalizováno; příjem/obsah neověřen. Uvést přesnou bránu/timeout. |
| validní, 0 objektů | kompletní úspěšná audio UA bez 7 | X-PAD SPI na této audio komponentě není potvrzeno; nikoli závěr o celém MUX. |
| validní, nenulový | bez PAD SPI | Standardní packet SPI. |
| neověřený/chybí | validní PAD SI/PI | SPI přes X-PAD prokázáno, packet není podmínkou. |
| libovolný | neúplná/chybná UA inventura | Stav PAD neznámý; neoznačovat za nepřítomný. |

Přítomnost historického PI potvrzuje přenos SPI, ale ne aktuální program.

## Příklad – výhradně syntetický, není to RF log MUX 11A

Následující zkrácené řádky ilustrují výsledek hostitelského scénáře A;
číselné identity jsou testovací a nelze je přepsat jako skutečnou konfiguraci 11A.

```text
EPG OWNER mode=LIVE owner=PAD ... state=READY_LIVE ...
EPG MOTPATH path=PACKET groups=16 valid=16 ... dirs=16 unique=1 repeats=15 empty=16 tid=8CC2 dirBytes=16 objects=0 ...
EPG MOTPATH path=PAD groups=6 valid=6 ... dirs=2 unique=1 repeats=1 empty=0 tid=8CC2 dirBytes=39 objects=2 SI=1 PI=1 PNG=0 programmes=3 ...
EPG REJECT path=PACKET ... owner=15 ...
EPG TRANSPORT END
```

`mode=LIVE` zde označuje produkční větev v mocku s testovacími hodinami,
ne fyzický příjem. Celá zachycená skupina packet Directory má **27 B**, zatímco
vlastní Directory uvnitř má **16 B**. Dřívější 27B body fixture je pouze obecný
syntetický test prázdného Directory, nikoli přesná kopie tohoto 11A záznamu.

Původní log má 422 CRC-validních packet skupin; omezené vzorky PAD UA ukazují
slideshow 0x002. Neprokazují ani nevylučují, že jiná/stejná audio služba vysílá SPI.

## Co předat jako výsledek

Datum, délku okna, kanál/frekvenci, EID/ECC, audio SID/CID, packet SID/CID,
UA stav/generaci, owner, přírůstky obou větví, Directory objects a SI/PI/PNG,
reset důvody, dostupnost hodin, skutečné TFT a pozorování audio/SLS. Nepřidávej
osobní cesty či účet. Uveď samostatně: technicky možné ANO; nové RF na 11A
PROKÁZÁNO / NEOVĚŘENO podle výše uvedených důkazů; syntetická odolnost je
jiný výsledek. Závada zdroje nebo chybějící obsah jedné větve není nepřítomnost
EPG v celém multiplexu.
