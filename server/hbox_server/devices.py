"""Устройства: токены, настройки записи, расписание и последнее состояние.

Хранятся в data/devices.json; при первом запуске устройства переносятся из HBOX_DEVICES (.env).
Расписание — часы работы по дням недели в Asia/Almaty; сервер сам решает «писать сейчас или нет»
и говорит устройству, когда спросить снова (recheck), — устройству не нужен часовой пояс.
"""

import json
import os
import secrets
import threading
import time
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

TZ = ZoneInfo("Asia/Almaty")
DAYS = ["пн", "вт", "ср", "чт", "пт", "сб", "вс"]
FORMATS = ("mp3", "m4a")
BITRATES = (64000, 96000, 128000, 160000)
# устройство всё равно спрашивает настройки не реже раза в 5 минут
RECHECK_MAX = 300

DEFAULT = {
    "record": True,
    "format": "mp3",
    "bitrate": 128000,
    # 7 строк "ЧЧ:ММ-ЧЧ:ММ" по дням с понедельника; "" — весь день не писать; null — без расписания
    "schedule": None,
}

_lock = threading.Lock()


def _path() -> Path:
    from . import app  # путь к данным задаётся в app (подменяется в тестах)

    return app.DATA / "devices.json"


def load() -> dict:
    p = _path()
    try:
        devs = json.loads(p.read_text())
    except (FileNotFoundError, ValueError):
        devs = {}
    if not devs:
        for item in os.environ.get("HBOX_DEVICES", "").split(","):
            dev, _, token = item.strip().partition(":")
            if dev and token:
                devs[dev] = {"token": token, **DEFAULT}
    return devs


def save(devs: dict):
    p = _path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(devs, ensure_ascii=False, indent=1))
    tmp.replace(p)


def by_token(token: str) -> str | None:
    with _lock:
        for dev, d in load().items():
            if token and secrets.compare_digest(d["token"], token):
                return dev
    return None


def add(dev: str) -> str:
    with _lock:
        devs = load()
        if dev in devs:
            raise ValueError("уже есть")
        token = secrets.token_hex(16)
        devs[dev] = {"token": token, **DEFAULT}
        save(devs)
        return token


def update(dev: str, **fields):
    with _lock:
        devs = load()
        devs[dev].update(fields)
        save(devs)


def report(dev: str, status: dict):
    """Состояние, которое устройство прислало с запросом настроек."""
    with _lock:
        devs = load()
        devs[dev]["seen"] = int(time.time())
        devs[dev]["status"] = status
        save(devs)


def traffic(dev: str, nbytes: int, secs: float, now: float | None = None):
    """Принятые от устройства данные: сегодня/месяц (по Алматы), всего и время передачи сегодня —
    для средней скорости."""
    t = datetime.fromtimestamp(now or time.time(), TZ)
    day, month = t.strftime("%Y-%m-%d"), t.strftime("%Y-%m")
    with _lock:
        devs = load()
        if dev not in devs:
            return
        tr = devs[dev].setdefault("traffic", {"total": 0})
        if tr.get("day") != day:
            tr.update(day=day, today=0, today_s=0.0)
        if tr.get("month") != month:
            tr.update(month=month, this_month=0)
        tr["total"] += nbytes
        tr["today"] += nbytes
        tr["this_month"] += nbytes
        tr["today_s"] = round(tr["today_s"] + secs, 1)
        save(devs)


def parse_range(s: str) -> tuple[int, int] | None:
    """"10:00-23:30" → минуты (600, 1410); конец меньше начала — через полночь. "" → None."""
    s = s.strip()
    if not s:
        return None
    a, _, b = s.partition("-")
    ha, _, ma = a.strip().partition(":")
    hb, _, mb = b.strip().partition(":")
    start, end = int(ha) * 60 + int(ma or 0), int(hb) * 60 + int(mb or 0)
    if not (0 <= start < 1440 and 0 <= end <= 1440) or start == end:
        raise ValueError(s)
    return start, end


def _open_at(schedule: list[str], t: datetime) -> bool:
    minute = t.hour * 60 + t.minute
    today = parse_range(schedule[t.weekday()])
    yesterday = parse_range(schedule[(t.weekday() - 1) % 7])
    if today and (today[0] <= minute < today[1] or (today[1] < today[0] and minute >= today[0])):
        return True
    # вчерашний интервал через полночь
    return bool(yesterday and yesterday[1] < yesterday[0] and minute < yesterday[1])


def recording_now(d: dict, now: float | None = None) -> tuple[bool, int]:
    """Писать ли сейчас и через сколько секунд спросить снова."""
    if not d.get("record", True):
        return False, RECHECK_MAX
    schedule = d.get("schedule")
    if not schedule:
        return True, RECHECK_MAX
    t = datetime.fromtimestamp(now or time.time(), TZ).replace(second=0, microsecond=0)
    state = _open_at(schedule, t)
    # ближайшая смена в пределах RECHECK_MAX — проверяем по минутам
    for m in range(1, RECHECK_MAX // 60 + 1):
        if _open_at(schedule, t + timedelta(minutes=m)) != state:
            left = (t + timedelta(minutes=m)).timestamp() - (now or time.time())
            return state, max(5, int(left))
    return state, RECHECK_MAX
