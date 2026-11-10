@echo off
rem Прошивка ESP32-S3 SuperMini через USB-C. Порт - автоматически или: esp_flash.bat COM7
rem Части по своим адресам: токен устройства (NVS, 0x9000) не стирается. ota_data_initial.bin - загрузка
rem с первого слота (0x10000), даже если устройство уже обновлялось по воздуху во второй.
chcp 65001 >nul
cd /d "%~dp0"
if not exist partition-table.bin (
  echo Нет файлов bootloader.bin, partition-table.bin, hbox.bin рядом с esp_flash.bat
  pause
  exit /b 1
)
call :venv || goto :fail
set PORTARG=
set OTADATA=
if exist ota_data_initial.bin set OTADATA=0x3D0000 ota_data_initial.bin
if not "%1"=="" set PORTARG=--port %1
.venv\Scripts\python -m esptool --chip esp32s3 %PORTARG% write-flash --flash-size detect 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 hbox.bin %OTADATA% || goto :fail
echo Готово. Плата перезагрузится сама; если нет - нажмите RST.
pause
exit /b 0

:venv
rem .venv, которая не запускается (перенесённая, недосозданная), создаём заново
if exist .venv .venv\Scripts\python -c "" 2>nul || rmdir /s /q .venv
if not exist .venv (
  py -3 -m venv .venv || exit /b 1
)
.venv\Scripts\python -c "import serial, esptool" 2>nul || .venv\Scripts\python -m pip install --disable-pip-version-check -q -r requirements.txt
exit /b

:fail
echo Не получилось. Если порт не найден: зажмите BOOT, нажмите RST, отпустите BOOT и запустите снова.
pause
exit /b 1
