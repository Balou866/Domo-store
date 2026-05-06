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
Navigateur → FastAPI (port 9090) → ESP32 WebServer (port 80) → TB6612FNG → Moteur
```

**`app/main.py`** — FastAPI + APScheduler :
- Proxy toutes les commandes vers l'ESP32 (`ESP32_URL` env var). Le navigateur ne contacte jamais l'ESP32 directement.
- APScheduler gère les jobs cron (`CronTrigger`) et one-shot (`DateTrigger`), timezone `Europe/Paris`.
- Persistance des programmes dans `/app/data/schedules.json` (volume Docker). Les jobs sont reconstruits entièrement (`rebuild_jobs`) à chaque mutation.

**`app/static/index.html`** — UI vanilla HTML/JS/CSS, single-file, sans framework.

**`esp32/firmware/firmware.ino`** — Arduino/C++ :
- WiFi avec fallback sur un réseau secondaire (SSID2/pass2 en NVS via `Preferences`).
- Config persistante en NVS (`travel_time_ms`, `ssid2`, `pass2`) — modifiable à chaud via `POST /api/config` sans reflasher.
- Moteur géré par timer (`stopAt`) dans `loop()`, pas de thread.
- INA219 optionnel : si absent au démarrage, `ina219_ok = false` et mesures ignorées.

## Points clés

- Port ESP32 : **80** (WebServer interne). Port app web : **9090**.
- `TRAVEL_TIME_DEFAULT` dans le firmware = durée ms ouverture/fermeture complète. Mesurer à la main avant de régler.
- Déploiement Portainer : Stacks → Add stack → **Git Repository** → `https://github.com/Balou866/Domo-store`. Portainer clone et build directement, pas d'image externe nécessaire.
- `docker-compose.yml` utilise `build: ./app` — compatible Portainer Git Repository.
