import os
import time
import zlib

import pytest
from starlette.testclient import TestClient

from hbox_server import app as hbox

NAME = "cafe-1_20261110_090000.mp3"
AUTH = {"Authorization": "Bearer t1"}


@pytest.fixture
def client(tmp_path, monkeypatch):
    monkeypatch.setattr(hbox, "DATA", tmp_path)
    monkeypatch.setattr(hbox, "MIN_FREE_GB", 0)
    monkeypatch.setenv("HBOX_DEVICES", "cafe-1:t1,cafe-2:t2")
    with TestClient(hbox.app) as c:
        yield c


def put(client, data, offset, total, crc, name=NAME, auth=AUTH):
    return client.put(f"/hbox/v1/files/{name}", params={"offset": offset, "total": total,
                                                         "crc": f"{crc:08x}"},
                      content=data, headers=auth)


def test_whole_file(client, tmp_path):
    data = os.urandom(100_000)
    r = put(client, data, 0, len(data), zlib.crc32(data))
    assert r.json() == {"size": len(data), "done": True}
    assert (tmp_path / "cafe-1" / "2026-11-10" / NAME).read_bytes() == data
    assert client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json() == {"size": len(data), "done": True}


def test_resume_after_break(client, tmp_path):
    data = os.urandom(300_000)
    crc = zlib.crc32(data)
    # первый запрос оборвался после 120 000 байт
    r = put(client, data[:120_000], 0, len(data), crc)
    assert r.json() == {"size": 120_000, "done": False}
    r = client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json()
    assert r["size"] == 120_000 and r["done"] is False
    # дальше того, что есть, — нельзя: сервер говорит, сколько у него
    r = put(client, data[150_000:], 150_000, len(data), crc)
    assert r.status_code == 409 and r.json()["size"] == 120_000
    r = put(client, data[120_000:], 120_000, len(data), crc)
    assert r.json() == {"size": len(data), "done": True}
    assert (tmp_path / "cafe-1" / "2026-11-10" / NAME).read_bytes() == data
    assert not list((tmp_path / ".partial").rglob("*.part"))


def test_rewind_drops_broken_tail(client, tmp_path):
    data = os.urandom(200_000)
    crc = zlib.crc32(data)
    # обрыв: последние 8 КБ пришли испорченными (модем потерял байты)
    broken = data[:100_000] + os.urandom(8192)
    assert put(client, broken, 0, len(data), crc).json()["size"] == 108_192
    # устройство продолжает с последнего подтверждённого модемом места
    r = put(client, data[100_000:], 100_000, len(data), crc)
    assert r.json() == {"size": len(data), "done": True}
    assert (tmp_path / "cafe-1" / "2026-11-10" / NAME).read_bytes() == data


def good_points(check: str, data: bytes) -> list[int]:
    return [int(off) for off, crc in (item.split(":") for item in check.split(","))
            if int(crc, 16) == zlib.crc32(data[:int(off)])]


def test_checkpoints_find_broken_tail(client):
    data = os.urandom(200_000)
    broken = data[:100_000] + os.urandom(8192)
    put(client, broken, 0, len(data), zlib.crc32(data))
    r = client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json()
    offs = [int(item.split(":")[0]) for item in r["check"].split(",")]
    good = good_points(r["check"], broken)
    assert offs[-1] == 108_192 and good == offs  # CRC — по тому, что лежит на сервере
    # совпадают с файлом только точки до места порчи, и последняя — недалеко от неё
    good = good_points(r["check"], data)
    assert good == [o for o in offs if o <= 100_000] and good[-1] > 100_000 - 108_192 // 8


def test_checkpoints_cover_whole_file(tmp_path):
    data = os.urandom(1_000_000)
    part = tmp_path / "f.part"
    part.write_bytes(data)
    check = hbox.checkpoints(part, len(data), 600_000)
    offs = [int(item.split(":")[0]) for item in check.split(",")]
    assert offs[-1] == len(data) and offs == sorted(set(offs)) and 600_000 in offs
    assert len(offs) <= 14 and len(check) < 320  # буфер "check" на устройстве — 320 байт
    assert good_points(check, data) == offs


