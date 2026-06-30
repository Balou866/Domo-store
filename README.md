# Volet Roulant Domotique

Contrôle WiFi d'un store via ESP32 + BTS7960 (IBT-2 43A). Interface web hébergée en Docker, programmation horaire incluse.

---

## Matériel

| Composant | Référence |
|-----------|-----------|
| Microcontrôleur | ESP32 DevKitC (USB-C, CH340C ou CP2102) |
| Driver moteur | BTS7960 / IBT-2 (43A peak) |
| Capteur courant | INA219 (I2C) |
| Moteur | 32GP-31ZY DC 12V 112 tr/min 1.3A (6.5A stall) |
| Breadboard | MB-102 + module alimentation HW131 |
| Alimentation | Adaptateur DC 12V (courant ≥ 2A) |

---

## Câblage — Tableaux exhaustifs

Vérifier **chaque pin** de chaque composant avant de mettre sous tension.

---

### Module alimentation HW131 (MB-102)

| Pin HW131 | Branchement |
|-----------|-------------|
| IN+ | + adaptateur 12V DC |
| IN− | − adaptateur 12V DC (GND) |
| Rail 3.3V | → ESP32 3V3, INA219 VCC |
| Rail 5V | → IBT-2 VCC, IBT-2 R_EN, IBT-2 L_EN |
| Rail GND | → GND commun (ESP32, IBT-2, INA219, alim 12V −) |

> HW131 a deux rails indépendants. Mettre le **rail 3.3V** (rail ESP32) et le **rail 5V** (rail IBT-2).

---

### INA219 (capteur de courant — 4 pins signal + 2 pins mesure)

| Pin INA219 | Branchement |
|------------|-------------|
| VCC | Rail **3.3V** (HW131) |
| GND | Rail **GND** commun |
| SDA | ESP32 **D21** (GPIO21) |
| SCL | ESP32 **D22** (GPIO22) |
| VIN+ | **+12V** depuis adaptateur (avant IBT-2) |
| VIN− | **B+** du BTS7960 (IBT-2) |

> L'INA219 est en série sur la ligne 12V → mesure le courant total entrant dans le driver.

---

### BTS7960 / IBT-2 (driver moteur)

| Pin IBT-2 | Branchement |
|-----------|-------------|
| VCC | Rail **5V** (HW131) |
| GND | Rail **GND** commun |
| RPWM | ESP32 **D14** (GPIO14) — PWM ouverture |
| LPWM | ESP32 **D27** (GPIO27) — PWM fermeture |
| R_EN | Rail **5V** (câblé direct — toujours actif) |
| L_EN | Rail **5V** (câblé direct — toujours actif) |
| B+ | **+12V** depuis INA219 VIN− |
| B− | Rail **GND** commun |
| M+ | Moteur **fil 1** |
| M− | Moteur **fil 2** |

> Si le moteur tourne dans le mauvais sens : inverser M+ ↔ M−.
> GPIO14/GPIO27 (3.3V) suffisent — seuil logique BTS7960 ≈ 2.4V.

---

### ESP32 DevKitC (récapitulatif de tous les pins utilisés)

