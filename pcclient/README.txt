Hbox — утилиты для Windows

1. Python 3.12 (если нет): в PowerShell
       winget install Python.Python.3.12

2. Прошивка ESP32-S3 SuperMini: bootloader.bin, partition-table.bin, hbox.bin рядом -> esp_flash.bat
   (токен в плате при перепрошивке сохраняется)
   (порт находится сам; иначе esp_flash.bat COM7).

3. Подключение (SuperMini):
   микрофон INMP441: WS=4, SCK=5, SD=6, L/R -> GND, VDD -> 3V3;
   модем ML307R: пины 12 и 13 (TX/RX можно перепутать - плата найдёт сама), GND общий,
   питание модема 5 В до 2 А;
   microSD YS-41 (позже): CS=10, SCK=7, MOSI=8, MISO=9, VCC -> 5V.

4. Токен устройства (один раз, хранится в плате): token.bat ТОКЕН
   Токен выдаёт сервер (файл .env, HBOX_DEVICES).

5. Работа: monitor.bat - вывод платы. Раз в минуту строка HBOX STATUS:
     rec=1 - идёт запись, files - сколько файлов записано, cpu - загрузка кодированием,
     overflows - потери звука (должно быть 0), pending - ждут отправки,
     dropped - выброшены (не успели отправить, должно быть 0),
     up - связь: idle (всё отправлено), send, wait (ошибка, повтор), modem/net/tls (подключение).
   Строки HBOX UP sent ... - файл ушёл на сервер, со скоростью.
   Клавиша s + Enter - состояние сейчас.
