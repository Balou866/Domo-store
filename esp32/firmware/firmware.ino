#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Preferences.h>

// ======= CONFIGURATION =======
const char* WIFI_SSID     = "ENTER SSID";
const char* WIFI_PASSWORD = "ENTER SSID PASSWORD";

const int TRAVEL_TIME_DEFAULT = 60000;

// Pins TB6612FNG
#define PIN_AIN1  26
#define PIN_AIN2  27
#define PIN_PWMA  14
#define PIN_STBY  25

#define PWM_DUTY  200  // 0-255 (~78% vitesse)
// =============================

WebServer server(80);
Adafruit_INA219 ina219;
Preferences prefs;
bool ina219_ok = false;

int travelTimeMs = TRAVEL_TIME_DEFAULT;

enum MotorState { STOPPED, OPENING, CLOSING };
MotorState motorState = STOPPED;
unsigned long stopAt = 0;

// --- WiFi ---

void connectWiFi() {
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
    Serial.println("\nIP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nEchec WiFi — aucun réseau disponible");
  }
}

// --- Moteur ---

void motorOpen() {
  digitalWrite(PIN_STBY, HIGH);
  digitalWrite(PIN_AIN1, HIGH);
  digitalWrite(PIN_AIN2, LOW);
  ledcWrite(PIN_PWMA, PWM_DUTY);
  motorState = OPENING;
  stopAt = millis() + travelTimeMs;
}

void motorClose() {
  digitalWrite(PIN_STBY, HIGH);
  digitalWrite(PIN_AIN1, LOW);
  digitalWrite(PIN_AIN2, HIGH);
  ledcWrite(PIN_PWMA, PWM_DUTY);
  motorState = CLOSING;
  stopAt = millis() + travelTimeMs;
}

void motorStop() {
  digitalWrite(PIN_AIN1, LOW);
  digitalWrite(PIN_AIN2, LOW);
  ledcWrite(PIN_PWMA, 0);
  digitalWrite(PIN_STBY, LOW);
  motorState = STOPPED;
  stopAt = 0;
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
  StaticJsonDocument<256> doc;
  const char* states[] = {"stopped", "opening", "closing"};
  doc["state"]         = states[motorState];
  doc["ip"]            = WiFi.localIP().toString();
  doc["rssi"]          = WiFi.RSSI();
  doc["travel_time_ms"] = travelTimeMs;
  if (ina219_ok) {
    doc["current_mA"] = ina219.getCurrent_mA();
    doc["voltage_V"]  = ina219.getBusVoltage_V();
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleGetConfig() {
  addCORSHeaders();
  StaticJsonDocument<256> doc;
  doc["travel_time_ms"] = travelTimeMs;
  doc["ssid1"]          = WIFI_SSID;
  doc["ssid2"]          = prefs.getString("ssid2", "");
  // password2 non exposé
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSetConfig() {
  addCORSHeaders();
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON invalide\"}");
    return;
  }
  if (doc.containsKey("travel_time_ms")) {
    travelTimeMs = doc["travel_time_ms"].as<int>();
    prefs.putInt("travel_ms", travelTimeMs);
    Serial.println("travel_time_ms → " + String(travelTimeMs));
  }
  if (doc.containsKey("ssid2")) {
    prefs.putString("ssid2", doc["ssid2"].as<String>());
    Serial.println("ssid2 → " + doc["ssid2"].as<String>());
  }
  if (doc.containsKey("password2")) {
    prefs.putString("pass2", doc["password2"].as<String>());
    Serial.println("password2 mis à jour");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- Setup / Loop ---

void setup() {
  Serial.begin(115200);

  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_STBY, OUTPUT);
  motorStop();

  ledcAttach(PIN_PWMA, 5000, 8);

  prefs.begin("volet", false);
  travelTimeMs = prefs.getInt("travel_ms", TRAVEL_TIME_DEFAULT);
  Serial.println("travel_time_ms = " + String(travelTimeMs));

  ina219_ok = ina219.begin();
  if (!ina219_ok) Serial.println("INA219 non détecté");

  connectWiFi();

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

  server.begin();
  Serial.println("Serveur HTTP démarré");
}

void loop() {
  server.handleClient();

  if (motorState != STOPPED && millis() >= stopAt) {
    motorStop();
    Serial.println("Arrêt automatique fin de course");
  }
}
