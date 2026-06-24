#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Preferences.h>

// ======= CONFIGURATION =======
#define FIRMWARE_VERSION "1.4.0"

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

// --- Vitesses (3 niveaux) ---
int speedPct[3]    = {50, 75, 100};   // low / med / high (% PWM)
int speedLevel     = 2;               // 0=low 1=med 2=high (niveau actif)

// --- Temps de course par niveau (ms, course complète 0<->100%) ---
int travelOpen[3]  = {TRAVEL_TIME_DEFAULT, TRAVEL_TIME_DEFAULT, TRAVEL_TIME_DEFAULT};
int travelClose[3] = {TRAVEL_TIME_DEFAULT, TRAVEL_TIME_DEFAULT, TRAVEL_TIME_DEFAULT};

// --- Détection butée / obstacle (INA219) ---
bool stopOnCurrentOpen  = false;
int  thresholdOpen      = 1000;
bool stopOnCurrentClose = false;
int  thresholdClose     = 1000;

// --- Soft-start ---
bool softStart          = false;
#define SOFT_START_MS 500

// --- Calibration ---
int  calThreshold       = 1200;       // mA, seuil détection butée pendant calibration
#define CAL_STEP_TIMEOUT_MS 120000UL  // sécurité : abandon si pas de butée

// --- Position ---
float positionPct       = 0.0;        // 0 = fermé, 100 = ouvert

float spikeCurrent_mA   = 0.0;

enum MotorState { STOPPED, OPENING, CLOSING };
MotorState motorState = STOPPED;

unsigned long stopAt        = 0;
unsigned long motorStartedAt = 0;
unsigned long lastWifiCheck  = 0;
#define WIFI_CHECK_INTERVAL_MS 20000
#define CURRENT_CHECK_DELAY_MS 500

// Mouvement courant
float moveStartPos   = 0.0;
int   moveTargetPct  = 0;
int   moveFullTravel = TRAVEL_TIME_DEFAULT;
int   activeSpeedPct = 100;

// Calibration (machine à états)
bool          calibrating = false;
int           calStep     = 0;        // 0 = homing close, 1..6 = mesures
unsigned long calStepStart = 0;

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

static void saveTravelPrefs() {
  prefs.putInt("to_lo", travelOpen[0]);  prefs.putInt("to_me", travelOpen[1]);  prefs.putInt("to_hi", travelOpen[2]);
  prefs.putInt("tc_lo", travelClose[0]); prefs.putInt("tc_me", travelClose[1]); prefs.putInt("tc_hi", travelClose[2]);
}

// Applique le PWM courant (avec rampe soft-start si activée)
static void applyPwm() {
  int target = pctToPwm(activeSpeedPct);
  int out = target;
  if (softStart) {
    unsigned long e = millis() - motorStartedAt;
    if (e < SOFT_START_MS) out = (int)((long)target * e / SOFT_START_MS);
  }
  ledcWrite(PIN_RPWM, motorState == OPENING ? out : 0);
  ledcWrite(PIN_LPWM, motorState == CLOSING ? out : 0);
}

void motorStop() {
  ledcWrite(PIN_RPWM, 0);
  ledcWrite(PIN_LPWM, 0);
  motorState = STOPPED;
  stopAt = 0;
  prefs.putInt("pos", (int)round(positionPct));
  Serial.printf("motorStop — pos=%.0f%%\n", positionPct);
}

// Configure et démarre un mouvement (champs de suivi + PWM)
static void startMove(MotorState dir, int target, int speed, int full, unsigned long durMs) {
  moveStartPos    = positionPct;
  moveTargetPct   = target;
  moveFullTravel  = max(full, 1);
  activeSpeedPct  = speed;
  spikeCurrent_mA = 0.0;
  motorState      = dir;
  motorStartedAt  = millis();
  stopAt          = millis() + durMs;
  applyPwm();
}

