#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Preferences.h>

// ======= CONFIGURATION =======
#define FIRMWARE_VERSION "1.4.3"

const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

const int TRAVEL_TIME_DEFAULT = 60000;

// Pins BTS7960 (IBT-2)
#define PIN_RPWM  14  // ouverture
#define PIN_LPWM  27  // fermeture

// =============================

WebServer server(80);
Adafruit_INA219 ina219;
Preferences prefs;
bool ina219_ok = false;

int  motorSpeed         = 100;        // % PWM (slider 0-100)
float spikeCurrent_mA   = 0.0;        // max sur les 800 premières ms (pic démarrage)

// Imax « régime établi » : max sur la course en excluant la 1ère et la dernière
// seconde (inrush démarrage + pic de butée), via des tranches de 1 s à fold différé.
float maxCurrent_mA     = 0.0;        // résultat confirmé (aide réglage seuils)
float curBucketMax      = 0.0;        // max de la tranche en cours
unsigned long bucketStart = 0;
bool firstBucket          = true;
#define IMAX_BUCKET_MS 1000

int  travelTimeMs       = TRAVEL_TIME_DEFAULT;
bool stopOnTime         = true;
bool stopOnCurrentOpen  = false;
int  thresholdOpen      = 1000;
bool stopOnCurrentClose = false;
int  thresholdClose     = 1000;

bool softStart          = false;
#define SOFT_START_MS 500

enum MotorState { STOPPED, OPENING, CLOSING };
MotorState motorState = STOPPED;
unsigned long stopAt         = 0;
unsigned long motorStartedAt = 0;
unsigned long lastWifiCheck  = 0;
#define WIFI_CHECK_INTERVAL_MS 20000
#define CURRENT_CHECK_DELAY_MS 500

// --- WiFi ---

void connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi principal");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); Serial.print("."); tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);
    Serial.println("\nIP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nEchec WiFi — aucun réseau disponible");
  }
}

// --- Moteur ---

static inline uint8_t pctToPwm(int pct) {
  return constrain(pct * 255 / 100, 0, 255);
}

// Applique le PWM courant (avec rampe soft-start si activée)
static void applyPwm() {
  int target = pctToPwm(motorSpeed);
  int out = target;
  if (softStart) {
    unsigned long e = millis() - motorStartedAt;
    if (e < SOFT_START_MS) out = (int)((long)target * e / SOFT_START_MS);
  }
  ledcWrite(PIN_RPWM, motorState == OPENING ? out : 0);
  ledcWrite(PIN_LPWM, motorState == CLOSING ? out : 0);
}

static void motorStart(MotorState dir) {
  spikeCurrent_mA = 0.0;
  maxCurrent_mA   = 0.0;
  curBucketMax    = 0.0;
  bucketStart     = millis();
  firstBucket     = true;
  motorState     = dir;
  motorStartedAt = millis();
  stopAt = stopOnTime ? millis() + travelTimeMs : 0;
  applyPwm();
}

void motorOpen()  { motorStart(OPENING); }
void motorClose() { motorStart(CLOSING); }

void motorStop() {
  ledcWrite(PIN_RPWM, 0);
  ledcWrite(PIN_LPWM, 0);
  motorState = STOPPED;
  stopAt = 0;
  Serial.println("motorStop — RPWM=0 LPWM=0");
}

// --- HTTP ---

void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  addCORSHeaders();
  server.send(204);
}

void handleCommand() {
  addCORSHeaders();
  String uri = server.uri();
  String action = uri.substring(uri.lastIndexOf('/') + 1);

  if (action == "open")       { motorOpen();  server.send(200, "application/json", "{\"ok\":true,\"action\":\"open\"}"); }
  else if (action == "close") { motorClose(); server.send(200, "application/json", "{\"ok\":true,\"action\":\"close\"}"); }
  else if (action == "stop")  { motorStop();  server.send(200, "application/json", "{\"ok\":true,\"action\":\"stop\"}"); }
  else server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown action\"}");
}

void handleReboot() {
  addCORSHeaders();
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  delay(300);
  ESP.restart();
}

