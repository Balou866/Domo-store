import json
import os
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from apscheduler.schedulers.asyncio import AsyncIOScheduler
from apscheduler.triggers.cron import CronTrigger
from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

ESP32_URL = os.getenv("ESP32_URL", "http://192.168.1.100")
DATA_FILE = Path("/app/data/schedules.json")
scheduler = AsyncIOScheduler(timezone="Europe/Paris")


def load_schedules() -> list:
    try:
        return json.loads(DATA_FILE.read_text()) if DATA_FILE.exists() else []
    except Exception:
        return []


def save_schedules(schedules: list):
    DATA_FILE.parent.mkdir(parents=True, exist_ok=True)
    DATA_FILE.write_text(json.dumps(schedules, indent=2))


async def send_command(action: str):
    async with httpx.AsyncClient(timeout=5.0) as client:
        r = await client.get(f"{ESP32_URL}/api/{action}")
        r.raise_for_status()


def rebuild_jobs(schedules: list):
    scheduler.remove_all_jobs()
    for s in schedules:
        if not s.get("enabled", True):
            continue
        days = s.get("days", "*")
        scheduler.add_job(
            send_command,
            CronTrigger(hour=s["hour"], minute=s["minute"], day_of_week=days),
            args=[s["action"]],
            id=s["id"],
            replace_existing=True,
        )


@asynccontextmanager
async def lifespan(app: FastAPI):
    rebuild_jobs(load_schedules())
    scheduler.start()
    yield
    scheduler.shutdown()


app = FastAPI(lifespan=lifespan)


class Schedule(BaseModel):
    id: str | None = None
    action: str        # "open" | "close"
    hour: int
    minute: int
    days: str = "*"   # format cron : "mon,fri" ou "*" pour tous les jours
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
    schedules.append(s.model_dump())
    save_schedules(schedules)
    rebuild_jobs(schedules)
    return {"ok": True, "id": s.id}


@app.delete("/api/schedules/{sid}")
async def delete_schedule(sid: str):
    schedules = [x for x in load_schedules() if x["id"] != sid]
    save_schedules(schedules)
    rebuild_jobs(schedules)
    return {"ok": True}


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
