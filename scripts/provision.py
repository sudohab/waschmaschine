#!/usr/bin/env python3
"""
WLAN-Zugang, Pi-Adresse und Token in den Waschmaschinen-Sensor schreiben.

WLAN-Passwort und Token werden **verdeckt** abgefragt und über die
USB-Leitung direkt in den Flash des ESP-01 geschrieben. Sie landen weder in
der Shell-Historie noch in einer Datei noch in einem Chat.

Ablauf:
  1. Skript starten – es fragt zuerst alles ab und wartet dann auf das Gerät
  2. ESP32-Board per USB anstecken (Befehlszeile ist immer offen), bzw.
     ESP-01 im USB-Adapter ohne GPIO0-Brücke einstecken (Befehlszeile
     30 Sekunden nach dem Einschalten offen, ohne Einrichtung dauerhaft).

Aufruf::

    ~/.platformio/penv/bin/python scripts/provision.py
    ~/.platformio/penv/bin/python scripts/provision.py --status
    ~/.platformio/penv/bin/python scripts/provision.py --host 192.168.178.113

Den Token erzeugt man auf dem Pi mit::

    sudo /opt/newsroom21/scripts/secrets.sh     → "11) Sensor-Token erzeugen"
"""

import argparse
import getpass
import os
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.exit("pyserial fehlt. Aufruf mit: ~/.platformio/penv/bin/python scripts/provision.py")

# USB-Seriell-Chips der Boards: CH340 (ESP-01-Adapter, viele DevKits),
# CH9102, CP2102. Der Port heißt mal ttyUSB0, mal ttyUSB1.
_USB_SERIAL = {(0x1A86, 0x7523), (0x1A86, 0x55D4), (0x10C4, 0xEA60)}


def find_adapter() -> str:
    from serial.tools import list_ports
    for info in list_ports.comports():
        if (info.vid, info.pid) in _USB_SERIAL:
            return info.device
    return ""


TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{43,128}$")
HOST_RE = re.compile(r"^[A-Za-z0-9.-]{1,63}$")


def wait_for_sensor(port: str) -> "serial.Serial":
    """Wartet, bis der Adapter steckt und die Befehlszeile bereit ist."""
    if not port:
        port = find_adapter()
        if port:
            print(f"Gerät gefunden an {port}. (Nur ESP-01: Läuft er schon länger als "
                  "30 s, Adapter kurz ziehen und wieder einstecken.)")
        else:
            print("Warte auf den Adapter – jetzt einstecken …")
            while not port:
                time.sleep(0.2)
                port = find_adapter()
            time.sleep(0.3)
    try:
        # Einmal öffnen und wieder schließen: Direkt nach dem Einstecken lief
        # beim ERSTEN Öffnen die Ausgabe am PC im Kreis (28.09.2026, Ursache
        # auf der PC-Seite); ab dem zweiten Öffnen war es in allen Tests sauber.
        serial.Serial(port, 115200, timeout=0.5).close()
        time.sleep(0.5)
        connection = serial.Serial(port, 115200, timeout=0.5)
    except serial.SerialException as exc:
        sys.exit(f"Port {port} nicht nutzbar: {exc}")
    # Auf die Startmeldung warten (höchstens 5 s) oder einfach fragen.
    deadline = time.time() + 5
    while time.time() < deadline:
        raw = connection.readline()
        if b"Befehlszeile" in raw:
            break
    connection.reset_input_buffer()
    return connection


def send(connection, command: str, secret: str = "") -> list:
    """Befehl senden, Antwortzeilen sammeln. `secret` wird nie ausgegeben."""
    line = f"{command} {secret}" if secret else command
    connection.write((line + "\n").encode("utf-8"))
    connection.flush()
    line = ""
    answers = []
    deadline = time.time() + 3
    while time.time() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").rstrip()
        if text and not text.startswith("[melden]"):
            answers.append(text)
            if text.startswith(("[ok]", "[fehler]")):
                break
            deadline = time.time() + 0.5
    return answers


def show(answers) -> bool:
    for answer in answers:
        print("  " + answer)
    return any(a.startswith("[ok]") for a in answers) or not answers or any(
        a.startswith("[status]") for a in answers)


def main() -> int:
    parser = argparse.ArgumentParser(description="Waschmaschinen-Sensor einrichten")
    parser.add_argument("--port", default="", help="Vorgabe: Adapter selbst suchen")
    parser.add_argument("--host", default="192.168.178.113",
                        help="Adresse von newsroom21 (Vorgabe: 192.168.178.113)")
    parser.add_argument("--status", action="store_true", help="nur anzeigen, was hinterlegt ist")
    parser.add_argument("--skip-wifi", action="store_true", help="WLAN nicht neu eintragen")
    args = parser.parse_args()

    if not HOST_RE.match(args.host):
        sys.exit("Adresse ungültig: nur Buchstaben, Ziffern, Punkt, Bindestrich")

    # Erst alles abfragen, dann verbinden: Das Befehlsfenster des Sensors ist
    # nach dem Einschalten nur 30 Sekunden offen.
    ssid = password = token = ""
    if not args.status:
        if not args.skip_wifi:
            ssid = input("WLAN-Name: ").strip()
            password = getpass.getpass("WLAN-Passwort (bleibt unsichtbar): ")
        print("Token vom Pi (sudo scripts/secrets.sh -> 11). Die Eingabe bleibt unsichtbar.")
        token = getpass.getpass("Token (leer = nicht ändern): ").strip()
        if token and not TOKEN_RE.match(token):
            sys.exit("Das sieht nicht wie ein Token aus (43 Zeichen, A–Z, a–z, 0–9, - und _).")

    connection = wait_for_sensor(args.port)
    try:
        if args.status:
            show(send(connection, "STATUS"))
            return 0
        if ssid:
            show(send(connection, "WIFI-SSID", ssid))
            ok = show(send(connection, "WIFI-PASS", password))
            password = ""
            if not ok:
                return 1
        show(send(connection, "HOST", args.host))
        if token:
            show(send(connection, "TOKEN", token))
            token = ""
        time.sleep(4)   # WLAN braucht einen Moment
        show(send(connection, "STATUS"))
        print("Fertig. 30 s nach dem letzten Befehl wechselt RX zum Sensor-Eingang;")
        print("danach meldet der Sensor alle 10 s an newsroom21.")
        return 0
    finally:
        password = token = ""
        connection.close()


if __name__ == "__main__":
    sys.exit(main())
