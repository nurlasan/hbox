"""Замер скорости модема (hbox-xsg): TLS-сокет принимает поток байт и считает скорость.

Плата открывает TLS-соединение и шлёт данные; сервер печатает раз в 5 с и в конце:
адрес, байты, секунды, КБ/с. Ответ не нужен — подтверждение доставки плата берёт из TCP ACK
(AT+MIPSACK у ML307R).
"""

import asyncio
import os
import ssl
import time

PORT = int(os.environ.get("PORT", "18600"))
TLS_DIR = os.environ.get("TLS_DIR", "/tls")


def log(*args):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *args, flush=True)


async def handle(reader, writer):
    peer = writer.get_extra_info("peername")[0]
    start = last = time.monotonic()
    total = last_total = 0
    log(f"{peer} connected")
    try:
        while chunk := await asyncio.wait_for(reader.read(65536), timeout=120):
            total += len(chunk)
            now = time.monotonic()
            if now - last >= 5:
                log(f"{peer} {total} B, now {(total - last_total) / (now - last) / 1024:.1f} KB/s")
                last, last_total = now, total
    except (asyncio.TimeoutError, ConnectionError, ssl.SSLError) as e:
        log(f"{peer} {type(e).__name__}")
    sec = time.monotonic() - start
    log(f"{peer} done {total} B in {sec:.1f} s = {total / max(sec, 1e-3) / 1024:.1f} KB/s")
    writer.close()


async def main():
    ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ctx.load_cert_chain(f"{TLS_DIR}/server.crt", f"{TLS_DIR}/server.key")
    server = await asyncio.start_server(handle, "0.0.0.0", PORT, ssl=ctx)
    log(f"listening on {PORT}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
