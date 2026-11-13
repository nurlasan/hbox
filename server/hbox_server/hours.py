"""Отчёт по часам: сколько записано в каждый час (Алматы), сколько МБ, передан ли файл, где пропуски.

Источник — файлы на сервере (готовые и недокачанные) и состояние устройства. Устройство пишет файл
с начала часа; файл, начатый посреди часа, — после перезапуска или смены настроек. Пропуск между
полученными файлами — записи нет (питание, перезапуск, испорченный файл). Время после последнего
полученного файла — ещё на карте устройства (в очереди), если устройство на связи и пишет.
"""

import json
import re
import struct
from datetime import datetime, timezone
from pathlib import Path

from . import devices

NAME = re.compile(r"^[A-Za-z0-9-]{1,32}_(\d{8})_(\d{6})\.(mp3|m4a)$")
DAYS_SHOWN = 3
ONLINE_S = 6 * 60

# MPEG-1 Layer III, кбит/с по индексу; MPEG-2/2.5 — второй ряд
MP3_KBPS = ((0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0),
            (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0))

_durations: dict[tuple[str, float, int], float] = {}


def _mp3_seconds(f, size: int) -> float | None:
    head = f.read(10)
    skip = 0
    if head[:3] == b"ID3":
        skip = 10 + ((head[6] & 0x7F) << 21 | (head[7] & 0x7F) << 14 | (head[8] & 0x7F) << 7 | head[9] & 0x7F)
    f.seek(skip)
    buf = f.read(4096)
    for i in range(len(buf) - 3):
        if buf[i] == 0xFF and buf[i + 1] & 0xE0 == 0xE0:
            version, layer = buf[i + 1] >> 3 & 3, buf[i + 1] >> 1 & 3
            kbps = MP3_KBPS[0 if version == 3 else 1][buf[i + 2] >> 4]
            if layer == 1 and kbps:
                return (size - skip - i) * 8 / (kbps * 1000)
    return None


def _m4a_seconds(f, size: int) -> float | None:
    """длительность из moov/mvhd; без moov (оборванная запись) — None"""
    pos = 0
    while pos + 8 <= size:
        f.seek(pos)
        n, kind = struct.unpack(">I4s", f.read(8))
        head = 8
        if n == 1:
            n, head = struct.unpack(">Q", f.read(8))[0], 16
        if n < head:
            return None
        if kind == b"moov":
            end = pos + n
            p = pos + head
            while p + 8 <= end:
                f.seek(p)
                m, sub = struct.unpack(">I4s", f.read(8))
                if sub == b"mvhd":
                    ver = f.read(4)[0]
                    if ver == 1:
                        f.read(16)
                        scale, dur = struct.unpack(">IQ", f.read(12))
                    else:
                        f.read(8)
                        scale, dur = struct.unpack(">II", f.read(8))
                    return dur / scale if scale else None
                if m < 8:
                    return None
                p += m
            return None
        pos += n
    return None


def seconds(path: Path, size: int, mtime: float, bitrate: int) -> float:
    key = (str(path), mtime, size)
    if key not in _durations:
        try:
            with open(path, "rb") as f:
                s = _m4a_seconds(f, size) if path.suffix == ".m4a" else _mp3_seconds(f, size)
        except (OSError, struct.error, IndexError):
            s = None
        _durations[key] = s if s else size * 8 / bitrate
    return _durations[key]


def start_of(name: str) -> float | None:
    m = NAME.match(name)
    if not m:
        return None
    return datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S").replace(
        tzinfo=timezone.utc).timestamp()


