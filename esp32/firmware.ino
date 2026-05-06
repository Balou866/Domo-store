#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ======= CONFIGURATION =======
const char* WIFI_SSID     = "VOTRE_WIFI";
const char* WIFI_PASSWORD = "VOTRE_MOT_DE_PASSE";

// Durée d'ouverture/fermeture complète en millisecondes
// Ajustez selon votre volet (temps pour aller d'un bout à l'autre)
const int TRAVEL_TIME_MS = 15000;

// Pins TB6612FNG
#define PIN_AIN1  26
#define PIN_AIN2  27
#define PIN_PWMA  14
#define PIN_STBY  25

// =============================

WebServer server(80);

enum MotorState { STOPPED, OPENING, CLOSING };
MotorState motorState = STOPPED;
unsigned long stopAt = 0;

void motorOpen() {
  digitalWrite(PIN_STBY, HIGH);
  digitalWrite(PIN_AIN1, HIGH);
  digitalWrite(PIN_AIN2, LOW);
  ledcWrite(0, 200); // ~78% vitesse, ajustable
  motorState = OPENING;
  stopAt = millis() + TRAVEL_TIME_MS;
}

void motorClose() {
  digitalWrite(PIN_STBY, HIGH);
  digitalWrite(PIN_AIN1, LOW);
  digitalWrite(PIN_AIN2, HIGH);
  ledcWrite(0, 200);
  motorState = CLOSING;
  stopAt = millis() + TRAVEL_TIME_MS;
}

void motorStop() {
  digitalWrite(PIN_AIN1, LOW);
  digitalWrite(PIN_AIN2, LOW);
  ledcWrite(0, 0);
  motorState = STOPPED;
  stopAt = 0;
}

void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleCommand() {
  addCORSHeaders();
  String action = server.pathArg(0);
  if (action == "open")       { motorOpen();  server.send(200, "application/json", "{\"ok\":true,\"action\":\"open\"}"); }
  else if (action == "close") { motorClose(); server.send(200, "application/json", "{\"ok\":true,\"action\":\"close\"}"); }
  else if (action == "stop")  { motorStop();  server.send(200, "application/json", "{\"ok\":true,\"action\":\"stop\"}"); }
  else server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown action\"}");
}

void handleStatus() {
  addCORSHeaders();
  StaticJsonDocument<128> doc;
  const char* states[] = {"stopped","opening","closing"};
  doc["state"]    = states[motorState];
  doc["ip"]       = WiFi.localIP().toString();
  doc["rssi"]     = WiFi.RSSI();
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

  // PWM channel 0, 5kHz, 8-bit
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_PWMA, 0);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connexion WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  server.on("/api/{}", HTTP_GET, handleCommand);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/open",  HTTP_OPTIONS, []() { addCORSHeaders(); server.send(204); });
  server.on("/api/close", HTTP_OPTIONS, []() { addCORSHeaders(); server.send(204); });
  server.on("/api/stop",  HTTP_OPTIONS, []() { addCORSHeaders(); server.send(204); });

  server.begin();
  Serial.println("Serveur HTTP démarré");
}

void loop() {
  server.handleClient();

  // Arrêt automatique après TRAVEL_TIME_MS
  if (motorState != STOPPED && millis() >= stopAt) {
    motorStop();
    Serial.println("Arrêt automatique fin de course");
  }
}
