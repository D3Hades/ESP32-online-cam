# ESP32-online-cam
Программа выводит видеопоток камеры с помощью http сервера  
Так же есть возможно начать/остановить запись на SD карту  
Сохраняет кадры в папку с названием содержащим время начала записи  
Например: /2025-02-15_12-34-56/00001.jpg

В веб странице есть кнопка которая на 3 секунды подает сигнал на реле  
ESP32 подключена к wifi, данные для подключения находятся ниже  
Прошивка делалась для ESP32-CAM с 4 МБ PSRAM и камерой OV2640  
Возможны ошибки, работа прошивки не проверялась!  
