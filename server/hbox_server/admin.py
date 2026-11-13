"""Сайт Hbox: устройства, их состояние и файлы, настройки записи, новые устройства.

Вход — логин admin и пароль HBOX_ADMIN_PASSWORD (.env), HTTP Basic. Время на страницах — Asia/Almaty.
"""

import asyncio
import base64
import html
import json
import logging
import os
import re
import secrets
import time
import urllib.request
import zlib
from datetime import datetime
from urllib.parse import urlparse

from starlette.requests import Request
from starlette.responses import FileResponse, HTMLResponse, RedirectResponse, Response
from starlette.routing import Route

from . import devices, hours

log = logging.getLogger("hbox")

DEV_NAME = re.compile(r"^[a-z0-9-]{1,32}$")
DAY = re.compile(r"^\d{4}-\d{2}-\d{2}$")
BASE = "/hbox/admin"
ONLINE_S = 6 * 60  # устройство спрашивает настройки не реже раза в 5 минут

STATUS_TITLES = {
    "pending": "ждут отправки", "dropped": "выброшено", "overflows": "потери звука",
    "rec_errors": "ошибки записи", "up_errors": "ошибки связи", "csq": "сигнал (0–31)",
    "cpu": "кодирование, %", "sent": "отправлено файлов", "kbps": "скорость, КБ/с",
    "files": "записано файлов", "uptime": "работает, мин", "reset": "причина перезапуска",
    "pending_mb": "ждут отправки, МБ", "sd_free": "SD свободно", "sd_total": "SD всего",
    "fw": "прошивка", "fw_bad": "не запустилась прошивка", "op": "оператор",
}
QUEUE_BAD_H = 6     # отправка отстаёт больше чем на столько часов записи
SD_BAD_DAYS = 2     # места на карте меньше чем на столько дней записи

# esp_reset_reason_t (ESP-IDF 5.5)
RESETS = {
    0: "неизвестно", 1: "включение питания", 2: "кнопка/внешний сброс", 3: "программный", 4: "сбой прошивки",
    5: "сторожевой таймер (прерывания)", 6: "сторожевой таймер (задача)", 7: "сторожевой таймер",
    8: "выход из сна", 9: "просадка питания", 10: "SDIO", 11: "USB", 12: "JTAG", 13: "eFuse",
    14: "помеха по питанию", 15: "зависание CPU",
}
BAD_RESETS = {4, 5, 6, 7, 9, 14, 15}


def status_value(k: str, v) -> str:
    if k == "reset":
        return RESETS.get(v, str(v))
    if k in ("sd_free", "sd_total"):
        return f"{v / 1024:.1f} ГБ"
    return str(v)


def mb_per_hour(d: dict) -> float:
    return d["bitrate"] / 8 * 3600 / 1e6


def last_file(dev: str) -> float | None:
    """mtime последнего целиком принятого файла"""
    return max((m for _, items in files_of(dev, 2) for _, _, m in items), default=None)


def health(dev: str, d: dict, now: float) -> tuple[list[str], list[str]]:
    """(строки состояния карты и отправки, проблемы)"""
    st, lines, problems = d.get("status", {}), [], []
    rate = mb_per_hour(d)
    if "pending_mb" in st:
        hours = st["pending_mb"] / rate
        lines.append(f"очередь {hours:.1f} ч записи ({st['pending_mb']} МБ)")
        if hours > QUEUE_BAD_H:
            problems.append(f"отправка отстаёт на {hours:.0f} ч")
    if st.get("sd_total"):
        days = st["sd_free"] / (rate * 24)
        lines.append(f"SD свободно {st['sd_free'] / 1024:.1f} из {st['sd_total'] / 1024:.0f} ГБ "
                     f"(≈ {days:.0f} дн записи)")
        if days < SD_BAD_DAYS:
            problems.append(f"на SD места на {days:.1f} дн")
    elif "sd_total" in st:
        problems.append("нет SD-карты")
    last = last_file(dev)
    if last:
        ago = (now - last) / 3600
        lines.append(f"последний файл на сервере {local(last)}"
                     + (f" ({ago:.0f} ч назад)" if ago >= 1 else ""))
        if ago > 3 and st.get("pending"):
            problems.append(f"файлов не было {ago:.0f} ч")
    ota = d.get("ota")
    if ota and st.get("fw_bad") == ota["version"]:
        problems.append(f"прошивка {ota['version']} не запустилась, работает {st.get('fw', '?')}")
    return lines, problems


