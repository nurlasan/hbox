"""Симулятор бокса Hbox на ноутбуке: микрофон ноутбука вместо INMP441, папка вместо microSD, интернет вместо модема.

Делает то же, что плата (протокол — docs/protocol.md):
  - без токена просится на сервер (GET /register) — на сайте в «Новые платы» нажать «Принять»;
  - пишет MP3 моно 44.1 кГц часовыми файлами <канал>_YYYYMMDD_HHMMSS.mp3 (время начала UTC) в sim_data/;
  - раз в ≤5 мин спрашивает настройки и шлёт состояние (запись вкл/выкл, расписание, битрейт — с сайта);
  - отправляет закрытые файлы с докачкой и проверкой CRC;
  - если на сайте включён живой поток — досылает текущий файл каждые несколько секунд.

Нужен ffmpeg в PATH (Windows: winget install Gyan.FFmpeg). Только стандартная библиотека Python.

    python sim.py                    микрофон по умолчанию (первый найденный)
    python sim.py --list             список микрофонов
    python sim.py --mic "Микрофон (Realtek Audio)"
    python sim.py --server https://hbox.fabtech.uz   (по умолчанию)
    python sim.py --reset            забыть токен (попроситься заново)
"""

import argparse
import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
import zlib
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlparse

FW = "sim-1"
HERE = Path(__file__).resolve().parent
DATA = HERE / "sim_data"
STATE = HERE / "sim_state.json"
KEEP_SENT_S = 7 * 86400
CONFIG_EVERY_S = 300
PART = 1 << 20
PART_LIVE = 128 * 1024
LIVE_MIN = 16 * 1024
LIVE_MAX = 256 * 1024
NAME = re.compile(r"^[A-Za-z0-9-]{1,32}_\d{8}_\d{6}\.mp3$")

_KBPS = ((0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0),
         (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0))
_RATES = {3: (44100, 48000, 32000), 2: (22050, 24000, 16000), 0: (11025, 12000, 8000)}


def log(*a):
    print(datetime.now().strftime("%H:%M:%S"), *a, flush=True)


def frame_len(b, i: int) -> int:
    if i + 4 > len(b) or b[i] != 0xFF or b[i + 1] & 0xE0 != 0xE0:
        return 0
    ver, layer = b[i + 1] >> 3 & 3, b[i + 1] >> 1 & 3
    br, sr, pad = b[i + 2] >> 4, b[i + 2] >> 2 & 3, b[i + 2] >> 1 & 1
    kbps = _KBPS[0 if ver == 3 else 1][br]
    if ver == 1 or layer != 1 or sr == 3 or not kbps:
        return 0
    return (144 if ver == 3 else 72) * kbps * 1000 // _RATES[ver][sr] + pad


# --- микрофон ---

