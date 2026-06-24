# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

### Web app (Docker)
```bash
# Prod — build depuis les sources (port 9090)
docker compose up -d --build

# Dev local sans Docker (port 9090)
cd app
pip install -r requirements.txt
ESP32_URL=http://192.168.1.XXX uvicorn main:app --reload --port 9090
```

### Firmware ESP32
Ouvrir `esp32/firmware/firmware.ino` dans Arduino IDE. Configurer SSID/password + `TRAVEL_TIME_DEFAULT`, puis Upload via **ESP32 Dev Module**.

## Architecture

```
Navigateur → FastAPI (port 9090) → ESP32 WebServer (port 80) → BTS7960 (IBT-2) → Moteur
```

**`app/main.py`** — FastAPI + APScheduler :
- Proxy toutes les commandes vers l'ESP32 (`ESP32_URL` env var). **Exception : OTA firmware** — l'upload se fait navigateur → ESP32 directement (POST vers l'IP de l'ESP32 depuis le browser) pour éviter de faire transiter de gros binaires par le backend.
- APScheduler gère les jobs cron (`CronTrigger`) et one-shot (`DateTrigger`), timezone `Europe/Paris`.
- Persistance des programmes dans `/app/data/schedules.json` (volume Docker). Les jobs sont reconstruits entièrement (`rebuild_jobs`) à chaque mutation.
- `send_command_with_retry` : mécanisme de retry pour les tâches planifiées. Nombre de tentatives et délai configurables via UI et `POST /api/retry-config`.
- Timeout `httpx.AsyncClient` : **180s** (nécessaire pour les gros transfers OTA).

**`app/static/index.html`** — UI vanilla HTML/JS/CSS, single-file, sans framework.

**`esp32/firmware/firmware.ino`** — Arduino/C++ :
- WiFi : SSID/password principaux codés en dur (`WIFI_SSID`/`WIFI_PASSWORD`) — accès par IP sur le réseau local, donc forcément fixés au flash. Reconnexion auto si perte (`loop()`).
- Config persistante en NVS (`speed_pct[3]`, `speed_level`, `travel_open[3]`, `travel_close[3]`, `position`, `soft_start`, `cal_threshold`, `stop_on_cur_open`, `stop_on_cur_close`, `threshold_open`, `threshold_close`) — modifiable à chaud via `POST /api/config` sans reflasher.
- Moteur géré par timer (`stopAt`) dans `loop()`, pas de thread.
- **Position en % (0=fermé, 100=ouvert)** : suivie par le temps écoulé pendant le mouvement, persistée en NVS. `GET /api/position?p=NN` vise une position cible. `open`/`close` = `goto 100`/`0`.
- **3 vitesses** (lente/moyenne/rapide = `speed_pct[0..2]`), `speed_level` = niveau actif. Les temps de course (`travel_open/close`) sont stockés par vitesse car le RPM change le temps de parcours.
- **Calibration auto** (`POST /api/calibrate`, nécessite INA219) : homing + 3×(montée+fermeture) jusqu'aux butées (pic courant > `cal_threshold`), mesure et sauve les 6 temps. Timeout sécurité 120 s/étape.
- **Soft-start** optionnel (`soft_start`) : rampe PWM sur 500 ms au démarrage pour réduire le pic de courant.
- INA219 optionnel : si absent au démarrage, `ina219_ok = false` et mesures ignorées (calibration indisponible).
- Arrêt par seuil de courant (INA219) : si `stop_on_cur_open/close` activé, arrêt quand le courant dépasse `threshold_open/close` (mA) — détection butée/obstacle, recale la position aux extrêmes. `POST /api/reboot` redémarre l'ESP32. Firmware **v1.4.0**.
- Driver moteur : **BTS7960 (IBT-2)**. Pins : RPWM=GPIO14 (ouverture), LPWM=GPIO27 (fermeture). R_EN/L_EN câblés au 5V (toujours actifs). Remplace TB6612FNG grillé.

## Points clés

- Port ESP32 : **80** (WebServer interne). Port app web : **9090**.
- `TRAVEL_TIME_DEFAULT` dans le firmware = durée ms ouverture/fermeture complète. Mesurer à la main avant de régler.
- Déploiement Portainer : Stacks → Add stack → **Git Repository** → `https://github.com/Balou866/Domo-store`. Portainer clone et build directement, pas d'image externe nécessaire.
- `docker-compose.yml` utilise `build: ./app` — compatible Portainer Git Repository.
