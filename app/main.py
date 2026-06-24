import asyncio
import json
import logging
import os
import uuid
from contextlib import asynccontextmanager
from datetime import date, datetime
from pathlib import Path
from typing import Literal

import httpx
from apscheduler.events import EVENT_JOB_EXECUTED
from apscheduler.schedulers.asyncio import AsyncIOScheduler
from apscheduler.triggers.cron import CronTrigger
from apscheduler.triggers.date import DateTrigger
from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

ESP32_URL = os.getenv("ESP32_URL", "http://192.168.1.100")
DATA_FILE = Path("/app/data/schedules.json")
RETRY_FILE = Path("/app/data/retry_config.json")
scheduler = AsyncIOScheduler(timezone="Europe/Paris")


def load_json(path: Path, default):
    try:
        return json.loads(path.read_text()) if path.exists() else default
    except Exception:
        return default


def save_json(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2))


class RetryConfig(BaseModel):
    retries: int = 3
    delay: float = 5.0


retry_config = RetryConfig(**load_json(RETRY_FILE, {}))


def load_schedules() -> list:
    return load_json(DATA_FILE, [])


def save_schedules(schedules: list):
    save_json(DATA_FILE, schedules)


async def send_command(action: str, position: int | None = None):
    url = f"{ESP32_URL}/api/position?p={position}" if position is not None else f"{ESP32_URL}/api/{action}"
    async with httpx.AsyncClient(timeout=5.0) as client:
        r = await client.get(url)
        r.raise_for_status()


async def send_command_with_retry(action: str, position: int | None = None):
    retries = retry_config.retries
    delay = retry_config.delay
    label = f"position {position}%" if position is not None else action
    for attempt in range(1, retries + 1):
        try:
            await send_command(action, position)
            logging.info("Job %s OK (tentative %d/%d)", label, attempt, retries)
            return
        except Exception as e:
            logging.warning("Job %s échoué (tentative %d/%d) : %s", label, attempt, retries, e)
            if attempt < retries:
                await asyncio.sleep(delay)
    logging.error("Job %s abandonné après %d tentatives", label, retries)


def rebuild_jobs(schedules: list):
    scheduler.remove_all_jobs()
    for s in schedules:
        if not s.get("enabled", True):
            continue
        if s.get("one_time_date"):
            y, m, d = (int(x) for x in s["one_time_date"].split("-"))
            trigger = DateTrigger(
                run_date=datetime(y, m, d, s["hour"], s["minute"]),
                timezone="Europe/Paris",
            )
        else:
            trigger = CronTrigger(
                hour=s["hour"],
                minute=s["minute"],
                day_of_week=s.get("days", "*"),
                timezone="Europe/Paris",
            )
        scheduler.add_job(
            send_command_with_retry,
            trigger,
            args=[s["action"], s.get("position")],
            id=s["id"],
            replace_existing=True,
        )


def cleanup_one_time(event):
    """Supprime les programmes 'une fois' après leur exécution."""
    schedules = load_schedules()
    remaining = [s for s in schedules if not (s["id"] == event.job_id and s.get("one_time_date"))]
    if len(remaining) != len(schedules):
        save_schedules(remaining)
        logging.info("Programme one-shot %s exécuté et supprimé", event.job_id)


@asynccontextmanager
async def lifespan(app: FastAPI):
    scheduler.add_listener(cleanup_one_time, EVENT_JOB_EXECUTED)
    rebuild_jobs(load_schedules())
    scheduler.start()
    yield
    scheduler.shutdown()


app = FastAPI(lifespan=lifespan)


class Schedule(BaseModel):
    id: str | None = None
    action: Literal["open", "close"]
    hour: int
    minute: int
    days: str = "*"
    one_time_date: date | None = None
    position: int | None = None
    enabled: bool = True
    label: str = ""