def test_rewind_stays_in_unverified_tail(client):
    """Обрыв посреди куска: порча в непроверенном хвосте не откатывает за проверенное."""
    data = os.urandom(4 << 20)
    crc, mb = zlib.crc32(data), 1 << 20
    for k in range(3):  # три куска по 1 МБ проверены по upto
        chunk = data[k * mb:(k + 1) * mb]
        r = client.put(f"/hbox/v1/files/{NAME}?offset={k * mb}&total={len(data)}&crc={crc:08x}"
                       f"&upto={zlib.crc32(data[:(k + 1) * mb]):08x}", content=chunk, headers=AUTH)
        assert r.json()["size"] == (k + 1) * mb
    # четвёртый оборвался: 600 КБ дошли, но с порчей на 200 КБ от начала куска
    bad = 3 * mb + 200_000
    tail = data[3 * mb:bad] + os.urandom(1000) + data[bad + 1000:3 * mb + 600_000]
    put(client, tail, 3 * mb, len(data), crc)
    r = client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json()
    good = good_points(r["check"], data)
    assert 3 * mb in good and bad - 600_000 // 8 < good[-1] <= bad


def test_parts_with_upto(client, tmp_path):
    data = os.urandom(300_000)
    crc = zlib.crc32(data)

    def part(a, b, body=None):
        return client.put(f"/hbox/v1/files/{NAME}", params={
            "offset": a, "total": len(data), "crc": f"{crc:08x}", "upto": f"{zlib.crc32(data[:b]):08x}"},
            content=data[a:b] if body is None else body, headers=AUTH)

    assert part(0, 100_000).json() == {"size": 100_000, "done": False}
    # кусок испорчен по дороге — выброшен только он
    bad = bytearray(data[100_000:200_000])
    bad[5] ^= 1
    r = part(100_000, 200_000, bytes(bad))
    assert r.status_code == 422 and r.json()["size"] == 100_000
    assert client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json()["size"] == 100_000
    assert part(100_000, 200_000).json()["size"] == 200_000
    assert part(200_000, 300_000).json() == {"size": 300_000, "done": True}
    assert (tmp_path / "cafe-1" / "2026-11-10" / NAME).read_bytes() == data


def test_crc_mismatch_starts_over(client):
    data = os.urandom(10_000)
    r = put(client, data, 0, len(data), zlib.crc32(data) ^ 1)
    assert r.status_code == 422
    assert client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json() == {"size": 0, "done": False,
                                                                         "check": ""}


def test_repeat_after_done_is_ok(client):
    data = os.urandom(1000)
    put(client, data, 0, len(data), zlib.crc32(data))
    r = put(client, data, 0, len(data), zlib.crc32(data))
    assert r.json() == {"size": 1000, "done": True}


def test_more_than_total(client):
    r = put(client, b"x" * 20, 0, 10, 0)
    assert r.status_code == 400


def test_auth_and_names(client):
    assert put(client, b"x", 0, 1, 0, auth={"Authorization": "Bearer nope"}).status_code == 401
    assert put(client, b"x", 0, 1, 0, name="../etc_20261110_090000.mp3").status_code in (400, 404)
    assert put(client, b"x", 0, 1, 0, name="cafe.wav").status_code == 400


def test_devices_are_separate(client, tmp_path):
    data = b"abc"
    put(client, data, 0, 3, zlib.crc32(data), auth={"Authorization": "Bearer t2"})
    assert (tmp_path / "cafe-2" / "2026-11-10" / NAME).exists()
    assert client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json()["done"] is False


def test_disk_full(client, monkeypatch):
    monkeypatch.setattr(hbox, "MIN_FREE_GB", 1e9)
    assert put(client, b"x", 0, 1, zlib.crc32(b"x")).status_code == 507