def firmware_box(dev: str, d: dict, rels: list[dict]) -> str:
    st, ota = d.get("status", {}), d.get("ota")
    if not ota:
        state = "обновлений не назначено"
    elif st.get("fw") == ota["version"]:
        state = f"обновлена до {esc(ota['version'])}"
    elif st.get("fw_bad") == ota["version"]:
        state = (f'<span class="bad">{esc(ota["version"])} не запустилась — устройство вернулось '
                 f"на прежнюю</span>")
    else:
        state = (f"ждёт обновления до {esc(ota['version'])} ({ota['size'] // 1024} КБ): устройство "
                 f"скачает при следующей связи (до 5 минут) и перезагрузится")
    cancel = (f'<form method="post" action="{BASE}/device/{esc(dev)}/firmware" style="display:inline">'
              f'<input type="hidden" name="cancel" value="1"><button>Отменить</button></form>'
              if ota and st.get("fw") != ota["version"] else "")
    cur = st.get("fw")
    rows = ""
    for r in rels:
        when = local(datetime.fromisoformat(r["date"].replace("Z", "+00:00")).timestamp()) if r["date"] else ""
        if r["version"] == cur:
            act = '<span class="ok">установлена</span>'
        else:
            act = (f'<form method="post" action="{BASE}/device/{esc(dev)}/firmware" style="margin:0;display:inline">'
                   f'<input type="hidden" name="release" value="{esc(r["version"])}"><button>Установить</button></form>')
        if r.get("zip"):
            act += f' <a class="small" href="{esc(r["zip"])}">zip для USB</a>'
        rows += (f'<tr><td><b>{esc(r["version"])}</b><div class="small">{esc(r["title"])}</div></td>'
                 f'<td class="small">{esc(when)}</td><td>{act}</td></tr>')
    gh = (f'<h2>Сборки с GitHub</h2><table>{rows}</table>' if rows else
          '<p class="muted">Сборки с GitHub не получены (нет связи с GitHub или ещё нет релизов).</p>')
    return (f'<div class="box"><h2>Прошивка</h2><p>Сейчас: <b>{esc(cur or "?")}</b> · {state} {cancel}</p>{gh}'
            f'<details><summary class="small">или файл с компьютера</summary>'
            f'<form method="post" action="{BASE}/device/{esc(dev)}/firmware" enctype="multipart/form-data">'
            f'<input type="file" name="firmware" accept=".bin" required> <button>Загрузить и обновить</button>'
            f'</form></details><p class="muted">Устройство скачает прошивку при следующей связи (до 5 минут). '
            f"Если новая прошивка не выйдет на связь, устройство само вернётся на прежнюю.</p></div>")