@app.get("/api/control/{action}")
async def control(action: str):
    if action not in ("open", "close", "stop"):
        raise HTTPException(400, "Action invalide")
    try:
        await send_command(action)
        return {"ok": True, "action": action}
    except Exception as e:
        raise HTTPException(503, f"ESP32 inaccessible : {e}")


@app.get("/api/position/{pct}")
async def position(pct: int):
    if not 0 <= pct <= 100:
        raise HTTPException(400, "Position 0-100")
    try:
        await send_command("position", pct)
        return {"ok": True, "target": pct}
    except Exception as e:
        raise HTTPException(503, f"ESP32 inaccessible : {e}")


@app.post("/api/calibrate")
async def calibrate():
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            r = await client.post(f"{ESP32_URL}/api/calibrate")
            r.raise_for_status()
            return r.json()
    except Exception as e:
        raise HTTPException(503, f"ESP32 inaccessible : {e}")


@app.post("/api/reboot")
async def reboot():
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            r = await client.post(f"{ESP32_URL}/api/reboot")
            r.raise_for_status()
            return r.json()
    except (httpx.RemoteProtocolError, httpx.ReadError):
        return {"ok": True, "reboot": True}
    except Exception as e:
        raise HTTPException(503, f"ESP32 inaccessible : {e}")


@app.get("/api/retry-config")
async def get_retry_config():
    return retry_config


@app.post("/api/retry-config")
async def set_retry_config(payload: RetryConfig):
    retry_config.retries = payload.retries
    retry_config.delay = payload.delay
    save_json(RETRY_FILE, retry_config.model_dump())
    return retry_config


@app.get("/api/status")
async def status():
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            r = await client.get(f"{ESP32_URL}/api/status")
            return r.json()
    except Exception:
        return {"state": "offline"}


@app.get("/api/schedules")
async def get_schedules():
    return load_schedules()


@app.post("/api/schedules")
async def add_schedule(s: Schedule):
    if s.id is None:
        s.id = str(uuid.uuid4())[:8]
    schedules = [x for x in load_schedules() if x["id"] != s.id]
    schedules.append(s.model_dump(mode="json"))
    save_schedules(schedules)
    rebuild_jobs(schedules)
    return {"ok": True, "id": s.id}


@app.delete("/api/schedules/{sid}")
async def delete_schedule(sid: str):
    schedules = [x for x in load_schedules() if x["id"] != sid]
    save_schedules(schedules)
    rebuild_jobs(schedules)
    return {"ok": True}


@app.get("/api/esp32/config")
async def esp32_get_config():
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            r = await client.get(f"{ESP32_URL}/api/config")
            return r.json()
    except Exception:
        return {"error": "ESP32 inaccessible"}


@app.post("/api/esp32/config")
async def esp32_set_config(payload: dict):
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            r = await client.post(f"{ESP32_URL}/api/config", json=payload)
            r.raise_for_status()
            return r.json()
    except Exception as e:
        raise HTTPException(503, f"ESP32 inaccessible : {e}")


@app.post("/api/ota")
async def ota_update(file: UploadFile = File(...)):
    content = await file.read()
    try:
        async with httpx.AsyncClient(timeout=180.0) as client:
            r = await client.post(
                f"{ESP32_URL}/api/ota",
                files={"firmware": (file.filename, content, "application/octet-stream")},
            )
            r.raise_for_status()
            return r.json()
    except (httpx.RemoteProtocolError, httpx.ReadError):
        return {"ok": True, "reboot": True}
    except Exception as e:
        logging.error("OTA exception type=%s msg=%r", type(e).__name__, str(e))
        raise HTTPException(503, f"OTA échoué : {type(e).__name__}: {e}")


@app.patch("/api/schedules/{sid}/toggle")
async def toggle_schedule(sid: str):
    schedules = load_schedules()
    for s in schedules:
        if s["id"] == sid:
            s["enabled"] = not s.get("enabled", True)
    save_schedules(schedules)
    rebuild_jobs(schedules)
    return {"ok": True}


app.mount("/", StaticFiles(directory="/app/static", html=True), name="static")
