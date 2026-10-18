# Home Assistant: slanje poruka i komandi na sat

Primeri akcija za automatizacije i skripte u Home Assistantu. Sat prima komande preko MQTT-a.
Ako je u kartici MQTT uključen Discovery, HA sam napravi uređaj sa entitetima
(Poruka, Osvetljaj, Režim prikaza, Prikaži stranicu, Test ekran, Restart…).

Primeri koriste YAML sintaksu iz HA 2024.10 i novijeg (`triggers:`, `actions:`, `action:`).
U starijim verzijama piše se `trigger:`, `action:` i `service:`.

## Pre početka

- **Osnovna tema** (`<base>`) je u kartici MQTT web UI-ja, podrazumevano `matrix_clock_xxxx`
  (`xxxx` je deo ID-a čipa). U primerima zameni `matrix_clock_xxxx` svojom temom.
- **ID entiteta** proveri u HA: *Settings → Devices & services → MQTT → uređaj sata*.
  U primerima je `text.matrixclock_xxxx_poruka`.
- Poruke **ne slati kao `retain: true`**. Zadržana poruka bi se ponovo prikazala pri svakom
  ponovnom povezivanju sata sa brokerom.

## Kako se poruka prikazuje

Tema `<base>/command/message` prima običan tekst ili JSON:

| Sadržaj | Značenje |
|---|---|
| `Večera je gotova` | poruka, traje 60 s |
| `{"text":"Večera je gotova","seconds":120}` | poruka, traje 120 s (1–3600) |
| prazno, ili `{"text":""}` | odmah ukloni trenutnu poruku |

- Poruka odmah prekida prikaz vremena i ponavlja se dok ne istekne zadato vreme.
  Kratka poruka stoji, a duga se skroluje i uvek se prikaže bar jednom do kraja.
- Nova poruka zamenjuje prethodnu.
- Najviše 150 bajtova UTF-8 (slova `č ć ž š đ` zauzimaju po 2 bajta). Duža poruka se odbacuje.
- Pismo (latinica ili ćirilica) se bira u web UI-ju; poruka se piše normalno, latinicom.
- U režimu `dnd` (ne uznemiravaj) poruke se ne prikazuju, a u režimu `off` ekran je ugašen.
  Poruka ipak ističe po zadatom vremenu, pa se ne pojavi naknadno.

## 1. Najjednostavnije: entitet „Poruka“

Poruka traje 60 s.

```yaml
actions:
  - action: text.set_value
    target:
      entity_id: text.matrixclock_xxxx_poruka
    data:
      value: "Večera je gotova"
```

## 2. Poruka sa zadatim trajanjem

```yaml
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/message
      payload: '{"text":"Stiže paket","seconds":300}'
```

Kad tekst dolazi iz šablona (stanja senzora, imena osobe…), JSON je najbezbednije
napraviti filterom `to_json`, jer on ispravno obradi navodnike i specijalne znakove:

```yaml
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/message
      payload: >-
        {{ {"text": "Napolju je " ~ states("sensor.spoljna_temperatura") ~ " °C",
            "seconds": 30} | to_json }}
```

## 3. Ukloni poruku

```yaml
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/message
      payload: ""
```

## 4. Skripta za višekratnu upotrebu

Jedna skripta, pa iz bilo koje automatizacije samo `script.poruka_na_sat` sa tekstom i trajanjem.
Skripta se dodaje u *Settings → Automations & scenes → Scripts → ⋮ → Edit in YAML*.

```yaml
alias: Poruka na sat
mode: queued
fields:
  tekst:
    name: Tekst
    required: true
    selector:
      text:
  sekundi:
    name: Trajanje (s)
    default: 60
    selector:
      number:
        min: 5
        max: 3600
        unit_of_measurement: s
sequence:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/message
      payload: >-
        {{ {"text": tekst[:60], "seconds": sekundi | int(60)} | to_json }}
```

`tekst[:60]` skraćuje dugačak tekst na 60 znakova, da ne pređe ograničenje od 150 bajtova.

Upotreba:

```yaml
actions:
  - action: script.poruka_na_sat
    data:
      tekst: "Veš mašina je završila"
      sekundi: 600
```

