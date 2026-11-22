"""Устройства: токены, настройки записи, расписание и последнее состояние.

Хранятся в data/devices.json; при первом запуске устройства переносятся из HBOX_DEVICES (.env).
Расписание — часы работы по дням недели в Asia/Almaty; сервер сам решает «писать сейчас или нет»
и говорит устройству, когда спросить снова (recheck), — устройству не нужен часовой пояс.
"""

import json
import os
import re
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
# путь до сервера (firmware/main/uplink.c); без значения — устройство выбирает само
ROUTES = {"direct": "напрямую (18443)", "cf": "через Cloudflare (443)"}
# устройство всё равно спрашивает настройки не реже раза в 5 минут
RECHECK_MAX = 300
# запуски устройства (по uptime) — для отчёта по часам: где пропуск из-за питания
BOOTS_KEPT = 200

DEFAULT = {
    "record": True,
    "format": "mp3",
    "bitrate": 128000,
    # 7 строк "ЧЧ:ММ-ЧЧ:ММ" по дням с понедельника; "" — весь день не писать; null — без расписания
    "schedule": None,
}

_lock = threading.Lock()

# регистрация новой платы без токена: она шлёт IMEI модема и MAC ESP, на сайте её принимают под именем,
# и при следующем запросе плата получает токен. Заявки — data/register.json.
IMEI = re.compile(r"^\d{14,20}$")  # ML307R на AT+CGSN отдаёт серийный номер (19 цифр), IMEI — AT+CGSN=1
MAC = re.compile(r"^[0-9a-f]{12}$")
REGISTER_KEPT = 30  # непринятых заявок — не больше (кто-то шлёт мусор)


def _path() -> Path:
    from . import app  # путь к данным задаётся в app (подменяется в тестах)

    return app.DATA / "devices.json"


def _reg_path() -> Path:
    from . import app

    return app.DATA / "register.json"


def requests() -> dict:
    try:
        return json.loads(_reg_path().read_text())
    except (FileNotFoundError, ValueError):
        return {}


def _reg_save(regs: dict):
    p = _reg_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(regs, ensure_ascii=False, indent=1))
    tmp.replace(p)


def register(imei: str, mac: str, fw: str, now: float | None = None) -> str | None:
    """Заявка платы; токен — когда её приняли на сайте (и плата та же: MAC совпал)."""
    now = now or time.time()
    with _lock:
        regs = requests()
        r = regs.get(imei)
        if r and r.get("dev"):
            if r["mac"] != mac:
                return None
            r["seen"] = now
            _reg_save(regs)
            d = load().get(r["dev"])
            return d["token"] if d else None
        old = next((n for n, d in load().items() if d.get("imei") == imei), None)
        regs[imei] = {"mac": mac, "fw": fw, "first": (r or {}).get("first", now), "seen": now, "was": old}
        waiting = sorted((k for k in regs if not regs[k].get("dev")), key=lambda k: regs[k]["seen"])
        for k in waiting[:-REGISTER_KEPT]:
            del regs[k]
        _reg_save(regs)
        return None


def approve(imei: str, name: str):
    """Принять заявку под именем: новое устройство или прежнее с этим IMEI (новый токен)."""
    with _lock:
        regs, devs = requests(), load()
        r = regs.get(imei)
        if not r:
            raise ValueError("нет заявки")
        if name in devs and devs[name].get("imei") != imei:
            raise ValueError(f"{name} уже есть")
        d = devs.get(name) or dict(DEFAULT)
        d.update(token=secrets.token_hex(16), imei=imei, mac=r["mac"])
        devs[name] = d
        r["dev"] = name
        save(devs)
        _reg_save(regs)


def reject(imei: str):
    with _lock:
        regs = requests()
        if regs.pop(imei, None) is not None:
            _reg_save(regs)


def claimed(dev: str):
    """Плата пришла со своим токеном — заявка больше не нужна."""
    with _lock:
        regs = requests()
        done = [k for k, r in regs.items() if r.get("dev") == dev]
        if done:
            for k in done:
                del regs[k]
            _reg_save(regs)


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
    from . import hours
    with _lock:
        devs = load()
        d = devs[dev]
        boot = hours.boot_seen(d, status, time.time())
        if boot:
            d["boots"] = (d.get("boots", []) + [boot])[-BOOTS_KEPT:]
        d["seen"] = int(time.time())
        d["status"] = status
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
