// ============================================================
//  Waschmaschinen-Sensor – Erschütterungs-Sensor SW-420 an newsroom21
//
//  Aufgabe: Erschütterungen zählen und alle 10 Sekunden an newsroom21
//  melden (POST /api/sensor/report {"pulses": N}). Ob die Maschine läuft
//  oder fertig ist, entscheidet newsroom21 (app/washers.py) – so lassen sich
//  die Schwellen in der Weboberfläche ändern, ohne neu aufzuspielen.
//
//  Zwei Bauarten, eine Firmware (siehe HANDBUCH.md):
//
//    ESP32 (esp32dev)  SW-420 DO -> GPIO27, passiver Summer -> GPIO25.
//                      Befehlszeile über USB immer offen. Der Summer ist
//                      zum Ausprobieren der Hinweistöne (Befehl TON …) und
//                      piept kurz, wenn newsroom21 „fertig“ meldet.
//
//    ESP-01S (esp01s)  SW-420 DO -> GPIO3 (RX). GPIO0/GPIO2 müssen beim
//                      Einschalten HIGH sein, der Sensor könnte sie auf LOW
//                      ziehen. RX ist deshalb nach dem Einschalten 30 s lang
//                      Befehlszeile (jeder Befehl verlängert), danach
//                      Sensor-Eingang. Ohne Einrichtung bleibt sie offen.
//
//  Einrichtung über USB: scripts/provision.py.
//
//  Sicherheit:
//    • Nur HTTPS, geprüft gegen die Root-CA von newsroom21 (in der Firmware,
//      öffentlich). Kein setInsecure(), keine Ausnahme.
//    • Token und WLAN-Passwort werden nie ausgegeben – STATUS zeigt nur, OB
//      etwas hinterlegt ist.
//    • Kein Webserver, kein offener Port, kein OTA.
// ============================================================

#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>

#if defined(ESP8266)
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#define SENSOR_ON_RX 1
#else
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#define HAS_BUZZER 1
#define HAS_OTA 1
#endif

#include "firmware_info.h"
#include "root_ca.h"
#if defined(HAS_OTA)
#include "ota.h"
#endif

#if defined(SENSOR_ON_RX)
static const uint8_t PIN_SENSOR = 3;      // RX
static const uint8_t PIN_LED = 2;         // blaue LED des ESP-01S
static const bool LED_ACTIVE_LOW = true;
#else
static const uint8_t PIN_SENSOR = 27;
static const uint8_t PIN_BUZZER = 25;
static const uint8_t PIN_LED = 2;         // blaue LED der üblichen DevKits
static const bool LED_ACTIVE_LOW = false;
#endif

static const unsigned long CONSOLE_WINDOW_MS = 30000;
static const unsigned long REPORT_INTERVAL_MS = 10000;
// Kommt so lange keine Meldung durch, startet der Sensor neu – der
// sicherste Weg aus einem hängenden WLAN- oder TLS-Zustand.
static const unsigned long RESTART_AFTER_MS = 15UL * 60UL * 1000UL;

static const char* CONFIG_PATH = "/private/sensor.cfg";
static const char* TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

static void led(bool on) { digitalWrite(PIN_LED, (on != LED_ACTIVE_LOW) ? HIGH : LOW); }

// ---- Einrichtung (LittleFS: zwei Zeilen, Pi-Adresse und Token) ----------

static String host;
static String token;

static bool validHost(const String& value) {
  if (value.length() == 0 || value.length() > 63) return false;
  for (char c : value)
    if (!isalnum((unsigned char)c) && c != '.' && c != '-') return false;
  return true;
}

static bool validToken(const String& value) {
  if (value.length() < 43 || value.length() > 128) return false;
  for (char c : value)
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  return true;
}

static void loadConfig() {
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f || f.size() > 512) return;
  String h = f.readStringUntil('\n');
  String t = f.readStringUntil('\n');
  f.close();
  h.trim();
  t.trim();
  if (validHost(h)) host = h;
  if (validToken(t)) token = t;
}

