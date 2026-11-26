"""Живой поток устройства: MP3 по ссылке с ключом, из файла, который устройство досылает во время записи.

Устройство шлёт текущий часовой файл кусками каждые несколько секунд (PUT без total — app.py), он лежит
в DATA/.partial/<устройство>/<имя>.part и растёт. Поток отдаёт его хвост по мере роста; когда начинается
новый час — переходит на следующий файл. MP3 без заголовков (shine) — кадры просто идут подряд.

    GET /hbox/v1/stream/<устройство>.mp3?key=<ключ>   (или Authorization: Bearer <ключ>)
    → 200 audio/mpeg, поток не кончается, пока идут данные
    → 401 неверный ключ, 503 устройство сейчас не присылает звук

Ключ — devices[<устройство>]["stream_key"], выдаётся и меняется на сайте.
"""

import asyncio
import secrets
import time
from pathlib import Path

from starlette.requests import Request
from starlette.responses import JSONResponse, StreamingResponse

from . import devices

# начать поток чуть раньше края — плеер сразу получает звук, а не ждёт следующего куска
START_BACK = 96 * 1024
# столько без новых байт — устройство пропало, закрываем
IDLE_CLOSE_S = 90
# на подключении: последний кусок не старше — устройство в эфире
FRESH_S = 60
# текущий файл не растёт столько, а следующий уже есть — переходим (хвост дойдёт позже обычной отправкой)
SWITCH_S = 20
POLL_S = 1.0
READ_MAX = 64 * 1024

# MPEG-1 / MPEG-2 Layer III
_KBPS = ((0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0),
         (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0))
_RATES = {3: (44100, 48000, 32000), 2: (22050, 24000, 16000), 0: (11025, 12000, 8000)}


def frame_len(b: bytes, i: int) -> int:
    """длина кадра MP3 Layer III с позиции i или 0, если там не заголовок"""
    if i + 4 > len(b) or b[i] != 0xFF or b[i + 1] & 0xE0 != 0xE0:
        return 0
    ver, layer = b[i + 1] >> 3 & 3, b[i + 1] >> 1 & 3
    br, sr, pad = b[i + 2] >> 4, b[i + 2] >> 2 & 3, b[i + 2] >> 1 & 1
    if ver == 1 or layer != 1 or sr == 3 or not _KBPS[0 if ver == 3 else 1][br]:
        return 0
    kbps = _KBPS[0 if ver == 3 else 1][br]
    return (144 if ver == 3 else 72) * kbps * 1000 // _RATES[ver][sr] + pad


def sync(b: bytes) -> int:
    """позиция первого кадра, за которым сразу идёт ещё один (не случайные 0xFF), или -1"""
    for i in range(len(b) - 4):
        n = frame_len(b, i)
        if n and (i + n + 4 > len(b) or frame_len(b, i + n)):
            return i
    return -1


def _live_files(dev: str, data: Path) -> list[Path]:
    """недокачанные mp3 устройства по времени начала (имя содержит время)"""
    d = data / ".partial" / dev
    return sorted(d.glob("*.mp3.part")) if d.is_dir() else []


def _final_of(part: Path, data: Path, dev: str) -> Path:
    name = part.name[:-5]
    day = name.rsplit("_", 2)[1]
    return data / dev / f"{day[:4]}-{day[4:6]}-{day[6:]}" / name


def _size(p: Path) -> int:
    try:
        return p.stat().st_size
    except FileNotFoundError:
        return -1


def _read(p: Path, pos: int, n: int) -> bytes:
    try:
        with open(p, "rb") as f:
            f.seek(pos)
            return f.read(n)
    except FileNotFoundError:
        return b""


def key_of(request: Request) -> str:
    auth = request.headers.get("authorization", "")
    if auth.startswith("Bearer "):
        return auth.removeprefix("Bearer ").strip()
    return request.query_params.get("key", "")


def new_key() -> str:
    return secrets.token_urlsafe(24)


async def chunks(dev: str, data: Path):
    """байты потока: с края текущего файла, дальше по мере прихода и по следующим файлам"""
    files = _live_files(dev, data)
    cur = files[-1]
    size = _size(cur)
    pos = max(0, size - START_BACK)
    aligned = False
    last_new = time.monotonic()
    while True:
        # файл дописан и перенесён в готовые — читаем оттуда
        path = cur if cur.exists() else _final_of(cur, data, dev)
        size = _size(path)
        if 0 <= size < pos:
            pos = size  # устройство откатило хвост (порча по дороге) — продолжаем с нового края
        if size > pos:
            b = await asyncio.to_thread(_read, path, pos, min(size - pos, READ_MAX))
            if not aligned:
                i = sync(b)
                if i < 0:
                    pos += max(0, len(b) - 4)
                    await asyncio.sleep(POLL_S)
                    continue
                b, pos, aligned = b[i:], pos + i, True
            # отдаём только целые кадры: хвост куска мог оборваться посреди кадра
            end = 0
            while (n := frame_len(b, end)) and end + n <= len(b):
                end += n
            if end:
                yield b[:end]
                pos += end
                last_new = time.monotonic()
                continue
            if frame_len(b, 0) == 0:
                aligned = False  # потеряли кадры — ищем заново
                continue
        newer = [p for p in _live_files(dev, data) if p.name > cur.name]
        idle = time.monotonic() - last_new
        if newer and (path != cur or idle >= SWITCH_S):
            cur, pos, aligned = newer[0], 0, False
            continue
        if idle >= IDLE_CLOSE_S:
            return
        await asyncio.sleep(POLL_S)


def make_handler(data_dir):
    async def stream(request: Request):
        file = request.path_params["file"]
        dev = file.removesuffix(".mp3")
        d = devices.load().get(dev)
        key = key_of(request)
        if not d or not d.get("stream_key") or not key or not secrets.compare_digest(d["stream_key"], key):
            return JSONResponse({"error": "bad key"}, status_code=401)
        data = data_dir()
        files = _live_files(dev, data)
        if not files or time.time() - files[-1].stat().st_mtime > FRESH_S:
            return JSONResponse({"error": "device is not streaming now"}, status_code=503)
        return StreamingResponse(chunks(dev, data), media_type="audio/mpeg",
                                 headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"})
    return stream
