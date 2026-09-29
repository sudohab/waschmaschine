#pragma once
// Version dieser Firmware. Vor jeder Veröffentlichung (scripts/release.sh)
// FIRMWARE_BUILD erhöhen: Das Gerät nimmt nur Updates mit HÖHERER Build-Nummer
// an, und die Nummer ist mitsigniert (kein Zurückstufen).
//   FIRMWARE_BUILD    JJJJMMTTNN
//   FIRMWARE_VERSION  Anzeige in newsroom21
#define FIRMWARE_VERSION "wm.2026.09.29.1"
#define FIRMWARE_BUILD 2026092901LL

// Welches Abbild zu diesem Gerät passt (Ordner firmware/<modell>/ in newsroom21)
#if defined(ESP8266)
#define FIRMWARE_MODEL "washer-esp01"
#else
#define FIRMWARE_MODEL "washer-esp32"
#endif
