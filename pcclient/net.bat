@echo off
rem Тест скорости модема: запускает замер и показывает вывод платы. Порт: net.bat COM7
chcp 65001 >nul
cd /d "%~dp0"
if exist .venv .venv\Scripts\python -c "" 2>nul || rmdir /s /q .venv
if not exist .venv py -3 -m venv .venv
.venv\Scripts\python -c "import serial" 2>nul || .venv\Scripts\python -m pip install --disable-pip-version-check -q -r requirements.txt
.venv\Scripts\python monitor.py %*
pause
