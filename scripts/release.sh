#!/bin/bash
# Neue Sensor-Firmware für Updates über das Netz bereitlegen (nur auf dem PC).
#
#   1. In firmware/include/firmware_info.h FIRMWARE_BUILD erhöhen
#      (und FIRMWARE_VERSION anpassen)
#   2. scripts/release.sh
#   3. in newsroom21 committen und pushen, auf dem Pi das Update installieren
#   4. in der Weboberfläche (Tab Waschmaschine) „Firmware aktualisieren“
#
# Signiert wird mit ~/.config/newsroom21-firmware/signing-key.pem
# (newsroom21/scripts/firmware/publish.py).
set -euo pipefail
cd "$(dirname "$0")/.."
NEWSROOM="${NEWSROOM21_REPO:-../newsroom21}"
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
info="firmware/include/firmware_info.h"

build="$(sed -n 's/^#define FIRMWARE_BUILD \([0-9]\{10\}\)LL$/\1/p' "$info")"
label="$(sed -n 's/^#define FIRMWARE_VERSION "\([A-Za-z0-9._-]*\)"$/\1/p' "$info")"
[[ -n "$build" && -n "$label" ]] || { echo "FIRMWARE_BUILD/FIRMWARE_VERSION in $info nicht lesbar" >&2; exit 1; }

# Der eingebaute öffentliche Schlüssel muss zum privaten passen – sonst würde
# das Gerät jedes Update ablehnen.
scripts/make_ota_pubkey.sh >/dev/null
(cd firmware && "$PIO" run -e esp32dev)

python3 "$NEWSROOM/scripts/firmware/publish.py" --model washer-esp32 --build "$build" \
  --label "$label" --bin firmware/.pio/build/esp32dev/firmware.bin