STYLE = """
:root{--bg:#f4f5f7;--card:#fff;--line:#e4e6ea;--text:#1d1f23;--muted:#6b7280;--accent:#2563eb;
--ok:#16a34a;--bad:#dc2626;--rec:#ef4444;--chip:#eef1f5;--off:#cfd4dc;--warn:#d97706}
@media (prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#181b21;--line:#2a2f38;--text:#e6e8eb;
--muted:#9aa3af;--accent:#60a5fa;--ok:#22c55e;--bad:#f87171;--chip:#232831;--off:#3a404a}}
*{box-sizing:border-box}
body{font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;margin:0;padding:20px 16px;
background:var(--bg);color:var(--text)}
.page{max-width:1200px;margin:0 auto}
h1{font-size:22px;font-weight:650;margin:0 0 16px;letter-spacing:-.01em} h2{font-size:16px;margin:18px 0 8px}
a{color:var(--accent);text-decoration:none} a:hover{text-decoration:underline}
table{border-collapse:collapse;width:100%} td,th{border-bottom:1px solid var(--line);padding:6px 8px;
text-align:left;vertical-align:top} th{font-weight:600}
.ok{color:var(--ok)} .bad{color:var(--bad)} .muted{color:var(--muted)}
form.box,div.box{background:var(--card);padding:16px;margin:12px 0;border:1px solid var(--line);border-radius:14px}
input,select,button{font:inherit;padding:6px 10px;border:1px solid var(--line);border-radius:8px;
background:var(--card);color:var(--text)} button{background:var(--accent);color:#fff;border:0;cursor:pointer}
audio{height:32px;vertical-align:middle;max-width:100%} code{background:var(--chip);padding:1px 4px;border-radius:4px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;
box-shadow:0 1px 2px rgba(0,0,0,.04);display:flex;flex-direction:column;gap:12px;min-width:0}
.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.spread{justify-content:space-between}
.name{font-size:18px;font-weight:650;color:var(--text)}
.dot{width:9px;height:9px;border-radius:50%;background:var(--off);display:inline-block}
.dot.on{background:var(--ok);box-shadow:0 0 0 3px color-mix(in srgb,var(--ok) 20%,transparent)}
.sig{display:inline-flex;align-items:center;gap:6px;color:var(--muted);font-size:13px}
.sig svg rect{fill:var(--off)} .sig svg rect.on{fill:var(--ok)} .sig b{color:var(--text);font-size:12px}
.chip{display:inline-flex;align-items:center;gap:5px;background:var(--chip);border-radius:8px;
padding:3px 9px;font-size:13px;font-weight:600}
.chip svg{width:14px;height:14px;fill:none;stroke:currentColor;stroke-width:2}
.rec{display:inline-flex;align-items:center;gap:7px;font-weight:750;font-size:13px;letter-spacing:.08em;
color:var(--rec);border:1.5px solid var(--rec);border-radius:8px;padding:2px 9px}
.rec i{width:9px;height:9px;border-radius:50%;background:var(--rec);animation:pulse 1.2s ease-in-out infinite}
.rec.idle{color:var(--muted);border-color:var(--off)} .rec.idle i{background:var(--off);animation:none}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1);box-shadow:0 0 0 0 rgba(239,68,68,.55)}
50%{opacity:.35;transform:scale(.8);box-shadow:0 0 0 6px rgba(239,68,68,0)}}
@media (prefers-reduced-motion:reduce){.rec i{animation:none}}
.tabs{display:flex;gap:4px;border-bottom:1px solid var(--line);margin:0 0 16px;overflow-x:auto}
.tab{padding:8px 14px;border-bottom:2px solid transparent;color:var(--muted);white-space:nowrap}
.tab:hover{color:var(--text);text-decoration:none} .tab.on{color:var(--accent);border-bottom-color:var(--accent);font-weight:600}
.sd,.net{display:flex;align-items:center;gap:10px}
.net svg{width:22px;height:22px;flex:none;fill:none;stroke:var(--accent,#2f7de1);stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
.sd svg{width:22px;height:26px;flex:none;fill:none;stroke:var(--muted);stroke-width:1.6}
.bar{flex:1;height:8px;background:var(--chip);border-radius:6px;overflow:hidden;min-width:60px}
.bar b{display:block;height:100%;background:var(--accent);border-radius:6px}
.bar.full b{background:var(--bad)}
.small{font-size:13px;color:var(--muted)}
.problems{background:color-mix(in srgb,var(--bad) 10%,transparent);color:var(--bad);border-radius:10px;
padding:8px 10px;font-size:13px}
.last{font-size:13px;overflow-wrap:anywhere}
.k-sent{background:var(--ok)} .k-sending,.k-queue{background:var(--warn)} .k-gap{background:var(--bad)}
.k-off{background:var(--off)} .k-unknown{background:repeating-linear-gradient(45deg,var(--off) 0 3px,transparent 3px 6px)}
.legend{display:flex;gap:14px;flex-wrap:wrap;font-size:13px;color:var(--muted)}
.legend i{display:inline-block;width:12px;height:12px;border-radius:3px;vertical-align:-2px;margin-right:5px}
.chart{display:grid;grid-template-columns:repeat(24,1fr);gap:3px;align-items:end;height:110px;margin:8px 0 2px}
.chart a{display:flex;flex-direction:column-reverse;height:100%;background:var(--chip);border-radius:4px;overflow:hidden}
.chart a:hover{outline:2px solid var(--accent)}
.chart-x{display:grid;grid-template-columns:repeat(24,1fr);gap:3px;font-size:11px;color:var(--muted);text-align:center}
.strip{position:relative;height:14px;background:var(--chip);border-radius:4px;min-width:160px;overflow:hidden}
.strip i{position:absolute;top:0;bottom:0}
table.hours td{vertical-align:middle;white-space:nowrap} table.hours td.why{white-space:normal}
.scroll{overflow-x:auto}
"""


def _core():
    from . import app

    return app


def _authorized(request: Request) -> bool:
    password = os.environ.get("HBOX_ADMIN_PASSWORD", "")
    auth = request.headers.get("authorization", "")
    if not password or not auth.startswith("Basic "):
        return False
    try:
        user, _, given = base64.b64decode(auth[6:]).decode().partition(":")
    except ValueError:
        return False
    return user == "admin" and secrets.compare_digest(given, password)


def _same_origin(request: Request) -> bool:
    """POST только со своих страниц: браузер шлёт Basic-пароль и на чужие формы."""
    origin = request.headers.get("origin") or request.headers.get("referer") or ""
    return urlparse(origin).netloc == request.headers.get("host", "")


def guarded(handler):
    async def wrapper(request: Request):
        if not _authorized(request):
            return Response("Нужен вход", 401, {"WWW-Authenticate": 'Basic realm="Hbox"'})
        if request.method == "POST" and not _same_origin(request):
            return Response("Чужая страница", 403)
        return await handler(request)

    return wrapper


def esc(v) -> str:
    return html.escape(str(v))