def test_config(client):
    c = client.get("/hbox/v1/config", headers=AUTH).json()
    assert c["channel"] == "cafe-1" and c["format"] == "mp3" and c["record"] is True


def test_cleanup_retention(client, tmp_path, monkeypatch):
    monkeypatch.setattr(hbox, "RETENTION_DAYS", 1)
    data = b"abc"
    put(client, data, 0, 3, zlib.crc32(data))
    f = tmp_path / "cafe-1" / "2026-11-10" / NAME
    hbox.cleanup(now=time.time())
    assert f.exists()
    hbox.cleanup(now=time.time() + 2 * 86400)
    assert not f.exists()


def test_cleanup_low_disk_removes_oldest(client, tmp_path, monkeypatch):
    for i, name in enumerate(["cafe-1_20261110_080000.mp3", "cafe-1_20261110_090000.mp3"]):
        put(client, b"a", 0, 1, zlib.crc32(b"a"), name=name)
        os.utime(tmp_path / "cafe-1" / "2026-11-10" / name, (1000 + i, 1000 + i))
    free = iter([0, 100])  # мало места, после первого удаления — хватает
    monkeypatch.setattr(hbox, "free_gb", lambda: next(free))
    hbox.cleanup(now=2000)
    left = [p.name for p in (tmp_path / "cafe-1" / "2026-11-10").iterdir()]
    assert left == ["cafe-1_20261110_090000.mp3"]


# --- устройства, расписание, сайт ---

from datetime import datetime  # noqa: E402

from hbox_server import admin, devices  # noqa: E402

ADMIN = ("admin", "pw")
SITE = {"Origin": "http://testserver"}


@pytest.fixture
def site(client, monkeypatch):
    monkeypatch.setenv("HBOX_ADMIN_PASSWORD", "pw")
    monkeypatch.setattr(admin, "_releases", (time.time(), []))  # без похода на GitHub
    return client


def at(s):
    return datetime.fromisoformat(s).replace(tzinfo=devices.TZ).timestamp()


def test_schedule():
    d = {"record": True, "schedule": ["10:00-23:00", "", "", "", "22:00-02:00", "", ""]}
    # 2026-11-09 — понедельник
    assert devices.recording_now(d, at("2026-11-09 09:59"))[0] is False
    assert devices.recording_now(d, at("2026-11-09 10:00"))[0] is True
    assert devices.recording_now(d, at("2026-11-09 22:59"))[0] is True
    assert devices.recording_now(d, at("2026-11-09 23:00"))[0] is False
    assert devices.recording_now(d, at("2026-11-10 12:00"))[0] is False  # вторник пустой
    assert devices.recording_now(d, at("2026-11-13 23:30"))[0] is True   # пятница через полночь
    assert devices.recording_now(d, at("2026-11-14 01:59"))[0] is True
    assert devices.recording_now(d, at("2026-11-14 02:00"))[0] is False
    # перед сменой — спросить снова к моменту смены
    state, recheck = devices.recording_now(d, at("2026-11-09 09:58"))
    assert state is False and 60 <= recheck <= 120
    assert devices.recording_now({"record": False, "schedule": None})[0] is False
    assert devices.recording_now({"record": True, "schedule": None}) == (True, devices.RECHECK_MAX)


def test_config_reports_status(client):
    r = client.get("/hbox/v1/config?rec=1&pending=2&csq=27&reset=9&bogus=x", headers=AUTH).json()
    assert r["recheck"] == devices.RECHECK_MAX
    d = devices.load()["cafe-1"]
    assert d["status"] == {"rec": 1, "pending": 2, "csq": 27, "reset": 9} and d["seen"] > 0


def test_site_needs_password(client, monkeypatch):
    monkeypatch.setenv("HBOX_ADMIN_PASSWORD", "pw")
    assert client.get("/hbox/admin/").status_code == 401
    assert client.get("/hbox/admin/", auth=("admin", "no")).status_code == 401


