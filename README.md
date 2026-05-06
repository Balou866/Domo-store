# Volet Roulant Domotique

Contrôle WiFi d'un store via ESP32 + TB6612FNG. Interface web hébergée en Docker, programmation horaire incluse.

---

## Matériel

| Composant | Référence |
|-----------|-----------|
| Microcontrôleur | ESP32 DevKitC |
| Driver moteur | TB6612FNG |
| Capteur courant | INA219 (I2C) — *prévu, non encore câblé dans le firmware* |
| Moteur | ZGY370 / JGY370 DC 12V 15 tr/min |
| Breadboard | MB-102 + module alimentation HW131 |

---

## Câblage

### Alimentation

```
Alimentation externe 12V DC
    ├── (+) ──────────────────────── TB6612 VM
    └── (−) ──────────────────────── GND commun (ESP32 + TB6612 + INA219)

Module HW131 (jumper sur 3.3V)
    ├── Entrée : 12V DC (ou USB 5V séparé)
    └── Sortie 3.3V → VCC ESP32, VCC TB6612, VCC INA219
```

> ⚠️ Le TB6612 a deux alimentations distinctes : **VCC** (logique 3.3V) et **VM** (moteur 12V).  
> Ne pas oublier la **masse commune** entre l'alim 12V et l'ESP32.

---

### ESP32 → TB6612FNG

| TB6612FNG | ESP32 GPIO | Rôle |
|-----------|-----------|------|
| AIN1 | GPIO **26** | Direction (sens 1) |
| AIN2 | GPIO **27** | Direction (sens 2) |
| PWMA | GPIO **14** | Vitesse PWM |
| STBY | GPIO **25** | Standby — doit être HIGH pour activer le driver |
| VCC | **3.3V** | Alimentation logique |
| GND | **GND** | Masse commune |
| VM | **+12V** | Alimentation moteur |
| AOUT1 | — | Fil moteur 1 |
| AOUT2 | — | Fil moteur 2 |

> Si le moteur tourne dans le mauvais sens, inverser AOUT1 et AOUT2.

---

### ESP32 → INA219 (capteur de courant — optionnel)

| INA219 | ESP32 GPIO |
|--------|-----------|
| SDA | GPIO **21** |
| SCL | GPIO **22** |
| VCC | **3.3V** |
| GND | **GND** |
| VIN+ | **+12V** (depuis l'alimentation) |
| VIN− | **VM** du TB6612 |

L'INA219 est placé **en série** sur la ligne 12V qui alimente le TB6612. Il mesure le courant moteur en temps réel (utile pour détecter une fin de course par surintensité).

---

### Schéma bloc

```
+12V ─────┬──────── INA219 VIN+ → VIN− ──────── TB6612 VM
          │                                          │
         GND ←──────────────────────────────────── GND
                                                     │
ESP32 ──── GPIO26/27/14/25 ──────── TB6612 AIN1/AIN2/PWMA/STBY
ESP32 ──── GPIO21/22 ─────────────── INA219 SDA/SCL
ESP32 ──── 3.3V ──────────────────── TB6612 VCC + INA219 VCC

TB6612 AOUT1 ─── Moteur (+)
TB6612 AOUT2 ─── Moteur (−)
```

---

## Firmware ESP32

### Prérequis Arduino IDE

1. **Support ESP32** : Fichier → Préférences → URL de gestionnaire supplémentaire :
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   Puis Outils → Gestionnaire de cartes → installer **esp32 by Espressif Systems**

2. **Bibliothèques** (Outils → Gérer les bibliothèques) :
   - `ArduinoJson` (Benoit Blanchon) — version ≥ 6.x

3. **Carte** : `ESP32 Dev Module`, fréquence CPU 240MHz

### Configuration

Éditer `esp32/firmware.ino` :

```cpp
const char* WIFI_SSID     = "MON_RESEAU";       // votre SSID
const char* WIFI_PASSWORD = "MON_MOT_DE_PASSE"; // votre clé WiFi

// Durée en millisecondes pour passer de fermé à ouvert (ou inversement)
// Mesurer à la main : chronométrer une ouverture complète
const int TRAVEL_TIME_MS = 15000;
```

### Flash

```
1. Brancher l'ESP32 en USB
2. Outils → Port → sélectionner le bon COM
3. Upload (Ctrl+U)
4. Moniteur Série → 115200 bauds
5. Noter l'IP affichée : "IP: 192.168.x.x"
```

---

## Interface Web — Installation Docker / Portainer

### Option A — Portainer (Stack, copier-coller)

Portainer → Stacks → Add stack → coller :

```yaml
services:
  volet:
    image: ghcr.io/balou866/domo-store:latest
    container_name: volet-roulant
    ports:
      - "8080:8080"
    environment:
      - ESP32_URL=http://192.168.1.XXX   # ← IP notée lors du flash
      - TZ=Europe/Paris
    volumes:
      - volet_data:/app/data
    restart: unless-stopped

volumes:
  volet_data:
```

### Option B — depuis les sources (si pas d'image publiée)

Sur le serveur :
```bash
git clone https://github.com/Balou866/Domo-store.git
cd Domo-store
# Éditer ESP32_URL dans docker-compose.yml
docker compose up -d --build
```

### Accès

```
http://<IP_DU_SERVEUR>:8080
```

---

## Fonctionnement

| Fonction | Description |
|----------|-------------|
| Contrôle manuel | Boutons Ouvrir / Stop / Fermer en temps réel |
| Programmation | Horaires récurrents : tous les jours, semaine, week-end, jour précis |
| Statut | Indicateur live (IP ESP32, niveau WiFi, état moteur) |
| Arrêt automatique | Le moteur s'arrête après `TRAVEL_TIME_MS` ms sans commande Stop |
| Persistance | Les programmes sont sauvegardés dans un volume Docker |

---

## Dépannage

| Symptôme | Cause probable | Solution |
|----------|---------------|----------|
| « ESP32 hors ligne » dans l'UI | IP incorrecte | Vérifier `ESP32_URL` dans docker-compose |
| Moteur tourne dans le mauvais sens | AOUT1/AOUT2 inversés | Échanger les deux fils moteur |
| Moteur ne tourne pas du tout | STBY non câblé | Vérifier GPIO25 → TB6612 STBY |
| Volet ne s'arrête pas | `TRAVEL_TIME_MS` trop grand | Réduire et reflasher |
| L'heure des programmes est décalée | Fuseau horaire manquant | Ajouter `TZ=Europe/Paris` dans docker-compose |
| Programmes perdus au redémarrage | Volume Docker absent | Vérifier le bloc `volumes:` dans docker-compose |

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
│  /api/status (proxy ESP32)  │
└──────────┬──────────────────┘
           │ HTTP GET (WiFi local)
┌──────────▼──────────────────┐
│  ESP32 WebServer            │
│  /api/open|close|stop       │
│  /api/status                │
└──────────┬──────────────────┘
           │ GPIO PWM
┌──────────▼──────────────────┐
│  TB6612FNG → Moteur 12V     │
└─────────────────────────────┘
```
