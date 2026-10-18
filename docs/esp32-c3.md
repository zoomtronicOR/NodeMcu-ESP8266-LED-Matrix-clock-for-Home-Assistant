# ESP32-C3 SuperMini: povezivanje i flešovanje

Isti firmware radi i na ploči **ESP32-C3 SuperMini** (ESP32-C3FH4, 4 MB flash u čipu, USB-C).
Sve mogućnosti su iste kao na ESP8266 NodeMCU: web UI, MQTT/Home Assistant, OpenWeatherMap,
senzori i OTA. Razlikuju se samo pinovi, taster za Wi-Fi podešavanje i način flešovanja.

> **Status:** podrška za ESP32-C3 je napisana i kompajlira se, ali **još nije proverena na pravoj
> ploči**. Ako nešto ne radi, pošalji serijski log (`pio device monitor -e esp32c3`, pa komanda `status`).

## Povezivanje

Na ploči je pored svakog pina odštampan broj GPIO-a (`0`–`10`, `20`, `21`) ili `5V`, `G`, `3.3`.
Raspored pinova po ivicama se razlikuje između proizvođača, pa uvek gledaj **oznake na ploči**.

### MAX7219 matrica

| MAX7219 | ESP32-C3 SuperMini | Napomena |
|---|---|---|
| VCC | `5V` | 5 V iz USB-a; za više od 4 modula ili jak osvetljaj, poseban izvor 5 V |
| GND | `G` | zajednička masa |
| DIN | `GPIO6` | na ploči je ovaj pin često označen kao MOSI |
| CS | `GPIO7` | SS |
| CLK | `GPIO4` | SCK |

### Senzor (opciono, jedan aktivan)

| Senzor | Pin senzora | ESP32-C3 SuperMini |
|---|---|---|
| AHT20/AHT21, BME280/BMP280 (I²C) | SDA | `GPIO1` |
| | SCL | `GPIO0` |
| | VCC / GND | `3.3` / `G` |
| DHT22 | podaci | `GPIO3` (podrazumevano); može i `GPIO10`, `GPIO1`, `GPIO0`, bira se u web UI-ju |
| | VCC / GND | `3.3` / `G`, uz pull-up otpornik 10 kΩ na 3,3 V (većina modula ga ima) |

### Taster

**BOOT** (`GPIO9`): drži ga 3 s dok sat radi, pa se pokreće pristupna tačka za Wi-Fi podešavanje.
To je isti taster koji na NodeMCU ploči nosi oznaku FLASH.

### Pinove ne koristiti za druge stvari

| Pin | Razlog |
|---|---|
| `GPIO2`, `GPIO8`, `GPIO9` | „strapping“ pinovi: njihov nivo pri uključenju bira način pokretanja čipa. Na `GPIO8` je i plava LED, a na `GPIO9` taster BOOT. |
| `GPIO20`, `GPIO21` | UART (RX/TX). Bootloader na njih šalje poruke pri pokretanju. |
| `GPIO18`, `GPIO19` | USB, nisu izvedeni na ploču. |

Neke šeme označavaju `GPIO8`/`GPIO9` kao SDA/SCL. Firmware namerno koristi `GPIO1`/`GPIO0`,
da senzor ne bi mogao da ometa pokretanje i da taster BOOT ostane slobodan.

### Napajanje i nivoi

- MAX7219 radi na 5 V, a ESP32-C3 na 3,3 V, isto kao NodeMCU. Signali od 3,3 V obično rade.
  Za pouzdan rad na dužim kablovima koristi pretvarač nivoa (npr. 74HCT125) na DIN, CS i CLK.
- Pin `5V` je direktno vezan za USB napajanje. Pri jakom osvetljaju i više modula, matricu
  napajaj iz posebnog izvora od 5 V. Mase moraju biti spojene.

## Flešovanje (prvi put, preko USB-a)

USB-C je vezan direktno na čip (ugrađen USB serijski port), pa **nije potreban drajver**
(Windows 10/11, Linux i macOS ga prepoznaju sami). Kabl mora da prenosi podatke, ne samo napajanje.

