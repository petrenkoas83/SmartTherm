# SmartTherm

Version 0.8.6.0


Open source for [SmartTherm](https://www.umkikit.ru/index.php?route=product/product&path=67&product_id=103) ESP8266/ESP32 OpenTherm controller

Use:
* code of [OpenTherm Library by ihormelnyk](https://github.com/ihormelnyk/opentherm_library)
* [AutoConnect Library by Hieromon](https://github.com/Hieromon/AutoConnect)
* [DS18B20 Library by robtillaart](https://github.com/RobTillaart/DS18B20_RT)

Build with [PlatformIO](https://platformio.org/)

### Опциональная поддержка HTTPS
Можно включить HTTPS-сервер на порту 443: при обращении по `https://IP/...` браузер перенаправляется на `http://IP/...`. Интерфейс (AutoConnect) продолжает работать по HTTP. Включение увеличивает размер прошивки примерно на 300 KB.

**ВАЖНО:** Прошивка с HTTPS может не поместиться в стандартный раздел приложения (1.31 MB) на платах ESP32 с 4 MB flash. Текущий размер с оптимизацией: ~1.49 MB.

**Варианты решения:**
1. **Использовать env `esp32devdeb_https`** — сборка с HTTPS, оптимизацией для размера и **минимальным набором веб-страниц** (`-DWEB_PAGES_MINIMAL`): только Setup (логин/пароль, диапазоны температуры, настройки MQTT). Остальное управление через MQTT. Размер: ~1.47 MB (превышение ~155 KB). Отключены страницы: Info, About, Debug, SetTemp, SetupAdd, SendBLOR, PID, Relay, OT2.
2. **Изменить partition table** — увеличить раздел приложения за счёт других разделов (например, уменьшить OTA или файловую систему).
3. **Отключить MQTT** — в `src/Smart_Config.h` изменить `#define MQTT_USE 0` для ESP32 (экономия ~50-100 KB, но теряется функциональность MQTT/PID).
4. **Использовать плату с большим flash** (8 MB или больше).

**Как включить HTTPS:**
1. Использовать env `esp32devdeb_https` в `platformio.ini` (уже настроен с оптимизацией и своей partition table).
2. В каталоге проекта уже есть `compat/esp32/hwcrypto/sha.h` для совместимости библиотеки с текущим ядром ESP32.
3. **Сертификат:** по умолчанию генерируется **на хосте при сборке** (RSA 2048) и встраивается в прошивку (`include/cert_embed.h`). Если файла нет — перед сборкой автоматически вызывается `scripts/generate_https_cert.sh` (нужны `openssl` и `python3`). Ручной запуск: `bash scripts/generate_https_cert.sh`. Раньше сертификат создавался на контроллере при первом запуске (до ~1 мин) — этот режим отключён для env `esp32devdeb_https`.

Features:
* [Captive portal](https://en.wikipedia.org/wiki/Captive_portal) before WiFi connection
* Web interface after WiFi connection
* [OpenTherm](https://en.wikipedia.org/wiki/OpenTherm) interface for Gas/Electric boiler contol (HVAC)
* [Personal cloud control](https://github.com/Evgen2/SmartServer) used
* [Android application for local/remote control](https://github.com/Evgen2/SmartThermClient) (betatest)
* TCP/UDP API interface
* up to 2 DS18B20 temperature sensors

0.8.6.0
* Web refactor: split Web.cpp into separate page modules (src/Web/*.cpp), Shared.hpp/Shared.cpp, SetupControls.hpp
* platformio: esp32devdeb environment
* Versioning: BiosDate in IdentifySelf and Info page

0.8.5.11 (Evgen2)
* DS18B20 disconnect detection, OpenTherm debug, small fixes

0.8.5.10 (Evgen2)
* AutoConnect/MQTT async behaviour (callbacks), WiFi/MQTT stability, CH2 for DHW fix

0.8.5.9
* delayed write config to flash after MQTT change mode or target temperature

0.8.5.8
* RTC watchdog

0.8.5.7
* watchdog
* disable brownout detector at startup
* reset_reason led indicator at sturtup, work if reset is not power/reset or sowtware reset
* check for nan and inf in pid

0.8.5.6
* bugfix + pidcontrol bugfix

0.8.5.5
* bugfix in [AutoConnect](https://github.com/Evgen2/AutoConnect)
* remote logging testing

0.8.5.4
* bugfixed, clean build with -Wall

0.8.5.3
* Pid & WCA fixes

0.8.5.2
* User can set CPU frequency 240/160/80MHz
  It is possible that a lower frequency will result in more stable operation of the controller
* AutoConnect 1.4.5
* Add base temperature for weather-compensated automation (WCA)
* Pid
** more aggressive dissipation of the integral for different signs of the error and the integral
** more fast heater start at setpoint and current temperature difference more than 2 degrees
* Planner fix
* Indication of a large number of OpenTherm errors if more 30% at webinterface

0.8.5.1
* ST2 combined mode

0.8.5
* The OpenTherm request cycle has been changed to request dynamic planner with two priority levels.
* support for
** DHWFlowRate (ID 19)
** TdhwSetUBTdhwSetLB (ID 48)
** MaxTSetUBMaxTSetLB (ID 49)
** MaxCapacityMinModLevel (ID15)

0.8.4
* Add remote OT log
* Close AP after conection to WiFi router after timeout

0.8.3
* Add support for slave OpenTherm interface
* Change MQTT server string up to 80 characters
* MQTT settings read/write to separate config file
* Add link to controller web page from HA MQTT device card
* Add effective modulation for the previous hour MQTT sensor

0.8.2
* Add build variant with onboard relay
* Add support for OT:MaxRelModLevelSetting
* Add support for OT:RemoteRequest (BLOR)

0.8.1
* At PID startup and room setpoint change recalculate the integral part of PID
  to speed up reaching the target setpoint. I.e start and restart PID with non zero integral

0.8.0
* TCP API changes for Andriod application & remote server support
* PID changes
* add use ID29 (Tstorage) as Indirect Water Heaters temperature for Buderus
* Immergas fix

0.7.5
* add  WinterMode (ID0:HB5) and  Use_OTC (ID0:HB3) support
* speedup OT startup ~2 sec
* add binary CH and HW sensors to MQTT
* MQTT connect after detecting boiler Capabilities if OT work
* MQTT connect to server without reset at MQTT config changes


0.7.4 changes
* PID + weather-compensated automation (standalone + HA)

0.7.3 changes
* fixed autoreconnect to WiFi

0.7.1 changes
* Add MQTT and MQTT discovery for home assistant

v 0.6 changes
* TCP/UDP interface, Windows/Linux application [SmartServer](https://github.com/Evgen2/SmartServer) for TCP/UDP API
* config saved and read after reboot
* Hot water and CH2 enabled
* Increased free RAM 


## License
Copyright (c) 2022-2024 Evgen2. Licensed under the [MIT license](/LICENSE?raw=true).