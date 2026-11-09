"""Сервер-приёмник Hbox: часовые файлы с устройств с докачкой.

Протокол — docs/protocol.md. Коротко:
    GET  /hbox/v1/files/<имя>                    → {"size": принято байт, "done": файл целый}
    PUT  /hbox/v1/files/<имя>?offset=N&total=T&crc=<crc32 hex>
         тело — байты файла с позиции N; при обрыве принятое сохраняется, устройство
         спрашивает GET и продолжает с size. Когда принято T байт и CRC сошёлся — файл готов.
    GET  /hbox/v1/config?<состояние>             → настройки записи устройства
    POST /hbox/speedtest                         → замер скорости (тело выбрасывается)
    /hbox/admin/...                              → сайт (admin.py)

Устройство определяется по токену (Authorization: Bearer); устройства и их настройки —
devices.py (data/devices.json).
"""

import asyncio
import contextlib
import json
import logging
import os
import re
import shutil
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

from starlette.applications import Starlette
from starlette.requests import ClientDisconnect, Request
from starlette.responses import JSONResponse, PlainTextResponse
from starlette.routing import Mount, Route

from . import admin, devices

log = logging.getLogger("hbox")

DATA = Path(os.environ.get("HBOX_DATA", "/data"))
# меньше этого свободного места — не принимать (диск сервера общий с другими проектами)
MIN_FREE_GB = float(os.environ.get("HBOX_MIN_FREE_GB", "3"))
RETENTION_DAYS = float(os.environ.get("HBOX_RETENTION_DAYS", "30"))
# недокачанный файл, к которому никто не возвращается, — удалить
PARTIAL_DAYS = 7

NAME = re.compile(r"^[A-Za-z0-9-]{1,32}_(\d{8})_(\d{6})\.(mp3|m4a)$")

# состояние устройства в запросе настроек (числа; см. firmware/main/uplink.c)
STATUS_FIELDS = ("rec", "files", "pending", "dropped", "overflows", "rec_errors", "cpu", "csq",
                 "up_errors", "sent", "kbps", "uptime")


def device_of(request: Request) -> str | None:
    auth = request.headers.get("authorization", "")
    return devices.by_token(auth.removeprefix("Bearer ").strip()) if auth.startswith("Bearer ") else None


def free_gb() -> float:
    return shutil.disk_usage(DATA).free / 1e9


def paths(dev: str, name: str) -> tuple[Path, Path, Path]:
    """готовый файл, недокачанный, состояние недокачанного"""
    day = NAME.match(name).group(1)
    final = DATA / dev / f"{day[:4]}-{day[4:6]}-{day[6:]}" / name
    part = DATA / ".partial" / dev / (name + ".part")
    return final, part, part.with_suffix(".json")


def load_state(state: Path) -> dict:
    try:
        return json.loads(state.read_text())
    except (FileNotFoundError, ValueError):
        return {"size": 0, "crc": 0}


def save_state(state: Path, size: int, crc: int, total: int):
    tmp = state.with_suffix(".tmp")
    tmp.write_text(json.dumps({"size": size, "crc": crc, "total": total}))
    tmp.replace(state)


def error(status: int, message: str, **extra) -> JSONResponse:
    return JSONResponse({"error": message, **extra}, status_code=status)


# Одна загрузка одного файла за раз: повторный PUT после обрыва ждёт, пока старый допишет.
_locks: dict[tuple[str, str], asyncio.Lock] = {}


async def file_status(request: Request):
    dev = device_of(request)
    if not dev:
        return error(401, "unknown device")
    name = request.path_params["name"]
    if not NAME.match(name):
        return error(400, "bad name")
    final, part, state = paths(dev, name)
    if final.exists():
        return JSONResponse({"size": final.stat().st_size, "done": True})
    st = load_state(state)
    # размер по файлу: состояние могло не успеть записаться при обрыве
    size = min(st["size"], part.stat().st_size) if part.exists() else 0
    return JSONResponse({"size": size, "done": False})