1. Instaliraj [PlatformIO](https://platformio.org/), kao dodatak za VS Code ili kao CLI.
2. Poveži ploču USB-C kablom.
3. U folderu projekta pokreni:
   ```
   pio run -e esp32c3 -t upload
   ```
   Prvi build preuzima ESP32 platformu i alate, nekoliko stotina MB, pa traje nekoliko minuta.
   Port se pronalazi sam. Ako ih ima više, dodaj `--upload-port COMx`.
4. Ako upload javlja da ne može da se poveže (`Failed to connect`, `No serial data received`):
   - drži taster **BOOT**, kratko pritisni **RST** (ili isključi i uključi USB kabl), pa pusti BOOT;
   - ponovi `pio run -e esp32c3 -t upload`;
   - posle uspešnog upisa pritisni **RST**.
   To je potrebno uglavnom samo prvi put, ili ako je firmware na ploči „zaglavljen“.
5. Serijski monitor:
   ```
   pio device monitor -e esp32c3
   ```
   USB port se kratko gubi pri svakom restartu, pa prve linije loga mogu da promaknu.
   Ukucaj `status` i Enter za trenutno stanje.

Posle toga sve je isto kao na NodeMCU-u: sat pokreće Wi-Fi mrežu `MatrixClock-XXXX`
(lozinka `matrica123`), a dalje se podešava preko web UI-ja (vidi [README](../README.md#prvo-pokretanje)).

## Ažuriranje preko mreže (OTA)

- **Web UI:** kartica **Sistem → Ažuriranje firmware-a**, fajl `.pio/build/esp32c3/firmware.bin`.
- **espota:** posle postavljanja admin lozinke:
  ```
  # PowerShell
  $env:MATRIX_OTA_PASS="admin-lozinka"; pio run -e ota_c3 -t upload
  ```
  U `platformio.ini` u `[env:ota_c3]` zameni `upload_port = matrixclock-xxxx.local` imenom
  svog sata ili IP adresom.
- Svaki firmware nosi oznaku ploče. Web OTA odbija firmware za drugu ploču, na primer kad
  se `nodemcuv2/firmware.bin` greškom pošalje na ESP32-C3 ili obrnuto, a postojeći firmware ostaje.
  espota tu proveru nema, ali `pio run -e ota_c3` uvek pravi i šalje firmware za ESP32-C3.

Raspored flash memorije (`min_spiffs.csv`): 2 × 1,9 MB za firmware (trenutni + OTA) i 128 KB
za podešavanja (LittleFS). Firmware zauzima oko 1,3 MB.

## Wi-Fi problemi kod SuperMini ploča

Neke serije SuperMini ploča imaju lošu antenu. Ploča se tada ne povezuje na mrežu ili često
gubi vezu, iako je ruter blizu. Rešenje je manja snaga predajnika. U `platformio.ini`, u
`[env:esp32c3]`, ukloni `;` ispred reda:

```
    -DMATRIX_WIFI_TX_POWER=WIFI_POWER_8_5dBm
```

pa ponovo flešuj. Domet je nešto manji, ali je veza obično stabilna.

## Razlike u odnosu na ESP8266

| | ESP8266 NodeMCU | ESP32-C3 SuperMini |
|---|---|---|
| Okruženje | `nodemcuv2`, `ota` | `esp32c3`, `ota_c3` |
| Firmware | `.pio/build/nodemcuv2/firmware.bin` | `.pio/build/esp32c3/firmware.bin` |
| USB | CH340 (drajver) | ugrađen u čip (bez drajvera) |
| Taster za Wi-Fi podešavanje | FLASH (GPIO0) | BOOT (GPIO9) |
| Slobodna memorija (RAM) | ~35 KB | oko 200 KB (procena) |
| Flash za firmware | 1 MB (OTA do ~500 KB) | 1,9 MB (OTA do 1,9 MB) |
| Podešavanja | LittleFS 1 MB | LittleFS 128 KB |

Podešavanja se mogu preneti sa jednog sata na drugi: **Sistem → Rezervna kopija podešavanja →
Preuzmi**, pa na drugom satu **Učitaj iz fajla**. Lozinke se ne prenose. Pin za DHT22 koji ne postoji na novoj ploči se pri uvozu zamenjuje
podrazumevanim (`GPIO3`), pa ga po potrebi izaberi ponovo.
