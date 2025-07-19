Haier Module
====================

Haier Air Conditioner [remote controller](https://github.com/memric/HaierModule) firmware.

Based on the [ESP32 Telegram bot](https://github.com/memric/ESP32_Telegrambot) project.

## Building

### Set up and configure the project

* Install and setup ESP-IDF. Run project configuration:

```
idf.py set-target esp32c3
idf.py menuconfig
```

* Set WiFi SSID & Password in *Project configuration* section.

* Set Telegram bot Token.

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py build
```

```
idf.py -p PORT flash
```

## Useful links and reference projects
- [Haier WiFi project](https://github.com/instalator/Haier_WiFi)
- [Управление кондиционером Haier на базе ESP8266](https://blog.instalator.ru/archives/433)
- [Haier Protocol project](https://github.com/paveldn/HaierProtocol)
