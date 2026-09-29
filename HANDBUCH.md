# Waschmaschinen-Sensor (ESP32 oder ESP-01S + SW-420)

Ein ESP zählt die Erschütterungen der Waschmaschine und meldet sie alle
zehn Sekunden an newsroom21. Ob die Maschine läuft oder fertig ist, erkennt
newsroom21 (Tab **Einstellungen → Waschmaschine**, Beschreibung im
newsroom21-HANDBUCH, Abschnitt „Optionaler Sensor an der Waschmaschine“).

Eine Firmware, zwei Bauarten (`firmware/platformio.ini`):

| | ESP32-DevKit (`esp32dev`) – **empfohlen** | ESP-01S (`esp01s`) |
|---|---|---|
| Strom | USB-Kabel direkt | eigener Adapter mit AMS1117 nötig |
| Aufspielen | `pio run -e esp32dev -t upload` | GPIO0-Draht-Trick (unten) |
| Sensor DO | **GPIO27** | GPIO3 (RX), 30-s-Befehlsfenster |
| Summer | passiver Summer an **GPIO25** | – |

Der ESP-01S (MAC `08:3a:8d:de:eb:8c`) ist bespielt und liegt als Reserve;
seine Werksfirmware (AT 1.2.0.0) steht in
`backup/esp01s-vorher-083a8ddeeb8c.bin` (nicht in Git).

## Verkabelung ESP32

```
SW-420 VCC -> 3V3        Summer (HW-508) S -> GPIO25
SW-420 GND -> GND        Summer +          -> 3V3
SW-420 DO  -> GPIO27     Summer -          -> GND
```

Kabel zum Sensor bis ca. 2 m problemlos, bis 5 m mit verdrilltem Signal/GND.
Der Motor der Maschine streut Störungen ein – Kabel kurz halten, bei Bedarf
100 nF zwischen VCC und GND direkt am Sensor.

**Summer ausprobieren** (Befehlszeile, z. B. `pio device monitor`):
`TON KLINGEL`, `TON ANRUF`, `TON WARNUNG`, `TON FERTIG`. Meldet newsroom21 den
Zustand „fertig“, spielt der Sensor selbst einmal `FERTIG`. Die Töne sind
Vorschläge für die Hinweistöne der Uhren.

## Verkabelung ESP-01S

```
SW-420 VCC  ->  ESP-01 3V3
SW-420 GND  ->  ESP-01 GND
SW-420 DO   ->  ESP-01 RX (GPIO3)
```

**Warum RX?** GPIO0 und GPIO2 müssen beim Einschalten HIGH sein. Der Sensor
könnte sie auf LOW ziehen, und der ESP-01 startete nicht. GPIO3 hat keine
solche Bedingung.

Strom: Der ESP-01 braucht 3,3 V mit bis zu 300 mA. Im Betrieb über einen
Netzteil-Adapter für den ESP-01 (mit Spannungsregler) an einem USB-Netzteil –
**nie 5 V direkt an den ESP-01**.

Pinbelegung (Oberseite, Pins zu dir, Antenne oben; mit dem Aufdruck auf der
Rückseite vergleichen):

```
GND   GPIO2  GPIO0  RX
TX    EN     RST    3V3
```

## Aufspielen ESP-01S (USB-Adapter ohne PROG-Schalter)

Der Adapter aus dem 5er-Set hat keinen Programmierschalter und keine
Reset-Leitungen. In den Programmiermodus geht es von Hand:

1. Firmware bauen: `cd firmware && ~/.platformio/penv/bin/pio run`
2. Skript starten (wartet bis zu 2 Minuten):
   ```bash
   PYTHONPATH=~/.platformio/tools/tool-esptoolpy \
     ~/.platformio/penv/bin/python scripts/flash.py
   ```
3. Adapter ziehen. Auf der Oberseite des ESP-01 einen Draht **fest** auf die
   Lötpunkte **GND** und **GPIO0** drücken (nicht GPIO2 dazwischen berühren,
   nie 3V3 und GND verbinden), Adapter einstecken, 3 s halten, loslassen.
4. Das Skript sichert beim ersten Mal den ganzen Flash nach `backup/`, löscht
   und schreibt die Firmware – alles in **einem** Durchgang. (Zwei getrennte
   esptool-Aufrufe scheiterten am Gerät: Nach dem ersten reagierte der ESP-01
   nicht mehr, 28.09.2026.)
5. Adapter ziehen, **ohne** Draht wieder einstecken.

