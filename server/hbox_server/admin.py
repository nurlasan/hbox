"""Сайт Hbox: устройства, их состояние и файлы, настройки записи, новые устройства.

Вход — логин admin и пароль HBOX_ADMIN_PASSWORD (.env), HTTP Basic. Время на страницах — Asia/Almaty.
"""

import base64
import html
import os
import re
import secrets
import time
from datetime import datetime
from urllib.parse import urlparse

from starlette.requests import Request
from starlette.responses import FileResponse, HTMLResponse, RedirectResponse, Response
from starlette.routing import Route

from . import devices

DEV_NAME = re.compile(r"^[a-z0-9-]{1,32}$")
DAY = re.compile(r"^\d{4}-\d{2}-\d{2}$")
BASE = "/hbox/admin"
ONLINE_S = 6 * 60  # устройство спрашивает настройки не реже раза в 5 минут

STATUS_TITLES = {
    "pending": "ждут отправки", "dropped": "выброшено", "overflows": "потери звука",
    "rec_errors": "ошибки записи", "up_errors": "ошибки связи", "csq": "сигнал (0–31)",
    "cpu": "кодирование, %", "sent": "отправлено файлов", "kbps": "скорость, КБ/с",
    "files": "записано файлов", "uptime": "работает, мин", "reset": "причина перезапуска",
}

# esp_reset_reason_t (ESP-IDF 5.5)
RESETS = {
    0: "неизвестно", 1: "включение питания", 2: "кнопка/внешний сброс", 3: "программный", 4: "сбой прошивки",
    5: "сторожевой таймер (прерывания)", 6: "сторожевой таймер (задача)", 7: "сторожевой таймер",
    8: "выход из сна", 9: "просадка питания", 10: "SDIO", 11: "USB", 12: "JTAG", 13: "eFuse",
    14: "помеха по питанию", 15: "зависание CPU",
}
BAD_RESETS = {4, 5, 6, 7, 9, 14, 15}


def status_value(k: str, v) -> str:
    return RESETS.get(v, str(v)) if k == "reset" else str(v)

STYLE = """
body{font:15px/1.45 system-ui,sans-serif;margin:0;padding:16px;background:#f6f6f4;color:#222}
h1{font-size:20px;margin:0 0 12px} h2{font-size:16px;margin:20px 0 8px}
a{color:#1a5fb4} table{border-collapse:collapse;background:#fff;width:100%}
td,th{border-bottom:1px solid #e3e3e0;padding:6px 8px;text-align:left;vertical-align:top}
th{font-weight:600;background:#eeeeea} .ok{color:#1b7f3b} .bad{color:#b42318} .muted{color:#777}
form.box,div.box{background:#fff;padding:12px;margin:8px 0;border:1px solid #e3e3e0}
input,select,button{font:inherit;padding:4px 6px} audio{height:32px;vertical-align:middle}
.wrap{overflow-x:auto} code{background:#eee;padding:1px 4px}
@media (prefers-color-scheme: dark){body{background:#161616;color:#ddd}
table,form.box,div.box{background:#202020;border-color:#333} th{background:#2a2a2a}
td,th{border-color:#333} a{color:#8ab4f8} code{background:#333}}
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
        f"<title>{esc(title)}</title><style>{STYLE}</style></head><body>{body}</body></html>",
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


def schedule_text(d: dict) -> str:
    s = d.get("schedule")
    if not s:
        return "круглосуточно"
    return ", ".join(f"{devices.DAYS[i]} {esc(r) if r else 'нет'}" for i, r in enumerate(s))


@guarded
async def index(request: Request):
    devs = devices.load()
    now = time.time()
    rows = []
    for dev, d in sorted(devs.items()):
        seen = d.get("seen")
        online = seen and now - seen < ONLINE_S
        st = d.get("status", {})
        rec_now, _ = devices.recording_now(d)
        days = files_of(dev, 1)
        today = days[0][1] if days else []
        last = player(dev, days[0][0], today[0][0]) if today else '<span class="muted">нет</span>'
        problems = [f"{STATUS_TITLES[k]}: {st[k]}" for k in ("dropped", "overflows", "rec_errors")
                    if st.get(k)]
        if st.get("reset") in BAD_RESETS:
            problems.append(f"перезапуск: {RESETS[st['reset']]}")
        rows.append(
            f'<tr><td><a href="{BASE}/device/{esc(dev)}"><b>{esc(dev)}</b></a></td>'
            f'<td class="{"ok" if online else "bad"}">{"на связи" if online else "нет связи"}'
            f'<br><span class="muted">{local(seen) if seen else "никогда"}</span></td>'
            f'<td>{"пишет" if st.get("rec") else "не пишет"}'
            f'<br><span class="muted">{"по расписанию: " if d.get("schedule") else ""}'
            f'{"запись" if rec_now else "пауза"} · {esc(d["format"])} {d["bitrate"] // 1000}</span></td>'
            f'<td>{st.get("pending", "–")} ждут · {len(today)} за день'
            f'<br><span class="muted">{st.get("kbps", "–")} КБ/с · сигнал {st.get("csq", "–")}</span></td>'
            f'<td class="bad">{"<br>".join(map(esc, problems))}</td><td>{last}</td></tr>')
    body = (
        "<h1>Hbox — устройства</h1>"
        f'<div class="wrap"><table><tr><th>Устройство</th><th>Связь</th><th>Запись</th>'
        f'<th>Файлы</th><th>Проблемы</th><th>Последний файл</th></tr>{"".join(rows)}</table></div>'
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
           "added": "Устройство добавлено. Скачайте файл токена ниже и запустите его рядом с платой."}
    note = msg.get(request.query_params.get("msg", ""), "")
    body = (
        f'<p><a href="{BASE}/">← все устройства</a></p><h1>{esc(dev)}</h1>'
        + (f'<div class="box ok">{note}</div>' if note else "")
        + f'<form class="box" method="post">'
        f'<label><input type="checkbox" name="record"{" checked" if d.get("record", True) else ""}> '
        f"Запись включена</label><br><br>Формат <select name=\"format\">{fmt}</select> "
        f'Битрейт <select name="bitrate">{br}</select>'
        f'<h2>Расписание (время Алматы)</h2><label><input type="checkbox" name="use_schedule"'
        f'{" checked" if d.get("schedule") else ""}> писать только в часы работы</label>'
        f'<table style="width:auto">{rows}</table><p class="muted">Пустая строка — в этот день не писать. '
        f"23:00-02:00 — через полночь. Новые настройки — с начала следующего файла.</p>"
        f"<button>Сохранить</button></form>"
        f'<div class="box"><h2>Состояние</h2>'
        f'<p>{"Последняя связь: " + local(d["seen"]) if d.get("seen") else "Ещё не выходило на связь"}</p>'
        f'<table style="width:auto">{status}</table></div>'
        f'<div class="box">Токен: <a href="{BASE}/token/{esc(dev)}.bat" download>token_{esc(dev)}.bat</a> '
        f'<span class="muted">— запустить рядом с платой (папка pcclient)</span></div>'
        f"{files or '<p class=muted>Файлов пока нет</p>'}")
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
    devices.update(dev, record=bool(form.get("record")), format=fmt,
                   bitrate=bitrate if bitrate in devices.BITRATES else 128000, schedule=schedule)
    return RedirectResponse(f"{BASE}/device/{dev}?msg=saved", 303)


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
    Route("/token/{dev}.bat", token_bat),
    Route("/file/{dev}/{day}/{name}", file),
]