def test_site_flow(site, tmp_path):
    data = b"abc"
    put(site, data, 0, 3, zlib.crc32(data))
    page = site.get("/hbox/admin/", auth=ADMIN).text
    assert "cafe-1" in page and NAME in page

    r = site.post("/hbox/admin/add", data={"name": "bar-1"}, headers=SITE, auth=ADMIN,
                  follow_redirects=False)
    assert r.status_code == 303
    token = devices.load()["bar-1"]["token"]
    assert token in site.get("/hbox/admin/token/bar-1.bat", auth=ADMIN).text
    # новое устройство сразу принимается
    assert site.get("/hbox/v1/config", headers={"Authorization": f"Bearer {token}"}).json()["channel"] == "bar-1"

    form = {"record": "on", "format": "m4a", "bitrate": "96000", "use_schedule": "on",
            "day0": "10:00-23:00", **{f"day{i}": "" for i in range(1, 7)}}
    assert site.post("/hbox/admin/device/cafe-1", data=form, headers=SITE, auth=ADMIN,
                     follow_redirects=False).status_code == 303
    d = devices.load()["cafe-1"]
    assert d["format"] == "m4a" and d["bitrate"] == 96000 and d["schedule"][0] == "10:00-23:00"
    assert site.get("/hbox/v1/config", headers=AUTH).json()["format"] == "m4a"

    bad = {**form, "day1": "25:99"}
    site.post("/hbox/admin/device/cafe-1", data=bad, headers=SITE, auth=ADMIN)
    assert devices.load()["cafe-1"]["schedule"][1] == ""

    assert site.get(f"/hbox/admin/file/cafe-1/2026-11-10/{NAME}", auth=ADMIN).content == data
    assert site.get("/hbox/admin/file/cafe-1/2026-11-10/..%2F..%2Fdevices.json", auth=ADMIN).status_code == 404


def test_site_rejects_foreign_post(site):
    r = site.post("/hbox/admin/add", data={"name": "evil"}, headers={"Origin": "https://evil.example"},
                  auth=ADMIN)
    assert r.status_code == 403 and "evil" not in devices.load()


def test_site_shows_queue_and_sd(site):
    data = b"abc"
    put(site, data, 0, 3, zlib.crc32(data))
    site.get("/hbox/v1/config?pending=3&pending_mb=173&sd_free=1024&sd_total=30000&reset=9",
             headers=AUTH)
    page = site.get("/hbox/admin/", auth=ADMIN).text
    # 128 кбит/с = 57.6 МБ/ч: 173 МБ ≈ 3 ч; 1 ГБ — меньше суток записи
    assert "очередь 3.0 ч записи (173 МБ)" in page and "SD свободно 1.0 из 29 ГБ" in page
    assert "на SD места на 0.7 дн" in page and "просадка питания" in page
    assert "последний файл на сервере" in page
    site.get("/hbox/v1/config?pending=0&sd_free=0&sd_total=0", headers=AUTH)
    assert "нет SD-карты" in site.get("/hbox/admin/device/cafe-1", auth=ADMIN).text


def test_traffic_and_operator(site):
    assert "передачи ещё не было" in site.get("/hbox/admin/", auth=ADMIN).text
    data = os.urandom(3_000_000)
    put(site, data, 0, len(data), zlib.crc32(data))
    site.get("/hbox/v1/config?csq=20&op=KCELL&fw=abc", headers=AUTH)
    page = site.get("/hbox/admin/", auth=ADMIN).text
    assert "KCELL 4G" in page and "в среднем за сегодня" in page
    assert "сегодня 3 МБ · месяц 3 МБ · всего 3 МБ" in page
    tr = devices.load()["cafe-1"]["traffic"]
    assert tr["total"] == tr["today"] == len(data)
    # новые сутки: сегодня с нуля, всего копится
    devices.traffic("cafe-1", 1000, 1.0, now=time.time() + 86400 * 40)
    tr = devices.load()["cafe-1"]["traffic"]
    assert tr["today"] == tr["this_month"] == 1000 and tr["total"] == len(data) + 1000
    site.get("/hbox/v1/config?pending=1&op=bad%20op%22", headers=AUTH)
    assert "op" not in devices.load()["cafe-1"]["status"]