Der Adapter heißt mal `/dev/ttyUSB0`, mal `/dev/ttyUSB1` – beide Skripte
suchen ihn selbst (USB-Kennung `1a86:7523`, CH340).

## Einrichten (WLAN, Pi-Adresse, Token)

Vorher auf dem Pi: `sudo /opt/newsroom21/scripts/secrets.sh` → **11)
Sensor-Token erzeugen**, Name `waschmaschine`; `compose.sensor.yaml` in
`COMPOSE_FILE` eintragen (newsroom21-HANDBUCH).

```bash
~/.platformio/penv/bin/python scripts/provision.py
```

Das Skript fragt **zuerst** WLAN-Name, WLAN-Passwort und Token ab (verdeckt)
und wartet dann auf den Adapter. Die Werte gehen über die USB-Leitung direkt
in den ESP-01 – nicht in eine Datei, nicht in die Shell-Historie. Nur
nachsehen: `scripts/provision.py --status`.

Beim ESP32 ist die Befehlszeile immer offen. **Befehlsfenster beim ESP-01S:**
Nach dem Einschalten ist RX 30 Sekunden lang die
Befehlszeile (jeder Befehl verlängert um 30 s). Danach wird RX zum
Sensor-Eingang. Ohne vollständige Einrichtung bleibt die Befehlszeile offen.
Die blaue LED blinkt langsam, solange sie offen ist, und kurz bei jeder
angenommenen Meldung.

Befehle (für Handarbeit mit `pio device monitor`): `STATUS`, `WIFI-SSID
<name>`, `WIFI-PASS <passwort>`, `HOST <adresse>`, `TOKEN <wert>`,
`FORGET-TOKEN`, `FORGET-WIFI`, `ZAEHLER` (Erschütterungen seit der letzten Abfrage – zum
Einstellen des Drehreglers), `RESTART`, `HELP`. Token und Passwort werden nie
ausgegeben.

## Betrieb

- Alle 10 s `POST /api/sensor/report {"pulses": N}` per HTTPS, geprüft gegen
  die Root-CA von newsroom21 (in der Firmware: `firmware/include/root_ca.h`,
  erzeugt von `scripts/make_root_ca.sh`, gültig bis 2036).
- Verbindung per IP ohne Namensabgleich, Kette voll geprüft – wie bei den
  ESP8266-Uhren (UHR).
- Kommt 15 Minuten lang keine Meldung durch, startet der Sensor neu.
- Empfindlichkeit: Drehregler am SW-420 **und** „Empfindlichkeit“ im Tab.
  Die Zahl der Erschütterungen je Meldung steht im Tab unter dem Zustand –
  damit lässt sich der Regler einstellen (Maschine aus: 0, im Schleudern: viele).

## Zurück zur Werksfirmware

```bash
PYTHONPATH=~/.platformio/tools/tool-esptoolpy ~/.platformio/penv/bin/python -m esptool \
  --before no-reset write-flash 0 backup/esp01s-vorher-083a8ddeeb8c.bin
```
(vorher GPIO0-Brücke wie oben)

## Updates über das Netz (OTA, nur ESP32)

Ab Firmware `wm.2026.09.29.1` (Build `2026092901`) holt sich der ESP32 neue
Firmware selbst von newsroom21. **Diese erste OTA-fähige Fassung muss einmal
per USB drauf**, danach nie wieder.

Neue Fassung veröffentlichen:

1. `firmware/include/firmware_info.h`: `FIRMWARE_BUILD` erhöhen (JJJJMMTTNN)
   und `FIRMWARE_VERSION` anpassen.
2. `scripts/release.sh` – baut, bindet den öffentlichen Schlüssel ein
   (`scripts/make_ota_pubkey.sh`), signiert und legt das Abbild in
   `../newsroom21/firmware/washer-esp32/`.
3. newsroom21 committen und pushen, auf dem Pi das Update installieren.
4. Tab **Waschmaschine** → „Firmware aktualisieren“.

Serielle Ausgabe beim Update: `[ota] Signatur gültig – lade …`, `[ota] 10 %` …
`[ota] fertig – Neustart`, nach dem Neustart `[ota] neue Firmware auf
Bewährung` und `[ota] newsroom21 erreicht – neue Firmware bestätigt`. Erreicht
die neue Fassung newsroom21 nicht binnen 5 Minuten, kehrt der ESP32 zur alten
zurück.

Speicher: `default.csv` mit zwei Programmbereichen zu je 1,25 MB; die Firmware
belegt 1,15 MB (88 %). Der ESP-01 kann kein OTA.
