FROM python:3.11-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY hbox_server hbox_server
COPY tests tests
CMD ["uvicorn", "hbox_server.app:app", "--host", "0.0.0.0", "--port", "8000", "--proxy-headers"]