static bool saveConfig() {
  LittleFS.mkdir("/private");
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  f.print(host);
  f.print('\n');
  f.print(token);
  f.print('\n');
  f.close();
  return true;
}

// Name des gespeicherten WLANs – auch wenn gerade keine Verbindung besteht.
static String storedSsid() {
#if defined(ESP8266)
  return WiFi.SSID();
#else
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return String();
  return String(reinterpret_cast<const char*>(conf.sta.ssid));
#endif
}

static bool configured() {
  return validHost(host) && validToken(token) && storedSsid().length() > 0;
}

// ---- Zählen -------------------------------------------------------------

static volatile uint32_t pulses = 0;
#if !defined(ESP8266)
static portMUX_TYPE pulseLock = portMUX_INITIALIZER_UNLOCKED;
#endif

// Jede Flanke zählt: Der SW-420 schaltet je nach Modul bei Erschütterung
// auf HIGH oder auf LOW – so ist die Richtung egal.
static void IRAM_ATTR onPulse() {
#if defined(ESP8266)
  pulses++;
#else
  portENTER_CRITICAL_ISR(&pulseLock);
  pulses++;
  portEXIT_CRITICAL_ISR(&pulseLock);
#endif
}

static uint32_t takePulses() {
#if defined(ESP8266)
  noInterrupts();
  const uint32_t value = pulses;
  pulses = 0;
  interrupts();
#else
  portENTER_CRITICAL(&pulseLock);
  const uint32_t value = pulses;
  pulses = 0;
  portEXIT_CRITICAL(&pulseLock);
#endif
  return value;
}

static void startCounting() {
  pinMode(PIN_SENSOR, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), onPulse, CHANGE);
}

// ---- Summer (nur ESP32) -------------------------------------------------
//
// Vorschläge für die Hinweistöne, die später auch die Uhren bekommen sollen.
// Hier nur zum Anhören: Befehl "TON KLINGEL|ANRUF|WARNUNG|FERTIG".

#if defined(HAS_BUZZER)
struct Note { uint16_t hz; uint16_t ms; };   // hz = 0: Pause

static void play(const Note* notes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (notes[i].hz) tone(PIN_BUZZER, notes[i].hz);
    else noTone(PIN_BUZZER);
    delay(notes[i].ms);
  }
  noTone(PIN_BUZZER);
}

static const Note kDoorbell[] = {{659, 350}, {0, 40}, {523, 600}};                 // Ding-Dong
static const Note kCall[] = {{1000, 50}, {1300, 50}, {1000, 50}, {1300, 50},        // Trillern
                             {1000, 50}, {1300, 50}, {0, 250},
                             {1000, 50}, {1300, 50}, {1000, 50}, {1300, 50},
                             {1000, 50}, {1300, 50}};
static const Note kWarning[] = {{2000, 150}, {0, 100}, {2000, 150}, {0, 100}, {2000, 150}};
static const Note kDone[] = {{523, 120}, {659, 120}, {784, 220}};                  // aufsteigend

template <size_t N>
static void play(const Note (&notes)[N]) { play(notes, N); }

// Rechteck von Hand, an tone()/LEDC vorbei – zum Eingrenzen, ob ein
// stummer Summer an der Verkabelung oder an der Tonerzeugung liegt.
static void rawBeep(unsigned hz, unsigned ms) {
  pinMode(PIN_BUZZER, OUTPUT);
  const unsigned half = 500000U / hz;
  const unsigned long end = millis() + ms;
  while (millis() < end) {
    digitalWrite(PIN_BUZZER, HIGH);
    delayMicroseconds(half);
    digitalWrite(PIN_BUZZER, LOW);
    delayMicroseconds(half);
  }
}