// Lance un mouvement vers une position cible (0..100) à une vitesse donnée (%)
static void motorMoveTo(int targetPct, int speed, int fullTravelMs) {
  targetPct = constrain(targetPct, 0, 100);
  if (fabs(targetPct - positionPct) < 0.5) { return; }
  MotorState dir = (targetPct > positionPct) ? OPENING : CLOSING;
  unsigned long dur = (unsigned long)(fabs(targetPct - positionPct) / 100.0 * max(fullTravelMs, 1));
  startMove(dir, targetPct, speed, fullTravelMs, dur);
  Serial.printf("Move %s vers %d%% (vit=%d%%)\n", dir == OPENING ? "OUVERTURE" : "FERMETURE", targetPct, speed);
}

// Mouvement utilisateur (open/close/position) à la vitesse sélectionnée
void motorGoTo(int targetPct) {
  MotorState dir = (targetPct > positionPct) ? OPENING : CLOSING;
  int full = (dir == OPENING) ? travelOpen[speedLevel] : travelClose[speedLevel];
  motorMoveTo(targetPct, speedPct[speedLevel], full);
}

void motorOpen()  { motorGoTo(100); }
void motorClose() { motorGoTo(0); }

// Met à jour la position pendant le mouvement
static void updatePosition() {
  if (motorState == STOPPED) return;
  float traveled = (float)(millis() - motorStartedAt) / moveFullTravel * 100.0;
  positionPct = moveStartPos + (motorState == OPENING ? traveled : -traveled);
  positionPct = constrain(positionPct, 0.0, 100.0);
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
  if (calibrating) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"calibration en cours\"}"); return; }
  String uri = server.uri();
  String action = uri.substring(uri.lastIndexOf('/') + 1);

  if (action == "open")       { motorOpen();  server.send(200, "application/json", "{\"ok\":true,\"action\":\"open\"}"); }
  else if (action == "close") { motorClose(); server.send(200, "application/json", "{\"ok\":true,\"action\":\"close\"}"); }
  else if (action == "stop")  { motorStop();  server.send(200, "application/json", "{\"ok\":true,\"action\":\"stop\"}"); }
  else server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown action\"}");
}

void handlePosition() {
  addCORSHeaders();
  if (calibrating) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"calibration en cours\"}"); return; }
  if (!server.hasArg("p")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"p manquant\"}"); return; }
  int p = constrain(server.arg("p").toInt(), 0, 100);
  motorGoTo(p);
  String out = "{\"ok\":true,\"target\":" + String(p) + "}";
  server.send(200, "application/json", out);
}

// Redéfinit le compteur de position sans bouger le moteur (recalage manuel)
void handleSetPos() {
  addCORSHeaders();
  if (motorState != STOPPED) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"moteur en mouvement\"}"); return; }
  if (!server.hasArg("p")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"p manquant\"}"); return; }
  positionPct = constrain(server.arg("p").toInt(), 0, 100);
  prefs.putInt("pos", (int)round(positionPct));
  String out = "{\"ok\":true,\"position\":" + String((int)round(positionPct)) + "}";
  server.send(200, "application/json", out);
}

// Mouvement forcé sur la course complète, ignore la position courante (recalage)
void handleForce() {
  addCORSHeaders();
  if (calibrating) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"calibration en cours\"}"); return; }
  String dir = server.arg("dir");
  if (dir != "open" && dir != "close") { server.send(400, "application/json", "{\"ok\":false,\"error\":\"dir open|close\"}"); return; }

  MotorState d = (dir == "open") ? OPENING : CLOSING;
  int full = (d == OPENING) ? travelOpen[speedLevel] : travelClose[speedLevel];
  startMove(d, (d == OPENING) ? 100 : 0, speedPct[speedLevel], full, full);  // toujours course complète
  server.send(200, "application/json", "{\"ok\":true,\"force\":true}");
}

