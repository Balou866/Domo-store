#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// ======= CONFIGURATION =======
const char* WIFI_SSID     = "VOTRE_WIFI";
const char* WIFI_PASSWORD = "VOTRE_MOT_DE_PASSE";

// Durée d'ouverture/fermeture complète en millisecondes
const int TRAVEL_TIME_MS = 15000;

// Pins TB6612FNG
#define PIN_AIN1  26
#define PIN_AIN2  27
#define PIN_PWMA  14
#define PIN_STBY  25

#define PWM_DUTY  200  // 0-255 (~78% vitesse), ajustable
// =============================

WebServer server(80);
Adafruit_INA219 ina219;
bool ina219_ok = false;

enum MotorState { STOPPED, OPENING, CLOSING };
MotorState motorState = STOPPED;
unsigned long stopAt = 0;

void motorOpen() {
  digitalWrite(PIN_STBY, HIGH);
  digitalWrite(PIN_AIN1, HIGH);
  digitalWrite(PIN_AIN2, LOW);
  ledcWrite(PIN_PWMA, PWM_DUTY);
  motorState = OPENING;
  stopAt = millis() + TRAVEL_TIME_MS;
}

void motorClose() {
  digitalWrite(PIN_STBY, HIGH);
  digitalWrite(PIN_AIN1, LOW);
  digitalWrite(PIN_AIN2, HIGH);
  ledcWrite(PIN_PWMA, PWM_DUTY);
  motorState = CLOSING;
  stopAt = millis() + TRAVEL_TIME_MS;
}

void motorStop() {
  digitalWrite(PIN_AIN1, LOW);
  digitalWrite(PIN_AIN2, LOW);
  ledcWrite(PIN_PWMA, 0);
  digitalWrite(PIN_STBY, LOW);
  motorState = STOPPED;
  stopAt = 0;
}

void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
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
  doc["state"] = states[motorState];
  doc["ip"]    = WiFi.localIP().toString();
  doc["rssi"]  = WiFi.RSSI();
  if (ina219_ok) {
    doc["current_mA"] = ina219.getCurrent_mA();
    doc["voltage_V"]  = ina219.getBusVoltage_V();
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_STBY, OUTPUT);
  motorStop();

  // API LEDC compatible Arduino ESP32 3.x : ledcAttachPin(pin, freq_Hz, resolution_bits)
  ledcAttachPin(PIN_PWMA, 5000, 8);

  ina219_ok = ina219.begin();
  if (!ina219_ok) Serial.println("INA219 non détecté — câblage I2C à vérifier");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connexion WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  // Routes spécifiques enregistrées avant les génériques
  server.on("/api/status", HTTP_GET,     handleStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/open",   HTTP_GET,     handleCommand);
  server.on("/api/open",   HTTP_OPTIONS, handleOptions);
  server.on("/api/close",  HTTP_GET,     handleCommand);
  server.on("/api/close",  HTTP_OPTIONS, handleOptions);
  server.on("/api/stop",   HTTP_GET,     handleCommand);
  server.on("/api/stop",   HTTP_OPTIONS, handleOptions);

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
