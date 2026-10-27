# DualSense Wake

[Русский](README.md) · **English**

Press PS on a gamepad paired to a **powered-off** PC and get an event in Home Assistant — use it to wake the PC (Wake-on-LAN), turn on the TV, and so on. The gamepad stays paired to the PC.

Tested with DualSense. Should work with DualShock 4 and other pads that reconnect to their host over Bluetooth Classic; BLE pads (e.g. Xbox Series) won't work.

## How it works

On PS press the gamepad pages the Bluetooth address of its PC. An ESP32 takes over that address, receives the connection request, rejects it before any pairing or authentication, and fires the event. While the PC is on, the ESP32 stays silent so the gamepad connects to the PC as usual.

## Requirements

- **Original ESP32** (WROOM-32, DevKit). ESP32-S2/S3/C3/C6 have no Bluetooth Classic.
- Home Assistant with the ESPHome integration.

## Setup

1. Open **https://lnikazzzl.github.io/esphome-gamepad-wake/** in Chrome or Edge, connect the ESP32 over USB, click *Install* and enter your Wi-Fi.
   Skipped Wi-Fi? Join the open access point `dualsense-wake-xxxxxx` and set it there.
2. Home Assistant discovers the device; add it (no encryption key).
3. Set **PC Bluetooth MAC**. The ESP32 reboots; **ESP Bluetooth Address** should show the same value.
   - Windows: Device Manager → Bluetooth → your adapter → Properties → Details → *Bluetooth device address*
   - Linux: `bluetoothctl list` · macOS: System Information → Bluetooth
4. Recommended: set **PC IP** and allow ping on the PC (Windows: `netsh advfirewall firewall add rule name="ICMP echo" protocol=icmpv4:8,any dir=in action=allow`).
5. Create an automation triggered by **PS Button**, e.g. *Wake on LAN: send magic packet* to the PC.

## Entities

| Entity | |
|---|---|
| **PS Button** | Event `press` — the automation trigger |
| **Enabled** | Master switch |
| **PC Bluetooth MAC** | Address to answer for; changing it reboots the ESP32 |
| **PC IP** | Optional; stay silent while the PC answers ping |
| **Pause After Press** | Stay silent this long after a press, so the next press reaches the booted PC (default 90 s, 0 = off) |
| **Ignore After PC Off** | Stay silent this long after the PC goes offline, while the gamepad keeps retrying (default 30 s, needs PC IP) |
| **Any Device Triggers** | Off: only gamepads fire the event. Other paired devices (headphones, mice) are rejected silently |
| **Listening** | Whether the ESP32 currently answers for the PC |
| **PC Online** · **Last Device** · **ESP Bluetooth Address** | Diagnostics |
| **Restart** | Reboot the ESP32 |

The ESP32 listens only when *Enabled* is on, the MAC is set, no pause is running and — with *PC IP* set — the PC is offline and Wi-Fi is up.

**Without PC IP**, it starts listening again after the pause even if the PC is on, and may catch the gamepad's next reconnect; press PS again. To avoid that, set PC IP, or switch *Enabled* off from any "PC is on" sensor (HASS.Agent, a power-metering plug).

## Limitations

- One ESP32 answers for one PC.
- If the PC sleeps with Bluetooth wake enabled, both may answer. Set PC IP or switch *Enabled* off while it sleeps.
- Wi-Fi and Bluetooth share the ESP32 radio; on weak Wi-Fi a press can be missed.
- The API, OTA and fallback AP have no password, to stay plug-and-play. Anyone on your LAN can reflash the device. Add `api: encryption` and an OTA password if that matters to you.
- The adoptable config pulls the component from `main`.

## Building

`esphome run dualsense-wake.factory.yaml` builds the local component. To publish a fork: replace `lNikazzzl/esphome-gamepad-wake` (and the `lnikazzzl.github.io` URL) everywhere, set *Settings → Pages → Source* to *GitHub Actions*, push to `main` (drafts a release and builds it), then run *Actions → Release*.

License: MIT.
