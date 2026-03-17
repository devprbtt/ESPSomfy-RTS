# CC1101 RF Monitor

This is a new, simpler firmware built from the same ESP32 + CC1101 foundation as the original project, but without any Somfy-specific protocol handling.

## What it does

- Captures raw pulse timings from `GDO2` on the CC1101
- Replays raw pulse timings through `GDO0`
- Hosts an open fallback AP with captive portal behavior
- Supports saved STA Wi-Fi credentials for joining an existing network
- Exposes a browser UI for:
  - live RX/TX terminal output
  - radio settings
  - manual pulse transmit
  - saving and replaying learned commands
- Exposes a simple telnet console on port `23`
- Stores learned commands in LittleFS

## Pinout

| CC1101 Pin | Function | ESP32 Pin |
| --- | --- | --- |
| 1 | GND | GND |
| 2 | VCC | 3V3 |
| 3 | GDO0 / TX data | GPIO 13 |
| 4 | CSN | GPIO 5 |
| 5 | SCK | GPIO 18 |
| 6 | MOSI | GPIO 23 |
| 7 | MISO | GPIO 19 |
| 8 | GDO2 / RX data | GPIO 12 |

## Notes

- The firmware defaults to asynchronous raw mode on the CC1101 and is aimed at common remote-control style pulse capture and replay.
- `ASK/OOK` is the default modulation because that is the most common starting point for simple RF remotes.
- You will still need to upload the `data/` folder to LittleFS so `/index.html` is available.
- This is a generic RF monitor, not a decoder for every protocol automatically. The main output is raw timings that you can inspect, replay, and organize.