def test_device_tabs(site):
    page = site.get("/hbox/admin/device/cafe-1", auth=ADMIN).text
    assert 'class="tab on" href="/hbox/admin/device/cafe-1?tab=state"' in page and "Подробно" in page
    assert "Сохранить" not in page
    assert "Сохранить" in site.get("/hbox/admin/device/cafe-1?tab=settings", auth=ADMIN).text
    assert "Загрузить и обновить" in site.get("/hbox/admin/device/cafe-1?msg=ota", auth=ADMIN).text
    assert "Подробно" in site.get("/hbox/admin/device/cafe-1?tab=nonsense", auth=ADMIN).text


def fake_image(version: str, project: str = "hbox", size: int = 40_000) -> bytes:
    """Похоже на образ ESP-IDF: 0xE9, esp_app_desc_t с версией и проектом."""
    blob = bytearray(os.urandom(size))
    blob[0] = 0xE9
    blob[32:36] = (0xABCD5432).to_bytes(4, "little")
    blob[48:80] = version.encode().ljust(32, b"\0")
    blob[80:112] = project.encode().ljust(32, b"\0")
    return bytes(blob)


def test_firmware_from_github(site, monkeypatch):
    blob = fake_image("fed1234")
    rel = {"version": "fed1234", "title": "fed1234: Clock fix", "date": "2026-11-12T09:00:00Z",
           "url": "https://github.com/x/releases/download/fw-fed1234/hbox.bin", "zip": "https://x/z.zip"}
    monkeypatch.setattr(admin, "_releases", (time.time(), [rel]))
    monkeypatch.setattr(admin, "_get", lambda url, limit: blob if url == rel["url"] else b"")
    page = site.get("/hbox/admin/device/cafe-1?tab=fw", auth=ADMIN).text
    assert "fed1234: Clock fix" in page and 'name="release" value="fed1234"' in page and "zip для USB" in page
    r = site.post("/hbox/admin/device/cafe-1/firmware", data={"release": "fed1234"}, headers=SITE, auth=ADMIN,
                  follow_redirects=False)
    assert r.status_code == 303 and devices.load()["cafe-1"]["ota"]["version"] == "fed1234"
    bad = site.post("/hbox/admin/device/cafe-1/firmware", data={"release": "nope"}, headers=SITE, auth=ADMIN)
    assert "Нет такой сборки" in bad.text


def test_api_under_cloudflare_prefix(client):
    data = b"xyz"
    r = client.put(f"/hbox/admin/dev/v1/files/{NAME}", params={"offset": 0, "total": 3, "crc": f"{zlib.crc32(data):08x}"},
                   content=data, headers=AUTH)
    assert r.json()["done"] is True
    assert client.get("/hbox/admin/dev/v1/config", headers=AUTH).status_code == 200
    assert client.get("/hbox/admin/dev/v1/config").status_code == 401


def test_route_setting(site):
    assert "route" not in site.get("/hbox/v1/config", headers=AUTH).json()
    devices.update("cafe-1", route="cf")
    assert site.get("/hbox/v1/config", headers=AUTH).json()["route"] == "cf"
    assert 'value="cf" selected' in site.get("/hbox/admin/device/cafe-1?tab=settings", auth=ADMIN).text


