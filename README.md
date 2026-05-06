# Volet Roulant Domotique

Contrôle WiFi d'un store via ESP32 + TB6612FNG. Interface web hébergée en Docker, programmation horaire incluse.

---

## Matériel

| Composant | Référence |
|-----------|-----------|
| Microcontrôleur | ESP32 DevKitC (USB-C, CH340C ou CP2102) |
| Driver moteur | TB6612FNG |
| Capteur courant | INA219 (I2C) |
| Moteur | ZGY370 / JGY370 DC 12V 15 tr/min |
| Breadboard | MB-102 + module alimentation HW131 |
| Alimentation | Adaptateur DC 12V (courant ≥ 1A) |

---

## Câblage — Tableaux exhaustifs

Vérifier **chaque pin** de chaque composant avant de mettre sous tension.

---

### Module alimentation HW131 (MB-102)

| Pin HW131 | Branchement |
|-----------|-------------|
| IN+ | + adaptateur 12V DC |
| IN− | − adaptateur 12V DC (GND) |
| Rail 3.3V | → ESP32 3V3, TB6612 VCC, INA219 VCC |
| Rail GND | → GND commun (ESP32, TB6612, INA219, alim 12V −) |

> Jumper HW131 sur **3.3V** (pas 5V).

---

### INA219 (capteur de courant — 4 pins signal + 2 pins mesure)

| Pin INA219 | Branchement |
|------------|-------------|
| VCC | Rail **3.3V** (HW131) |
| GND | Rail **GND** commun |
| SDA | ESP32 **D21** (GPIO21) |
| SCL | ESP32 **D22** (GPIO22) |
| VIN+ | **+12V** depuis adaptateur (avant TB6612) |
| VIN− | **VM** du TB6612 |

> L'INA219 est en série sur la ligne 12V → mesure le courant moteur.

---

### TB6612FNG (driver moteur — tous les pins)

| Pin TB6612FNG | Branchement |
|---------------|-------------|
| VM | **+12V** depuis INA219 VIN− |
| VCC | Rail **3.3V** (HW131) |
| GND | Rail **GND** commun |
| STBY | ESP32 **D25** (GPIO25) |
| AIN1 | ESP32 **D26** (GPIO26) |
| AIN2 | ESP32 **D27** (GPIO27) |
| PWMA | ESP32 **D14** (GPIO14) |
| AOUT1 | Moteur **fil 1** |
| AOUT2 | Moteur **fil 2** |
| BIN1 | **non connecté** (canal B inutilisé) |
| BIN2 | **non connecté** |
| PWMB | **non connecté** |
| BOUT1 | **non connecté** |
| BOUT2 | **non connecté** |

> Si le moteur tourne dans le mauvais sens : inverser AOUT1 ↔ AOUT2.

---

### ESP32 DevKitC (récapitulatif de tous les pins utilisés)

| Pin ESP32 | Rôle | Branché sur |
|-----------|------|-------------|
| 3V3 | Alimentation logique | Rail 3.3V HW131, TB6612 VCC, INA219 VCC |
| GND | Masse | Rail GND commun |
| D14 (GPIO14) | PWM vitesse moteur | TB6612 PWMA |
| D21 (GPIO21) | I2C SDA | INA219 SDA |
| D22 (GPIO22) | I2C SCL | INA219 SCL |
| D25 (GPIO25) | Standby driver | TB6612 STBY |
| D26 (GPIO26) | Direction moteur A | TB6612 AIN1 |
| D27 (GPIO27) | Direction moteur B | TB6612 AIN2 |
| Tous autres pins | **non connectés** | — |

---

### Moteur ZGY370 / JGY370

| Fil moteur | Branchement |
|------------|-------------|
| Fil 1 (ex: rouge) | TB6612 **AOUT1** |
| Fil 2 (ex: noir) | TB6612 **AOUT2** |

---

### Schéma de flux 12V (ligne de puissance)

```
Adaptateur 12V (+)
    └── HW131 IN+
    └── INA219 VIN+
              └── INA219 VIN−
                        └── TB6612 VM
                                  └── (interne H-bridge)
                                            ├── AOUT1 → Moteur fil 1
                                            └── AOUT2 → Moteur fil 2

Adaptateur 12V (−) → GND commun ← ESP32 GND ← TB6612 GND ← INA219 GND
```

---

## Firmware ESP32 — Installation pas à pas

### 1. Installer Arduino IDE

Télécharger **Arduino IDE 2.x** : https://www.arduino.cc/en/software

### 2. Ajouter le support ESP32

1. **Fichier → Préférences**
2. Champ "URL de gestionnaire de cartes supplémentaires" → coller :
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Outils → Type de carte → Gestionnaire de cartes**
4. Chercher `esp32` → installer **"esp32 by Espressif Systems"** (version 3.x)

### 3. Installer les bibliothèques

**Outils → Gérer les bibliothèques** — chercher et installer :

| Bibliothèque | Auteur |
|---|---|
| `ArduinoJson` | Benoit Blanchon |
| `Adafruit INA219` | Adafruit |

Accepter les dépendances si proposé.

### 4. Installer le driver USB

Brancher l'ESP32 en USB-C. Ouvrir le **Gestionnaire de périphériques** Windows.

- Si le chip affiché est **CP2102** → installer driver Silicon Labs :
  `https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers`
  → télécharger "CP210x Windows Drivers" → `CP210xVCPInstaller_x64.exe` → redémarrer

- Si le chip affiché est **CH340** → installer driver CH340 :
  chercher "CH340 driver" → site `wch-ic.com` → redémarrer

Après redémarrage, l'ESP32 doit apparaître sous **Ports (COM et LPT)** avec un numéro COM (ex: COM5).