def page(title: str, body: str) -> HTMLResponse:
    return HTMLResponse(
        f'<!doctype html><html lang="ru"><head><meta charset="utf-8">'
        f'<meta name="viewport" content="width=device-width,initial-scale=1">'
        f"<title>{esc(title)}</title><style>{STYLE}</style></head><body><div class=\"page\">{body}</div>"
        f"</body></html>",
        headers={"Cache-Control": "no-store"})


def local(ts: float) -> str:
    return datetime.fromtimestamp(ts, devices.TZ).strftime("%d.%m %H:%M")


def files_of(dev: str, days: int = 3) -> list[tuple[str, list]]:
    """[(день, [(имя, байт, mtime)])], новые первыми."""
    root = _core().DATA / dev
    if not root.is_dir():
        return []
    out = []
    for day in sorted((p for p in root.iterdir() if DAY.match(p.name)), reverse=True)[:days]:
        items = sorted(((p.name, p.stat().st_size, p.stat().st_mtime) for p in day.iterdir()),
                       reverse=True)
        out.append((day.name, items))
    return out


def player(dev: str, day: str, name: str) -> str:
    url = f"{BASE}/file/{esc(dev)}/{esc(day)}/{esc(name)}"
    return (f'<audio controls preload="none" src="{url}"></audio> '
            f'<a href="{url}" download>{esc(name)}</a>')


HOUR_KINDS = {"sent": "передано", "sending": "передаётся", "queue": "на карте, в очереди",
              "gap": "нет записи", "off": "вне расписания", "unknown": "неизвестно (нет связи)"}


def hours_box(dev: str, d: dict, now: float) -> str:
    """Вкладка «По часам»: график за день и таблица по каждому часу (Алматы)."""
    items = hours.pieces(_core().DATA, dev, d.get("bitrate") or 128000)
    rows = hours.hours(d, items, now)
    if not rows:
        return "<p class=muted>Файлов пока нет</p>"
    legend = "".join(f'<span><i class="k-{k}"></i>{t}</span>' for k, t in HOUR_KINDS.items())
    out = [f'<div class="legend">{legend}</div>']
    for day, rs in hours.by_day(rows):
        rec = sum(r["rec"] for r in rs)
        gap = sum(r["gap"] for r in rs)
        mb = sum(r["mb"] for r in rs)
        title = datetime.strptime(day, "%Y-%m-%d").strftime("%d.%m.%Y")
        cols = {datetime.fromtimestamp(r["start"], devices.TZ).hour: r for r in rs}
        bars, xs = [], []
        for h in range(24):
            r = cols.get(h)
            xs.append(f"<span>{h}</span>" if h % 3 == 0 else "<span></span>")
            if not r:
                bars.append('<a style="visibility:hidden"></a>')
                continue
            parts = {}
            for a, b, k, _ in r["segs"]:
                parts[k] = parts.get(k, 0) + b - a
            fill = "".join(f'<i class="k-{k}" style="height:{v / 60 * 100:.1f}%"></i>'
                           for k, v in sorted(parts.items(), key=lambda x: list(HOUR_KINDS).index(x[0])))
            tip = f'{h:02d}:00 — записано {hours.fmt_min(r["rec"])}, {r["mb"]:.0f} МБ, {r["state"][1]}'
            bars.append(f'<a href="#h-{day}-{h}" title="{esc(tip)}">{fill}</a>')
        trs = []
        for r in reversed(rs):
            h0 = datetime.fromtimestamp(r["start"], devices.TZ)
            strip = "".join(f'<i class="k-{k}" style="left:{a / 60 * 100:.2f}%;width:{(b - a) / 60 * 100:.2f}%" '
                            f'title="{h0.hour:02d}:{int(a):02d}–{h0.hour:02d}:{min(int(b), 59):02d} '
                            f'{esc(n or HOUR_KINDS[k])}"></i>' for a, b, k, n in r["segs"])
            kind, text = r["state"]
            cls = {"sent": "ok", "gap": "bad", "partial": "bad"}.get(kind, "")
            why = "; ".join(r["notes"])
            if r["gap"] >= 60:
                why = f'пропуск {hours.fmt_min(r["gap"])}' + (f": {why}" if why else "")
            trs.append(f'<tr id="h-{day}-{h0.hour}"><td>{h0:%H}:00–{(h0.hour + 1) % 24:02d}:00</td>'
                       f'<td style="width:40%"><div class="strip">{strip}</div></td>'
                       f'<td>{hours.fmt_min(r["rec"])}</td><td>{r["mb"]:.1f} МБ</td>'
                       f'<td class="{cls}">{esc(text)}</td><td class="why small">{esc(why)}</td></tr>')
        out.append(
            f'<div class="box"><h2 style="margin-top:0">{title} · записано {hours.fmt_min(rec)}'
            f' · {mb:.0f} МБ' + (f' · <span class="bad">пропуски {hours.fmt_min(gap)}</span>' if gap >= 60 else "")
            + f'</h2><div class="chart">{"".join(bars)}</div><div class="chart-x">{"".join(xs)}</div>'
            f'<div class="scroll"><table class="hours"><tr><th>Час</th><th>Минуты часа</th><th>Записано</th>'
            f'<th>Объём</th><th>Файл</th><th>Причина</th></tr>{"".join(trs)}</table></div></div>')
    boots = d.get("boots", [])[-10:]
    if boots:
        out.append('<div class="box"><h2 style="margin-top:0">Запуски устройства</h2>' + "".join(
            f'<div>{local(b["t"])} — {esc(RESETS.get(b.get("reset"), "?"))}</div>' for b in reversed(boots))
            + "</div>")
    out.append('<p class="muted">Время — Алматы. Часы, полученные сервером, — зелёные; после последнего '
               "полученного файла запись ещё на карте устройства. Пропуск между файлами — записи нет: "
               "питание, перезапуск или файл испорчен.</p>")
    return "".join(out)