void handleStatus() {
  addCORSHeaders();
  StaticJsonDocument<384> doc;
  const char* states[] = {"stopped", "opening", "closing"};
  doc["firmware"]       = FIRMWARE_VERSION;
  doc["state"]          = states[motorState];
  doc["ip"]             = WiFi.localIP().toString();
  doc["rssi"]           = WiFi.RSSI();
  doc["travel_time_ms"] = travelTimeMs;
  doc["motor_speed"]    = motorSpeed;
  if (ina219_ok) {
    doc["current_mA"]       = ina219.getCurrent_mA();
    doc["voltage_V"]        = ina219.getBusVoltage_V();
    doc["spike_current_mA"] = spikeCurrent_mA;
    doc["max_current_mA"]   = maxCurrent_mA;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleGetConfig() {
  addCORSHeaders();
  StaticJsonDocument<512> doc;
  doc["travel_time_ms"]    = travelTimeMs;
  doc["stop_on_time"]      = stopOnTime;
  doc["stop_on_cur_open"]  = stopOnCurrentOpen;
  doc["threshold_open"]    = thresholdOpen;
  doc["stop_on_cur_close"] = stopOnCurrentClose;
  doc["threshold_close"]   = thresholdClose;
  doc["motor_speed"]       = motorSpeed;
  doc["soft_start"]        = softStart;
  doc["ssid1"]             = WIFI_SSID;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleOTAUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    motorStop();
    if (Update.isRunning()) Update.abort();
    Serial.printf("OTA START: %s size=%u heap=%u\n", upload.filename.c_str(), upload.totalSize, ESP.getFreeHeap());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Serial.print("OTA begin FAIL: "); Update.printError(Serial); }
    else Serial.println("OTA begin OK");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.print("OTA write FAIL: "); Update.printError(Serial);
      Update.abort();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("OTA END: total=%u\n", upload.totalSize);
    if (Update.end(true)) Serial.println("OTA end OK");
    else { Serial.print("OTA end FAIL: "); Update.printError(Serial); }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("OTA ABORTED");
    Update.abort();
  }
}

void handleOTADone() {
  addCORSHeaders();
  if (Update.hasError()) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"OTA echoue\"}");
  } else {
    server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    delay(500);
    ESP.restart();
  }
}