## Primeri automatizacija

### Neko je na vratima

Poruka na 2 minuta, uz pun osvetljaj. Ručni osvetljaj važi do sledeće smene dan/noć;
ovde se posle 2 minuta vraća na 3 (stavi svoju uobičajenu vrednost).

```yaml
alias: Sat - zvono na vratima
triggers:
  - trigger: state
    entity_id: binary_sensor.zvono
    to: "on"
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/brightness
      payload: "15"
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/message
      payload: '{"text":"Neko je na vratima!","seconds":120}'
  - delay: "00:02:00"
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/brightness
      payload: "3"
mode: restart
```

### Veš mašina je završila

```yaml
alias: Sat - veš mašina
triggers:
  - trigger: numeric_state
    entity_id: sensor.ves_masina_snaga
    below: 3
    for: "00:03:00"
actions:
  - action: script.poruka_na_sat
    data:
      tekst: "Veš mašina je završila"
      sekundi: 900
```

### Otvoren prozor dok radi grejanje

```yaml
alias: Sat - otvoren prozor
triggers:
  - trigger: state
    entity_id: binary_sensor.prozor_dnevna
    to: "on"
    for: "00:05:00"
conditions:
  - condition: state
    entity_id: climate.dnevna
    state: heat
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/message
      payload: '{"text":"Zatvori prozor u dnevnoj sobi","seconds":600}'
```

### Ko je stigao kući

```yaml
alias: Sat - dobrodošlica
triggers:
  - trigger: state
    entity_id:
      - person.osoba1
      - person.osoba2
    to: home
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/message
      payload: >-
        {{ {"text": "Dobro došli, " ~ trigger.to_state.name, "seconds": 60} | to_json }}
```

### Podsetnik u zadato vreme

```yaml
alias: Sat - podsetnik za smeće
triggers:
  - trigger: time
    at: "20:00:00"
conditions:
  - condition: time
    weekday: [tue, fri]
actions:
  - action: script.poruka_na_sat
    data:
      tekst: "Iznesi smeće"
      sekundi: 1800
```

### Noću bez poruka, ujutro normalno

```yaml
alias: Sat - noćni režim
triggers:
  - trigger: time
    at: "23:00:00"
    id: noc
  - trigger: time
    at: "07:00:00"
    id: dan
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/display_mode
      payload: "{{ 'dnd' if trigger.id == 'noc' else 'normal' }}"
```

Isto se može uraditi i entitetom „Režim prikaza“:
`action: select.select_option` sa `option: dnd`.

### Ugasi ekran kad niko nije kod kuće

```yaml
alias: Sat - niko kod kuće
triggers:
  - trigger: state
    entity_id: zone.home
    to: "0"
    id: prazno
  - trigger: numeric_state
    entity_id: zone.home
    above: 0
    id: neko
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/display_mode
      payload: "{{ 'off' if trigger.id == 'prazno' else 'normal' }}"
```

### Prikaži spoljnu temperaturu na zahtev

Odmah prebacuje na stranicu „Napolju“ (OpenWeatherMap), zatim se rotacija nastavlja normalno.

```yaml
actions:
  - action: mqtt.publish
    data:
      topic: matrix_clock_xxxx/command/page
      payload: weather
```

Stranice: `time`, `date`, `weekday`, `messages`, `sensor`, `weather`.

## Sve komande

| Tema | Sadržaj |
|---|---|
| `<base>/command/message` | tekst ili `{"text":"...","seconds":60}`; prazno = ukloni |
| `<base>/command/brightness` | `0`–`15`, važi do sledeće smene dan/noć |
| `<base>/command/display_mode` | `normal`, `off`, `dnd` (ne čuva se posle restarta) |
| `<base>/command/page` | `time`, `date`, `weekday`, `messages`, `sensor`, `weather` |
| `<base>/command/test` | bilo šta: test ekran |
| `<base>/command/restart` | bilo šta: restart sata |

Neispravna komanda se odbacuje. Broj odbačenih komandi je polje `rejected` u temi
`<base>/telemetry`, a u serijskom logu piše `MQTT: odbačena komanda`.
