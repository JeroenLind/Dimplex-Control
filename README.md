# Dimplex Opti-myst BLE bridge (ESP32)

Control a **Dimplex Opti-myst electric fireplace** from your browser and from **Home Assistant**, using an ESP32 that talks to the fireplace over Bluetooth LE. The bridge replaces the Bluetooth remote, or runs next to it: one button releases the Bluetooth link so the original remote works again.

Built and tested on a **Dimplex Engine 56/600** (Pro Range, ENG56-600, firmware EP4120). It is based on the work of Lammerink (see below), who used a different model. Other Pro Range models probably work, but nothing here has been verified on them.

## Features

- **Fireplace control:** flame effect on/off, flame level F1–F6, heater on/off, setpoint, heater power low/high (0.9 / 1.8 kW).
- **Web interface** at `http://dimplex-control.local/` with three pages:
  - **Main:** Bluetooth link, flame effect, heater, setpoint and the internal temperature.
  - **Settings:** flame level, heater power, volume (models with sound), choosing the fireplace (scan or enter the address and passkey) and a factory reset.
  - **Debug:** CSV status log download, raw registers, temperature history, connection and system information.
- **MQTT with Home Assistant auto-discovery:** switches for the fireplace, heater and Bluetooth link, numbers for flame level and setpoint, and sensors for heater power, internal temperature and raw registers.
- **Responsive:** web, BLE and MQTT run in separate FreeRTOS tasks, so a slow broker or a fireplace that is out of range does not freeze the page.
- **Installable as an app:** add the web interface to the home screen of an iPhone or Android phone so it opens full screen with its own icon (see below).
- **Diagnostics:** the Debug page and Home Assistant show why the bridge last restarted, how many starts there have been and health values such as uptime, memory and Wi-Fi signal. The last log lines before a crash survive the restart.
- **Status LED:** blink codes show Wi-Fi, MQTT and Bluetooth state (optional external LEDs per connection).
- **Setup without re-flashing:** Wi-Fi, MQTT broker and fireplace are configured through a captive portal (`Dimplex-Setup`) and stored in flash.
- The fireplace clock is set from NTP every time the bridge connects.

## What you need

- An ESP32 board (developed on the *ESP32 Dev Module*).
- Arduino IDE (or compatible) with the ESP32 board package and these libraries: **NimBLE-Arduino 2.x**, **PubSubClient** (2.8 or newer), **WiFiManager**.
- An MQTT broker (for example the Mosquitto add-on in Home Assistant) if you want Home Assistant integration. The web interface works without it.
- The **pairing passkey** of your fireplace (see below).

## Getting started

1. Open `Dimplex-Control/Dimplex-Control.ino` in the Arduino IDE (the folder also contains `icons.h`, which shows up as a second tab) and check the settings at the top: the time zone string in `loop()` (default is Central European Time), and the default fireplace address, passkey and MQTT credentials. You can also change these later in the setup portal.
2. Flash the sketch to the ESP32.
3. On first boot (or after a factory reset) the ESP32 opens the Wi-Fi network **Dimplex-Setup** (password `dimplex123`). Join it, choose your Wi-Fi network and enter the MQTT broker, the fireplace and the passkey. Leave the fireplace address empty to detect it automatically by its Bluetooth name (`FI####<Dimplex>`).
4. Open `http://dimplex-control.local/` (or the IP address in the serial log).
5. In Home Assistant the device **Dimplex Control** appears automatically through MQTT discovery. The web interface carries the same name.

**The passkey is different for every fireplace.** You get it by sniffing the pairing between the original remote and the fireplace and cracking it. Lammerink's repository describes how.

The fireplace accepts only **one Bluetooth connection at a time**. While the bridge is connected the original remote cannot connect: press *Released (use remote)* on the Main page (or switch off the *Bluetooth link* in Home Assistant) and the remote works again.

## Install on your phone

The web interface can be put on the home screen of a phone, so it opens like an app: full screen, without an address bar, with the flame icon. It is the same web page, so the phone has to be on the same Wi-Fi network as the bridge.

**iPhone (Safari, or Chrome on iOS 16.4 and newer):** open `http://dimplex-control.local/`, tap the *Share* button and choose *Add to Home Screen*. This works over plain HTTP.

**Android (Chrome):** Chrome offers *Install app* only for pages served over HTTPS, and the bridge serves plain HTTP. Two ways around that:

1. *Allow it for this address* (simple, personal use): open `chrome://flags`, search for **Insecure origins treated as secure**, enter `http://dimplex-control.local` (or the bridge's IP address, exactly as you use it), set it to *Enabled* and restart Chrome. Then open the page and use the menu (three dots) → *Install app*.
2. *Serve it over HTTPS:* put a reverse proxy with a trusted certificate in front of the bridge (for example the Nginx or Caddy add-on of Home Assistant, or Nabu Casa remote access) and install the app from that address.

Without either, *Add to Home screen* in the Chrome menu still puts a shortcut with the icon on your home screen. Chrome may then open it as a normal browser tab instead of an app.

If your phone cannot resolve `dimplex-control.local`, use the bridge's IP address instead (give it a fixed address in your router so it does not change).

## LED codes

| Pattern | Meaning |
|---|---|
| fast blink | setup portal open |
| 1 flash + pause | no Wi-Fi |
| 2 flashes + pause | Wi-Fi ok, no MQTT broker |
| 3 flashes + pause | Wi-Fi and MQTT ok, no Bluetooth link to the fireplace |
| slow blink | everything ok, but Bluetooth released for the original remote |
| steady on | everything connected |

The onboard LED on GPIO 2 is used; many ESP32 boards have one. Set `LED_PIN_WIFI`, `LED_PIN_MQTT` and `LED_PIN_BLE` at the top of the sketch for one external LED per connection.

## About the temperature

The temperature the fireplace reports comes from a sensor **inside the fireplace**, not from the room. It rises quickly when the heater runs and keeps rising for a while after it is switched off. The fireplace has no room sensor, so do not use this value as the room temperature of a thermostat.

## Documentation

- [`design.md`](docs/design.md): the design of the project: external interfaces (Bluetooth, Wi-Fi, MQTT, web), program structure, key flows and design decisions.
- [`register-map.md`](docs/register-map.md): the GATT registers of the fireplace, with a confidence level per register.
- [`experiments.md`](docs/experiments.md): how the registers were found (dumps, comparisons, measurements).
- [`task-architecture.md`](docs/task-architecture.md): how the task layout was reviewed and specified (history; `design.md` describes the current state).

## Credits

This project builds on **[Lammerink/Dimplex-Optimyst-BLE](https://github.com/Lammerink/Dimplex-Optimyst-BLE)**. Go there to learn how the Bluetooth communication between remote and fireplace can be **sniffed and cracked** (to find the pairing passkey) and how the **register dumping** works (comparing the fireplace's registers before and after one change to find out what each register does). Lammerink's project was made for a different fireplace model (CAS400LNH); this one was made for the Dimplex Engine 56/600, so behaviour and register details can differ between models.

## How this was made

The firmware and the documentation in this repository were written with the help of [Claude](https://claude.ai) (an AI assistant from Anthropic), working from my own register dumps, sniffed traffic and test results. I tested the result on my own fireplace, but the code has not been through a formal review, so read it before you trust it with a heater.

## Disclaimer

This is a hobby project, not affiliated with or supported by Dimplex. It controls a heating appliance: test with the fireplace within reach and keep the power switch accessible. Writing to unknown registers is at your own risk. Use it at your own risk.
