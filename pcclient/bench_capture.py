"""Принимает с платы результат теста кодировщиков (прошивка hbox, bench.c).

Плата раз в минуту пишет 10 с MP3 и 10 с AAC и выгружает их в консоль USB:
    HBOX BENCH codec=mp3 ... cpu=..%
    HBOX FILE bench.mp3 <байт>
    <base64>...
    HBOX END <crc32>
Скрипт печатает строки HBOX BENCH и сохраняет файлы рядом с собой.
"""

import argparse
import base64
import sys
import zlib
from pathlib import Path

import serial
from serial.tools import list_ports

ESP_USB_VID = 0x303A


def find_port():
    for p in list_ports.comports():
        if p.vid == ESP_USB_VID:
            return p.device
    sys.exit("Плата не найдена. Укажите порт: bench.bat COM7")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent))
    args = ap.parse_args()
    port = args.port or find_port()
    want = {"bench.mp3", "bench.aac"}
    print(f"Порт {port}. Жду замер (до минуты)...")

    with serial.Serial(port, 115200, timeout=90) as s:
        name, size, chunks = None, 0, []
        while want:
            raw = s.readline()
            if not raw:
                sys.exit("Плата молчит 90 с. Нажмите RST и запустите снова.")
            line = raw.decode("ascii", "replace").strip()
            if name is None:
                if line.startswith("HBOX FILE "):
                    _, _, name, size = line.split()
                    size, chunks = int(size), []
                elif line.startswith("HBOX"):
                    print(line)
            elif line.startswith("HBOX END "):
                data = base64.b64decode("".join(chunks))
                crc = int(line.split()[2], 16)
                if len(data) != size or zlib.crc32(data) != crc:
                    print(f"{name}: битый приём ({len(data)} из {size} байт), жду следующий")
                else:
                    path = Path(args.out) / name
                    path.write_bytes(data)
                    print(f"Сохранён {path} ({size} байт)")
                    want.discard(name)
                name = None
            else:
                chunks.append(line)


if __name__ == "__main__":
    main()