void handleSetConfig() {
  addCORSHeaders();
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON invalide\"}");
    return;
  }
  if (doc.containsKey("travel_time_ms"))    { travelTimeMs = doc["travel_time_ms"].as<int>();           prefs.putInt("travel_ms",  travelTimeMs); }
  if (doc.containsKey("stop_on_time"))      { stopOnTime = doc["stop_on_time"].as<bool>();              prefs.putBool("stop_time", stopOnTime); }
  if (doc.containsKey("stop_on_cur_open"))  { stopOnCurrentOpen = doc["stop_on_cur_open"].as<bool>();   prefs.putBool("stop_cur_o", stopOnCurrentOpen); }
  if (doc.containsKey("threshold_open"))    { thresholdOpen = doc["threshold_open"].as<int>();           prefs.putInt("thr_open",   thresholdOpen); }
  if (doc.containsKey("stop_on_cur_close")) { stopOnCurrentClose = doc["stop_on_cur_close"].as<bool>();  prefs.putBool("stop_cur_c", stopOnCurrentClose); }
  if (doc.containsKey("threshold_close"))   { thresholdClose = doc["threshold_close"].as<int>();         prefs.putInt("thr_close",  thresholdClose); }
  if (doc.containsKey("soft_start"))        { softStart = doc["soft_start"].as<bool>();                  prefs.putBool("soft",      softStart); }
  if (doc.containsKey("motor_speed")) {
    motorSpeed = constrain(doc["motor_speed"].as<int>(), 0, 100);
    prefs.putInt("motor_spd", motorSpeed);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- Setup / Loop ---

void setup() {
  Serial.begin(115200);

  ledcAttach(PIN_RPWM, 20000, 8);
  ledcAttach(PIN_LPWM, 20000, 8);
  ledcWrite(PIN_RPWM, 0);
  ledcWrite(PIN_LPWM, 0);

  prefs.begin("volet", false);
  travelTimeMs       = prefs.getInt("travel_ms",  TRAVEL_TIME_DEFAULT);
  stopOnTime         = prefs.getBool("stop_time", true);
  stopOnCurrentOpen  = prefs.getBool("stop_cur_o", false);
  thresholdOpen      = prefs.getInt("thr_open",   1000);
  stopOnCurrentClose = prefs.getBool("stop_cur_c", false);
  thresholdClose     = prefs.getInt("thr_close",  1000);
  motorSpeed         = prefs.getInt("motor_spd",  100);
  softStart          = prefs.getBool("soft", false);

  ina219_ok = ina219.begin();
  if (!ina219_ok) Serial.println("INA219 non détecté");

  connectWiFi();

  ArduinoOTA.setHostname("volet-roulant");
  ArduinoOTA.onStart([]() { motorStop(); Serial.println("ArduinoOTA: debut"); });
  ArduinoOTA.onEnd([]()   { Serial.println("ArduinoOTA: termine"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("ArduinoOTA erreur [%u]\n", e); });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA pret (volet-roulant.local)");

  server.on("/api/status",  HTTP_GET,     handleStatus);
  server.on("/api/status",  HTTP_OPTIONS, handleOptions);
  server.on("/api/config",  HTTP_GET,     handleGetConfig);
  server.on("/api/config",  HTTP_POST,    handleSetConfig);
  server.on("/api/config",  HTTP_OPTIONS, handleOptions);
  server.on("/api/open",    HTTP_GET,     handleCommand);
  server.on("/api/open",    HTTP_OPTIONS, handleOptions);
  server.on("/api/close",   HTTP_GET,     handleCommand);
  server.on("/api/close",   HTTP_OPTIONS, handleOptions);
  server.on("/api/stop",    HTTP_GET,     handleCommand);
  server.on("/api/stop",    HTTP_OPTIONS, handleOptions);
  server.on("/api/reboot",  HTTP_POST,    handleReboot);
  server.on("/api/reboot",  HTTP_OPTIONS, handleOptions);
  server.on("/api/ota",     HTTP_POST,    handleOTADone, handleOTAUpload);
  server.on("/api/ota",     HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("Serveur HTTP démarré");
}

// Arrêt par seuil de courant (butée / obstacle)
void checkCurrentStop(float cur) {
  if (millis() - motorStartedAt < CURRENT_CHECK_DELAY_MS) return;
  if (motorState == OPENING && stopOnCurrentOpen && cur > thresholdOpen) {
    motorStop();
    Serial.printf("Arrêt courant ouverture: %.0f mA\n", cur);
  } else if (motorState == CLOSING && stopOnCurrentClose && cur > thresholdClose) {
    motorStop();
    Serial.printf("Arrêt courant fermeture: %.0f mA\n", cur);
  }
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi perdu — reconnexion...");
      connectWiFi();
      lastWifiCheck = millis();
    }
  }

  if (motorState == STOPPED) return;

  // Soft-start : réapplique le PWM tant que la rampe monte
  if (softStart && millis() - motorStartedAt < SOFT_START_MS) applyPwm();

  // Lecture unique du courant par tour, réutilisée pour le suivi et la coupure
  float cur = ina219_ok ? ina219.getCurrent_mA() : 0.0;
  if (ina219_ok) {
    if (millis() - motorStartedAt < 800 && cur > spikeCurrent_mA) spikeCurrent_mA = cur; // pic démarrage

    // Imax régime : accumule par tranche de 1 s, fold différé (jette 1ère tranche + tranche en cours)
    if (cur > curBucketMax) curBucketMax = cur;
    if (millis() - bucketStart >= IMAX_BUCKET_MS) {
      if (!firstBucket && curBucketMax > maxCurrent_mA) maxCurrent_mA = curBucketMax;
      firstBucket  = false;
      curBucketMax = 0.0;
      bucketStart  = millis();
    }
  }

  if (stopAt != 0 && millis() >= stopAt) {
    motorStop();
    Serial.println("Arrêt timer");
  } else if (ina219_ok) {
    checkCurrentStop(cur);
  }
}
