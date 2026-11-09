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
    assert client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json() == {"size": 120_000, "done": False}
    # повтор с неверной позиции — сервер говорит, сколько у него есть
    r = put(client, data[50_000:], 50_000, len(data), crc)
    assert r.status_code == 409 and r.json()["size"] == 120_000
    r = put(client, data[120_000:], 120_000, len(data), crc)
    assert r.json() == {"size": len(data), "done": True}
    assert (tmp_path / "cafe-1" / "2026-11-10" / NAME).read_bytes() == data
    assert not list((tmp_path / ".partial").rglob("*.part"))


def test_crc_mismatch_starts_over(client):
    data = os.urandom(10_000)
    r = put(client, data, 0, len(data), zlib.crc32(data) ^ 1)
    assert r.status_code == 422
    assert client.get(f"/hbox/v1/files/{NAME}", headers=AUTH).json() == {"size": 0, "done": False}


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