def test_ota_flow(site):
    blob = fake_image("abc1234")
    url = "/hbox/admin/device/cafe-1/firmware"
    bad = site.post(url, files={"firmware": ("x.bin", fake_image("v", project="other"))}, headers=SITE,
                    auth=ADMIN)
    assert "не прошивка Hbox" in bad.text and not devices.load()["cafe-1"].get("ota")
    r = site.post(url, files={"firmware": ("hbox.bin", blob)}, headers=SITE, auth=ADMIN,
                  follow_redirects=False)
    assert r.status_code == 303
    ota = devices.load()["cafe-1"]["ota"]
    assert ota == {"name": f"{zlib.crc32(blob):08x}.bin", "size": len(blob), "crc": zlib.crc32(blob),
                   "version": "abc1234"}
    # старая прошивка получает назначение
    r = site.get("/hbox/v1/config?fw=old1", headers=AUTH).json()
    assert r["ota_version"] == "abc1234" and r["ota_size"] == len(blob)
    assert r["ota_crc"] == f"{zlib.crc32(blob):08x}" and r["ota_name"] == ota["name"]
    assert "ждёт обновления до abc1234" in site.get("/hbox/admin/device/cafe-1?tab=fw", auth=ADMIN).text
    # качает кусками
    fw = f"/hbox/v1/firmware/{ota['name']}"
    got = b"".join(site.get(fw, params={"offset": o, "len": 16384}, headers=AUTH).content
                   for o in range(0, len(blob), 16384))
    assert got == blob
    assert site.get(fw, params={"offset": 0, "len": 16384}).status_code == 401
    assert site.get(fw, params={"offset": 0, "len": 10**7}, headers=AUTH).status_code == 400
    whole = site.get(fw, params={"offset": 100, "len": len(blob)}, headers=AUTH).content
    assert whole == blob[100:]  # остаток после обрыва — одним ответом
    blocks = site.get(fw, params={"blocks": 1}, headers=AUTH).text.split(",")
    assert blocks == [f"{zlib.crc32(blob[i:i + 32768]):08x}" for i in range(0, len(blob), 32768)]
    assert site.get("/hbox/v1/firmware/../devices.json", params={"offset": 0, "len": 9},
                    headers=AUTH).status_code == 404
    # обновилась — больше не предлагаем
    assert "ota_version" not in site.get("/hbox/v1/config?fw=abc1234", headers=AUTH).json()
    assert "обновлена до abc1234" in site.get("/hbox/admin/device/cafe-1?tab=fw", auth=ADMIN).text
    # встала — назначение снято: после прошивки по USB другой версии старую не вернём
    assert not devices.load()["cafe-1"].get("ota")
    assert "ota_version" not in site.get("/hbox/v1/config?fw=newer9", headers=AUTH).json()
    # откат: не предлагаем снова, на сайте — проблема
    devices.update("cafe-1", ota=ota)
    assert "ota_version" not in site.get("/hbox/v1/config?fw=old1&fw_bad=abc1234", headers=AUTH).json()
    assert "прошивка abc1234 не запустилась" in site.get("/hbox/admin/", auth=ADMIN).text
    # мусор в версии не сохраняем
    site.get("/hbox/v1/config?pending=1&fw=a%20b", headers=AUTH)
    assert "fw" not in devices.load()["cafe-1"]["status"]
    site.post(url, data={"cancel": "1"}, headers=SITE, auth=ADMIN)
    assert not devices.load()["cafe-1"].get("ota")


def test_signal_icon():
    from hbox_server import admin

    assert admin.signal_bars(26) == 4 and admin.signal_bars(12) == 2 and admin.signal_bars(99) == 0
    svg = admin.signal_icon(12)
    assert svg.count("<rect") == 4 and svg.count("/>") == 4 and svg.count('class="on"') == 2
    off = admin.signal_icon(26, "ALTEL", online=False)
    assert 'class="on"' not in off and 'class="sig off"' in off


