@echo off
rem Тест кодировщиков: ждёт замер с платы, сохраняет bench.mp3 и bench.aac. Порт: bench.bat COM7
chcp 65001 >nul
cd /d "%~dp0"
if exist .venv .venv\Scripts\python -c "" 2>nul || rmdir /s /q .venv
if not exist .venv py -3 -m venv .venv
.venv\Scripts\python -c "import serial" 2>nul || .venv\Scripts\python -m pip install --disable-pip-version-check -q -r requirements.txt
.venv\Scripts\python bench_capture.py %*
pause