static bool playByName(String name) {
  name.toUpperCase();
  if (name == "ROH") { rawBeep(2000, 1500); return true; }
  // Pin 3 s dauerhaft an: Ein AKTIVER Summer piept dann von selbst, ein
  // passiver knackt nur beim Ein- und Ausschalten.
  if (name == "AN") {
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, HIGH);
    delay(3000);
    digitalWrite(PIN_BUZZER, LOW);
    return true;
  }
  // Suchlauf 1–6 kHz in 6 s: Wo ist der Piezo am lautesten (Eigenfrequenz)?
  if (name == "SUCHE") {
    for (unsigned hz = 1000; hz <= 6000; hz += 100) {
      Serial.printf("%u Hz\n", hz);
      tone(PIN_BUZZER, hz);
      delay(120);
    }
    noTone(PIN_BUZZER);
    return true;
  }
  // Eine einzelne Frequenz, z. B. "TON 3000" (200–10000 Hz, 1,5 s)
  const long hz = name.toInt();
  if (hz >= 200 && hz <= 10000) {
    tone(PIN_BUZZER, (unsigned)hz);
    delay(1500);
    noTone(PIN_BUZZER);
    return true;
  }
  if (name == "KLINGEL") play(kDoorbell);
  else if (name == "ANRUF") play(kCall);
  else if (name == "WARNUNG") play(kWarning);
  else if (name == "FERTIG") play(kDone);
  else return false;
  return true;
}
#endif

// ---- Befehlszeile -------------------------------------------------------

static bool consoleActive = true;
static unsigned long consoleUntil = CONSOLE_WINDOW_MS;   // jeder Befehl verlängert
static String consoleLine;
static String pendingSsid;

static void printHelp() {
  Serial.println(F("Befehle: STATUS | WIFI-SSID <name> | WIFI-PASS <passwort> | HOST <adresse> |"));
  Serial.println(F("         TOKEN <wert> | FORGET-TOKEN | FORGET-WIFI | ZAEHLER | RESTART | HELP"));
#if defined(HAS_BUZZER)
  Serial.println(F("         TON KLINGEL|ANRUF|WARNUNG|FERTIG|SUCHE|AN|<Hz>"));
#endif
}

// MAC direkt aus dem Chip – WiFi.macAddress() liefert auf dem ESP32 kurz
// nach dem Start manchmal noch 00:00:00:00:00:00.
static String macText() {
#if defined(ESP8266)
  return WiFi.macAddress();
#else
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
#endif
}

static void printStatus() {
  const String ssid = storedSsid();
  Serial.printf("[status] Firmware %s, MAC %s\n", FIRMWARE_VERSION, macText().c_str());
  Serial.printf("[status] WLAN %s: %s\n", ssid.length() ? ssid.c_str() : "(keins)",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "nicht verbunden");
  Serial.printf("[status] newsroom21: %s, Token %s\n", host.length() ? host.c_str() : "(keine Adresse)",
                token.length() ? "hinterlegt" : "fehlt");
}

