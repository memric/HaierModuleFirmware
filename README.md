Haier Module
====================

ESP-IDF based firmware for the Haier air conditioner remote control. It includes the MQTT client to integrate with popular smart home solutions.

The firmware is designed for use with [remote controller](https://github.com/memric/HaierModule) but can be easily ported to other ESP32-based hardware.

## Haier protocol

**Common frame structure (known fields)**

| Index  | Value | Description            | Note                                                                | Supported |
|--------|-------|------------------------|---------------------------------------------------------------------|-----------|
| 0      | 0xFF  | Frame separator        |                                                                     |           |
| 1      | 0xFF  | Frame separator        |                                                                     |           |
| 2      |       | Payload length         | Packet size - 3 bytes of Header and Checksum                        |           |
| 3      | 0x00  | Dummy byte             |                                                                     |           |
| 4      | 0x00  | Dummy byte             |                                                                     |           |
| 5      | 0x00  | Dummy byte             |                                                                     |           |
| 6      | 0x00  | Dummy byte             |                                                                     |           |
| 7      | 0x00  | Dummy byte             |                                                                     |           |
| 8      | 0x01  | Dummy byte             |                                                                     |           |
| 9      |       | Frame type (direction) | 0x01 - Request, 0x02 - Responce                                     |           |
| 10     |       | Frame type (direction) | 0x4D - Request, 0x6D - Responce                                     |           |
| 11     |       | Command                | 0x5F on request                                                     |           |
| 12     |       | ?                      |                                                                     |           |
| 13     |       | Current temperature    |                                                                     | ✅        |
| 14..22 |       | ?                      |                                                                     |           |
| 23     |       | Mode                   | 0x00 - Smart, 0x01 - Cool, 0x02 - Heat, 0x03 - Fan only, 0x04 - Dry | ✅        |
| 24     |       | ?                      |                                                                     |           |
| 25     |       | Fan speed              | 0x00 - Max, 0x01 - Medium, 0x02 - Min, 0x03 - Auto                  | ✅        |
| 26     |       | ?                      |                                                                     |           |
| 27     |       | Deflector              | 0x00 - Stationary, 0x01 - Vertical, 0x02 - Horizontal, 0x03 - Auto  | ✅        |
| 28     |       | ?                      | ? Pult lock ?                                                       | ❌        |
| 29     |       | Enabled                | 0x00 - Off, 0x_1 - On, ...?                                         | ✅        |
| 30     |       | ?                      |                                                                     |           |
| 31     |       | Fresh ?                | 0x00 - Off, 0x01 - On                                               | ❌        |
| 32..34 |       | ?                      |                                                                     |           |
| 35     |       | Temperature setting    | Desired temperature - 16º                                           | ✅        |
| 36     |       | Checksum               | The sum of bytes above                                              |           |

## MQTT

The device periodically sends MQTT messages using the configured topic (by default, `haier`) and the `sys` and `state` subtopics.

The `haier/sys` topic includes a paylod in JSON format which contains some system information:

```json
{"AppVer":"0.1.0","IdfVer":"v6.0","LifeTime":79317,"ResetReason":4,"ReconnCntr":1,"Temp":36,"IP":"192.168.1.100","WiFiRSSI":-27}
```

The `haier/state` topic includes a paylod in JSON format which contains AC state:

```json
{"Enabled":0,"TempCurr":26,"TempSet":24,"Mode":1,"Fan":3,"Deflector":2}
```

The `set/#` subtopic is used for control commands. For example to set AC desired temperature send the tempemperature value with topic `haier/set/temp`.

The table below contains a list of supported command topics.

| Topic                 | Value                                        | Description                  |
|-----------------------|----------------------------------------------|------------------------------|
| `haier/set/restart`   |                                              | Restarts module              |
| `haier/set/enable`    | `1` or `on` - enable, `0` or `off` - disable | Enable/disable AC            |
| `haier/set/temp`      | `[16..30)`                                   | Desired AC temperature value |
| `haier/set/mode`      | See the ***Haier protocol table*** above     | AC mode (Heat/cool/fan etc.) |
| `haier/set/fan`       | See the ***Haier protocol table*** above     | AC fan speed mode            |
| `haier/set/deflector` | See the ***Haier protocol table*** above     | AC deflector mode            |

Also the `haier/lwt` topic is exists which periodically sends `online` message to control device connection. 

## Building

Use ESP IDF Programming guide and [Get Started](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32c3/get-started/index.html) page to install and setup ESP-IDF.

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