### 5. Configurer le firmware

Ouvrir `esp32/firmware/firmware.ino` dans Arduino IDE.

Modifier :
```cpp
const char* WIFI_SSID     = "MON_RESEAU_WIFI";
const char* WIFI_PASSWORD = "MON_MOT_DE_PASSE";

// Durée en ms pour ouvrir/fermer complètement (mesurer à la main)
const int TRAVEL_TIME_DEFAULT = 15000;
```

### 6. Sélectionner la carte et le port

- **Outils → Type de carte → esp32 → ESP32 Dev Module**
- **Outils → Port → COMx** (le port apparu à l'étape 4)

### 7. Uploader

Cliquer **→ Upload**. Si erreur `Failed to connect` :
1. Maintenir le bouton **BOOT** sur l'ESP32
2. Cliquer Upload
3. Relâcher BOOT quand "Connecting..." apparaît

### 8. Récupérer l'IP

**Outils → Moniteur Série → 115200 bauds** (menu en bas à droite, scroller jusqu'à 115200)

Appuyer sur le bouton **EN** (reset) sur l'ESP32 :
```
WiFi principal..........
IP: 192.168.1.XXX
Serveur HTTP démarré
```

### 9. Vérifier

Dans le navigateur :
```
http://192.168.1.XXX/api/status
```
Réponse attendue :
```json
{"state":"stopped","ip":"192.168.1.XXX","rssi":-65,"travel_time_ms":15000}
```

---

## Interface Web — Installation Docker / Portainer

### Option A — Portainer (Web editor, copier-coller)

Portainer → Stacks → Add stack → **Web editor** → coller :

```yaml
services:
  volet:
    build:
      context: https://github.com/Balou866/Domo-store.git#main:app
    container_name: volet-roulant
    ports:
      - "9090:9090"
    environment:
      - ESP32_URL=http://192.168.1.XXX   # ← IP notée à l'étape 8
      - TZ=Europe/Paris
    volumes:
      - volet_data:/app/data
    restart: unless-stopped

volumes:
  volet_data:
```

Docker clone le repo GitHub et build l'image automatiquement. Aucune CLI nécessaire.

### Option B — depuis les sources (CLI)

```bash
git clone https://github.com/Balou866/Domo-store.git
cd Domo-store
# Éditer ESP32_URL dans docker-compose.yml
docker compose up -d --build
```

### Accès

```
http://<IP_DU_SERVEUR>:9090
```

---

## Fonctionnalités

| Fonction | Description |
|----------|-------------|
| Contrôle manuel | Boutons Ouvrir / Stop / Fermer |
| Programmation récurrente | Tous les jours, semaine, week-end, jour précis |
| Programmation one-shot | "Une seule fois" à une date donnée |
| Configuration à chaud | Travel time + WiFi secondaire via l'UI (sans reflasher) |
| WiFi secondaire | Fallback automatique si le WiFi principal est absent |
| Courant / tension | Affiché en temps réel si INA219 câblé |
| Persistance | Programmes et config sauvegardés (Docker volume + NVS ESP32) |

---

## API ESP32 (accès direct)

| Méthode | Endpoint | Description |
|---------|----------|-------------|
| GET | `/api/status` | État moteur, IP, RSSI, courant INA219 |
| GET | `/api/open` | Ouvrir le store |
| GET | `/api/close` | Fermer le store |
| GET | `/api/stop` | Arrêt immédiat |
| GET | `/api/config` | Lire la configuration actuelle |
| POST | `/api/config` | Modifier travel_time_ms, ssid2, password2 |

Exemple modification travel time :
```bash
curl -X POST http://192.168.1.XXX/api/config \
  -H "Content-Type: application/json" \
  -d '{"travel_time_ms": 20000}'
```

---

## Dépannage

| Symptôme | Cause probable | Solution |
|----------|---------------|----------|
| « ESP32 hors ligne » dans l'UI | IP incorrecte | Vérifier `ESP32_URL` dans docker-compose |
| Moteur tourne dans le mauvais sens | AOUT1/AOUT2 inversés | Échanger les deux fils moteur |
| Moteur ne tourne pas | STBY non câblé | Vérifier D25 → TB6612 STBY |
| Volet ne s'arrête pas | `travel_time_ms` trop grand | Modifier via l'UI → Configuration |
| Upload Arduino échoue | Mauvais port ou timing | Maintenir BOOT pendant l'upload |
| Aucun port COM visible | Driver USB absent | Installer CP2102 ou CH340 selon la puce |
| WiFi ne connecte pas | SSID/password incorrect | Vérifier dans le firmware, reflasher |
| Programmes perdus au redémarrage | Volume Docker absent | Vérifier bloc `volumes:` dans docker-compose |

---

## Architecture

```
┌─────────────────────────────┐
│  Navigateur (port 8080)     │
│  HTML/JS vanilla            │
└──────────┬──────────────────┘
           │ HTTP REST
┌──────────▼──────────────────┐
│  FastAPI + APScheduler      │  Docker
│  /api/control/{action}      │
│  /api/schedules (CRUD)      │
│  /api/esp32/config (proxy)  │
│  /api/status (proxy ESP32)  │
└──────────┬──────────────────┘
           │ HTTP (WiFi local)
┌──────────▼──────────────────┐
│  ESP32 WebServer            │
│  /api/open|close|stop       │
│  /api/status                │
│  /api/config (GET/POST)     │
└──────────┬──────────────────┘
           │ GPIO PWM + I2C
┌──────────▼──────────────────┐
│  TB6612FNG → Moteur 12V     │
│  INA219 → mesure courant    │
└─────────────────────────────┘
```
