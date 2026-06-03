#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Preferences.h>

// ======= CONFIGURATION =======
#define FIRMWARE_VERSION "1.3.0"

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

int  motorSpeed         = 100;
float spikeCurrent_mA   = 0.0;

int  travelTimeMs       = TRAVEL_TIME_DEFAULT;
bool stopOnTime         = true;
bool stopOnCurrentOpen  = false;
int  thresholdOpen      = 1000;
bool stopOnCurrentClose = false;
int  thresholdClose     = 1000;

enum MotorState { STOPPED, OPENING, CLOSING };
MotorState motorState = STOPPED;
unsigned long stopAt = 0;
unsigned long motorStartedAt = 0;
unsigned long lastWifiCheck = 0;
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

  if (WiFi.status() != WL_CONNECTED) {
    String ssid2 = prefs.getString("ssid2", "");
    String pass2 = prefs.getString("pass2", "");
    if (ssid2.length() > 0) {
      WiFi.disconnect();
      WiFi.begin(ssid2.c_str(), pass2.c_str());
      Serial.print("\nWiFi secondaire (" + ssid2 + ")");
      tries = 0;
      while (WiFi.status() != WL_CONNECTED && tries < 20) {
        delay(500); Serial.print("."); tries++;
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);
    Serial.println("\nIP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nEchec WiFi — aucun réseau disponible");
  }
}

// --- Moteur ---

static inline uint8_t pwmValue() {
  return constrain(motorSpeed * 255 / 100, 0, 255);
}

static void motorStart(MotorState dir) {
  spikeCurrent_mA = 0.0;
  ledcWrite(PIN_RPWM, dir == OPENING ? pwmValue() : 0);
  ledcWrite(PIN_LPWM, dir == CLOSING ? pwmValue() : 0);
  motorState    = dir;
  motorStartedAt = millis();
  stopAt = stopOnTime ? millis() + travelTimeMs : 0;
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

void handleStatus() {
  addCORSHeaders();
  StaticJsonDocument<320> doc;
  const char* states[] = {"stopped", "opening", "closing"};
  doc["firmware"]      = FIRMWARE_VERSION;
  doc["state"]         = states[motorState];
  doc["ip"]            = WiFi.localIP().toString();
  doc["rssi"]          = WiFi.RSSI();
  doc["travel_time_ms"] = travelTimeMs;
  doc["motor_speed"]   = motorSpeed;
  if (ina219_ok) {
    doc["current_mA"]       = ina219.getCurrent_mA();
    doc["voltage_V"]        = ina219.getBusVoltage_V();
    doc["spike_current_mA"] = spikeCurrent_mA;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleGetConfig() {
  addCORSHeaders();
  StaticJsonDocument<512> doc;
  doc["travel_time_ms"]      = travelTimeMs;
  doc["stop_on_time"]        = stopOnTime;
  doc["stop_on_cur_open"]    = stopOnCurrentOpen;
  doc["threshold_open"]      = thresholdOpen;
  doc["stop_on_cur_close"]   = stopOnCurrentClose;
  doc["threshold_close"]     = thresholdClose;
  doc["motor_speed"]         = motorSpeed;
  doc["ssid1"]               = WIFI_SSID;
  doc["ssid2"]               = prefs.getString("ssid2", "");
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
  if (doc.containsKey("travel_time_ms"))    { travelTimeMs = doc["travel_time_ms"].as<int>();          prefs.putInt("travel_ms",  travelTimeMs); }
  if (doc.containsKey("stop_on_time"))      { stopOnTime = doc["stop_on_time"].as<bool>();              prefs.putBool("stop_time", stopOnTime); }
  if (doc.containsKey("stop_on_cur_open"))  { stopOnCurrentOpen = doc["stop_on_cur_open"].as<bool>();  prefs.putBool("stop_cur_o", stopOnCurrentOpen); }
  if (doc.containsKey("threshold_open"))    { thresholdOpen = doc["threshold_open"].as<int>();          prefs.putInt("thr_open",   thresholdOpen); }
  if (doc.containsKey("stop_on_cur_close")) { stopOnCurrentClose = doc["stop_on_cur_close"].as<bool>(); prefs.putBool("stop_cur_c", stopOnCurrentClose); }
  if (doc.containsKey("threshold_close"))   { thresholdClose = doc["threshold_close"].as<int>();        prefs.putInt("thr_close",  thresholdClose); }
  if (doc.containsKey("motor_speed")) {
    motorSpeed = constrain(doc["motor_speed"].as<int>(), 10, 100);
    prefs.putInt("motor_spd", motorSpeed);
  }
  if (doc.containsKey("ssid2"))    { prefs.putString("ssid2", doc["ssid2"].as<String>()); }
  if (doc.containsKey("password2")){ prefs.putString("pass2", doc["password2"].as<String>()); }
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- Setup / Loop ---

void setup() {
  Serial.begin(115200);

  ledcAttach(PIN_RPWM, 20000, 8);
  ledcAttach(PIN_LPWM, 20000, 8);
  motorStop();

  prefs.begin("volet", false);
  travelTimeMs       = prefs.getInt("travel_ms",  TRAVEL_TIME_DEFAULT);
  stopOnTime         = prefs.getBool("stop_time", true);
  stopOnCurrentOpen  = prefs.getBool("stop_cur_o", false);
  thresholdOpen      = prefs.getInt("thr_open",   1000);
  stopOnCurrentClose = prefs.getBool("stop_cur_c", false);
  thresholdClose     = prefs.getInt("thr_close",  1000);
  motorSpeed         = prefs.getInt("motor_spd",  100);

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
  server.on("/api/ota",     HTTP_POST,    handleOTADone, handleOTAUpload);
  server.on("/api/ota",     HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("Serveur HTTP démarré");
}

void checkCurrentStop() {
  if (millis() - motorStartedAt < CURRENT_CHECK_DELAY_MS) return;
  float cur = ina219.getCurrent_mA();
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

  if (motorState != STOPPED) {
    if (ina219_ok && millis() - motorStartedAt < 800) {
      float cur = ina219.getCurrent_mA();
      if (cur > spikeCurrent_mA) spikeCurrent_mA = cur;
    }
    if (stopAt != 0 && millis() >= stopAt) {
      motorStop();
      Serial.println("Arrêt timer");
    } else if (ina219_ok) {
      checkCurrentStop();
    }
  }
}
