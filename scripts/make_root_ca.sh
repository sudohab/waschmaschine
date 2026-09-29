#!/bin/bash
# Erzeugt firmware/include/root_ca.h aus der Root-CA von newsroom21.
# Die CA ist öffentlich (kein Schlüssel) – sie darf in die Firmware.
# Nur nötig, falls Caddy je eine neue Root-CA erzeugt (die jetzige gilt bis 2036).
set -euo pipefail
cd "$(dirname "$0")/.."
src="${1:-../newsroom21/newsroom21-root-ca.crt}"
grep -q "BEGIN CERTIFICATE" "$src"
! grep -q "PRIVATE KEY" "$src"
{
  echo "// Automatisch erzeugt von scripts/make_root_ca.sh – nicht von Hand ändern."
  echo "// Quelle: $(basename "$src") ($(openssl x509 -in "$src" -noout -subject | sed 's/^subject=//'))"
  echo "#pragma once"
  echo "#include <pgmspace.h>"
  echo "static const char kNewsroomRootCa[] PROGMEM = R\"PEM("
  cat "$src"
  echo ")PEM\";"
} > firmware/include/root_ca.h
echo "firmware/include/root_ca.h geschrieben"
