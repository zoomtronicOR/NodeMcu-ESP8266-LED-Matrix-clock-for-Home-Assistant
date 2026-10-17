# NodeMCU ESP8266 LED matrični sat za Home Assistant

Matrični sat na ESP8266 (NodeMCU) sa MAX7219 LED matricom (4–16 modula 8×8, podrazumevano 8×32).
Prikazuje vreme, datum, dan u nedelji i poruke, ima lokalni web interfejs i opcionu MQTT integraciju
sa Home Assistantom. Ceo interfejs i prikaz su na srpskom, latinicom ili ćirilicom.

Osnovno pravilo dizajna: **sat ne zavisi od Wi-Fi-ja, MQTT-a, web UI-ja ni senzora**. Glavna petlja je
neblokirajuća, a kvar bilo kog podsistema ne zaustavlja prikaz vremena.

## Mogućnosti

- **Vreme:** 12/24 h, opcione sekunde, trepćuća dvotačka, 3 fonta sata sa ciframa iste širine. NTP sa 3 servera, vremenska zona sa automatskim letnjim računanjem vremena. Bez mreže vreme nastavlja iz internog sata.
- **Srpski tekst:** `č ć ž š đ` i ćirilica (UTF-8). Nazivi dana i meseci se biraju latinicom ili ćirilicom. Nepodržani znakovi se prikazuju kao □.
- **Prikaz:**
  - Rotacija stranica (vreme, datum, dan, do 6 poruka), sa podesivim redosledom i trajanjem.
  - Skrolovanje levo/desno/gore/dole. Kratak tekst stoji mirno.
  - Prioritetne poruke sa vremenom isteka.
- **Osvetljaj:** dnevni i noćni nivo, gornja granica, noćni režim (smanji ili ugasi) po rasporedu i režim „ne uznemiravaj“.
- **Wi-Fi:**
  - Pristupna tačka za podešavanje (captive portal) kad mreža nije podešena ili nije dostupna posle pokretanja, ili na taster FLASH (3 s).
  - Ponovno povezivanje u rastućim intervalima; mDNS `ime.local`.
- **Web UI:** kartice Pregled, Prikaz, Poruke, Vreme, Mreža, MQTT i Sistem. Ima pregled fonta, dijagnostiku, admin lozinku i izvoz/uvoz podešavanja. Svi unosi se proveravaju na uređaju.
- **Senzori (opciono, jedan aktivan):** AHT20/AHT21, BME280/BMP280 (I²C) ili DHT22. Imaju kalibracioni pomak i sopstvene neblokirajuće drajvere. Neispravan ili odsutan senzor ne utiče na sat.
- **MQTT / Home Assistant:** asinhroni klijent (ne blokira sat), Discovery, availability, telemetrija i komande.
- **Dijagnostika:** slobodna i najniža memorija, uptime, razlog restarta, najduža petlja, statusi Wi-Fi/NTP/MQTT.

## Hardver

| Funkcija | NodeMCU | GPIO |
|---|---|---|
| MAX7219 CLK | D5 | 14 |
| MAX7219 DIN | D7 | 13 |
| MAX7219 CS | D6 | 12 |
| I²C SDA (AHT2x, BME280) | D2 | 4 |
| I²C SCL (AHT2x, BME280) | D1 | 5 |
| DHT22 podaci (izbor u UI) | D0 / D1 / D2 | 16 / 5 / 4 |

- Matricu napajati sa **5 V** iz dovoljno jakog izvora, ne iz 3,3 V pina NodeMCU ploče. ESP i matrica moraju imati zajedničku masu.
- Za pouzdan rad preporučuje se pretvarač nivoa 3,3 V → 5 V za SPI linije (npr. 74HCT125).
- Orijentacija modula se bira u web UI-ju („Tip modula“) i proverava se „Test ekranom“.

## Build i flešovanje

Potreban je [PlatformIO](https://platformio.org/).

```
pio run                 # build
pio run -t upload       # flešovanje (port je u platformio.ini)
pio device monitor      # serijski monitor, 115200
```

Fontovi (`tools/fonts/*.txt` → `tools/gen_fonts.py`) i web UI (`web/index.html` → `tools/embed_web.py`)
generišu se automatski pri svakom buildu u `src/generated/`. Web UI je ugrađen u firmware, pa nije potreban
upload fajl sistema.

## Prvo pokretanje

1. Posle flešovanja sat pokreće Wi-Fi mrežu `MatrixClock-XXXX`, sa lozinkom `matrica123`. Ista uputstva skroluju na matrici.
2. Telefonom se poveži na tu mrežu i otvori `http://192.168.4.1`.
3. Na kartici **Mreža** izaberi kućnu mrežu, unesi lozinku i klikni **Sačuvaj**. Nova IP adresa se prikazuje na matrici.
4. Na kartici **Sistem** postavi administratorsku lozinku (korisnik `admin`) i promeni lozinku pristupne tačke.

Web interfejs je namenjen lokalnoj mreži; ne izlagati ga internetu.

## MQTT teme

`<base>` je osnovna tema iz podešavanja (podrazumevano `matrix_clock_XXXX`).

| Tema | Smer | Sadržaj |
|---|---|---|
| `<base>/status` | sat → | `online` / `offline` (retained, LWT) |
| `<base>/state` | sat → | `{"brightness":4,"mode":"normal","on":true}` (retained) |
| `<base>/telemetry` | sat → | RSSI, uptime, memorija, NTP status, IP, verzija |
| `<base>/sensor` | sat → | `{"temperature":22.5,"humidity":45.0,"pressure":1013.2}` (retained, pri promeni) |
| `<base>/command/brightness` | → sat | `0`–`15` (važi do sledeće smene dan/noć) |
| `<base>/command/message` | → sat | tekst ili `{"text":"...","seconds":60}`; prazno = ukloni |
| `<base>/command/display_mode` | → sat | `normal`, `off`, `dnd` |
| `<base>/command/page` | → sat | `time`, `date`, `weekday`, `messages` |
| `<base>/command/test`, `<base>/command/restart` | → sat | bilo koji sadržaj |

## Serijska konzola (oporavak bez mreže)

Komande se kucaju na 115200 baud: `help`, `status`, `wifi <ssid>|<lozinka>`, `portal`, `msg <tekst>`, `test`, `restart`, `factory`.

## Napomena za Windows i CH340

Ako se serijski port otvori jednom, a zatim javlja grešku 31 („A device attached to the system is not functioning“),
vrati CH340 drajver na verziju 3.4.2014.8 u Device Manageru.
