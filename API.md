# LoGiX Fixture HTTP API

Base URL: `http://<hostname>.local` or `http://<ip>`. When connected to the setup hotspot, use
`http://192.168.4.1`.

- All responses are JSON.
- `POST` bodies are JSON (`Content-Type: application/json`), except firmware upload, which is multipart.
- Errors return a 4xx/5xx status with `{"ok": false, "error": "message"}`.
- CORS is enabled (`Access-Control-Allow-Origin: *`).
- API version: **1** (`api` in `/api/info` and in the mDNS TXT record). Breaking changes bump it.
- Discovery: Bonjour `_logix._tcp`. See *Discovery and phone apps* in the README.

## Authentication

Authentication is off by default. When an API token is set (`POST /api/config {"apiToken":"generate"}`),
every endpoint except the public part of `GET /api/info` requires one of:

```
Authorization: Bearer <token>
X-Api-Key: <token>
?token=<token>                (query parameter, handy for /api/set links)
```

A missing or wrong token returns `401 {"ok":false,"error":"unauthorized: API token required"}`. For the
WebSocket, append `?token=<token>` to the URL. Recovery when the token is lost: send `factory-reset`
over USB serial (115200).

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/info` | Identity (public) + device, network, LED, IR, system info (with auth) |
| GET | `/api/state` | Current light state |
| POST | `/api/state` | Change light state (any subset of fields) |
| GET | `/api/set?…` | Same as POST `/api/state`, but from query parameters |
| GET | `/api/effects` | List of effects |
| POST | `/api/identify` | Blink the LEDs white for about 3 s |
| WS | `ws://<host>:81/` | Live state push + commands |
| GET | `/api/config` | Device settings (passwords are not returned) |
| POST | `/api/config` | Change name / GPIO / LED count / color order / IR / power limit |
| GET | `/api/wifi/scan` | Scan for WiFi networks (poll until done) |
| POST | `/api/wifi` | Save and connect to a WiFi network, or forget it |
| POST | `/api/update` | Upload and install firmware |
| POST | `/api/reboot` | Reboot |
| POST | `/api/reset` | Factory reset (erase all settings) and reboot |

---

## Light state

