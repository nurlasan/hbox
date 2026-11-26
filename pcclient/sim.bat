@echo off
rem Симулятор бокса Hbox на ноутбуке: микрофон ноутбука, отправка на сервер, живой поток.
rem   sim.bat                  — микрофон по умолчанию
rem   sim.bat --list           — список микрофонов
rem   sim.bat --mic "Имя"      — выбрать микрофон;  sim.bat --mic tone — тон вместо микрофона
rem   sim.bat --reset          — забыть токен и попроситься на сайт заново
chcp 65001 >nul
cd /d "%~dp0"
where ffmpeg >nul 2>nul || (
  echo Нужен ffmpeg. В PowerShell: winget install Gyan.FFmpeg  — потом открыть новое окно.
  pause
  exit /b 1
)
set PYTHONIOENCODING=utf-8
py -3 sim.py %*
pause