async def file_put(request: Request):
    dev = device_of(request)
    if not dev:
        return error(401, "unknown device")
    name = request.path_params["name"]
    if not NAME.match(name):
        return error(400, "bad name")
    try:
        offset = int(request.query_params["offset"])
        total = int(request.query_params["total"])
        want_crc = int(request.query_params["crc"], 16)
    except (KeyError, ValueError):
        return error(400, "need offset, total, crc")
    if free_gb() < MIN_FREE_GB:
        return error(507, "server disk full")

    final, part, state = paths(dev, name)
    async with _locks.setdefault((dev, name), asyncio.Lock()):
        if final.exists():
            return JSONResponse({"size": final.stat().st_size, "done": True})
        part.parent.mkdir(parents=True, exist_ok=True)
        st = load_state(state)
        size = min(st["size"], part.stat().st_size) if part.exists() else 0
        if offset != size:
            return error(409, "offset mismatch", size=size)
        crc = st["crc"] if size else 0
        try:
            with open(part, "r+b" if part.exists() else "wb") as f:
                f.truncate(size)
                f.seek(size)
                try:
                    async for chunk in request.stream():
                        if size + len(chunk) > total:
                            return error(400, "more than total", size=size)
                        f.write(chunk)
                        crc = zlib.crc32(chunk, crc)
                        size += len(chunk)
                except ClientDisconnect:
                    log.info("%s %s: disconnected at %d of %d", dev, name, size, total)
                f.flush()
                os.fsync(f.fileno())
        finally:
            save_state(state, size, crc, total)

        if size < total:
            return JSONResponse({"size": size, "done": False})
        if crc != want_crc:
            # испорчено где-то по дороге — заново с нуля
            part.unlink()
            state.unlink()
            log.warning("%s %s: crc %08x != %08x, dropped", dev, name, crc, want_crc)
            return error(422, "crc mismatch", size=0)
        final.parent.mkdir(parents=True, exist_ok=True)
        part.replace(final)
        state.unlink()
        log.info("%s %s: done, %d bytes", dev, name, size)
        return JSONResponse({"size": size, "done": True})


async def config(request: Request):
    dev = device_of(request)
    if not dev:
        return error(401, "unknown device")
    status = {}
    for k in STATUS_FIELDS:
        try:
            status[k] = int(request.query_params[k])
        except (KeyError, ValueError):
            pass
    if status:
        devices.report(dev, status)
    d = devices.load()[dev]
    record, recheck = devices.recording_now(d)
    return JSONResponse({"record": record, "format": d["format"], "bitrate": d["bitrate"],
                         "recheck": recheck, "channel": dev,
                         "time": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")})


async def speedtest(request: Request):
    start = time.monotonic()
    total = 0
    try:
        async for chunk in request.stream():
            total += len(chunk)
    except ClientDisconnect:
        pass
    sec = time.monotonic() - start
    result = f"{total} B in {sec:.1f} s = {total / max(sec, 1e-3) / 1024:.1f} KB/s"
    log.info("speedtest %s %s", request.headers.get("x-real-ip", "?"), result)
    return PlainTextResponse(result + "\n")


def cleanup(now: float | None = None):
    """Удаляет старые файлы и брошенные докачки; при нехватке места — самые старые готовые."""
    now = now or time.time()
    if not DATA.exists():
        return
    done = sorted((p for p in DATA.glob("*/*/*") if p.is_file() and NAME.match(p.name)),
                  key=lambda p: p.stat().st_mtime)
    for p in list(done):
        if now - p.stat().st_mtime > RETENTION_DAYS * 86400:
            log.info("cleanup: %s older than %g days", p, RETENTION_DAYS)
            p.unlink()
            done.remove(p)
    for p in (DATA / ".partial").glob("*/*"):
        if now - p.stat().st_mtime > PARTIAL_DAYS * 86400:
            log.info("cleanup: stale partial %s", p)
            p.unlink()
    # запас +1 ГБ над порогом приёма, чтобы приём не вставал
    while done and free_gb() < MIN_FREE_GB + 1:
        p = done.pop(0)
        log.warning("cleanup: low disk, removing oldest %s", p)
        p.unlink()


async def cleanup_loop():
    while True:
        try:
            await asyncio.to_thread(cleanup)
        except Exception:
            log.exception("cleanup failed")
        await asyncio.sleep(600)


@contextlib.asynccontextmanager
async def lifespan(app):
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    DATA.mkdir(parents=True, exist_ok=True)
    task = asyncio.create_task(cleanup_loop())
    yield
    task.cancel()


app = Starlette(
    routes=[
        Route("/hbox/v1/files/{name}", file_status, methods=["GET"]),
        Route("/hbox/v1/files/{name}", file_put, methods=["PUT"]),
        Route("/hbox/v1/config", config, methods=["GET"]),
        Route("/hbox/speedtest", speedtest, methods=["POST"]),
        Mount("/hbox/admin", routes=admin.routes),
    ],
    lifespan=lifespan,
)
