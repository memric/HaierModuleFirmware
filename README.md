# Haier Module

ESP-IDF based firmware for WiFi control of Haier air conditioners via MQTT. This project enables integration with popular smart home solutions.

## Overview

Haier Module is a complete solution for remotely controlling Haier air conditioners through an ESP32C3 microcontroller. It communicates with the AC unit using the proprietary Haier protocol and exposes control via MQTT.

The firmware is designed for use with the [HaierModule hardware](https://github.com/memric/HaierModule) but can be adapted to other ESP32-based platforms.

## Features

- **WiFi Connectivity** — Connect your AC to your home network
- **MQTT Integration** — Full MQTT support for smart home automation
- **AC Control** — Set temperature, mode, fan speed, and deflector position
- **Status Monitoring** — Real-time AC state and system diagnostics
- **Over-the-Air (OTA) Updates** — Update firmware remotely without disassembly (based on simple [FWTP](https://github.com/memric/FWTP) protocol)
- **Automatic Reconnection** — Handles WiFi and MQTT disconnections gracefully
- **System Information** — Monitor temperature, WiFi signal strength, and uptime
- **Telegram Integration** — Optional Telegram bot for notifications (Legacy)

## Haier Protocol

### Frame Structure

The Haier protocol uses a fixed-format frame for communication with the AC unit:

| Byte | Value | Description | Status |
|------|-------|-------------|--------|
| 0-1 | 0xFF, 0xFF | Frame separator/header | ✅ |
| 2 | - | Payload length (size - 3) | ✅ |
| 3-8 | 0x00 | Reserved/dummy bytes | - |
| 9 | 0x01 | Frame start marker | ✅ |
| 10 | - | Frame direction (0x4D=req, 0x6D=resp) | ✅ |
| 11 | - | Frame type (0x01=req, 0x02=resp) | ✅ |
| 12 | 0x5F | Command code (request) | ✅ |
| 13 | - | **Current temperature (°C)** | ✅ |
| 14-22 | - | Reserved | - |
| 23 | - | **Mode** (0=Smart, 1=Cool, 2=Heat, 3=Fan, 4=Dry) | ✅ |
| 24 | - | Reserved | - |
| 25 | - | **Fan speed** (0=Max, 1=Med, 2=Min, 3=Auto) | ✅ |
| 26 | - | Reserved | - |
| 27 | - | **Deflector** (0=Off, 1=Vert, 2=Horiz, 3=Auto) | ✅ |
| 28 | - | Lock/reserved | ❌ |
| 29 | - | **Power** (0=Off, 1=On) | ✅ |
| 30 | - | Reserved | - |
| 31 | - | Fresh air (experimental) | ❌ |
| 32-34 | - | Reserved | - |
| 35 | - | **Temperature setpoint** (value - 16 = °C) | ✅ |
| 36 | - | Checksum (sum of all bytes) | ✅ |

**Legend**: ✅ = Implemented & Tested, ❌ = Not yet implemented, - = Not applicable

## MQTT Interface

The device communicates via MQTT on three main topic categories:

### System Information (`haier/sys`)

Published periodically with system status:

```json
{
  "AppVer": "0.1.0",
  "IdfVer": "v6.0",
  "LifeTime": 79317,
  "ResetReason": 4,
  "ReconnCntr": 1,
  "Temp": 36,
  "IP": "192.168.1.100",
  "WiFiRSSI": -27
}
```

| Field | Description |
|-------|-------------|
| `app` | Firmware application version |
| `idf` | ESP-IDF version |
| `lifetime` | Uptime in seconds |
| `reset_reason` | ESP reset reason code |
| `recon` | WiFi connection attempts |
| `temp` | SoC temperature in °C |
| `ip` | Device IP address |
| `rssi` | WiFi signal strength (dBm) |

### AC State (`haier/state`)

Published periodically with current AC status:

```json
{
  "Enabled": 0,
  "TempCurr": 26,
  "TempSet": 24,
  "Mode": 1,
  "Fan": 3,
  "Deflector": 2
}
```

| Field | Description |
|-------|-------------|
| `Enabled` | AC power state (0=off, 1=on) |
| `TempCurr` | Current room temperature in °C |
| `TempSet` | Desired temperature setpoint (16-30) °C |
| `Mode` | Operating mode (0=Smart, 1=Cool, 2=Heat, 3=Fan, 4=Dry) |
| `Fan` | Fan speed (0=Max, 1=Medium, 2=Min, 3=Auto) |
| `Deflector` | Deflector mode (0=Off, 1=Vertical, 2=Horizontal, 3=Auto) |

### Control Commands (`haier/set/*`)

Send commands to control the AC:

| Topic | Payload | Description |
|-------|---------|-------------|
| `haier/set/enable` | `1`, `on` (enable) or `0`, `off` (disable) | Turn AC on/off |
| `haier/set/temp` | `16`-`30` | Set desired temperature |
| `haier/set/mode` | `0`-`4` | Set operating mode |
| `haier/set/fan` | `0`-`3` | Set fan speed |
| `haier/set/deflector` | `0`-`3` | Set deflector mode |
| `haier/set/restart` | (any value) | Restart the module |

### Last Will & Testament (`haier/lwt`)

Indicates device connectivity status:
- **Payload**: `online` (sent periodically)
- **Use**: Configure in your MQTT broker to detect disconnections 

## Hardware Requirements

- **ESP32-C3** microcontroller or compatible ESP32 variant
- **3.3V - 5V UART** level converter for ESP32-AC communication

## Prerequisites

1. **ESP-IDF v6.0 or later** — Follow the [official setup guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32c3/get-started/index.html)
2. **Git** — For cloning this repository
4. **MQTT Broker** — For smart home integration (e.g., Mosquitto, Home Assistant)

## Installation & Configuration

### Clone the Repository

```bash
git clone https://github.com/memric/HaierModule.git
cd HaierModule
```

### Set Target and Configure

```bash
idf.py set-target esp32c3

idf.py menuconfig
```

### Configuration Options

In the menuconfig, set the following under **Project configuration**:

| Option | Description |
|--------|-------------|
| **WiFi SSID** | Your WiFi network name |
| **WiFi Password** | Your WiFi password |
| **MQTT Broker** | IP or hostname of your MQTT broker |
| **MQTT Port** | MQTT broker port (default: 1883) |
| **Telegram Token** (optional) | For Telegram bot notifications |
| **OTA Updates** (optional) | Enable/disable over-the-air updates |

### Build and Flash

```bash
idf.py build

idf.py -p /dev/ttyUSB0 flash

idf.py -p /dev/ttyUSB0 monitor
```

For macOS, the serial port is typically `/dev/tty.usbserial-*`. For Linux, it's usually `/dev/ttyUSB0`.

## References & Related Projects

- [Haier Module Hardware](https://github.com/memric/HaierModule) — The original hardware design
- [Haier WiFi Project](https://github.com/instalator/Haier_WiFi) — Alternative ESP8266 implementation
- [Haier Protocol Research](https://github.com/paveldn/HaierProtocol) — Protocol documentation and analysis
- [Управление кондиционером Haier на базе ESP8266](https://blog.instalator.ru/archives/433) — Russian language blog post with protocol details
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/index.html) — Official framework documentation
- [WQTT](https://www.wqtt.ru) — Cloud MQTT broker

## License

This project is provided as-is for personal and educational use.
