#!/bin/bash
# Erzeugt firmware/include/ota_pubkey.h aus dem ÖFFENTLICHEN Signierschlüssel.
# Mit diesem Schlüssel prüft das Gerät jedes Update über das Netz. Der private
# Schlüssel (signing-key.pem) wird hier nicht angefasst.
set -euo pipefail
cd "$(dirname "$0")/.."
src="${1:-$HOME/.config/newsroom21-firmware/signing-pub.pem}"
grep -q "BEGIN PUBLIC KEY" "$src"
! grep -q "PRIVATE KEY" "$src"
{
  echo "// Automatisch erzeugt von scripts/make_ota_pubkey.sh – nicht von Hand ändern."
  echo "// Fingerabdruck (SHA-256 über DER, gekürzt): $(openssl pkey -pubin -in "$src" -outform DER | sha256sum | cut -c1-16)"
  echo "#pragma once"
  echo "static const char kOtaPublicKey[] = R\"PEM("
  cat "$src"
  echo ")PEM\";"
} > firmware/include/ota_pubkey.h
echo "firmware/include/ota_pubkey.h geschrieben"
