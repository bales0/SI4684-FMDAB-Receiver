# Uchované vstupy EPG testů

Tyto soubory jsou trvalé vstupy mimo `.pio` a `_usertesting`. Původní kopie
zůstaly zachované. `manifest.json` uvádí původ, velikosti a SHA-256.
Nejde o nová data z aktuálního vysílání: skutečný záznam obsahuje historický
program pro 12. 11. 2024. Při replay musí zůstat zapnutý virtuální testovací čas.

| Soubor | Účel |
|---|---|
| device-monitor-261023-160924.log | Původní skutečný UART RAW záznam MSC/SPI; obsahuje i neúplné výpisy, které replay vyřazuje. |
| SI4684_MUX11B_EPG_kompletni_dekodovani.zip | Původní referenční dekódování pro hostitelský regresní test. |
| synthetic_programme_groups.bin | Syntetický MOT adresář, SI a PI: Synthetic EPG One, tři pořady; bez loga. |
| synthetic_logo_0.png / synthetic_logo_1.png | Stejné RGB logo 96×64, běžné PNG / Adam7. |
| synthetic_logo_0_groups.bin / synthetic_logo_1_groups.bin | Kompletní syntetický program včetně SI odkazu a MOT objektu loga. |
| hardware_synthetic_*_20261024.json | Historické výsledky testů konkrétního firmware; příznak loop označuje stav tehdy, nikoli kontrolu právě běžícího procesu. |

Binární streamy mají opakovaně dvoubajtovou délku little endian a MSC skupinu
včetně CRC16. Nejsou to samostatné PNG ani hotové ASCII E1/E2 řádky.
DSRV hlavičky, session a CRC32 UART přenosu sestavuje klient.

Příkazy spouštějte z kořene projektu. Offline ověření všech uložených hashů,
regenerace shodných syntetických PNG/streamů a kontrola MSC CRC bez COM portu:

```powershell
python tests/epg/synthetic_logo_test.py --verify-fixtures
```

Skutečný záznam přes E1 (port musí být volný, připravený firmware s EPG):

```powershell
python tools/epg_replay.py tests/fixtures/epg/device-monitor-261023-160924.log --port COM4 --baud 115200 --station 40e231533233 --time 2024-11-12T04:30:00+01:00 --loop
```

Syntetický program přes E2, všech devět scénářů a závěrečná PAD smyčka;
nepotřebuje předem generovat data v `.pio`:

```powershell
python -u tools/epg_synthetic.py --port COM4 --input tests/fixtures/epg/synthetic_programme_groups.bin --loop
```

Test čtyř kombinací MSC/PAD × běžné/Adam7 PNG a následná PAD Adam7 smyčka:

```powershell
python -u tests/epg/synthetic_logo_test.py --port COM4
```

Skript používá uchované vstupy a nejdříve ověří manifest; nové reporty a log
ukládá do `.pio/epg_acceptance`. EPG musí být viditelné v přehledu pro kontrolu
automatem očekávaného Adam7 `draw=1`. Na konci zůstane logo ve smyčce;
Ctrl+C požádá o STOP a návrat LIVE. Port nesmí současně vlastnit jiný klient.
Virtuální stanice se nepřelaďuje, NVS ani firmware se nepřepisují.

Hostitelská kontrola skutečného záznamu proti uložené referenci:

```powershell
python tests/test_epg_replay.py --input tests/fixtures/epg/device-monitor-261023-160924.log --reference tests/fixtures/epg/SI4684_MUX11B_EPG_kompletni_dekodovani.zip
```

Původní testy mohou mít stále výchozí cesty do `_usertesting`/`Specification`;
příkazy výše explicitně vybírají uchované kopie. `.pio` lze znovu vytvořit;
tyto vstupy po jeho vyčištění zůstanou. Vstupy nejsou součástí firmware.
Syntetická injekce ověřuje MCU zpracování DSRV, nikoli RF ani odbalení PAD v čipu.
