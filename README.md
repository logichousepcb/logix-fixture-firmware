# LoGiX Base Fixture

A small USB-powered WiFi LED controller built around an **ESP-12F (ESP8266)**. It drives addressable
LED strips (WS2812B and similar) from three identical 3-pin headers and takes commands from an IR remote
through a TSOP38238 receiver. This repo holds the schematic and a **custom firmware written from scratch**
(not WLED). The firmware includes:

- a **setup hotspot with captive portal** for entering WiFi credentials (no hard-coded secrets),
- a **web UI** for light control, WiFi, device name, GPIO/hardware settings, and firmware updates,
- a **JSON HTTP API** for Home Assistant, Node-RED, scripts, and so on ([API.md](API.md)),
- **IR remote** support (common 24-key RGB remote),
- a **USB current limiter**, so a long strip can't brown out the USB port,
- **firmware updates** from the browser, `curl`, or PlatformIO.

Schematic: [`Sheet_1 copy.png`](Sheet_1%20copy.png) (EasyEDA, Rev 1.0, 2023-12-10).

---

## 1. Hardware

### Block overview

```
USB-C (VBUS 5V) ──┬──────────────────────────────► 5V pin on LED headers U2 / U3 / U4
                  └─► AMS1117-3.3 ──► 3.3V ──► ESP-12F, CH340B, TSOP38238
USB D+/D- ──► CH340B ──TX/RX──► ESP-12F UART0
              DTR/RTS ──► Q1/Q2 (BC817) auto-reset ──► EN / GPIO0
ESP-12F GPIO2 ──► DATA pin on LED headers U2 / U3 / U4   (all three in parallel)
TSOP38238 OUT ──► ESP-12F GPIO4
```

### Parts (from the schematic)

| Ref | Part | Purpose |
| --- | --- | --- |
| U1 | ESP-12F (ESP8266MOD), 4 MB flash | MCU + WiFi |
| U7 | CH340B | USB-to-serial for programming and logs |
| Q1, Q2 + R2, R3 (10 k) | BC817 | Classic DTR/RTS auto-reset and boot-mode circuit |
| R1 (10 k) | pull-up | EN (CH_PD) pulled to 3.3 V |
| USBC1 | TYPE-C-31-M-12 | Power + data input |
| R4, R7 (5.1 k) | CC1/CC2 pull-downs | Identify the board as a USB-C sink so chargers supply 5 V |
| U6 | AMS1117-3.3 | 5 V → 3.3 V regulator |
| C5/C7 (10 µF), C6/C1 (100 nF) | | Regulator input/output decoupling |
| LED13 | TSOP38238 | 38 kHz IR receiver |
| U2, U3, U4 | KH-2.54FH 1×3P | LED strip outputs: **5V · GPIO2 · GND** |
| P1 | 1×5 female header | Serial/flash header: **TX · RX · 3.3V · GPIO0 · GND** |

### ESP8266 pin map