def schedule_text(d: dict) -> str:
    s = d.get("schedule")
    if not s:
        return "круглосуточно"
    return ", ".join(f"{devices.DAYS[i]} {esc(r) if r else 'нет'}" for i, r in enumerate(s))


def signal_bars(csq) -> int:
    """CSQ 0–31 (99 — нет данных) → 0–4 палочки"""
    if not isinstance(csq, int) or csq >= 99 or csq < 2:
        return 0
    return 1 if csq < 10 else 2 if csq < 15 else 3 if csq < 20 else 4


def signal_icon(csq, op: str | None = None) -> str:
    n = signal_bars(csq)
    on = ' class="on"'
    bars = "".join(f'<rect x="{i * 5}" y="{12 - 3 * i}" width="3.5" height="{4 + 3 * i}" rx="1"'
                   f'{on if i < n else ""}/>' for i in range(4))
    return (f'<span class="sig" title="сигнал CSQ {esc(csq)}"><svg width="22" height="18" '
            f'viewBox="0 0 19 16">{bars}</svg><b>{esc(op) + " " if op else ""}4G</b></span>')


def size_text(n: int) -> str:
    return f"{n / 1e9:.2f} ГБ" if n >= 1e9 else f"{n / 1e6:.0f} МБ"


def traffic_line(d: dict, now: float) -> str:
    """Значок передачи, средняя скорость за сегодня и принятый трафик (сегодня, месяц, всего)."""
    tr = d.get("traffic")
    if not tr:
        return f'<div class="net">{ICON_NET}<span class="small">передачи ещё не было</span></div>'
    t = datetime.fromtimestamp(now, devices.TZ)
    today = tr.get("today", 0) if tr.get("day") == t.strftime("%Y-%m-%d") else 0
    month = tr.get("this_month", 0) if tr.get("month") == t.strftime("%Y-%m") else 0
    secs = tr.get("today_s", 0) if today else 0
    speed = f"{today / secs / 1024:.0f} КБ/с" if secs >= 1 else "–"
    return (f'<div class="net">{ICON_NET}<div><b>{speed}</b> <span class="small">в среднем за сегодня</span>'
            f'<div class="small">трафик: сегодня {size_text(today)} · месяц {size_text(month)} · '
            f'всего {size_text(tr.get("total", 0))}</div></div></div>')


ICON_FILE = ('<svg viewBox="0 0 24 24"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/>'
             '<path d="M14 3v5h5"/></svg>')
ICON_WAVE = '<svg viewBox="0 0 24 24"><path d="M3 12h3l3-7 4 14 3-9 2 2h3"/></svg>'
ICON_NET = '<svg viewBox="0 0 24 24"><path d="M7 20V5M3 9l4-4 4 4M17 4v15M13 15l4 4 4-4"/></svg>'
ICON_SD = '<svg viewBox="0 0 22 26"><path d="M7 1h13v24H2V6z"/><path d="M8 5v4M11 5v4M14 5v4M17 5v4"/></svg>'


