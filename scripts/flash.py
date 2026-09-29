#!/usr/bin/env python3
"""
Firmware auf den ESP-01 spielen – über den USB-Adapter ohne PROG-Schalter.

Der Adapter hat keine Reset-Leitungen. Der ESP-01 muss deshalb von Hand in
den Programmiermodus: GPIO0 beim Einstecken mit GND verbinden (HANDBUCH.md).

Ablauf in EINEM Durchgang, weil der ESP-01 zwischen zwei esptool-Aufrufen
nicht zuverlässig ansprechbar bleibt (am Gerät gesehen, 28.09.2026):
  1. warten, bis der ESP-01 im Programmiermodus antwortet (bis zu 2 Minuten)
  2. nur beim ersten Mal: den kompletten Flash nach backup/ sichern
  3. Flash löschen, Firmware schreiben
Danach: Adapter ziehen, GPIO0-Brücke weg, wieder einstecken.

Aufruf (esptool 5 aus PlatformIO)::

    PYTHONPATH=~/.platformio/tools/tool-esptoolpy \\
      ~/.platformio/penv/bin/python scripts/flash.py
"""

import argparse
import os
import sys
import time

try:
    from esptool.cmds import (attach_flash, detect_chip, erase_flash, read_flash,
                              run_stub, write_flash)
except ImportError:
    sys.exit("esptool 5 fehlt. Aufruf mit PYTHONPATH=~/.platformio/tools/tool-esptoolpy "
             "~/.platformio/penv/bin/python scripts/flash.py")

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(HERE, "firmware", ".pio", "build", "esp01s", "firmware.bin")
BACKUP_DIR = os.path.join(HERE, "backup")
FLASH_SIZE = 0x100000   # ESP-01S: 1 MB


def find_adapter() -> str:
    """Der CH340-Adapter (USB-Kennung 1a86:7523) – mal ttyUSB0, mal ttyUSB1."""
    from serial.tools import list_ports
    for info in list_ports.comports():
        if info.vid == 0x1A86 and info.pid == 0x7523:
            return info.device
    return ""


def wait_for_bootloader(port: str, seconds: int):
    print(f"Warte bis zu {seconds} s auf den ESP-01 im Programmiermodus …")
    print("  GPIO0 mit GND verbinden, Adapter einstecken, 3 s halten, loslassen.")
    end = time.time() + seconds
    while time.time() < end:
        current = port or find_adapter()
        if current and os.path.exists(current):
            try:
                return detect_chip(current, 115200, "no-reset", connect_attempts=1)
            except Exception:
                pass
        time.sleep(0.3)
    sys.exit("Kein Programmiermodus erkannt. Brücke fester andrücken und noch einmal.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Waschmaschinen-Sensor aufspielen")
    parser.add_argument("--port", default="", help="Vorgabe: Adapter selbst suchen")
    parser.add_argument("--wait", type=int, default=120)
    args = parser.parse_args()

    if not os.path.exists(FIRMWARE):
        sys.exit(f"{FIRMWARE} fehlt – zuerst in firmware/ bauen: pio run")

    esp = wait_for_bootloader(args.port, args.wait)
    try:
        mac = ":".join(f"{b:02x}" for b in esp.read_mac())
        print(f"Verbunden: {esp.get_chip_description()}, MAC {mac}")
        esp = run_stub(esp)
        attach_flash(esp)
        esp.change_baud(460800)

        os.makedirs(BACKUP_DIR, exist_ok=True)
        backup = os.path.join(BACKUP_DIR, f"esp01s-vorher-{mac.replace(':', '')}.bin")
        if os.path.exists(backup):
            print(f"Sicherung liegt schon vor: {os.path.basename(backup)}")
        else:
            print("Sichere den bisherigen Flash (1 MB) …")
            read_flash(esp, 0, FLASH_SIZE, backup)

        print("Lösche den Flash …")
        erase_flash(esp)
        print("Schreibe die Firmware …")
        write_flash(esp, [(0, FIRMWARE)])
    finally:
        esp._port.close()
    print("Fertig. Adapter ziehen, GPIO0-Brücke entfernen, wieder einstecken.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
