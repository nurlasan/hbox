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

from hbox_server import devices  # noqa: E402

ADMIN = ("admin", "pw")
SITE = {"Origin": "http://testserver"}


@pytest.fixture
def site(client, monkeypatch):
    monkeypatch.setenv("HBOX_ADMIN_PASSWORD", "pw")
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


def fake_image(version: str, project: str = "hbox", size: int = 40_000) -> bytes:
    """Похоже на образ ESP-IDF: 0xE9, esp_app_desc_t с версией и проектом."""
    blob = bytearray(os.urandom(size))
    blob[0] = 0xE9
    blob[32:36] = (0xABCD5432).to_bytes(4, "little")
    blob[48:80] = version.encode().ljust(32, b"\0")
    blob[80:112] = project.encode().ljust(32, b"\0")
    return bytes(blob)


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
    assert "ждёт обновления до abc1234" in site.get("/hbox/admin/device/cafe-1", auth=ADMIN).text
    # качает кусками
    fw = f"/hbox/v1/firmware/{ota['name']}"
    got = b"".join(site.get(fw, params={"offset": o, "len": 16384}, headers=AUTH).content
                   for o in range(0, len(blob), 16384))
    assert got == blob
    assert site.get(fw, params={"offset": 0, "len": 16384}).status_code == 401
    assert site.get(fw, params={"offset": 0, "len": 10**7}, headers=AUTH).status_code == 400
    whole = site.get(fw, params={"offset": 100, "len": len(blob)}, headers=AUTH).content
    assert whole == blob[100:]  # остаток после обрыва — одним ответом
    assert site.get("/hbox/v1/firmware/../devices.json", params={"offset": 0, "len": 9},
                    headers=AUTH).status_code == 404
    # обновилась — больше не предлагаем
    assert "ota_version" not in site.get("/hbox/v1/config?fw=abc1234", headers=AUTH).json()
    assert "обновлена до abc1234" in site.get("/hbox/admin/device/cafe-1", auth=ADMIN).text
    # откат: не предлагаем снова, на сайте — проблема
    assert "ota_version" not in site.get("/hbox/v1/config?fw=old1&fw_bad=abc1234", headers=AUTH).json()
    assert "прошивка abc1234 не запустилась" in site.get("/hbox/admin/", auth=ADMIN).text
    # мусор в версии не сохраняем
    site.get("/hbox/v1/config?pending=1&fw=a%20b", headers=AUTH)
    assert "fw" not in devices.load()["cafe-1"]["status"]
    site.post(url, data={"cancel": "1"}, headers=SITE, auth=ADMIN)
    assert not devices.load()["cafe-1"].get("ota")