def device_card(dev: str, d: dict, now: float) -> str:
    seen = d.get("seen")
    online = bool(seen and now - seen < ONLINE_S)
    st = d.get("status", {})
    rec_now, _ = devices.recording_now(d)
    days = files_of(dev, 1)
    today = days[0][1] if days else []
    problems = [f"{STATUS_TITLES[k]}: {st[k]}" for k in ("dropped", "overflows", "rec_errors") if st.get(k)]
    if st.get("reset") in BAD_RESETS:
        problems.append(f"перезапуск: {RESETS[st['reset']]}")
    lines, more = health(dev, d, now)
    problems += more
    sd_line = next((x for x in lines if x.startswith("SD ")), None)
    lines = [x for x in lines if x is not sd_line]
    recording = online and st.get("rec")
    rec = ('<span class="rec"><i></i>REC</span>' if recording else
           f'<span class="rec idle"><i></i>{"ПАУЗА" if online and not rec_now else "НЕ ПИШЕТ"}</span>')
    if st.get("sd_total"):
        used = 100 - st["sd_free"] * 100 // st["sd_total"]
        sd = (f'<div class="sd">{ICON_SD}<div style="flex:1"><div class="bar{" full" if used > 90 else ""}">'
              f'<b style="width:{max(used, 2)}%"></b></div><div class="small">{esc(sd_line)} · занято {used}%'
              f"</div></div></div>")
    else:
        sd = f'<div class="sd">{ICON_SD}<span class="small">{"нет SD-карты" if "sd_total" in st else "SD: нет данных"}</span></div>'
    sched = "по расписанию" if d.get("schedule") else "круглосуточно"
    last = (f'<div class="last">{player(dev, days[0][0], today[0][0])}</div>' if today
            else '<div class="small">файлов за сегодня нет</div>')
    return (
        f'<div class="card"><div class="row spread"><div class="row">'
        f'<a class="name" href="{BASE}/device/{esc(dev)}">{esc(dev)}</a>'
        f'<span class="dot{" on" if online else ""}"></span>'
        f'<span class="small">{"на связи" if online else "нет связи"} · {local(seen) if seen else "никогда"}</span>'
        f"</div>{signal_icon(st.get('csq'), st.get('op'))}</div>"
        f'<div class="row">{rec}<span class="chip">{ICON_FILE}{esc(d["format"]).upper()}</span>'
        f'<span class="chip">{ICON_WAVE}{d["bitrate"] // 1000} kbps</span>'
        f'<span class="small">{sched}</span></div>'
        f"{sd}{traffic_line(d, now)}"
        f'<div class="small">{st.get("pending", "–")} ждут · {len(today)} за день'
        f'{"".join("<br>" + esc(x) for x in lines)}</div>'
        + (f'<div class="problems">{"<br>".join(map(esc, problems))}</div>' if problems else "")
        + f"{last}</div>")


@guarded
async def index(request: Request):
    devs = devices.load()
    now = time.time()
    cards = "".join(device_card(dev, d, now) for dev, d in sorted(devs.items()))
    body = (
        "<h1>Hbox — устройства</h1>"
        f'<div class="grid">{cards}</div>'
        f'<form class="box" method="post" action="{BASE}/add">Новое устройство: '
        '<input name="name" placeholder="cafe-1" pattern="[a-z0-9-]{1,32}" required> '
        '<button>Добавить</button> <span class="muted">латиница, цифры, дефис — начало имени файлов</span></form>'
        f'<p class="muted">Время — Алматы. Свободно на сервере: {_core().free_gb():.1f} ГБ</p>')
    return page("Hbox", body)