| GPIO | Connected to | Firmware default |
| --- | --- | --- |
| GPIO1 (TXD0) / GPIO3 (RXD0) | CH340B and P1 TX/RX | Serial console, 115200 baud |
| GPIO2 | LED headers U2/U3/U4 data | **LED data** (driven by hardware UART1, so it doesn't block interrupts) |
| GPIO4 | TSOP38238 output | **IR receiver** |
| GPIO0 | P1 pin 4 + auto-reset (Q2) | Boot mode strap |
| EN | R1 pull-up + auto-reset (Q1) | Reset |
| GPIO5, 12, 13, 14, 15, 16 | not connected | free |

### Hardware notes

- **The three LED headers are in parallel.** They all share the same data line, so all three strips show
  the same pattern. Set *LED count* to the length of the longest strip.
- **LED 5 V comes directly from USB VBUS.** At full white, a WS2812 LED draws about 60 mA, so 30 LEDs need
  about 1.8 A. The firmware's power limit (default **500 mA total**) dims the output when the estimated
  current goes over budget. Raise it in *Hardware → Power limit* if you power the board from a capable USB
  charger.
- **The data line is 3.3 V.** Most WS2812B strips accept it over short leads. If the first LED flickers,
  keep the data wire short or add a 74AHCT125 level shifter.
- **GPIO2 is also the ESP-12F's on-module blue LED.** It flickers along with the LED data. This is normal.
- GPIO2 must be HIGH at boot, and the strip's data input doesn't stop that, so it boots reliably.

---

## 2. Flashing the firmware

The firmware is a [PlatformIO](https://platformio.org/) project (`platformio.ini`, `src/`).

```powershell
pio run                     # build  -> .pio/build/usb/firmware.bin
pio run -e usb -t upload    # flash over serial (COM12)
pio device monitor -e usb   # serial log at 115200
```

### 1 MB vs 4 MB modules

Most ESP-12F modules have 4 MB of flash, but some batches have only 1 MB. A 4 MB image uploads
without complaint on a 1 MB module, but then it won't boot: the serial port shows only the ROM
banner (`ets_main.c` at 74880 baud). Check the module before flashing:

```powershell
python ~/.platformio/packages/tool-esptoolpy/esptool.py --port COM12 flash_id   # "Detected flash size"
```

| Flash | USB env | OTA env | Layout |
| --- | --- | --- | --- |
| 4 MB | `usb` | `ota` | 1 MB sketch slot, 2 MB LittleFS |
| 1 MB | `usb1m` | `ota1m` | ~470 KB sketch slot (OTA still fits), 64 KB LittleFS |

For a 1 MB board, run `pio run -e usb1m -t upload`. OTA updates to it must also use the `ota1m` build.
The device's updater rejects a 4 MB image ("new flash config wrong"), so picking the wrong one
fails safely. The first 1 MB board (MAC `ec:fa:bc:97:01:8d`) would only sync at 115200 baud.

### Option A: on-board USB-C (CH340B) (current setup, COM12)

Plug in USB-C, set `upload_port` in `platformio.ini` to the CH340's COM port, and upload. The DTR/RTS
transistors reset the chip into the bootloader automatically.

### Option B: FTDI adapter on header P1 

P1 only has TX/RX/3.3V/GPIO0/GND, with no DTR/RTS, so **you have to enter flash mode by hand**:

| FTDI | P1 |
| --- | --- |
| TX | RX (pin 2) |
| RX | TX (pin 1) |
| GND | GND (pin 5) |
| 3.3 V (**set the FTDI jumper to 3.3 V**) | 3.3V (pin 3), or leave unconnected and power the board from USB-C |
| GND jumper wire | GPIO0 (pin 4) **while powering up** |

1. Connect GPIO0 (P1 pin 4) to GND.
2. Power-cycle the board (unplug and replug USB-C or the FTDI 3.3 V). It is now in the bootloader.
3. Run `pio run -e usb -t upload`.
4. Remove the GPIO0 jumper and power-cycle again to run the new firmware.

> Never feed 5 V logic into P1. The ESP8266 is a 3.3 V part.

After the first serial flash, you can install every later update over WiFi (see section 6).

---

## 3. First-time setup (WiFi)

1. Power the board. With no saved network, it starts a hotspot named **`LoGiX-Setup-XXXX`**
   (`XXXX` = last 4 hex digits of the MAC address). It is open by default. The name never changes,
   even after you rename the device, so a phone app can find and join it.
2. Join the hotspot with a phone or laptop. The "sign in to network" page should pop up. If it doesn't,
   browse to **http://192.168.4.1**.
3. On the **Wi-Fi** tab, tap **Scan**, choose your network, enter the password, and tap **Connect**.
4. The page shows the result. On success it shows the new IP address and the address
   **http://logix-fixture-xxxx.local**. Switch your device back to your home WiFi and open that address.
   The setup hotspot turns off by itself about a minute after the connection succeeds.

**If the saved network goes away** (router off, password changed), the fixture keeps retrying. After
30 seconds it also brings the setup hotspot back so you can fix it. While a phone is connected to the
hotspot, retries pause so the portal stays usable.

---

## 4. Web UI

Open `http://<hostname>.local` or the device's IP address.

| Section | What it does |
| --- | --- |
| **Control** (top card) | Power, brightness, color picker + swatches, effect, speed, live current estimate |
| **Wi-Fi** | Scan, connect to a new network, forget the network (reboots into setup mode) |
| **Device** | Friendly **name**, which sets the hostname (`Kitchen Shelf` → `kitchen-shelf.local`) and the Bonjour name. Optional setup-hotspot password. **Identify** blinks the LEDs. **API token** on/off, with a pairing QR code. |
| **Hardware** | **LED data GPIO**, **LED count**, **color order**, **IR receiver GPIO**, **power limit**. Use this when you move the firmware to a different board. Saving reboots. |
| **Firmware** | Upload a new `firmware.bin` with a progress bar. Settings are kept. |
| **Info** | Network, IR, memory, and uptime details; reboot; factory reset |

Effects: Solid, Breathe, Rainbow, Rainbow Cycle, Color Wipe, Theater Chase, Twinkle, Comet, Candle, Christmas (classic red/green/blue/orange/yellow bulbs with a gentle per-bulb twinkle; speed 0 = steady),
and three color-pair themes, 2 LEDs of each color repeating. The plain version slowly marches along
the strip (speed 0 = still). The **Solid** version (e.g. "Buffalo Bills Solid") never moves.

| Effect | Solid variant | Colors |
| --- | --- | --- |
| Buffalo Bills (10) | Buffalo Bills Solid (13) | blue `(0,51,141)`, red `(198,12,48)`, white |
| Buffalo Sabres (11) | Buffalo Sabres Solid (14) | navy `(0,48,135)`, gold `(255,184,28)` |
| Lake Shore (12) | Lake Shore Solid (15) | white, forest green `(34,139,34)` |
State (on, brightness, color, effect, speed) is saved to flash 5 s after the last change and restored
at power-up.

### Using a different board

You can choose any of the following in *Hardware*:

- LED data: GPIO 2 (best: hardware UART, never disturbs IR or WiFi), 3 (I2S DMA, disables serial),
  or 0, 1, 4, 5, 12, 13, 14, 15 (bit-banged).
- IR receiver: none, or GPIO 0–5, 12–14.

GPIO 6–11 (flash) and GPIO16 (no interrupt or LED driver support) are blocked. GPIO15 is blocked for IR
because a TSOP holds the line high and GPIO15 must be low at boot.

---

## 5. IR remote

Works with the common **24-key NEC "RGB LED" remote**:

| Row | Keys |
| --- | --- |
| 1 | Brightness + · Brightness − · OFF · ON |
| 2–6 | Red/Green/Blue/White and 12 color shades (switch to the Solid effect) |
| right column | FLASH → Theater Chase · STROBE → Twinkle · FADE → Breathe · SMOOTH → Rainbow Cycle |

Holding brightness ± auto-repeats. The last received code is shown in *Info* and at `GET /api/info`
(`ir.lastCode`). This helps when adding codes for a different remote in `src/ir.cpp`.

---

## 6. Firmware updates

The firmware runs from a 1 MB OTA slot, so it can replace itself over WiFi. Settings stored in LittleFS
are kept.

- **Browser:** *Firmware* tab → choose `.pio/build/usb/firmware.bin` → *Upload & install*.
- **PlatformIO over WiFi:** `pio run -e ota -t upload --upload-port 192.168.1.50` (or `name.local`).
- **curl:** `curl -F "firmware=@.pio/build/usb/firmware.bin" http://192.168.1.50/api/update`

### Update notice

When the web page loads, your **browser** asks GitHub for the latest release of
[logichousepcb/logix-fixture-firmware](https://github.com/logichousepcb/logix-fixture-firmware/releases).
If it is newer than the installed version, an orange **Update x.y.z** badge appears next to the
WiFi/IP pill. Clicking it opens the Firmware tab, which links the right file for this module (`-4mb.bin`
or `-1mb.bin`, picked from the flash size). Download it, then upload it. The device itself never goes
to the internet. The check needs the browser to have internet access, so it stays quiet on the setup
hotspot. The result is cached in the browser for 6 hours.

### Releasing a new version

1. Bump `FW_VERSION` in `src/config.h` (semantic versioning: the badge compares `major.minor.patch`).
2. `pio run -e usb -e usb1m`
3. Publish a GitHub release tagged `vX.Y.Z` with both builds attached as
   `logix-fixture-X.Y.Z-4mb.bin` (`.pio/build/usb/firmware.bin`) and
   `logix-fixture-X.Y.Z-1mb.bin` (`.pio/build/usb1m/firmware.bin`).

---

## 7. API

Full reference: **[API.md](API.md)**. Quick examples:

```bash
curl http://kitchen-shelf.local/api/state
curl "http://kitchen-shelf.local/api/set?on=toggle"
curl "http://kitchen-shelf.local/api/set?color=ff6a00&bri=200&effect=candle"
curl -X POST -H "Content-Type: application/json" \
     -d '{"on":true,"color":[0,64,255],"effect":"Rainbow Cycle","speed":90}' \
     http://kitchen-shelf.local/api/state
```

The API has CORS enabled, so you can call it from your own dashboards. Live state changes, including
changes made with the IR remote, are pushed over a WebSocket on port 81.

---

## 7a. Discovery and phone apps

Every fixture announces itself over Bonjour/mDNS as **`_logix._tcp`**. This is the same service type and
TXT scheme your other LoGiX devices (outlets, vents, RGB switches) use, so one app can list all of them:

| TXT key | Example | Meaning |
| --- | --- | --- |
| `manufacturer` | `Logix` | |
| `api` | `1` | API contract version |
| `fw` | `1.2.0` | Firmware version |
| `name` | `Molly's Tree` | Friendly name (the Bonjour instance name is the hostname) |
| `id` | `48e72971f314` | MAC address. **Stable key** for the device; IP and name can change. |
| `model` | `logixfixture` | Device type |
| `ws` | `81` | WebSocket port (fixture-specific) |
| `auth` | `0` / `1` | `1` = the API needs a token (fixture-specific) |

The device also keeps announcing `_http._tcp` for browsers.

**iOS app checklist:** Info.plist needs `NSBonjourServices = ["_logix._tcp"]` and an
`NSLocalNetworkUsageDescription`. Under App Transport Security, set `NSAllowsLocalNetworking = true`
(the device uses plain HTTP). Browse with `NWBrowser(for: .bonjourWithTXTRecord(type: "_logix._tcp", domain: nil))`.

**In-app WiFi onboarding:** the app asks the user for the 4 characters after `LoGiX-Setup-`, joins that
SSID with `NEHotspotConfiguration` (needs the Hotspot Configuration entitlement), then calls
`GET /api/wifi/scan` and `POST /api/wifi` at `http://192.168.4.1`. It polls `GET /api/info` until
`wifi.connected` is true, then rejoins the home network and finds the device by `id` over Bonjour.

**API token (optional, off by default):** *Device → API token → Generate*. Once a token is set, every API
call needs `Authorization: Bearer <token>` (or `X-Api-Key`, or `?token=`), and the WebSocket needs
`ws://host:81/?token=<token>`. Only the identity part of `GET /api/info` stays public, so an app can
show the device and see `"auth": true`. The Device tab shows a QR code containing
`{"logix":1,"id":…,"host":…,"token":…}` for pairing the app. If you lose the token, connect over USB serial
and send `factory-reset`.

---

## 8. Recovery and troubleshooting

| Symptom | Fix |
| --- | --- |
| Can't find the device | Look for the `LoGiX-Setup-XXXX` hotspot. Check the serial log (115200), which prints the IP and hostname at boot; send `info` for a summary. |
| Lost the API token or locked out | USB serial at 115200: send `factory-reset`. |
| `Timed out waiting for packet header` when flashing | Not in bootloader. Hold GPIO0 to GND while power-cycling (FTDI), or check that TX/RX are crossed. |
| Wrong colors (red shows as green) | *Hardware → Color order*. WS2812B = GRB, WS2811 often RGB or BRG. |
| LEDs dim or limited at high brightness | The power limit is active (shown under the control card). Raise it if your supply allows. |
| Changed to a GPIO and now nothing works | `POST /api/reset` or *Info → Factory reset*. Or re-flash over serial and erase: `pio run -e usb -t erase` then upload. |
| Forgot which network it's on | *Wi-Fi → Forget network*, or factory reset. |

Security note: by default the web UI and API have no login, so anyone on your LAN, or on the setup
hotspot while it is up, can control the device. Turn on the *API token* and set a *setup-hotspot
password* under *Device* if that matters to you. The token travels over plain HTTP, so it keeps out
casual access on your network but not someone who can capture your network traffic.

---

## 9. Project layout

```
platformio.ini        build envs: usb (COM12), ota (WiFi upload)
Sheet_1 copy.png      schematic
src/main.cpp          setup/loop wiring
src/config.*          settings + light state persistence (LittleFS JSON), reboot scheduling
src/net.*             WiFi station, fallback setup AP, captive DNS, mDNS/Bonjour (_logix._tcp)
src/web.*             HTTP server, JSON API, auth token, WebSocket (port 81), firmware upload, captive portal
src/web_ui.h          the single-page web UI (embedded HTML/CSS/JS)
src/leds.*            LED driver selection, effects, brightness fade, current limiter
src/ir.*              IR receiver + remote key map
```

Libraries: NeoPixelBus (Makuna), IRremoteESP8266 (NEC decoder only), ArduinoJson 7, arduinoWebSockets. Everything else
is the ESP8266 Arduino core.