// Lance une étape de calibration (la position n'est pas suivie ici : arrêt par butée)
static void calRunStep(MotorState dir, int speed) {
  activeSpeedPct  = speed;
  motorState      = dir;
  motorStartedAt  = millis();
  calStepStart    = millis();
  spikeCurrent_mA = 0.0;
  applyPwm();
}

void startCalibration() {
  calibrating = true;
  calStep = 0;                       // homing : fermeture jusqu'à butée basse, à vitesse haute
  calRunStep(CLOSING, speedPct[2]);
  Serial.println("CALIBRATION: démarrage (homing fermeture)");
}

void handleCalibrate() {
  addCORSHeaders();
  if (!ina219_ok) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"INA219 absent\"}"); return; }
  if (calibrating) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"déjà en cours\"}"); return; }
  startCalibration();
  server.send(200, "application/json", "{\"ok\":true,\"calibrating\":true}");
}

void handleReboot() {
  addCORSHeaders();
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  delay(300);
  ESP.restart();
}

void handleStatus() {
  addCORSHeaders();
  StaticJsonDocument<512> doc;
  const char* states[] = {"stopped", "opening", "closing"};
  doc["firmware"]       = FIRMWARE_VERSION;
  doc["state"]          = states[motorState];
  doc["position"]       = (int)round(positionPct);
  doc["ip"]             = WiFi.localIP().toString();
  doc["rssi"]           = WiFi.RSSI();
  doc["speed_level"]    = speedLevel;
  doc["motor_speed"]    = speedPct[speedLevel];
  doc["calibrating"]    = calibrating;
  if (calibrating) doc["cal_step"] = calStep;
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
  StaticJsonDocument<768> doc;
  JsonArray sp = doc.createNestedArray("speed_pct");
  JsonArray to = doc.createNestedArray("travel_open");
  JsonArray tc = doc.createNestedArray("travel_close");
  for (int i = 0; i < 3; i++) { sp.add(speedPct[i]); to.add(travelOpen[i]); tc.add(travelClose[i]); }
  doc["speed_level"]        = speedLevel;
  doc["stop_on_cur_open"]   = stopOnCurrentOpen;
  doc["threshold_open"]     = thresholdOpen;
  doc["stop_on_cur_close"]  = stopOnCurrentClose;
  doc["threshold_close"]    = thresholdClose;
  doc["soft_start"]         = softStart;
  doc["cal_threshold"]      = calThreshold;
  doc["position"]           = (int)round(positionPct);
  doc["ssid1"]              = WIFI_SSID;
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
  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON invalide\"}");
    return;
  }

  if (doc.containsKey("speed_pct")) {
    JsonArray a = doc["speed_pct"].as<JsonArray>();
    for (int i = 0; i < 3 && i < (int)a.size(); i++) {
      speedPct[i] = constrain(a[i].as<int>(), 10, 100);
    }
    prefs.putInt("spd_lo", speedPct[0]);
    prefs.putInt("spd_me", speedPct[1]);
    prefs.putInt("spd_hi", speedPct[2]);
  }
  if (doc.containsKey("speed_level")) {
    speedLevel = constrain(doc["speed_level"].as<int>(), 0, 2);
    prefs.putInt("lvl", speedLevel);
  }
  if (doc.containsKey("travel_open") || doc.containsKey("travel_close")) {
    if (doc.containsKey("travel_open")) {
      JsonArray a = doc["travel_open"].as<JsonArray>();
      for (int i = 0; i < 3 && i < (int)a.size(); i++) travelOpen[i] = a[i].as<int>();
    }
    if (doc.containsKey("travel_close")) {
      JsonArray a = doc["travel_close"].as<JsonArray>();
      for (int i = 0; i < 3 && i < (int)a.size(); i++) travelClose[i] = a[i].as<int>();
    }
    saveTravelPrefs();
  }
  if (doc.containsKey("stop_on_cur_open"))  { stopOnCurrentOpen = doc["stop_on_cur_open"].as<bool>();  prefs.putBool("stop_cur_o", stopOnCurrentOpen); }
  if (doc.containsKey("threshold_open"))    { thresholdOpen = doc["threshold_open"].as<int>();          prefs.putInt("thr_open",   thresholdOpen); }
  if (doc.containsKey("stop_on_cur_close")) { stopOnCurrentClose = doc["stop_on_cur_close"].as<bool>(); prefs.putBool("stop_cur_c", stopOnCurrentClose); }
  if (doc.containsKey("threshold_close"))   { thresholdClose = doc["threshold_close"].as<int>();        prefs.putInt("thr_close",  thresholdClose); }
  if (doc.containsKey("soft_start"))        { softStart = doc["soft_start"].as<bool>();                 prefs.putBool("soft",      softStart); }
  if (doc.containsKey("cal_threshold"))     { calThreshold = doc["cal_threshold"].as<int>();            prefs.putInt("cal_thr",    calThreshold); }
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
  speedPct[0]        = prefs.getInt("spd_lo", 50);
  speedPct[1]        = prefs.getInt("spd_me", 75);
  speedPct[2]        = prefs.getInt("spd_hi", 100);
  speedLevel         = prefs.getInt("lvl", 2);
  travelOpen[0]      = prefs.getInt("to_lo", TRAVEL_TIME_DEFAULT);
  travelOpen[1]      = prefs.getInt("to_me", TRAVEL_TIME_DEFAULT);
  travelOpen[2]      = prefs.getInt("to_hi", TRAVEL_TIME_DEFAULT);
  travelClose[0]     = prefs.getInt("tc_lo", TRAVEL_TIME_DEFAULT);
  travelClose[1]     = prefs.getInt("tc_me", TRAVEL_TIME_DEFAULT);
  travelClose[2]     = prefs.getInt("tc_hi", TRAVEL_TIME_DEFAULT);
  stopOnCurrentOpen  = prefs.getBool("stop_cur_o", false);
  thresholdOpen      = prefs.getInt("thr_open",   1000);
  stopOnCurrentClose = prefs.getBool("stop_cur_c", false);
  thresholdClose     = prefs.getInt("thr_close",  1000);
  softStart          = prefs.getBool("soft", false);
  calThreshold       = prefs.getInt("cal_thr", 1200);
  positionPct        = prefs.getInt("pos", 0);

  ina219_ok = ina219.begin();
  if (!ina219_ok) Serial.println("INA219 non détecté");

  connectWiFi();

  ArduinoOTA.setHostname("volet-roulant");
  ArduinoOTA.onStart([]() { motorStop(); Serial.println("ArduinoOTA: debut"); });
  ArduinoOTA.onEnd([]()   { Serial.println("ArduinoOTA: termine"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("ArduinoOTA erreur [%u]\n", e); });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA pret (volet-roulant.local)");

  server.on("/api/status",    HTTP_GET,     handleStatus);
  server.on("/api/status",    HTTP_OPTIONS, handleOptions);
  server.on("/api/config",    HTTP_GET,     handleGetConfig);
  server.on("/api/config",    HTTP_POST,    handleSetConfig);
  server.on("/api/config",    HTTP_OPTIONS, handleOptions);
  server.on("/api/open",      HTTP_GET,     handleCommand);
  server.on("/api/open",      HTTP_OPTIONS, handleOptions);
  server.on("/api/close",     HTTP_GET,     handleCommand);
  server.on("/api/close",     HTTP_OPTIONS, handleOptions);
  server.on("/api/stop",      HTTP_GET,     handleCommand);
  server.on("/api/stop",      HTTP_OPTIONS, handleOptions);
  server.on("/api/position",  HTTP_GET,     handlePosition);
  server.on("/api/position",  HTTP_OPTIONS, handleOptions);
  server.on("/api/setpos",    HTTP_GET,     handleSetPos);
  server.on("/api/setpos",    HTTP_OPTIONS, handleOptions);
  server.on("/api/force",     HTTP_GET,     handleForce);
  server.on("/api/force",     HTTP_OPTIONS, handleOptions);
  server.on("/api/calibrate", HTTP_POST,    handleCalibrate);
  server.on("/api/calibrate", HTTP_OPTIONS, handleOptions);
  server.on("/api/reboot",    HTTP_POST,    handleReboot);
  server.on("/api/reboot",    HTTP_OPTIONS, handleOptions);
  server.on("/api/ota",       HTTP_POST,    handleOTADone, handleOTAUpload);
  server.on("/api/ota",       HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("Serveur HTTP démarré");
}

// Détection de butée par courant (renvoie true si seuil dépassé)
bool currentStall(int threshold) {
  if (!ina219_ok) return false;
  if (millis() - motorStartedAt < CURRENT_CHECK_DELAY_MS) return false;
  return ina219.getCurrent_mA() > threshold;
}

// Avance la machine de calibration une fois la butée atteinte
void calNextStep() {
  unsigned long elapsed = millis() - motorStartedAt;

  if (calStep == 0) {
    positionPct = 0;                      // homing terminé : butée basse
    Serial.println("CALIBRATION: homing OK (pos=0)");
  } else {
    int level  = (calStep - 1) / 2;
    bool isOpen = ((calStep - 1) % 2) == 0;
    if (isOpen) { travelOpen[level] = elapsed;  positionPct = 100; }
    else        { travelClose[level] = elapsed; positionPct = 0; }
    Serial.printf("CALIBRATION: étape %d (niv %d %s) = %lu ms\n",
                  calStep, level, isOpen ? "open" : "close", elapsed);
  }

  calStep++;
  if (calStep > 6) {
    // terminé : sauvegarde
    ledcWrite(PIN_RPWM, 0); ledcWrite(PIN_LPWM, 0);
    motorState = STOPPED;
    calibrating = false;
    saveTravelPrefs();
    prefs.putInt("pos", 0);
    Serial.println("CALIBRATION: terminée et sauvegardée");
    return;
  }

  // lance l'étape suivante
  int level   = (calStep - 1) / 2;
  bool isOpen = ((calStep - 1) % 2) == 0;
  calRunStep(isOpen ? OPENING : CLOSING, speedPct[level]);
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

  // Suivi du pic de courant au démarrage
  if (ina219_ok && millis() - motorStartedAt < 800) {
    float cur = ina219.getCurrent_mA();
    if (cur > spikeCurrent_mA) spikeCurrent_mA = cur;
  }

  // --- Mode calibration ---
  if (calibrating) {
    if (currentStall(calThreshold)) {
      calNextStep();
    } else if (millis() - calStepStart > CAL_STEP_TIMEOUT_MS) {
      Serial.println("CALIBRATION: timeout étape — abandon");
      ledcWrite(PIN_RPWM, 0); ledcWrite(PIN_LPWM, 0);
      motorState = STOPPED;
      calibrating = false;
    }
    return;
  }

  // --- Mode normal ---
  updatePosition();

  if (stopAt != 0 && millis() >= stopAt) {
    positionPct = moveTargetPct;          // snap à la cible
    motorStop();
    Serial.println("Arrêt timer");
  } else if (motorState == OPENING && stopOnCurrentOpen && currentStall(thresholdOpen)) {
    if (moveTargetPct >= 100) positionPct = 100;  // butée haute
    motorStop();
    Serial.println("Arrêt courant ouverture (butée/obstacle)");
  } else if (motorState == CLOSING && stopOnCurrentClose && currentStall(thresholdClose)) {
    if (moveTargetPct <= 0) positionPct = 0;       // butée basse
    motorStop();
    Serial.println("Arrêt courant fermeture (butée/obstacle)");
  }
}
