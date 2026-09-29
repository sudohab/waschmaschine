// Firmware-Updates über das Netz – siehe ota.h
#if !defined(ESP8266)

#include "ota.h"

#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "firmware_info.h"
#include "ota_pubkey.h"
#include "root_ca.h"

// Die Kernbibliothek fragt beim Start, ob die Firmware selbst entscheidet,
// wann sie als gut gilt. Ja: erst nach der ersten erfolgreichen Meldung
// (confirmWorking). Ohne diese Funktion würde jede neue Firmware sofort
// als gut markiert – und der Rückfall griffe nie.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {

static const unsigned long kProbationMs = 5UL * 60UL * 1000UL;
static bool onProbation = false;
static unsigned long lastAttemptMs = 0;
static bool attempted = false;

void begin() {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    onProbation = true;
    Serial.println(F("[ota] neue Firmware auf Bewährung – gilt als gut, sobald newsroom21 antwortet"));
  }
}

void confirmWorking() {
  if (!onProbation) return;
  onProbation = false;
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.println(F("[ota] newsroom21 erreicht – neue Firmware bestätigt"));
}

void loop() {
  if (onProbation && millis() > kProbationMs) {
    Serial.println(F("[ota] newsroom21 seit 5 Minuten nicht erreicht – zurück zur alten Firmware"));
    delay(100);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

static String hex(const unsigned char* data, size_t len) {
  static const char digits[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0f];
  }
  return out;
}

static bool validSha(const char* sha) {
  if (!sha || strlen(sha) != 64) return false;
  for (const char* p = sha; *p; ++p)
    if (!isdigit((unsigned char)*p) && (*p < 'a' || *p > 'f')) return false;
  return true;
}

static bool verifySignature(const String& message, const char* signatureB64) {
  unsigned char signature[80];
  size_t signatureLen = 0;
  if (mbedtls_base64_decode(signature, sizeof(signature), &signatureLen,
                            reinterpret_cast<const unsigned char*>(signatureB64),
                            strlen(signatureB64)) != 0) {
    return false;
  }
  unsigned char hash[32];
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
                     hash, 0) != 0) {
    return false;
  }
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  int rc = mbedtls_pk_parse_public_key(&key, reinterpret_cast<const unsigned char*>(kOtaPublicKey),
                                       strlen(kOtaPublicKey) + 1);
  if (rc == 0) rc = mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, signatureLen);
  mbedtls_pk_free(&key);
  return rc == 0;
}

// Lädt das Abbild und schreibt es in den freien Programmbereich.
// true = geschrieben, SHA-256 stimmt, Umschalten vorgemerkt.
static bool download(const String& url, const String& token, size_t size, const char* sha) {
  WiFiClientSecure client;
  client.setCACert(kNewsroomRootCa);
  client.setHandshakeTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", "Bearer " + token);
  const int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() != (int)size) {
    Serial.printf("[ota] Laden abgelehnt: Status %d, Größe %d statt %u\n", code, http.getSize(),
                  (unsigned)size);
    http.end();
    return false;
  }
  if (!Update.begin(size, U_FLASH)) {
    Serial.printf("[ota] kein Platz für das Abbild: %s\n", Update.errorString());
    http.end();
    return false;
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  auto* stream = http.getStreamPtr();   // NetworkClient im Arduino-Kern 3.x
  static uint8_t buffer[4096];
  size_t written = 0;
  unsigned long lastData = millis();
  int lastPercent = -1;
  bool ok = true;
  while (written < size) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() - lastData > 20000UL) { ok = false; break; }   // Verbindung steht
      delay(2);
      continue;
    }
    const size_t want = min(min(available, sizeof(buffer)), size - written);
    const size_t got = stream->readBytes(buffer, want);
    if (got == 0) continue;
    mbedtls_sha256_update(&ctx, buffer, got);
    if (Update.write(buffer, got) != got) { ok = false; break; }
    written += got;
    lastData = millis();
    const int percent = (int)(written * 100 / size);
    if (percent / 10 != lastPercent / 10) {
      lastPercent = percent;
      Serial.printf("[ota] %d %%\n", percent);
    }
  }
  unsigned char digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  http.end();

  if (!ok || written != size) {
    Serial.println(F("[ota] Laden abgebrochen"));
    Update.abort();
    return false;
  }
  if (hex(digest, sizeof(digest)) != sha) {
    Serial.println(F("[ota] SHA-256 stimmt nicht – Abbild verworfen"));
    Update.abort();
    return false;
  }
  if (!Update.end()) {
    Serial.printf("[ota] Umschalten fehlgeschlagen: %s\n", Update.errorString());
    return false;
  }
  return true;
}

void handleOffer(JsonObjectConst offer, const String& url, const String& token) {
  // Nicht bei jeder Meldung erneut versuchen, falls etwas schiefging:
  // höchstens alle 2 Minuten (newsroom21 lässt ohnehin nur einen Download je
  // Minute zu).
  if (attempted && millis() - lastAttemptMs < 120000UL) return;
  if (onProbation) return;   // erst bestätigen, dann das nächste Update
  attempted = true;
  lastAttemptMs = millis();

  const char* model = offer["model"] | "";
  const long long build = offer["build"] | 0LL;
  const long long size = offer["size"] | 0LL;
  const char* sha = offer["sha256"] | "";
  const char* signature = offer["signature"] | "";
  const char* label = offer["label"] | "";

  if (strcmp(model, FIRMWARE_MODEL) != 0) {
    Serial.printf("[ota] Angebot für anderes Modell (%s) – ignoriert\n", model);
    return;
  }
  if (build <= FIRMWARE_BUILD) {
    Serial.println(F("[ota] Angebot ist nicht neuer – ignoriert"));
    return;
  }
  if (size <= 0 || size > (long long)ESP.getFreeSketchSpace() || !validSha(sha) ||
      strlen(signature) == 0 || strlen(signature) > 200) {
    Serial.println(F("[ota] Angebot unvollständig oder zu groß – ignoriert"));
    return;
  }
  const String message = String("newsroom21-fw|") + model + "|" + String(build) + "|" +
                         String(size) + "|" + sha;
  if (!verifySignature(message, signature)) {
    Serial.println(F("[ota] SIGNATUR UNGÜLTIG – Update abgelehnt"));
    return;
  }
  Serial.printf("[ota] Signatur gültig – lade %s (Build %lld, %lld Bytes)\n", label, build, size);
  if (!download(url, token, (size_t)size, sha)) return;
  Serial.println(F("[ota] fertig – Neustart mit der neuen Firmware"));
  delay(200);
  ESP.restart();
}

}  // namespace ota

#endif  // !ESP8266