@guarded
async def device(request: Request):
    dev = request.path_params["dev"]
    devs = devices.load()
    if dev not in devs:
        return Response("Нет такого устройства", 404)
    d = devs[dev]
    st = d.get("status", {})
    sched = d.get("schedule") or [""] * 7
    rows = "".join(
        f"<tr><td>{devices.DAYS[i]}</td><td><input name=\"day{i}\" value=\"{esc(sched[i])}\" "
        f'placeholder="10:00-23:00" size="12"></td></tr>' for i in range(7))
    fmt = "".join(f'<option{" selected" if f == d["format"] else ""}>{f}</option>'
                  for f in devices.FORMATS)
    br = "".join(f'<option value="{b}"{" selected" if b == d["bitrate"] else ""}>{b // 1000} кбит/с</option>'
                 for b in devices.BITRATES)
    status = "".join(f"<tr><td>{esc(STATUS_TITLES.get(k, k))}</td><td>{esc(status_value(k, v))}</td></tr>"
                     for k, v in st.items() if k in STATUS_TITLES)
    files = "".join(
        f"<h2>{esc(day)} · {len(items)} файлов · {sum(i[1] for i in items) / 1e6:.0f} МБ</h2>"
        + "".join(f"<div>{player(dev, day, n)} <span class=\"muted\">{size / 1e6:.1f} МБ</span></div>"
                  for n, size, _ in items[:48])
        for day, items in files_of(dev))
    msg = {"saved": "Сохранено. Устройство получит настройки в течение 5 минут.",
           "ota": "Прошивка загружена. Устройство обновится при следующей связи (до 5 минут).",
           "added": "Устройство добавлено. Скачайте файл токена ниже и запустите его рядом с платой."}
    note = msg.get(request.query_params.get("msg", ""), "")
    route = "".join(f'<option value="{k}"{" selected" if k == (d.get("route") or "") else ""}>{t}</option>'
                    for k, t in {"": "авто (сама переключается)", **devices.ROUTES}.items())
    lines, problems = health(dev, d, time.time())
    settings = (
        f'<form class="box" method="post">'
        f'<label><input type="checkbox" name="record"{" checked" if d.get("record", True) else ""}> '
        f"Запись включена</label><br><br>Формат <select name=\"format\">{fmt}</select> "
        f'Битрейт <select name="bitrate">{br}</select><br><br>Связь <select name="route">{route}</select>'
        f'<h2>Расписание (время Алматы)</h2><label><input type="checkbox" name="use_schedule"'
        f'{" checked" if d.get("schedule") else ""}> писать только в часы работы</label>'
        f'<table style="width:auto">{rows}</table><p class="muted">Пустая строка — в этот день не писать. '
        f"23:00-02:00 — через полночь. Новые настройки — с начала следующего файла.</p>"
        f"<button>Сохранить</button></form>")
    state = (
        f'<div class="grid">{device_card(dev, d, time.time())}'
        f'<div class="box" style="margin:0"><h2 style="margin-top:0">Подробно</h2>'
        f'<p>{"Последняя связь: " + local(d["seen"]) if d.get("seen") else "Ещё не выходило на связь"}</p>'
        + "".join(f"<p>{esc(x)}</p>" for x in lines)
        + "".join(f'<p class="bad">{esc(x)}</p>' for x in problems)
        + f'<table style="width:auto">{status}</table></div></div>')
    token = (f'<div class="box">Токен: <a href="{BASE}/token/{esc(dev)}.bat" download>token_{esc(dev)}.bat</a> '
             f'<span class="muted">— запустить рядом с платой (папка pcclient)</span></div>')
    tab_wanted = request.query_params.get("tab", "") or {"ota": "fw"}.get(request.query_params.get("msg", ""), "")
    tabs = {"state": ("Состояние", state),
            "hours": ("По часам", hours_box(dev, d, time.time()) if tab_wanted == "hours" else ""),
            "files": ("Записи", files or "<p class=muted>Файлов пока нет</p>"),
            "settings": ("Настройки", settings),
            "fw": ("Прошивка", firmware_box(dev, d, await releases() if tab_wanted == "fw" else [])),
            "token": ("Токен", token)}
    tab = tab_wanted if tab_wanted in tabs else {"saved": "settings", "ota": "fw", "added": "token"}.get(
        request.query_params.get("msg", ""), "state")
    nav = "".join(f'<a class="tab{" on" if k == tab else ""}" href="{BASE}/device/{esc(dev)}?tab={k}">{t}</a>'
                  for k, (t, _) in tabs.items())
    body = (
        f'<p><a href="{BASE}/">← все устройства</a></p><h1>{esc(dev)}</h1>'
        + (f'<div class="box ok">{note}</div>' if note else "")
        + f'<nav class="tabs">{nav}</nav>{tabs[tab][1]}')
    return page(f"Hbox — {dev}", body)


@guarded
async def device_save(request: Request):
    dev = request.path_params["dev"]
    if dev not in devices.load():
        return Response("Нет такого устройства", 404)
    form = await request.form()
    schedule = None
    if form.get("use_schedule"):
        schedule = [str(form.get(f"day{i}", "")).strip() for i in range(7)]
        try:
            for r in schedule:
                devices.parse_range(r)
        except ValueError as e:
            return page("Ошибка", f"<p>Не понял время «{esc(e)}». Пример: 10:00-23:00.</p>"
                                  f'<p><a href="{BASE}/device/{esc(dev)}">назад</a></p>')
    fmt = form.get("format") if form.get("format") in devices.FORMATS else "mp3"
    try:
        bitrate = int(form.get("bitrate", 128000))
    except ValueError:
        bitrate = 128000
    route = form.get("route") if form.get("route") in devices.ROUTES else None
    devices.update(dev, record=bool(form.get("record")), format=fmt,
                   bitrate=bitrate if bitrate in devices.BITRATES else 128000, schedule=schedule, route=route)
    return RedirectResponse(f"{BASE}/device/{dev}?msg=saved", 303)


GITHUB_REPO = os.environ.get("HBOX_GITHUB_REPO", "nurlasan/hbox")
_releases: tuple[float, list] = (0.0, [])


def _get(url: str, limit: int) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "hbox-server", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        data = r.read(limit + 1)
    if len(data) > limit:
        raise ValueError("слишком большой ответ")
    return data