def _hour_end(t: float) -> float:
    return (t // 3600 + 1) * 3600


def pieces(data: Path, dev: str, bitrate: int) -> list[dict]:
    """[{start, end, bytes, kind: sent|sending, name, pct}] по времени"""
    out = []
    root = data / dev
    for day in (root.iterdir() if root.is_dir() else []):
        if not day.is_dir():
            continue
        for p in day.iterdir():
            t = start_of(p.name)
            if t is None:
                continue
            st = p.stat()
            out.append({"start": t, "end": min(t + seconds(p, st.st_size, st.st_mtime, bitrate), _hour_end(t)),
                        "bytes": st.st_size, "kind": "sent", "name": p.name})
    part = data / ".partial" / dev
    for p in (part.glob("*.part") if part.is_dir() else []):
        t = start_of(p.name[:-5])
        if t is None:
            continue
        try:
            total = json.loads(p.with_suffix(".json").read_text()).get("total") or 0
        except (OSError, ValueError):
            total = 0
        got = p.stat().st_size
        total = max(total, got)
        out.append({"start": t, "end": min(t + total * 8 / bitrate, _hour_end(t)), "bytes": total, "kind": "sending",
                    "name": p.name[:-5], "pct": round(100 * got / total) if total else 0,
                    "mtime": p.stat().st_mtime})
    out.sort(key=lambda x: x["start"])
    return out


def _cut(segs: list, a: float, b: float, kind: str, note: str = ""):
    if b > a:
        segs.append((a, b, kind, note))


def timeline(d: dict, items: list[dict], now: float) -> list[tuple[float, float, str, str]]:
    """Отрезки (начало, конец, вид, причина): sent, sending, queue, gap, off (вне расписания), unknown."""
    segs: list = []
    boots = d.get("boots", [])
    st = d.get("status", {})
    # на связи: спрашивало настройки или докачивает файл (во время отправки настройки спрашивает реже)
    online = (d.get("seen") and now - d["seen"] < ONLINE_S) or any(
        now - it.get("mtime", 0) < ONLINE_S for it in items if it["kind"] == "sending")
    prev_end = None
    for it in items:
        if prev_end is not None and it["start"] > prev_end + 5:
            boot = next((b for b in boots if prev_end - 60 <= b["t"] <= it["start"] + 120), None)
            if boot and boot.get("reset") == 1:
                note = "не было питания"
            elif boot or it["start"] % 3600 > 5:
                note = "перезапуск устройства"
            else:
                note = "файл не получен"
            _cut(segs, prev_end, it["start"], "gap", note)
        _cut(segs, it["start"], min(it["end"], now), it["kind"])
        prev_end = max(prev_end or 0, it["end"])
    if prev_end is not None and prev_end < now - 5:
        if online and st.get("rec"):
            _cut(segs, prev_end, now, "queue", "на карте, ждёт отправки")
        elif online:
            _cut(segs, prev_end, now, "unknown", "запись выключена или вне расписания")
        else:
            _cut(segs, prev_end, now, "unknown", "нет связи с устройством")
    if d.get("schedule"):
        segs = _mark_schedule(d["schedule"], segs)
    return segs


def _mark_schedule(schedule: list[str], segs: list) -> list:
    """пропуск в нерабочее время — не пропуск"""
    out = []
    for a, b, kind, note in segs:
        if kind not in ("gap", "unknown"):
            out.append((a, b, kind, note))
            continue
        t = a
        while t < b:
            nxt = min(b, (t // 60 + 1) * 60)
            closed = not devices._open_at(schedule, datetime.fromtimestamp(t, devices.TZ))
            k, n = ("off", "вне расписания") if closed else (kind, note)
            if out and out[-1][2] == k and out[-1][1] == t and out[-1][3] == n:
                out[-1] = (out[-1][0], nxt, k, n)
            else:
                out.append((t, nxt, k, n))
            t = nxt
    return out


def hours(d: dict, items: list[dict], now: float) -> list[dict]:
    """Часы по Алматы за последние дни: [{start, segs: [(мин_от, мин_до, вид, причина)], sec, mb, state}]"""
    segs = timeline(d, items, now)
    if not segs:
        return []
    first = max(segs[0][0], now - DAYS_SHOWN * 86400)
    h = datetime.fromtimestamp(first, devices.TZ).replace(minute=0, second=0, microsecond=0).timestamp()
    out = []
    while h < now:
        e = h + 3600
        parts = [(max(a, h), min(b, e), k, n) for a, b, k, n in segs if a < e and b > h]
        sent = sum(b - a for a, b, k, _ in parts if k == "sent")
        sending = sum(b - a for a, b, k, _ in parts if k == "sending")
        queue = sum(b - a for a, b, k, _ in parts if k == "queue")
        gap = sum(b - a for a, b, k, _ in parts if k == "gap")
        mb = 0.0
        files = []
        for it in items:
            span = it["end"] - it["start"]
            over = min(it["end"], e) - max(it["start"], h)
            if over > 0 and span > 0:
                mb += it["bytes"] * over / span / 1e6
                files.append(it)
        # в очереди — файла ещё нет, объём по битрейту
        mb += queue * (d.get("bitrate") or 128000) / 8 / 1e6
        if sending:
            state = ("sending", f"передаётся {max(i.get('pct', 0) for i in files if i['kind'] == 'sending')}%")
        elif queue:
            state = ("queue", "в очереди")
        elif sent < 60 and gap:
            state = ("gap", "нет файла")
        elif sent and gap >= 60:
            state = ("partial", "передан частично")
        elif sent:
            state = ("sent", "передан")
        else:
            state = ("none", "—")
        notes = sorted({n for a, b, k, n in parts if n and k in ("gap", "unknown", "off") and b - a >= 60})
        out.append({"start": h, "end": min(e, now),
                    "segs": [((a - h) / 60, (b - h) / 60, k, n) for a, b, k, n in parts],
                    "rec": sent + sending + queue, "gap": gap, "mb": mb, "state": state,
                    "notes": notes, "files": [f["name"] for f in files]})
        h = e
    return out


def by_day(rows: list[dict]) -> list[tuple[str, list[dict]]]:
    days: dict[str, list] = {}
    for r in rows:
        days.setdefault(datetime.fromtimestamp(r["start"], devices.TZ).strftime("%Y-%m-%d"), []).append(r)
    return sorted(days.items(), reverse=True)


def boot_seen(d: dict, status: dict, now: float) -> dict | None:
    """Новый запуск устройства по uptime (минуты): {t, reset} или None."""
    up = status.get("uptime")
    if not isinstance(up, int):
        return None
    t = int(now - up * 60)
    last = (d.get("boots") or [{}])[-1].get("t")
    if last is not None and abs(t - last) < 180:
        return None
    return {"t": t, "reset": status.get("reset")}


def fmt_min(s: float) -> str:
    m = round(s / 60)
    return f"{m // 60} ч {m % 60:02d} мин" if m >= 60 else f"{m} мин"

