# HeizCore – Heizungsregler-Firmware für KinCony KC868-A6 / KC868-A8

Vollständige, witterungsgeführte Heizungsregelung nach dem Vorbild des
**OEG KMS-D** auf ESP32-Basis (ESP-IDF + FreeRTOS). Nach dem Flashen
sofort einsatzbereit – die gesamte Konfiguration erfolgt über die
integrierte Weboberfläche im Browser.

## Funktionsumfang

| Bereich | Funktionen |
|---|---|
| **Wärmeerzeuger** | Pellet-, Scheitholz-, Hackschnitzel-, Gas-, Ölkessel, Wärmepumpe, Elektro, Hybrid · Zustandsautomat (Aus/Bereit/Zündung/Leistungsbrand/Teillast/Volllast/Gluterhaltung/Störung) · Rücklaufanhebung · Zünd- und Übertemperaturüberwachung · Betriebsstunden, Brennerstarts, Verbrauchsschätzung |
| **Pufferspeicher** | 5-Punkt-Messung, Ladezustand %, Restenergie kWh, Lade-Sollfenster |
| **Heizkreise** | bis 20 Kreise, gemischt (Dreipunkt-Mischer, PI-geregelt) und ungemischt · Heizkurve (Steigung/Niveau) oder Mehrpunktkurve (6 Stützstellen) · Raumeinfluss · Heizgrenze/Sommerabschaltung · Betriebsarten Automatik/Tag/Nacht/Sommer/Urlaub/Frostschutz/Party |
| **Warmwasser** | Boilerladung mit Hysterese, WW-Vorrang, Zeitprogramm, wöchentlicher Legionellenschutz, Urlaubsmodus |
| **Solar** | Differenzregelung ΔT-Ein/Aus, Kollektorschutz, Speichermaximum, Ertragszähler Tag/Monat/Jahr/Gesamt |
| **Zeitprogramme** | 24 Programme, 6 Schaltfenster/Tag, Wochentagsmasken, über Mitternacht |
| **Weboberfläche** | responsives SPA direkt vom ESP32 (LittleFS) · **animiertes Hydraulikschema** (Wasserfluss, Pumpenrotation, Flamme, Speicherschichtung, 1-s-Aktualisierung über WebSocket) · Drag&Drop-Hydraulikeditor mit Plausibilitätsprüfung · Dark/Light · Deutsch/Englisch |
| **Schnittstellen** | MQTT inkl. Home-Assistant-Auto-Discovery und Node-RED-tauglichen Topics · Modbus RTU (RS485) und TCP, jeweils Master **oder** Slave · REST-API · WebSocket |
| **Benutzer** | Rollen Administrator/Servicetechniker/Benutzer/Gast, Sitzungscookies, SHA-256+Salt, erzwungene Passwortänderung beim Erststart |
| **Datenlogger** | 60-s-Raster, 30 Tage Detaildaten auf LittleFS, Diagramme 24 h/7 d/30 d, Export CSV/JSON |
| **System** | OTA per Datei-Upload oder URL mit Bootloader-Rollback · Backup-Export/-Import der Kompletteinstellung · Werksreset · Alarmhistorie |

## Projektstruktur

```
kc868-heizregler/
├── CMakeLists.txt            ESP-IDF-Projekt + LittleFS-Image
├── partitions.csv            NVS · 2×OTA (1,6 MB) · LittleFS (616 kB)
├── sdkconfig.defaults
├── main/                     Einstiegspunkt, Startreihenfolge
├── components/
│   ├── board/                HAL: PCF8574, DS18B20 (Bit-Bang-OneWire),
│   │                         NTC-ADC, Relais, Digitaleingänge
│   ├── core/                 Datenmodell (mutexgeschützt), Konfig-Store
│   │                         (JSON auf LittleFS, atomar), Alarmverwaltung
│   ├── control/              Regelungs-Task 1 s: Kessel, Puffer, WW,
│   │                         Solar, Heizkreise, Heizkurven, PID, Zeitprg.
│   ├── netsvc/               ETH/WLAN/AP-Fallback, NTP, MQTT+Discovery,
│   │                         Modbus RTU/TCP, OTA
│   ├── websrv/               HTTP-Server, REST-API, WebSocket-Push, Auth
│   └── datalog/              Binär-Ringlogger + CSV/JSON-Export
└── web/index.html            komplette Weboberfläche (eine Datei)
```