The fixture has one or more **outputs** (strips on their own GPIO): one on the ESP8266 board, up to
four on the WT32-ETH01. Each output has its own on/off, brightness, color, effect and speed. On top of
that, `on` and `bri` at the top level are the master power and brightness for the whole fixture
(an output's brightness is scaled by the master brightness).

### `GET /api/state`

```json
{
  "on": true,                 // master power
  "bri": 128,                 // master brightness
  "color": [255, 0, 0],       // color / effect / speed of the first output
  "hex": "#ff0000",
  "colors": ["#ff0000", "#ffa000", "#ffffff"],   // colors 1-3 (color 1 = "color"/"hex")
  "effect": 0,
  "effectName": "Solid",
  "speed": 128,
  "synced": false,            // true when every output shows the same thing
  "segments": [               // one entry per used output
    {"id": 0, "name": "Porch", "model": "arches4x24", "on": true, "bri": 255, "color": [255,0,0], "hex": "#ff0000",
     "effect": 0, "effectName": "Solid", "speed": 128},
    {"id": 3, "name": "Output 4", "model": "custom", "on": true, "bri": 255, "color": [255,147,41], "hex": "#ff9329",
     "effect": 3, "effectName": "Rainbow Cycle", "speed": 150}
  ]
}
```

Apps that treat the fixture as a single light can ignore `segments`: the top-level fields work as
before, and setting them changes every output. `synced` ignores colors that the effect doesn't use
(Rainbow, Christmas, team colors, ... use none; Solid uses color 1; Chase 2 Colors uses colors 1-2).

### `POST /api/state`

Send only the fields you want to change. The response is the full new state.

**All outputs** (top-level fields):

| Field | Type | Notes |
| --- | --- | --- |
| `on` | `true` / `false` / `"toggle"` | `"on"`, `"off"`, `1`, `0` also accepted |
| `bri` | 0–255 | Master brightness (fades over 400 ms) |
| `color` | `[r,g,b]` or `"#rrggbb"` / `"rrggbb"` | Color 1 |
| `colors` | Up to 3 colors, each `[r,g,b]` / `"#rrggbb"` / `null` (unchanged) | Colors 1-3, e.g. `[null,"#ffa000"]` sets only color 2 |
| `r`, `g`, `b` | 0–255 | Set individual channels of color 1 |
| `effect` | id (0–25) or name (case-insensitive) | See `/api/effects` |
| `speed` | 0–255 | Effect speed |

Setting `effect` or `speed` for all outputs also lines up their animations, so outputs running the
same effect move in step.

**One output**: add `"seg": <id>` and the same fields (`on` and `bri` then mean that output's own
power and brightness), or send several at once in `segments`:

| Field | Type | Notes |
| --- | --- | --- |
| `seg` | output id | The other fields go to this output only |
| `segments` | `[{"id": 0, ...}, ...]` | Each entry: `id` plus any of `on`, `bri`, `color`, `colors`, `r`/`g`/`b`, `effect`, `speed` |

Switching an output on also switches the master on. Unknown ids are rejected with
`unknown output (seg / segments id)`, and nothing is changed if any field is invalid.

Changing color, brightness, or effect does **not** turn the light on. Include `"on": true` if you want that.

```bash
curl -X POST -H "Content-Type: application/json" \
     -d '{"on":true,"bri":200,"color":"#00a0ff","effect":"Breathe"}' \
     http://fixture.local/api/state

# different things on different outputs
curl -X POST -H "Content-Type: application/json" \
     -d '{"on":true,"segments":[{"id":0,"color":"#ff0000","effect":0},{"id":1,"effect":"Christmas"},{"id":3,"on":false}]}' \
     http://fixture.local/api/state
```

### `GET /api/set`

Same fields as query parameters. Handy for buttons, browsers, and simple automations:

```
/api/set?on=toggle
/api/set?on=1&bri=255&color=ff0000
/api/set?effect=rainbow%20cycle&speed=60
/api/set?r=0&g=255&b=0
/api/set?seg=2&effect=comet          (output 3 only)
/api/set?seg=0&on=toggle             (output 1 on/off)
```

### `GET /api/effects`

```json
[{"id":0,"name":"Solid","colors":1},{"id":1,"name":"Breathe","colors":1},{"id":2,"name":"Rainbow","colors":0},
 ...
 {"id":24,"name":"Chase 3 Colors","colors":3},{"id":25,"name":"Bounce-In","colors":3}]
```

`colors` is how many of the output's colors 1-3 the effect draws with (0 = it has its own palette).

| id | Effect | id | Effect |
| --- | --- | --- | --- |
| 0 | Solid | 13 | Buffalo Bills Solid |
| 1 | Breathe | 14 | Buffalo Sabres Solid |
| 2 | Rainbow | 15 | Lake Shore Solid |
| 3 | Rainbow Cycle | 16 | Frost |
| 4 | Color Wipe | 17 | Valentine |
| 5 | Theater Chase | 18 | Fall |
| 6 | Twinkle | 19 | Solid 1 Color |
| 7 | Comet | 20 | Solid 2 Colors |
| 8 | Candle | 21 | Solid 3 Colors |
| 9 | Christmas | 22 | Chase 1 Color |
| 10 | Buffalo Bills | 23 | Chase 2 Colors |
| 11 | Buffalo Sabres | 24 | Chase 3 Colors |
| 12 | Lake Shore | 25 | Bounce-In |

---

## Info

### `GET /api/info`

Without a valid token (when auth is on), only the identity block comes back:

```json
{ "id": "48e72971f314", "name": "Molly's Tree", "model": "logixfixture", "manufacturer": "Logix",
  "firmware": "LoGiX Fixture", "version": "1.2.0", "api": 1, "auth": true, "wsPort": 81 }
```

Full response:

```json
{
  "id": "48e72971f314",          // stable device key (MAC, lowercase) - same as mDNS TXT "id"
  "name": "Molly's Tree",
  "model": "logixfixture",
  "manufacturer": "Logix",
  "firmware": "LoGiX Fixture",
  "version": "1.2.0",
  "api": 1,
  "auth": false,
  "wsPort": 81,
  "fixture": "snowflake91",      // model of the first used output
  "board": "esp12",              // "esp12" | "wt32-eth01"
  "hostname": "molly-s-tree",
  "build": "Nov 22 2026 10:15:02",
  "mac": "48:E7:29:71:F3:14",
  "chipId": "71F314",
  "ip": "192.168.1.50",          // home-network address (Ethernet preferred), when online
  "capabilities": { "rgb": true, "white": false, "effects": 10, "speed": true, "ir": true,
                    "identify": true, "websocket": true, "maxLeds": 800, "ota": true,
                    "ethernet": false, "outputs": 1 },   // outputs = how many the board supports
  "wifi": {
    "mode": "sta",               // "ap" | "sta" | "ap+sta" | "off" (Ethernet carrying traffic)
    "ssid": "HomeNet",
    "status": "connected",       // "not configured" | "connecting" | "connected" |
                                 // "wrong password" | "network not found" | "connection failed" | "idle" |
                                 // "standby (Ethernet connected)"
    "connected": true,
    "ip": "192.168.1.50",        // only when connected
    "rssi": -58,                 // only when connected
    "apActive": false,
    "apSsid": "…", "apIp": "192.168.4.1"   // only while the setup AP is up
  },
  "eth": { "link": true, "connected": true, "ip": "192.168.1.60",   // WT32-ETH01 only
           "speed": 100, "fullDuplex": true, "mac": "C8:F0:9E:DE:20:D3" },
  "leds": { "pin": 2, "count": 30, "colorOrder": "GRB",   // pin/colorOrder: first output; count: all
            "outputs": [{"id": 0, "name": "Output 1", "pin": 2, "count": 30, "colorOrder": "GRB", "model": "custom"}],
            "estimatedMa": 312, "maxMa": 500, "powerLimited": false },
  "ir":   { "pin": 4, "enabled": true, "lastCode": "0xF720DF", "lastAgeMs": 5120 },
  "system": { "uptimeS": 3600, "freeHeap": 38000, "sketchSize": 433000,
              "freeSketchSpace": 610000, "flashSize": 4194304, "resetReason": "Power On" }
              // ESP32 also lists "partitions": [{label, type, subtype, offset, size}, ...]
}
```

### `POST /api/identify`

Blinks all LEDs white 6 times over 3 seconds (even when the light is off), then restores. Returns
`{"ok":true}`. Use it in an app's device list so the user can tell fixtures apart.

---

## WebSocket (live updates)

`ws://<host>:81/` (add `?token=<token>` when auth is on). Text frames carry JSON.

**Server → client**

```json
{"type":"state","state":{"on":true,"bri":128,"color":[255,147,41],"hex":"#ff9329","effect":9,"effectName":"Christmas","speed":128,"synced":true,"segments":[...]}}
{"type":"error","error":"bri must be 0-255"}
```

The current state is sent on connect and again after every change from any source (web UI, HTTP API,
another app, the IR remote), at most once every 50 ms.

**Client → server**

```json
{"on":true,"bri":200}                       // same fields as POST /api/state
{"type":"set","state":{"effect":"Christmas"}}
{"type":"get"}                              // re-send current state
{"type":"identify"}
```

A successful `set` is confirmed by the next `state` broadcast. A rejected one returns an `error` to that
client only. With a bad token, the server sends an `error` and closes the connection.

---

## Settings

### `GET /api/config`

```json
{ "name": "LoGiX Fixture 3A4F", "hostname": "logix-fixture-3a4f",
  "ssid": "HomeNet", "hasPass": true, "hasApPass": false,
  "ledPin": 2, "ledCount": 30, "colorOrder": "GRB",   // output 1
  "maxOutputs": 1, "maxLeds": 800,
  "outputs": [{"pin": 2, "count": 30, "order": "GRB", "name": "", "model": "custom", "active": true}],   // maxOutputs entries; pin -1 = not used
  "irPin": 4, "maxMa": 500, "fixture": "custom",
  "setupSsid": "LoGiX-Setup-F314", "apiToken": "" }
```

### `POST /api/config`

Send any subset of fields. All of them are validated before anything is saved.

| Field | Values | Applies |
| --- | --- | --- |
| `name` | 1–32 chars. Hostname = lowercase, non-alphanumerics → `-` | reboot |
| `apPass` | `""` (open) or 8–63 chars | next time the AP starts |
| `outputs` | Array of up to `maxOutputs` entries `{pin, count, order, name, model, active}`; any field can be left out. `pin` -1 = output not used; `name` up to 24 chars; `active` false hides the output on the web UI's control card (it keeps running); `model` is a fixture model (see below). At least one output needs a pin, pins must differ, total LEDs ≤ 800 | reboot when a pin or count changes; order, name, model and active immediately |
| `ledPin` | Output 1's pin. ESP8266: 0, 1, 2, 3, 4, 5, 12, 13, 14, 15. WT32-ETH01: 1, 2, 3, 4, 5, 12, 14, 15, 17, 32, 33 | reboot |
| `ledCount` | Output 1's LED count, 1–800 | reboot |
| `colorOrder` | Output 1's order: `RGB` `RBG` `GRB` `GBR` `BRG` `BGR` | immediately |
| `irPin` | -1 (off). ESP8266: 0–5, 12–14. WT32-ETH01: an LED pin or 35, 36, 39 | reboot |
| `maxMa` | 0 (no limit) or 100–20000 | immediately |
| `fixture` | Output 1's model (from before each output had its own; `outputs[0].model` wins when both are sent) | immediately (reboot if the LED count changes) |
| `apiToken` | `"generate"` (random 24-hex), a 16–64 char `[A-Za-z0-9_-]` string, or `""` (off) | immediately; reboots when turning auth on/off (mDNS `auth` flag) |

Response: `{"ok":true}`, or `{"ok":true,"reboot":true}` when the device is about to restart (about 1.5 s).

**Fixture models** (per output). The icon is served at `/icon/<model>.webp` (none for `custom`).

| Model | Name | LED count |
| --- | --- | --- |
| `custom` | Custom / other | any |
| `string` | String Lights | any |
| `snowflake91` | 91 Pixel Snowflake | fixed at 91 |
| `arches4x24` | 4 Arches (24 px each) | fixed at 96 |
| `tree81` | 81 Pixel Light Tree | fixed at 81 |
When the token changes, the response includes the new `apiToken`.

```bash
curl -X POST -H "Content-Type: application/json" \
     -d '{"name":"Kitchen Shelf","ledCount":60,"maxMa":1500}' http://fixture.local/api/config

# WT32-ETH01: four outputs
curl -X POST -H "Content-Type: application/json" \
     -d '{"outputs":[{"pin":12,"count":126,"order":"BRG","name":"Porch"},{"pin":1,"count":125},{"pin":3,"count":125},{"pin":4,"count":125}]}' \
     http://fixture.local/api/config
```

---

## WiFi

### `GET /api/wifi/scan`

The first call starts an asynchronous scan and returns `{"scanning":true}`. Poll every 1–2 s until you get:

```json
{ "scanning": false,
  "networks": [ {"ssid":"HomeNet","rssi":-52,"secure":true,"channel":6}, … ] }
```

The list is de-duplicated by SSID, strongest first. The next call starts a fresh scan.

### `POST /api/wifi`

```json
{ "ssid": "HomeNet", "pass": "secret123" }
```

The device saves the credentials and connects immediately without rebooting. If the setup AP is up, it
stays up, so the client can poll `GET /api/info` → `wifi.status` / `wifi.ip` to see the result. The AP
shuts down 60 s after the connection succeeds.

`{"ssid": ""}` forgets the network and reboots into setup-hotspot mode (`"reboot": true`).

---

## Firmware and system

### `POST /api/update`

Multipart form upload of a `firmware.bin` (any field name). On success it returns
`{"ok":true,"reboot":true}` and restarts. Otherwise it returns `500` with the updater's error message.

```bash
curl -F "firmware=@.pio/build/usb/firmware.bin" http://fixture.local/api/update
```

### `POST /api/reboot`

Returns `{"ok":true,"reboot":true}` and restarts.

### `POST /api/reset`

Erases settings and saved state, then reboots into the setup hotspot.

---

## Home Assistant example

```yaml
rest_command:
  fixture_set:
    url: "http://kitchen-shelf.local/api/state"
    method: POST
    content_type: "application/json"
    payload: '{{ payload }}'

# usage in an automation/script:
# - service: rest_command.fixture_set
#   data: { payload: '{"on":true,"color":"#ff9329","bri":180}' }
```