static void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;
  // Eigene Ausgaben („[status] …“, „=== …“) sind nie ein Befehl. Kommen sie
  // zurück (Echo der Gegenseite, am Gerät nach dem Aufspielen gesehen,
  // 28.09.2026), würde der Sensor sonst endlos auf sich selbst antworten.
  if (line[0] == '[' || line[0] == '=') return;
  const int space = line.indexOf(' ');
  String command = space < 0 ? line : line.substring(0, space);
  String value = space < 0 ? String() : line.substring(space + 1);
  command.toUpperCase();

  if (command == "HELP" || command == "?") {
    printHelp();
  } else if (command == "STATUS") {
    printStatus();
  } else if (command == "WIFI-SSID") {
    if (value.length() == 0 || value.length() > 32) {
      Serial.println(F("[fehler] WLAN-Name: 1 bis 32 Zeichen"));
    } else {
      pendingSsid = value;
      Serial.println(F("[ok] WLAN-Name gemerkt – jetzt WIFI-PASS"));
    }
  } else if (command == "WIFI-PASS") {
    if (pendingSsid.length() == 0) {
      Serial.println(F("[fehler] zuerst WIFI-SSID"));
    } else if (value.length() < 8 || value.length() > 63) {
      Serial.println(F("[fehler] WLAN-Passwort: 8 bis 63 Zeichen"));
    } else {
      // Der Zugang landet im eigenen Speicher des WLAN-Treibers (ESP8266:
      // SDK-Bereich, ESP32: NVS); nach jedem Start verbindet er sich selbst.
      WiFi.persistent(true);
      WiFi.begin(pendingSsid.c_str(), value.c_str());
      pendingSsid = String();
      Serial.println(F("[ok] WLAN-Zugang gespeichert, verbinde …"));
    }
    value = String();
  } else if (command == "HOST") {
    if (!validHost(value)) {
      Serial.println(F("[fehler] Adresse: nur Buchstaben, Ziffern, Punkt, Bindestrich"));
    } else {
      host = value;
      Serial.println(saveConfig() ? F("[ok] Adresse gespeichert") : F("[fehler] Speichern fehlgeschlagen"));
    }
  } else if (command == "TOKEN") {
    if (!validToken(value)) {
      Serial.println(F("[fehler] Token hat nicht das erwartete Format"));
    } else {
      token = value;
      Serial.println(saveConfig() ? F("[ok] Token gespeichert") : F("[fehler] Speichern fehlgeschlagen"));
    }
    value = String();
  } else if (command == "FORGET-TOKEN") {
    token = String();
    saveConfig();
    Serial.println(F("[ok] Token gelöscht"));
  } else if (command == "FORGET-WIFI") {
#if defined(ESP8266)
    WiFi.disconnect(true);         // true = auch den gespeicherten Zugang löschen
#else
    WiFi.disconnect(true, true);   // zweites true = gespeicherten Zugang löschen
#endif
    Serial.println(F("[ok] WLAN-Zugang gelöscht"));
  } else if (command == "ZAEHLER") {
    // Zum Einstellen des Drehreglers am Sensor: Erschütterungen seit der
    // letzten Abfrage (bzw. Meldung an newsroom21).
    Serial.printf("[ok] %lu Erschütterungen\n", (unsigned long)takePulses());
  } else if (command == "RESTART") {
    Serial.println(F("[ok] Neustart"));
    delay(100);
    ESP.restart();
#if defined(HAS_BUZZER)
  } else if (command == "TON") {
    Serial.println(playByName(value) ? F("[ok] Ton gespielt")
                                     : F("[fehler] TON KLINGEL|ANRUF|WARNUNG|FERTIG"));
#endif
  } else {
    // Höchstens einmal je Sekunde – ebenfalls gegen Rückkopplung.
    static unsigned long lastUnknown = 0;
    if (millis() - lastUnknown > 1000UL) {
      lastUnknown = millis();
      Serial.println(F("[fehler] unbekannter Befehl – HELP"));
    }
  }
}

static void loopConsole() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      consoleUntil = millis() + CONSOLE_WINDOW_MS;
      handleCommand(consoleLine);
      consoleLine = String();
    } else if (consoleLine.length() < 200) {
      consoleLine += c;
    }
  }
}

#if defined(SENSOR_ON_RX)
// ESP-01: Nach dem Einrichtungsfenster wird RX zum Sensor-Eingang.
static void endConsole() {
  consoleActive = false;
  consoleLine = String();
  pendingSsid = String();
  Serial.println(F("[start] Befehlszeile geschlossen – RX ist jetzt der Sensor-Eingang"));
  Serial.flush();
  Serial.end();
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  startCounting();
}
#endif

// ---- Verbindung zu newsroom21 -------------------------------------------

#if defined(ESP8266)
// newsroom21 wird per IP angesprochen; Caddys Zertifikat trägt dafür einen
// IP-SAN. BearSSL vergleicht Namen aber nur mit DNS-Einträgen – eine
// Verbindung per IP-Adresse überspringt deshalb den Namensabgleich. Die Kette
// wird weiterhin vollständig gegen die Root-CA von newsroom21 geprüft
// (dasselbe Vorgehen wie bei den ESP8266-Uhren, UHR/CallMonitor.cpp).
class NewsroomClient : public BearSSL::WiFiClientSecure {
 public:
  int connect(const char* name, uint16_t port) override {
    IPAddress ip;
    if (ip.fromString(name)) return BearSSL::WiFiClientSecure::connect(ip, port);
    return BearSSL::WiFiClientSecure::connect(name, port);
  }
  int connect(const String& name, uint16_t port) override { return connect(name.c_str(), port); }
  using BearSSL::WiFiClientSecure::connect;
  std::unique_ptr<WiFiClient> clone() const override {
    return std::unique_ptr<WiFiClient>(new NewsroomClient(*this));
  }
};

