"""Сервер-приёмник Hbox: часовые файлы с устройств с докачкой.

Протокол — docs/protocol.md. Коротко:
    GET  /hbox/v1/files/<имя>                    → {"size": принято байт, "done": файл целый,
                                                     "check": "смещение:crc,..." — CRC начала файла}
    PUT  /hbox/v1/files/<имя>?offset=N&total=T&crc=<crc32 hex>[&upto=<crc32 hex>]
         тело — байты файла с позиции N (весь остаток или кусок); при обрыве принятое
         сохраняется, устройство спрашивает GET и продолжает с size (или раньше — тогда хвост
         отбрасывается). upto — CRC начала файла по конец этого куска: не сошёлся — кусок
         выбрасывается (422, size = N). Когда принято T байт и CRC сошёлся — файл готов.
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
from starlette.responses import JSONResponse, PlainTextResponse, RedirectResponse, Response
from starlette.routing import Mount, Route

from . import admin, devices

log = logging.getLogger("hbox")

DATA = Path(os.environ.get("HBOX_DATA", "/data"))
# меньше этого свободного места — не принимать (диск сервера общий с другими проектами)
MIN_FREE_GB = float(os.environ.get("HBOX_MIN_FREE_GB", "3"))
RETENTION_DAYS = float(os.environ.get("HBOX_RETENTION_DAYS", "7"))
# недокачанный файл, к которому никто не возвращается, — удалить
PARTIAL_DAYS = 7
# контрольные точки для докачки: CRC начала файла — CHECK_SPREAD точек по всему файлу (порча бывает и
# задолго до обрыва), конец последнего проверенного куска (ok) и CHECK_TAIL точек в непроверенном хвосте
# ok..size (до 1 МБ после обрыва). Всего ≤ 14 точек: у устройства под "check" 320 байт.
CHECK_STEP = 32768
CHECK_SPREAD = 4
CHECK_TAIL = 8
PART_MAX = 1 << 20  # кусок PUT на устройстве

NAME = re.compile(r"^[A-Za-z0-9-]{1,32}_(\d{8})_(\d{6})\.(mp3|m4a)$")

# состояние устройства в запросе настроек (числа; см. firmware/main/uplink.c)
STATUS_FIELDS = ("rec", "files", "pending", "dropped", "overflows", "rec_errors", "cpu", "csq",
                 "up_errors", "sent", "kbps", "uptime", "reset", "sd_free", "sd_total",
                 "pending_mb")


# прошивки для обновления по воздуху: DATA/firmware/<crc32>.bin
FW_NAME = re.compile(r"^[0-9a-f]{8}\.bin$")
FW_VERSION = re.compile(r"^[A-Za-z0-9._-]{1,32}$")
FW_MAX = 0x1E0000  # слот прошивки (firmware/partitions.csv)
FW_BLOCK = 32768  # устройство проверяет каждый блок по своей CRC до записи во флеш


def fw_dir() -> Path:
    return DATA / "firmware"


def app_info(blob: bytes) -> dict | None:
    """Версия и проект из образа ESP-IDF (esp_app_desc_t после заголовков образа и сегмента); None — не он."""
    if len(blob) < 256 or blob[0] != 0xE9 or int.from_bytes(blob[32:36], "little") != 0xABCD5432:
        return None
    text = lambda a, b: blob[a:b].split(b"\0")[0].decode("ascii", "replace")  # noqa: E731
    return {"version": text(48, 80), "project": text(80, 112)}


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


def save_state(state: Path, size: int, crc: int, total: int, ok: int):
    """ok — докуда принятое проверено (upto куска сошёлся)"""
    tmp = state.with_suffix(".tmp")
    tmp.write_text(json.dumps({"size": size, "crc": crc, "total": total, "ok": ok}))
    tmp.replace(state)


def checkpoints(part: Path, size: int, ok: int | None = None) -> str:
    """CRC-32 начала недокачанного файла: несколько точек по всему файлу, на ok, часто в хвосте ok..size
    и на size.

    Модем изредка теряет байты, и хвост на сервере приходит испорченным. Устройство сверяет
    эти CRC со своими и продолжает с последней совпавшей точки (раньше — тогда хвост отбрасывается).
    """
    if ok is None or not 0 <= ok <= size:
        ok = max(0, size - PART_MAX)  # старое состояние без ok: хвост не длиннее куска
    offsets = {size * k // CHECK_SPREAD // CHECK_STEP * CHECK_STEP for k in range(1, CHECK_SPREAD)}
    offsets |= {ok + (size - ok) * k // (CHECK_TAIL + 1) for k in range(CHECK_TAIL + 1)}
    offsets = sorted(o for o in offsets if 0 < o < size) + ([size] if size else [])
    out, crc, pos = [], 0, 0
    with open(part, "rb") as f:
        for off in offsets:
            while pos < off:
                chunk = f.read(min(off - pos, 1 << 20))
                if not chunk:
                    return ",".join(out)
                crc = zlib.crc32(chunk, crc)
                pos += len(chunk)
            out.append(f"{off}:{crc:08x}")
    return ",".join(out)


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
    check = await asyncio.to_thread(checkpoints, part, size, st.get("ok")) if size else ""
    return JSONResponse({"size": size, "done": False, "check": check})


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
        upto = int(request.query_params["upto"], 16) if "upto" in request.query_params else None
        body_len = int(request.headers.get("content-length", -1))
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
        if offset > size:
            return error(409, "offset mismatch", size=size)
        crc = st["crc"] if size else 0
        ok = min(st.get("ok", max(0, size - PART_MAX)), size)
        if offset < size:
            # устройство знает, докуда модем точно передал; хвост после обрыва мог прийти с дырой
            log.info("%s %s: rewind %d -> %d", dev, name, size, offset)
            crc = 0
            with open(part, "rb") as f:
                left = offset
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    crc = zlib.crc32(chunk, crc)
                    left -= len(chunk)
            size = offset
            ok = min(ok, offset)
        start_crc = crc
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
            if upto is not None and size == offset + body_len and crc != upto:
                # кусок пришёл целиком, но испорченным — выбрасываем только его
                log.warning("%s %s: part %d+%d crc %08x != %08x, dropped", dev, name, offset, body_len,
                            crc, upto)
                size, crc = offset, start_crc
                with open(part, "r+b") as f:
                    f.truncate(size)
                return error(422, "part crc mismatch", size=size)
            if upto is not None and size == offset + body_len:
                ok = size
        finally:
            save_state(state, size, crc, total, ok)

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
    for k in ("fw", "fw_bad"):  # версия прошивки и та, что не запустилась (откат)
        v = request.query_params.get(k, "")
        if FW_VERSION.match(v):
            status[k] = v
    if status:
        devices.report(dev, status)
    d = devices.load()[dev]
    record, recheck = devices.recording_now(d)
    answer = {"record": record, "format": d["format"], "bitrate": d["bitrate"],
              "recheck": recheck, "channel": dev,
              "time": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")}
    ota = d.get("ota")
    if ota and ota["version"] not in (status.get("fw"), status.get("fw_bad")):
        answer.update(ota_name=ota["name"], ota_size=ota["size"], ota_crc=f"{ota['crc']:08x}",
                      ota_version=ota["version"])
    return JSONResponse(answer)


async def firmware_get(request: Request):
    """Прошивка с offset: ?offset=&len= (устройство берёт всё сразу, после обрыва — остаток);
    ?blocks=1 — CRC-32 каждого блока FW_BLOCK через запятую."""
    if not device_of(request):
        return error(401, "unknown device")
    name = request.path_params["name"]
    path = fw_dir() / name
    if not FW_NAME.match(name) or not path.is_file():
        return error(404, "no such firmware")
    if request.query_params.get("blocks"):
        blob = path.read_bytes()
        crcs = (f"{zlib.crc32(blob[i:i + FW_BLOCK]):08x}" for i in range(0, len(blob), FW_BLOCK))
        return PlainTextResponse(",".join(crcs))
    try:
        offset = int(request.query_params["offset"])
        length = int(request.query_params["len"])
    except (KeyError, ValueError):
        return error(400, "need offset, len")
    if offset < 0 or not 0 < length <= FW_MAX:
        return error(400, "bad range")
    with open(path, "rb") as f:
        f.seek(offset)
        data = f.read(length)
    return Response(data, media_type="application/octet-stream")


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
        Route("/hbox/v1/firmware/{name}", firmware_get, methods=["GET"]),
        Route("/hbox/speedtest", speedtest, methods=["POST"]),
        Mount("/hbox/admin", routes=admin.routes),
        # https://hbox.fabtech.uz/ — сразу на сайт
        Route("/", lambda request: RedirectResponse(admin.BASE + "/")),
    ],
    lifespan=lifespan,
)