def ffmpeg_input(mic: str | None) -> list[str]:
    if mic == "tone":  # без микрофона: тон 440 Гц — проверить отправку и поток
        return ["-re", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100"]
    if sys.platform == "win32":
        return ["-f", "dshow", "-i", f"audio={mic or first_mic()}"]
    if sys.platform == "darwin":
        return ["-f", "avfoundation", "-i", f":{mic or 0}"]
    return ["-f", "pulse", "-i", mic or "default"]


def list_mics() -> list[str]:
    if sys.platform != "win32":
        return []
    r = subprocess.run(["ffmpeg", "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return re.findall(r'"([^"]+)" \(audio\)', r.stderr)


def first_mic() -> str:
    mics = list_mics()
    if not mics:
        sys.exit("Микрофон не найден (ffmpeg -list_devices true -f dshow -i dummy)")
    return mics[0]


class Recorder:
    """ffmpeg пишет MP3 в трубу; режем по кадрам на часовые файлы. Текущий — <имя>.part."""

    def __init__(self, mic):
        self.mic = mic
        self.lock = threading.Lock()
        self.proc = None
        self.bitrate = 128000
        self.cur = None  # {"name", "path", "f", "size", "start"}
        self.files = 0
        self.errors = 0
        self.channel = None

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self, channel: str, bitrate: int):
        self.stop()
        self.channel, self.bitrate = channel, bitrate
        cmd = (["ffmpeg", "-hide_banner", "-loglevel", "error"] + ffmpeg_input(self.mic) +
               ["-ac", "1", "-ar", "44100", "-c:a", "libmp3lame", "-b:a", f"{bitrate // 1000}k",
                "-write_xing", "0", "-id3v2_version", "0", "-write_id3v1", "0", "-f", "mp3", "pipe:1"])
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()
        log(f"запись: {bitrate // 1000} кбит/с, микрофон {self.mic or 'по умолчанию'}")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
        with self.lock:
            self._close()

    def _open(self, now: float):
        t = datetime.fromtimestamp(now, timezone.utc)
        name = f"{self.channel}_{t:%Y%m%d_%H%M%S}.mp3"
        path = DATA / (name + ".part")
        self.cur = {"name": name, "path": path, "f": open(path, "wb"), "size": 0, "start": now}
        self.files += 1

    def _close(self):
        if not self.cur:
            return
        c, self.cur = self.cur, None
        c["f"].close()
        if c["size"]:
            c["path"].replace(DATA / c["name"])
            log(f"файл записан: {c['name']} ({c['size'] / 1e6:.1f} МБ)")
        else:
            c["path"].unlink()

    def _read(self, proc):
        buf = b""
        while True:
            data = proc.stdout.read(4096)
            if not data:
                break
            buf += data
            if not frame_len(buf, 0) and len(buf) >= 4:
                j = buf.find(b"\xff", 1)  # не кадр — сдвиг до следующего заголовка
                buf = buf[j:] if j > 0 else b""
                self.errors += 1
                continue
            i = 0
            while (n := frame_len(buf, i)) and i + n <= len(buf):
                if proc is not self.proc:
                    return
                self._frame(buf[i:i + n])
                i += n
            buf = buf[i:]
        if proc is self.proc:
            log("ffmpeg остановился — нет микрофона?")
            self.errors += 1

    def _frame(self, fr: bytes):
        now = time.time()
        with self.lock:
            # новый час UTC — новый файл, как у платы
            if self.cur and int(now // 3600) != int(self.cur["start"] // 3600):
                self._close()
            if not self.cur:
                self._open(now)
            self.cur["f"].write(fr)
            self.cur["f"].flush()
            self.cur["size"] += len(fr)

    def live(self):
        with self.lock:
            return (self.cur["name"], self.cur["path"], self.cur["size"]) if self.cur else None


# --- сервер ---

class Server:
    def __init__(self, base: str):
        u = urlparse(base)
        self.host, self.port, self.https = u.hostname, u.port, u.scheme == "https"
        self.conn = None
        self.token = ""
        self.errors = 0
        self.sent_bytes = 0

    def request(self, method, path, body=b"", timeout=60):
        """(код, json) или (-1, {}) при обрыве"""
        for attempt in range(2):
            try:
                if not self.conn:
                    cls = http.client.HTTPSConnection if self.https else http.client.HTTPConnection
                    self.conn = cls(self.host, self.port, timeout=timeout)
                h = {"User-Agent": f"hbox/{FW}", "Content-Length": str(len(body))}
                if self.token:
                    h["Authorization"] = f"Bearer {self.token}"
                self.conn.request(method, "/hbox/v1" + path, body=body, headers=h)
                r = self.conn.getresponse()
                raw = r.read()
                if method == "PUT":
                    self.sent_bytes += len(body)
                try:
                    return r.status, json.loads(raw or b"{}")
                except ValueError:
                    return r.status, {}
            except (OSError, http.client.HTTPException) as e:
                self.conn = None
                if attempt:
                    self.errors += 1
                    log("нет связи с сервером:", e)
        return -1, {}


def crc_of(path: Path, size: int, start_crc=0, start=0) -> int:
    crc = start_crc
    with open(path, "rb") as f:
        f.seek(start)
        left = size - start
        while left > 0:
            b = f.read(min(left, 1 << 20))
            if not b:
                break
            crc = zlib.crc32(b, crc)
            left -= len(b)
    return crc


def verified(path: Path, size: int, check: str) -> tuple[int, int]:
    """сколько из принятого сервером совпадает с файлом (по контрольным CRC) и CRC до этого места"""
    good, good_crc, crc, pos = 0, 0, 0, 0
    if not check:
        return (size, crc_of(path, size)) if size else (0, 0)
    for item in check.split(","):
        off, want = item.split(":")
        off = int(off)
        crc = crc_of(path, off, crc, pos)
        pos = off
        if crc == int(want, 16):
            good, good_crc = off, crc
    return good, good_crc


class Sim:
    def __init__(self, args):
        self.args = args
        self.srv = Server(args.server)
        self.rec = Recorder(args.mic)
        self.st = json.loads(STATE.read_text()) if STATE.exists() else {}
        self.srv.token = self.st.get("token", "")
        self.mac = self.st.get("mac") or f"{uuid.getnode():012x}"
        self.imei = self.st.get("imei") or "99" + str(int(hashlib.sha1(self.mac.encode()).hexdigest(), 16))[:13]
        self.st.update(mac=self.mac, imei=self.imei)
        self.save()
        self.cfg = self.st.get("cfg", {"record": True, "bitrate": 128000, "channel": ""})
        self.live_on = False
        self.live = {"name": None, "off": -1, "crc": 0}
        self.config_at = 0
        self.t0 = time.time()
        self.sent_files = 0

    def save(self):
        STATE.write_text(json.dumps(self.st, ensure_ascii=False, indent=1))

    def pending(self) -> list[Path]:
        return sorted(p for p in DATA.glob("*.mp3") if NAME.match(p.name))

    def register(self):
        code, j = self.srv.request("GET", f"/register?imei={self.imei}&mac={self.mac}&fw={FW}")
        if code == 200 and j.get("token"):
            self.srv.token = self.st["token"] = j["token"]
            self.save()
            log("принят на сайте, токен получен")
            self.config_at = 0
        elif code == 202:
            log(f"ждёт на сайте: «Новые платы» → Принять (IMEI {self.imei}, MAC {self.mac})")
        else:
            log(f"регистрация не удалась ({code})")

    def config(self):
        pend = self.pending()
        du = shutil.disk_usage(DATA)
        q = {"rec": int(self.rec.running()), "files": self.rec.files, "pending": len(pend), "dropped": 0,
             "overflows": 0, "rec_errors": self.rec.errors, "cpu": 0, "csq": 31, "up_errors": self.srv.errors,
             "sent": self.sent_files, "kbps": 0, "uptime": int((time.time() - self.t0) / 60), "reset": 1,
             "sd_free": du.free >> 20, "sd_total": du.total >> 20,
             "pending_mb": sum(p.stat().st_size for p in pend) >> 20, "fw": FW, "op": "PC", "imei": self.imei}
        code, j = self.srv.request("GET", "/config?" + "&".join(f"{k}={v}" for k, v in q.items()))
        if code == 401:
            log("сервер не знает токен — прошусь заново")
            self.srv.token = self.st["token"] = ""
            self.save()
            return
        if code != 200:
            self.config_at = time.time() + 10
            return
        new = {"record": bool(j.get("record", True)), "bitrate": int(j.get("bitrate", 128000)),
               "channel": j.get("channel", "")}
        if new != self.cfg:
            log(f"настройки: канал {new['channel']}, запись {'вкл' if new['record'] else 'выкл'}, "
                f"{new['bitrate'] // 1000} кбит/с")
        changed = new["bitrate"] != self.cfg.get("bitrate") or new["channel"] != self.cfg.get("channel")
        self.cfg = self.st["cfg"] = new
        self.save()
        if j.get("live", False) != self.live_on:
            self.live_on = j.get("live", False)
            log("живой поток", "включён" if self.live_on else "выключен")
        if new["record"] and new["channel"] and (changed or not self.rec.running()):
            self.rec.start(new["channel"], new["bitrate"])
        elif not new["record"] and self.rec.running():
            self.rec.stop()
            log("запись выключена (с сайта или по расписанию)")
        self.config_at = time.time() + max(5, min(CONFIG_EVERY_S, int(j.get("recheck", CONFIG_EVERY_S))))

    def live_step(self) -> bool:
        """True — отправлен кусок"""
        cur = self.rec.live() if self.live_on else None
        if not cur:
            return False
        name, path, size = cur
        lv = self.live
        if lv["name"] != name:
            lv.update(name=name, off=-1, crc=0)
        if lv["off"] < 0:
            code, j = self.srv.request("GET", f"/files/{name}")
            if code != 200:
                return False
            if j.get("done"):
                lv["off"] = 1 << 62
                return False
            lv["off"], lv["crc"] = verified(path, min(j.get("size", 0), size), j.get("check", ""))
        if size - lv["off"] < LIVE_MIN:
            return False
        with open(path, "rb") as f:
            f.seek(lv["off"])
            body = f.read(min(size - lv["off"], LIVE_MAX))
        upto = zlib.crc32(body, lv["crc"])
        code, j = self.srv.request("PUT", f"/files/{name}?offset={lv['off']}&upto={upto:08x}", body)
        if code != 200 or j.get("size") != lv["off"] + len(body):
            log(f"живой кусок не принят ({code}) {j}")
            lv["off"] = -1
            return False
        lv["off"], lv["crc"] = lv["off"] + len(body), upto
        return True

    def send(self, path: Path) -> bool:
        """True — файл принят целиком"""
        name, total = path.name, path.stat().st_size
        code, j = self.srv.request("GET", f"/files/{name}")
        if code != 200:
            return False
        if not j.get("done"):
            crc_all = crc_of(path, total)
            off, crc = verified(path, min(j.get("size", 0), total), j.get("check", ""))
            t0, start = time.time(), off
            with open(path, "rb") as f:
                while True:
                    f.seek(off)
                    body = f.read(PART_LIVE if self.live_on else PART)
                    upto = zlib.crc32(body, crc)
                    code, j = self.srv.request(
                        "PUT", f"/files/{name}?offset={off}&total={total}&crc={crc_all:08x}&upto={upto:08x}", body)
                    if code != 200:
                        log(f"отправка {name} прервана ({code}) {j}")
                        return False
                    if j.get("done"):
                        break
                    off, crc = off + len(body), upto
                    if time.time() >= self.config_at:
                        self.config()
                    self.live_step()
            sec = time.time() - t0
            log(f"отправлен {name}: {(total - start) / 1e6:.1f} МБ за {sec:.0f} с "
                f"({(total - start) / 1024 / max(sec, 0.1):.0f} КБ/с)")
        sent = DATA / "sent"
        sent.mkdir(exist_ok=True)
        path.replace(sent / name)
        self.sent_files += 1
        return True

    def prune(self):
        for p in (DATA / "sent").glob("*.mp3"):
            if time.time() - p.stat().st_mtime > KEEP_SENT_S:
                p.unlink()

    def run(self):
        DATA.mkdir(exist_ok=True)
        # .part от прошлого запуска — оборванная запись: в очередь как есть
        for p in DATA.glob("*.mp3.part"):
            p.replace(p.with_suffix(""))
        log(f"симулятор Hbox, сервер {self.args.server}, папка {DATA}")
        # пишем сразу с прошлыми настройками, как плата, — даже без сети
        if self.cfg.get("record") and self.cfg.get("channel"):
            self.rec.start(self.cfg["channel"], self.cfg["bitrate"])
        status_at = 0
        try:
            while True:
                if not self.srv.token:
                    self.register()
                    time.sleep(30)
                    continue
                if time.time() >= self.config_at:
                    self.config()
                    self.prune()
                if time.time() >= status_at:
                    cur = self.rec.live()
                    log(f"состояние: запись {'идёт' if self.rec.running() else 'нет'}"
                        f"{' → ' + cur[0] if cur else ''}, ждут отправки {len(self.pending())}, "
                        f"отправлено {self.srv.sent_bytes / 1e6:.1f} МБ, живой поток "
                        f"{'вкл' if self.live_on else 'выкл'}")
                    status_at = time.time() + 60
                try:
                    if self.live_step():
                        continue
                except OSError:
                    self.live["off"] = -1  # файл закрылся на смене часа — дальше обычной отправкой
                pend = self.pending()
                if pend and self.send(pend[0]):
                    continue
                time.sleep(2 if not pend else 5)
        except KeyboardInterrupt:
            pass
        finally:
            self.rec.stop()
            log("остановлен")


def main():
    ap = argparse.ArgumentParser(description="Симулятор бокса Hbox на ноутбуке")
    ap.add_argument("--server", default="https://hbox.fabtech.uz")
    ap.add_argument("--mic", help='имя микрофона из --list; "tone" — тон 440 Гц вместо микрофона')
    ap.add_argument("--list", action="store_true", help="список микрофонов")
    ap.add_argument("--reset", action="store_true", help="забыть токен и попроситься заново")
    args = ap.parse_args()
    if not shutil.which("ffmpeg"):
        sys.exit("Нужен ffmpeg: winget install Gyan.FFmpeg (и открыть новое окно)")
    if args.list:
        print("\n".join(list_mics()) or "микрофонов не найдено")
        return
    if args.reset and STATE.exists():
        st = json.loads(STATE.read_text())
        st.pop("token", None)
        STATE.write_text(json.dumps(st))
    Sim(args).run()


if __name__ == "__main__":
    main()