## Bauen und Flashen

Voraussetzung: **ESP-IDF ≥ 5.1** ([Installation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)).

```bash
cd kc868-heizregler
idf.py set-target esp32
idf.py build                  # lädt littlefs + esp-modbus automatisch
idf.py -p /dev/ttyUSB0 flash  # flasht App UND Weboberfläche (LittleFS)
idf.py -p /dev/ttyUSB0 monitor
```

Das LittleFS-Image aus `web/` wird durch `FLASH_IN_PROJECT` automatisch
mitgeflasht – kein separater Schritt nötig.

## Erste Inbetriebnahme

1. Nach dem Flashen versucht das Gerät Ethernet (nur A8) und WLAN.
   Ohne Verbindung öffnet es nach 30 s den Access Point
   **„HeizCore-Setup“** → `http://192.168.4.1`
2. Anmeldung: **admin / admin** – die Weboberfläche erzwingt sofort
   ein neues Passwort.
3. **Einstellungen → Fühler**: „Neu suchen“ listet alle DS18B20 mit
   ROM-Code und Live-Temperatur. ROM-Code in die Referenzspalte der
   passenden Rolle eintragen (Kessel, Puffer oben/Mitte/unten, Boiler,
   Außen, Vorlauf HK1 …). NTC-Fühler: Quelle „NTC“, Referenz 0–3.
4. **Einstellungen → Relais**: Funktionszuordnung prüfen (Vorbelegung
   für A8: R1 Brenner, R2 Kesselpumpe, R3 Pumpe HK1, R4/R5 Mischer HK1,
   R6 Pumpe HK2, R7 Boilerladepumpe, R8 Solarpumpe).
5. **Einstellungen → Netzwerk**: WLAN-Zugangsdaten, MQTT-Broker,
   Modbus. Netzwerk-/Modbus-Änderungen werden nach Neustart wirksam.
6. Dashboard zeigt die Anlage als animiertes Hydraulikschema.

## Hardware-Hinweise (wichtig)

Die Pin-Vorbelegung entspricht der verbreiteten KinCony-Dokumentation:
I²C SDA=GPIO4/SCL=GPIO5, Eingänge PCF8574 @0x22, Relais @0x24
(low-aktiv), OneWire GPIO32/33, Analogeingänge GPIO36/39/34/35,
RS485 TX=GPIO27/RX=GPIO14, Ethernet (A8) LAN8720 MDC=23/MDIO=18,
RMII-Takt-Eingang GPIO0.

**KinCony hat Pinbelegungen zwischen Board-Revisionen geändert.**
Vor dem Anschluss von Fühlern und 230-V-Verbrauchern die Belegung der
eigenen Board-Revision gegen die KinCony-Produktseite prüfen und ggf.
in `components/board/board.c` (`load_defaults`) anpassen.

NTC-Eingänge erwarten einen 10-kΩ-Spannungsteiler gegen 3,3 V mit
NTC 10k B3950 (Beta-Gleichung, in `board.c` änderbar).

## MQTT-Topics (Auszug, Präfix `heizcore`)