| Pin ESP32 | Rôle | Branché sur |
|-----------|------|-------------|
| 3V3 | Alimentation logique (**entrée**) | Rail 3.3V HW131 *(l'INA219 VCC est sur le même rail, pas câblé directement au pin)* |
| GND | Masse | Rail GND commun |
| D14 (GPIO14) | PWM ouverture | IBT-2 RPWM |
| D21 (GPIO21) | I2C SDA | INA219 SDA |
| D22 (GPIO22) | I2C SCL | INA219 SCL |
| D27 (GPIO27) | PWM fermeture | IBT-2 LPWM |
| Tous autres pins | **non connectés** | — |

---

### Moteur 32GP-31ZY

| Fil moteur | Branchement |
|------------|-------------|
| Fil 1 (ex: rouge) | IBT-2 **M+** |
| Fil 2 (ex: noir) | IBT-2 **M−** |

---

### Schéma de flux 12V (ligne de puissance)

```
Adaptateur 12V (+)
    └── HW131 IN+
    └── INA219 VIN+
              └── INA219 VIN−
                        └── BTS7960 B+
                                  └── (interne H-bridge)
                                            ├── M+ → Moteur fil 1
                                            └── M− → Moteur fil 2

Adaptateur 12V (−) → GND commun ← ESP32 GND ← BTS7960 B− ← INA219 GND
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
{"firmware":"1.4.1","state":"stopped","ip":"192.168.1.XXX","rssi":-65,"travel_time_ms":15000,"motor_speed":100}
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
| Vitesse moteur | Slider 0-100% réglable à la volée |
| Arrêt par durée | Temps de course configurable (`travel_time_ms`) |
| Arrêt par courant | Seuil de coupure montée/descente via INA219 (butée / obstacle) |
| Démarrage progressif | Soft-start optionnel (rampe PWM) pour réduire le pic de courant |
| Programmation récurrente | Tous les jours, semaine, week-end, jour précis |
| Programmation one-shot | « Une seule fois » à une date donnée (auto-supprimée après exécution) |
| Configuration à chaud | Vitesse, durée, seuils courant via l'UI (sans reflasher) |
| Courant / tension | Affiché en temps réel si INA219 câblé |
| Persistance | Programmes et config sauvegardés (Docker volume + NVS ESP32) |

---

## Mise à jour firmware OTA (sans câble USB)

Deux méthodes disponibles après le premier flash USB :

### Méthode A — Via l'interface web

1. Arduino IDE → **Sketch → Export Compiled Binary** → récupérer le `.bin`
2. Interface web → carte **Mise à jour firmware** → sélectionner le `.bin` → **Flasher**
3. L'ESP32 se met à jour et redémarre automatiquement

### Méthode B — ArduinoOTA (Arduino IDE via WiFi)

1. **Outils → Port** → sélectionner `volet-roulant.local` (apparaît en réseau)
2. Cliquer **→ Upload** comme d'habitude — flash via WiFi

---

## API ESP32 (accès direct)

| Méthode | Endpoint | Description |
|---------|----------|-------------|
| GET | `/api/status` | État moteur, IP, RSSI, vitesse, courant INA219 |
| GET | `/api/open` | Ouvrir le store |
| GET | `/api/close` | Fermer le store |
| GET | `/api/stop` | Arrêt immédiat |
| GET | `/api/config` | Lire la configuration actuelle |
| POST | `/api/config` | Modifier vitesse, temps de course, seuils courant, soft-start |
| POST | `/api/reboot` | Redémarrer l'ESP32 |
| POST | `/api/ota` | Flash firmware OTA (multipart, champ `firmware`) |

Exemple — modifier le temps de course et la vitesse :
```bash
curl -X POST http://192.168.1.XXX/api/config \
  -H "Content-Type: application/json" \
  -d '{"travel_time_ms": 20000, "motor_speed": 80}'
```

---

## Dépannage

| Symptôme | Cause probable | Solution |
|----------|---------------|----------|
| « ESP32 hors ligne » dans l'UI | IP incorrecte | Vérifier `ESP32_URL` dans docker-compose |
| Moteur tourne dans le mauvais sens | M+/M− inversés | Échanger les deux fils moteur |
| Moteur ne démarre pas | R_EN/L_EN non câblés au 5V | Vérifier fils R_EN et L_EN → rail 5V IBT-2 |
| Volet ne s'arrête pas | `travel_time_ms` trop grand | Modifier via l'UI → Configuration |
| Arrêt courant trop tôt | Seuil mA trop bas | Augmenter le seuil de coupure montée/descente |
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
│  /api/reboot                │
│  /api/status                │
│  /api/config (GET/POST)     │
└──────────┬──────────────────┘
           │ GPIO PWM + I2C
┌──────────▼──────────────────┐
│  BTS7960 (IBT-2) → Moteur 12V  │
│  INA219 → mesure courant       │
└─────────────────────────────┘
```