def mp3_file(path, seconds):
    """CBR MP3 128 кбит/с: заголовок кадра MPEG-1 Layer III и нужный размер."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\xff\xfb\x90\x64" + b"\0" * (int(seconds * 16000) - 4))


def test_hour_report(site, tmp_path, monkeypatch):
    from hbox_server import hours
    day = tmp_path / "cafe-1" / "2026-11-12"
    # UTC: 05:00 целиком, 06:00 нет (питание), 07:20–08:00, 08:00 — передаётся
    mp3_file(day / "cafe-1_20261112_050000.mp3", 3600)
    mp3_file(day / "cafe-1_20261112_072000.mp3", 2400)
    part = tmp_path / ".partial" / "cafe-1"
    part.mkdir(parents=True)
    (part / "cafe-1_20261112_080000.mp3.part").write_bytes(b"\0" * 16000 * 900)
    (part / "cafe-1_20261112_080000.mp3.json").write_text('{"total": %d}' % (16000 * 3600))
    now = at("2026-11-12 13:30")  # 08:30 UTC
    devices.update("cafe-1", seen=int(now), status={"rec": 1, "uptime": 70, "reset": 1},
                   boots=[{"t": int(at("2026-11-12 12:20")), "reset": 1}])
    d = devices.load()["cafe-1"]
    rows = {datetime.fromtimestamp(r["start"], devices.TZ).hour: r
            for r in hours.hours(d, hours.pieces(tmp_path, "cafe-1", 128000), now)}
    assert rows[10]["state"] == ("sent", "передан") and round(rows[10]["rec"]) == 3600
    assert round(rows[10]["mb"], 1) == 57.6
    assert rows[11]["state"][0] == "gap" and rows[11]["notes"] == ["не было питания"]
    assert rows[12]["state"][0] == "partial" and round(rows[12]["rec"]) == 2400
    assert rows[13]["state"] == ("sending", "передаётся 25%")
    monkeypatch.setattr(time, "time", lambda: now)
    page = site.get("/hbox/admin/device/cafe-1?tab=hours", auth=ADMIN).text
    assert "передан частично" in page and "не было питания" in page and "40 мин" in page


def test_boots_recorded(client):
    client.get("/hbox/v1/config?rec=1&uptime=100&reset=1", headers=AUTH)
    client.get("/hbox/v1/config?rec=1&uptime=101&reset=1", headers=AUTH)
    client.get("/hbox/v1/config?rec=1&uptime=0&reset=4", headers=AUTH)
    assert [b["reset"] for b in devices.load()["cafe-1"]["boots"]] == [1, 4]


def test_register_by_imei(site):
    q = {"imei": "861234567890123", "mac": "A0B1C2D3E4F5", "fw": "abc1234"}
    assert site.get("/hbox/v1/register", params={**q, "imei": "x"}).status_code == 400
    assert site.get("/hbox/v1/register", params=q).status_code == 202
    page = site.get("/hbox/admin/", auth=ADMIN).text
    assert "861234567890123" in page and 'value="hbox-1"' in page
    # чужое имя не отдаём
    site.post("/hbox/admin/approve", data={"imei": q["imei"], "name": "cafe-1"}, headers=SITE, auth=ADMIN)
    assert devices.load()["cafe-1"]["token"] == "t1"
    assert site.post("/hbox/admin/approve", data={"imei": q["imei"], "name": "hbox-1"}, headers=SITE, auth=ADMIN,
                     follow_redirects=False).status_code == 303
    # другая плата с тем же IMEI токен не получает
    assert site.get("/hbox/v1/register", params={**q, "mac": "000000000000"}).status_code == 202
    token = site.get("/hbox/v1/register", params=q).json()["token"]
    assert devices.load()["hbox-1"]["imei"] == q["imei"]
    auth = {"Authorization": f"Bearer {token}"}
    assert site.get("/hbox/v1/config", params={"imei": q["imei"]}, headers=auth).json()["channel"] == "hbox-1"
    assert devices.requests() == {}  # забрала — заявка снята
    # плата потеряла токен: снова просится, предлагается прежнее имя, токен новый
    site.get("/hbox/v1/register", params=q)
    assert devices.requests()[q["imei"]]["was"] == "hbox-1"
    site.post("/hbox/admin/approve", data={"imei": q["imei"], "name": "hbox-1"}, headers=SITE, auth=ADMIN)
    assert site.get("/hbox/v1/register", params=q).json()["token"] != token
    assert site.get("/hbox/v1/config", headers=auth).status_code == 401
    # отклонить
    site.get("/hbox/v1/register", params={**q, "imei": "869999999999999"})
    site.post("/hbox/admin/reject", data={"imei": "869999999999999"}, headers=SITE, auth=ADMIN)
    assert "869999999999999" not in devices.requests()


def test_skip_queue(site):
    old, new = "cafe-1_20261110_090000.mp3", "cafe-1_20991110_090000.mp3"
    assert site.post("/hbox/admin/device/cafe-1/skip", headers=SITE, auth=ADMIN,
                     follow_redirects=False).status_code == 303
    assert site.get(f"/hbox/v1/files/{old}", headers=AUTH).json() == {"size": 0, "done": True}
    assert put(site, b"abc", 0, 3, zlib.crc32(b"abc"), name=old).json()["done"] is True
    assert not list((hbox.DATA / "cafe-1").rglob("*.mp3"))
    assert site.get(f"/hbox/v1/files/{new}", headers=AUTH).json()["done"] is False
    assert "Очистить очередь" in site.get("/hbox/admin/device/cafe-1", auth=ADMIN).text


def mp3_frames(n):
    """n кадров MP3 128 кбит/с 44.1 кГц (417 байт, без бита дополнения)"""
    return b"".join(b"\xff\xfb\x90\x64" + bytes([i % 256]) * 413 for i in range(n))


def test_live_stream(site, monkeypatch):
    from hbox_server import stream
    monkeypatch.setattr(stream, "IDLE_CLOSE_S", 0)
    monkeypatch.setattr(stream, "POLL_S", 0.01)
    url = "/hbox/v1/stream/cafe-1.mp3"
    assert site.get(url, params={"key": "x"}).status_code == 401
    # ключ создаётся на сайте; устройство узнаёт, что нужно досылать живой звук
    site.post("/hbox/admin/device/cafe-1/stream", headers=SITE, auth=ADMIN, follow_redirects=False)
    key = hbox.devices.load()["cafe-1"]["stream_key"]
    assert site.get("/hbox/v1/config", headers=AUTH).json()["live"] is True
    assert key in site.get("/hbox/admin/device/cafe-1", auth=ADMIN).text
    assert site.get(url, params={"key": key}).status_code == 503  # ещё ничего не прислало
    # живые куски без total; второй обрывается посреди кадра
    data = mp3_frames(30)
    live = "cafe-1_20261110_090000.mp3"
    for a, b in ((0, 5000), (5000, 9000)):
        r = site.put(f"/hbox/v1/files/{live}", params={"offset": a}, content=data[a:b], headers=AUTH)
        assert r.json() == {"size": b, "done": False}
    got = site.get(url, params={"key": key}).content
    assert got == data[:9000 // 417 * 417]  # только целые кадры
    assert site.get(url, headers={"Authorization": f"Bearer {key}"}).status_code == 200
    # конец часа: устройство дошлёт файл обычным образом
    r = put(site, data[9000:], 9000, len(data), zlib.crc32(data), name=live)
    assert r.json() == {"size": len(data), "done": True}
    site.post("/hbox/admin/device/cafe-1/stream/off", headers=SITE, auth=ADMIN, follow_redirects=False)
    assert site.get(url, params={"key": key}).status_code == 401
    assert "live" not in site.get("/hbox/v1/config", headers=AUTH).json()


def test_stream_sync():
    from hbox_server import stream
    data = b"\x00\xff\x12" + mp3_frames(3)
    assert stream.sync(data) == 3 and stream.frame_len(data, 3) == 417
