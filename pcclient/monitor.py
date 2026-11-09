"""Вывод платы Hbox в окне. Команда при запуске (по умолчанию «n» — тест модема сейчас).

    monitor.py [COM7] [--cmd n]
"""

import argparse
import sys
import time

import serial
from serial.tools import list_ports

ESP_USB_VID = 0x303A


def find_port():
    for p in list_ports.comports():
        if p.vid == ESP_USB_VID:
            return p.device
    sys.exit("Плата не найдена. Укажите порт: net.bat COM7")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--cmd", default="n")
    args = ap.parse_args()
    port = args.port or find_port()
    print(f"Порт {port}. Ctrl+C - выход.")
    with serial.Serial(port, 115200, timeout=1) as s:
        if args.cmd:
            s.write(args.cmd.encode())
        while True:
            line = s.readline().decode("utf-8", "replace").rstrip()
            if line:
                print(time.strftime("%H:%M:%S"), line, flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