async def releases() -> list[dict]:
    """Сборки прошивки с GitHub (релизы fw-<версия> с файлом hbox.bin), новые первыми; кэш 2 минуты."""
    global _releases
    if time.time() - _releases[0] < 120:
        return _releases[1]
    try:
        raw = await asyncio.to_thread(_get, f"https://api.github.com/repos/{GITHUB_REPO}/releases?per_page=8",
                                      1 << 20)
        out = []
        for r in json.loads(raw):
            asset = next((a for a in r.get("assets", []) if a["name"] == "hbox.bin"), None)
            if asset and r["tag_name"].startswith("fw-"):
                out.append({"version": r["tag_name"][3:], "title": r.get("name") or r["tag_name"],
                            "date": r.get("published_at", ""), "url": asset["browser_download_url"],
                            "zip": next((a["browser_download_url"] for a in r["assets"]
                                         if a["name"].endswith(".zip")), None)})
        _releases = (time.time(), out)
    except (OSError, ValueError, KeyError) as e:
        log.warning("github releases: %s", e)
        return _releases[1]
    return out


def assign_firmware(dev: str, blob: bytes) -> str | None:
    """Проверить hbox.bin и назначить устройству; текст ошибки или None."""
    core = _core()
    info = core.app_info(blob)
    if not info or info["project"] != "hbox":
        return "Это не прошивка Hbox. Нужен файл hbox.bin из архива прошивки."
    if len(blob) > core.FW_MAX:
        return f"Прошивка больше слота ({core.FW_MAX // 1024} КБ)."
    if not core.FW_VERSION.match(info["version"]):
        return f"Непонятная версия прошивки «{info['version']}»."
    crc = zlib.crc32(blob)
    name = f"{crc:08x}.bin"
    core.fw_dir().mkdir(parents=True, exist_ok=True)
    (core.fw_dir() / name).write_bytes(blob)
    devices.update(dev, ota={"name": name, "size": len(blob), "crc": crc, "version": info["version"]})
    return None


@guarded
async def firmware_upload(request: Request):
    """hbox.bin для обновления по воздуху: с GitHub (release), с компьютера или отмена назначенного."""
    dev = request.path_params["dev"]
    if dev not in devices.load():
        return Response("Нет такого устройства", 404)
    form = await request.form()
    back = f'<p><a href="{BASE}/device/{esc(dev)}?tab=fw">назад</a></p>'
    if form.get("cancel"):
        devices.update(dev, ota=None)
        return RedirectResponse(f"{BASE}/device/{dev}?tab=fw", 303)
    if form.get("release"):
        rel = next((r for r in await releases() if r["version"] == form.get("release")), None)
        if not rel:
            return page("Ошибка", f"<p>Нет такой сборки на GitHub.</p>{back}")
        try:
            blob = await asyncio.to_thread(_get, rel["url"], _core().FW_MAX)
        except (OSError, ValueError) as e:
            return page("Ошибка", f"<p>Не скачалась с GitHub: {esc(e)}</p>{back}")
    else:
        up = form.get("firmware")
        blob = await up.read() if hasattr(up, "read") else b""
    err = assign_firmware(dev, blob)
    if err:
        return page("Ошибка", f"<p>{esc(err)}</p>{back}")
    return RedirectResponse(f"{BASE}/device/{dev}?msg=ota", 303)


@guarded
async def add(request: Request):
    name = str((await request.form()).get("name", "")).strip().lower()
    if not DEV_NAME.match(name):
        return page("Ошибка", f'<p>Имя: латиница, цифры, дефис, до 32 символов.</p><p><a href="{BASE}/">назад</a></p>')
    try:
        devices.add(name)
    except ValueError:
        return page("Ошибка", f'<p>{esc(name)} уже есть.</p><p><a href="{BASE}/">назад</a></p>')
    return RedirectResponse(f"{BASE}/device/{name}?msg=added", 303)


@guarded
async def token_bat(request: Request):
    dev = request.path_params["dev"]
    d = devices.load().get(dev)
    if not d:
        return Response("Нет такого устройства", 404)
    bat = (f"@echo off\r\nrem Токен устройства {dev} (не публиковать)\r\n"
           f'call "%~dp0token.bat" {d["token"]} %1\r\n')
    return Response(bat.encode(), media_type="application/octet-stream",
                    headers={"Content-Disposition": f'attachment; filename="token_{dev}.bat"'})


@guarded
async def file(request: Request):
    dev, day, name = (request.path_params[k] for k in ("dev", "day", "name"))
    if not (DEV_NAME.match(dev) and DAY.match(day) and _core().NAME.match(name)):
        return Response("Нет файла", 404)
    path = _core().DATA / dev / day / name
    if not path.is_file():
        return Response("Нет файла", 404)
    return FileResponse(path, media_type="audio/mpeg" if name.endswith(".mp3") else "audio/mp4")


routes = [
    Route("/", index),
    Route("/add", add, methods=["POST"]),
    Route("/device/{dev}", device),
    Route("/device/{dev}", device_save, methods=["POST"]),
    Route("/device/{dev}/firmware", firmware_upload, methods=["POST"]),
    Route("/token/{dev}.bat", token_bat),
    Route("/file/{dev}/{day}/{name}", file),
]