```
heizcore/status                     online/offline (LWT, retained)
heizcore/outdoor/temp
heizcore/boiler/{temp,return,flue,power,state,runtime_h,starts,consumption_kg}
heizcore/buffer/{t1..t5,charge,energy_kwh}
heizcore/dhw/{temp,setpoint,charging}
heizcore/solar/{collector,store,pump,yield_day,yield_year}
heizcore/hc/<n>/{flow,flow_set,room,mixer,mode,pump}
heizcore/relay/<n>/state
heizcore/alarm                      JSON {level,code,msg,t}

Befehle:
heizcore/hc/<n>/mode/set            0=Auto 1=Tag 2=Nacht 3=Sommer 4=Urlaub 5=Frost 6=Party 7=Aus
heizcore/hc/<n>/day_temp/set        5–30 °C
heizcore/dhw/setpoint/set           30–70 °C
heizcore/boiler/setpoint/set        40–90 °C
heizcore/sensor/<n>/value/set       für Fühler mit Quelle MQTT
```

Home-Assistant-Discovery ist standardmäßig aktiv: Alle Temperaturen,
Ladezustand und Erträge erscheinen automatisch als Entitäten.

## Modbus-Registerkarte (Slave-Betrieb)

Temperaturen ×10, 0x8000 = ungültig.

| Bereich | Adresse | Inhalt |
|---|---|---|
| Input (FC04) | 0 | Außentemperatur |
| | 1–4 | Kessel: Ist, Rücklauf, Leistung %, Zustand |
| | 10–15 | Puffer T1–T5, Ladezustand % |
| | 20–21 | WW: Ist, Ladung 0/1 |
| | 30–31 | Solar: Kollektor, Speicher |
| | 40+4·n | HK n: VL-Ist, VL-Soll, Raum, Mischer % |
| Holding (FC03/06/16) | 100 | Kessel-Soll ×10 (schreibbar) |
| | 101 | WW-Soll ×10 (schreibbar) |
| | 110+2·n | HK n: Betriebsart, Raumsoll Tag ×10 (schreibbar) |
| Coils (FC01) | 0–7 | Relaiszustände (lesend) |

Master-Betrieb: Fühler mit Quelle „Modbus“ und Referenz `slave:register`
(z. B. `2:100`) werden zyklisch per FC04 gelesen.

## REST-API (Auszug)

```
POST /api/login {user,pass}        GET  /api/state
GET/POST /api/config               POST /api/hc/<n> {mode,rday,rnight,holidayUntil,partyUntil}
GET  /api/sensors                  POST /api/sensors/rescan
GET  /api/alarms[/history]         POST /api/boiler/reset
GET  /api/log?range=24h|7d|30d     GET  /api/log/export?fmt=csv|json
GET/POST /api/schema               GET/POST /api/backup
POST /api/ota  (Binärupload)       POST /api/ota/url {url}
POST /api/system/reboot|factory    GET  /api/system/info
```

## Sicherheitshinweis

Diese Firmware steuert eine Feuerungs-/Heizungsanlage. Sie ersetzt
**keine** bauartgeprüften Sicherheitseinrichtungen: Sicherheits-
temperaturbegrenzer (STB), thermische Ablaufsicherung und
Sicherheitsventile müssen unabhängig von dieser Regelung in Hardware
vorhanden und geprüft sein. Elektrische Installation (230 V an den
Relais) nur durch Fachpersonal, Einstellungen an Kesselparametern in
Abstimmung mit dem Kesselhersteller.

## Bekannte Grenzen / Ausbau

- E-Mail/Telegram-Versand: Konfiguration und Alarm-Hooks sind
  vorhanden; der eigentliche SMTP-/HTTPS-Versand ist als nächster
  Ausbauschritt in `alarms_register_notify()` einzuhängen.
- Modbus-Rekonfiguration erfordert einen Neustart (esp-modbus lässt
  sich nicht sauber im Betrieb neu initialisieren).
- Die Firmware wurde statisch geprüft, aber nicht auf realer Hardware
  gebaut/getestet – vor dem produktiven Einsatz `idf.py build` und
  einen Probelauf ohne angeschlossene Verbraucher durchführen.
