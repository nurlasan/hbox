# Сервер-приёмник Hbox

`hbox_server/app.py` — приём часовых файлов с докачкой, настройки устройства, замер скорости.
Протокол — `docs/protocol.md`. Перед ним nginx (`deploy/nginx.conf`) на 18443 снимает TLS.

```bash
docker compose -p hbox up -d --build                       # из корня репозитория, нужен .env (см. .env.example)
docker build -t hbox-server:dev server && docker run --rm hbox-server:dev python -m pytest -q
```

Файлы — `data/<канал>/<YYYY-MM-DD>/`, недокачанные — `data/.partial/`. Чистка раз в 10 минут:
старше `HBOX_RETENTION_DAYS` и самые старые, если свободно меньше `HBOX_MIN_FREE_GB` + 1 ГБ.