static BearSSL::X509List* trust = nullptr;
static BearSSL::Session session;   // beschleunigt den erneuten Aufbau
static NewsroomClient client;
#else
// ESP32 mit mbedTLS 3.6 (pioarduino) prüft auch den IP-Eintrag im
// Zertifikat – hier gibt es also gar keine Ausnahme (wie bei UHR64).
static WiFiClientSecure client;
#endif
static HTTPClient http;

static unsigned long lastReport = 0;
static unsigned long lastSuccess = 0;
static int lastCode = 0;
static String lastState;

static bool timeIsValid() { return time(nullptr) > 1704067200; }   // 01.01.2024

static void blink() {
  led(true);
  delay(30);
  led(false);
}

static bool prepareClient() {
#if defined(ESP8266)
  if (!trust) return false;
  client.setTrustAnchors(trust);
  client.setX509Time(time(nullptr));
  client.setSession(&session);
  // Caddy kann keine kleineren TLS-Fragmente (MFLN) – volle 16 KB Empfang.
  client.setBufferSizes(16384, 512);
#else
  client.setCACert(kNewsroomRootCa);
  client.setHandshakeTimeout(15);
#endif
  return true;
}

static void report() {
  const uint32_t count = takePulses();
  if (WiFi.status() != WL_CONNECTED || !timeIsValid() || !prepareClient()) return;

  const String url = "https://" + host + "/api/sensor/report";
  if (!http.begin(client, url)) return;
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Content-Type", "application/json");
  char status[160];
  snprintf(status, sizeof(status), "rssi=%d;heap=%u;uptime=%lu;fw=%s;ip=%s",
           WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), millis() / 1000UL, FIRMWARE_VERSION,
           WiFi.localIP().toString().c_str());
#if defined(HAS_OTA)
  // Modell und Build-Nummer: daran erkennt newsroom21, ob ein Update passt
  const size_t used = strlen(status);
  snprintf(status + used, sizeof(status) - used, ";model=%s;build=%lld", FIRMWARE_MODEL,
           (long long)FIRMWARE_BUILD);
#endif
  http.addHeader("X-Sensor-Status", status);

  char body[32];
  snprintf(body, sizeof(body), "{\"pulses\":%lu}", (unsigned long)count);
  const int code = http.POST(body);
  String state;
#if defined(HAS_OTA)
  // Antwort z. B. {"interval_s":10,"ok":true,"state":"running"} – nach einer
  // Update-Anforderung zusätzlich "firmware": {Manifest}.
  StaticJsonDocument<1024> answer;
  bool hasOffer = false;
  if (code == HTTP_CODE_OK && http.getSize() >= 0 && http.getSize() <= 1024 &&
      !deserializeJson(answer, http.getString(), DeserializationOption::NestingLimit(3))) {
    state = answer["state"] | "";
    hasOffer = answer["firmware"].is<JsonObject>();
  }
#else
  if (code == HTTP_CODE_OK && http.getSize() >= 0 && http.getSize() <= 256) {
    // Antwort z. B. {"interval_s":10,"ok":true,"state":"running"}
    const String answer = http.getString();
    const int at = answer.indexOf("\"state\":\"");
    if (at >= 0) state = answer.substring(at + 9, answer.indexOf('"', at + 9));
  }
