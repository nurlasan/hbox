"""Замер скорости модема (hbox-xsg): принимает POST /hbox/speedtest и считает скорость.

TLS снимает nginx на 18443 (deploy/nginx.conf), сюда приходит обычный HTTP. Плата шлёт тело потоком; сервер печатает раз в 5 с
и в конце: адрес, байты, секунды, КБ/с — и отвечает тем же одной строкой.
"""

import asyncio
import os
import time

PORT = int(os.environ.get("PORT", "8000"))


def log(*args):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *args, flush=True)


async def handle(reader, writer):
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=30)
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
        writer.close()
        return
    lines = head.decode("latin-1").split("\r\n")
    headers = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:] if l)}
    peer = headers.get("x-real-ip", writer.get_extra_info("peername")[0])
    left = int(headers.get("content-length", "0"))
    start = last = time.monotonic()
    total = last_total = 0
    log(f"{peer} {lines[0]} length={left}")
    try:
        while left > 0:
            chunk = await asyncio.wait_for(reader.read(min(left, 65536)), timeout=120)
            if not chunk:
                break
            total += len(chunk)
            left -= len(chunk)
            now = time.monotonic()
            if now - last >= 5:
                log(f"{peer} {total} B, now {(total - last_total) / (now - last) / 1024:.1f} KB/s")
                last, last_total = now, total
    except (asyncio.TimeoutError, ConnectionError) as e:
        log(f"{peer} {type(e).__name__}")
    sec = time.monotonic() - start
    result = f"{total} B in {sec:.1f} s = {total / max(sec, 1e-3) / 1024:.1f} KB/s"
    log(f"{peer} done {result}")
    body = (result + "\n").encode()
    writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n"
                 b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
    try:
        await writer.drain()
    except ConnectionError:
        pass
    writer.close()


async def main():
    server = await asyncio.start_server(handle, "0.0.0.0", PORT)
    log(f"listening on {PORT}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
