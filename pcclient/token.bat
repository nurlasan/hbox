@echo off
rem Записать в плату токен устройства: token.bat <токен> [COM7]
chcp 65001 >nul
cd /d "%~dp0"
if "%1"=="" (
  echo Укажите токен: token.bat ТОКЕН
  pause
  exit /b 1
)
if exist .venv .venv\Scripts\python -c "" 2>nul || rmdir /s /q .venv
if not exist .venv py -3 -m venv .venv
.venv\Scripts\python -c "import serial" 2>nul || .venv\Scripts\python -m pip install --disable-pip-version-check -q -r requirements.txt
.venv\Scripts\python monitor.py %2 --cmd "t %1"
pause
