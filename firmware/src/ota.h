#pragma once
// ============================================================
//  ota.h – Firmware-Updates über das Netz (nur ESP32)
//
//  Ablauf (angestoßen per Knopf in der newsroom21-Weboberfläche):
//    1. newsroom21 hängt an die Antwort auf eine Meldung das Manifest:
//       Modell, Build-Nummer, Größe, SHA-256, Signatur.
//    2. Das Gerät prüft VOR dem Laden: eigenes Modell, höhere Build-Nummer,
//       Signatur (ECDSA P-256) mit dem eingebauten öffentlichen Schlüssel.
//    3. Laden über dieselbe geprüfte HTTPS-Verbindung, dabei SHA-256
//       mitrechnen und direkt in den freien Programmbereich schreiben.
//    4. Passt die SHA-256, umschalten und neu starten.
//    5. Die neue Firmware gilt erst als gut, wenn sie newsroom21 erreicht
//       hat. Schafft sie das nicht binnen 5 Minuten, kehrt der ESP32 zur
//       alten Firmware zurück (Rückfall des Bootloaders).
//
//  Signiert wird die Zeichenkette
//      newsroom21-fw|<modell>|<build>|<größe>|<sha256>
//  (newsroom21/scripts/firmware/publish.py). Wer newsroom21 übernimmt, kann
//  Updates verhindern, aber keine fremde Firmware aufspielen.
// ============================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

namespace ota {

// Beim Start aufrufen: merkt sich, ob diese Firmware noch auf Bewährung läuft.
void begin();

// Nach jeder erfolgreichen Meldung an newsroom21 aufrufen: erklärt eine neue
// Firmware beim ersten Mal für gut.
void confirmWorking();

// Aus loop(): Rückfall, wenn eine neue Firmware newsroom21 nicht erreicht.
void loop();

// Manifest aus der Antwort prüfen und – wenn alles stimmt – laden,
// umschalten und neu starten. Kehrt nur bei einem Fehler zurück.
// url: https://<pi>/api/sensor/firmware, token: Geräte-Token.
void handleOffer(JsonObjectConst offer, const String& url, const String& token);

}  // namespace ota