#endif
  http.end();

  if (code == HTTP_CODE_OK) {
    lastSuccess = millis();
    blink();
#if defined(HAS_OTA)
    ota::confirmWorking();
#endif
    if (state.length() && state != lastState) {
      Serial.printf("[melden] Zustand bei newsroom21: %s\n", state.c_str());
#if defined(HAS_BUZZER)
      if (state == "done" && lastState.length()) play(kDone);
#endif
      lastState = state;
    }
  } else if (code < 0) {
    client.stop();   // beim nächsten Mal neu aufbauen
  }
  // Nur Änderungen protokollieren, sonst stünde alle 10 s dieselbe Zeile da.
  if (code != lastCode) {
    lastCode = code;
    if (code == HTTP_CODE_OK) {
      Serial.println(F("[melden] newsroom21 nimmt die Meldungen an"));
    } else if (code == HTTP_CODE_UNAUTHORIZED) {
      Serial.println(F("[melden] newsroom21 lehnt den Token ab (abgelaufen oder gesperrt?)"));
    } else {
      char tls[80] = "";
#if defined(ESP8266)
      const int tlsError = client.getLastSSLError(tls, sizeof(tls));
#else
      const int tlsError = client.lastError(tls, sizeof(tls));
#endif
      Serial.printf("[melden] fehlgeschlagen: Status %d, TLS-Fehler %d %s\n", code, tlsError, tls);
    }
  }
  Serial.printf("[melden] %lu Erschütterungen\n", (unsigned long)count);

#if defined(HAS_OTA)
  if (hasOffer) {
    client.stop();   // Speicher für die zweite TLS-Verbindung (Download)
    ota::handleOffer(answer["firmware"].as<JsonObjectConst>(),
                     "https://" + host + "/api/sensor/firmware", token);
  }
#endif
}

// ---- Start und Schleife -------------------------------------------------

void setup() {
  pinMode(PIN_LED, OUTPUT);
  led(false);
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n=== Waschmaschinen-Sensor %s ===\n", FIRMWARE_VERSION);
#if defined(HAS_OTA)
  ota::begin();
#endif

#if defined(ESP8266)
  if (!LittleFS.begin()) Serial.println(F("[FS] FEHLER: LittleFS nicht verfügbar"));
#else
  if (!LittleFS.begin(true)) Serial.println(F("[FS] FEHLER: LittleFS nicht verfügbar"));
  pinMode(PIN_BUZZER, OUTPUT);
  startCounting();   // eigener Pin – sofort zählen
#endif
  loadConfig();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // Gespeicherten Zugang benutzen – ohne einen meldet der ESP32 sonst einen
  // Fehler, der nach einem Defekt aussieht.
  if (storedSsid().length()) WiFi.begin();
  configTzTime(TIMEZONE, "pool.ntp.org", "de.pool.ntp.org");

#if defined(ESP8266)
  trust = new BearSSL::X509List(kNewsroomRootCa);
  if (!trust || trust->getCount() == 0) Serial.println(F("[TLS] FEHLER: Root-CA nicht lesbar"));
#else
  http.setConnectTimeout(3000);
#endif
  http.setReuse(true);
  http.setTimeout(5000);

  printStatus();
#if defined(SENSOR_ON_RX)
  Serial.printf("[start] Befehlszeile für %lu s offen – HELP für die Befehle\n", CONSOLE_WINDOW_MS / 1000UL);
#else
  Serial.println(F("[start] Befehlszeile offen – HELP für die Befehle"));
#endif
  lastSuccess = millis();
}

void loop() {
#if defined(SENSOR_ON_RX)
  if (consoleActive) {
    loopConsole();
    led((millis() / 500) % 2);   // langsames Blinken: Befehlszeile offen
    if ((long)(millis() - consoleUntil) > 0) {
      if (configured()) {
        led(false);
        endConsole();
      } else {
        static unsigned long lastHint = 0;
        if (millis() - lastHint > 30000UL) {
          lastHint = millis();
          Serial.println(F("[start] Noch nicht eingerichtet – Befehlszeile bleibt offen (HELP)"));
        }
      }
    }
    delay(10);
    return;
  }
#else
  loopConsole();
  if (!configured()) {
    lastSuccess = millis();   // ohne Einrichtung kein Neustart
    delay(10);
    return;
  }
#endif

  if (millis() - lastReport >= REPORT_INTERVAL_MS) {
    lastReport = millis();
    report();
  }
#if defined(HAS_OTA)
  ota::loop();
#endif
  if (millis() - lastSuccess > RESTART_AFTER_MS) {
    Serial.println(F("[start] seit 15 Minuten keine Meldung angekommen – Neustart"));
    delay(100);
    ESP.restart();
  }
  delay(10);
}
