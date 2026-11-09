Hbox — утилиты для Windows

1. Python 3.12 (если нет): в PowerShell
       winget install Python.Python.3.12

2. Прошивка ESP32-S3 SuperMini: положить hbox.bin рядом и запустить esp_flash.bat
   (порт находится сам; иначе esp_flash.bat COM7).

3. Тест кодировщиков (hbox-stf): микрофон INMP441 на WS=4, SCK=5, SD=6, L/R -> GND.
   Включить музыку рядом, запустить bench.bat. Через минуту в окне строки вида
       HBOX BENCH codec=mp3 ... cpu=35.2% worst_ms=... overflows=0
   и файлы bench.mp3, bench.m4a рядом - их можно послушать.
   cpu - доля ядра на кодирование (до 50% хорошо), overflows - потери звука (должно быть 0).
