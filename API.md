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

### `GET /api/state`

```json
{
  "on": true,
  "bri": 128,
  "color": [255, 147, 41],
  "hex": "#ff9329",
  "effect": 0,
  "effectName": "Solid",
  "speed": 128
}
```

### `POST /api/state`

Send only the fields you want to change. The response is the full new state.

| Field | Type | Notes |
| --- | --- | --- |
| `on` | `true` / `false` / `"toggle"` | `"on"`, `"off"`, `1`, `0` also accepted |
| `bri` | 0–255 | Master brightness (fades over 400 ms) |
| `color` | `[r,g,b]` or `"#rrggbb"` / `"rrggbb"` | |
| `r`, `g`, `b` | 0–255 | Set individual channels |
| `effect` | id (0–15) or name (case-insensitive) | See `/api/effects` |
| `speed` | 0–255 | Effect speed |

Changing color, brightness, or effect does **not** turn the light on. Include `"on": true` if you want that.

```bash
curl -X POST -H "Content-Type: application/json" \
     -d '{"on":true,"bri":200,"color":"#00a0ff","effect":"Breathe"}' \
     http://fixture.local/api/state
```

### `GET /api/set`

Same fields as query parameters. Handy for buttons, browsers, and simple automations:

```
/api/set?on=toggle
/api/set?on=1&bri=255&color=ff0000
/api/set?effect=rainbow%20cycle&speed=60
/api/set?r=0&g=255&b=0
```

### `GET /api/effects`

```json
[{"id":0,"name":"Solid"},{"id":1,"name":"Breathe"},{"id":2,"name":"Rainbow"},
 {"id":3,"name":"Rainbow Cycle"},{"id":4,"name":"Color Wipe"},{"id":5,"name":"Theater Chase"},
 {"id":6,"name":"Twinkle"},{"id":7,"name":"Comet"},{"id":8,"name":"Candle"},{"id":9,"name":"Christmas"},
 {"id":10,"name":"Buffalo Bills"},{"id":11,"name":"Buffalo Sabres"},{"id":12,"name":"Lake Shore"},
 {"id":13,"name":"Buffalo Bills Solid"},{"id":14,"name":"Buffalo Sabres Solid"},{"id":15,"name":"Lake Shore Solid"}]
```

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
  "fixture": "snowflake91",
  "hostname": "molly-s-tree",
  "build": "Nov 22 2026 10:15:02",
  "mac": "48:E7:29:71:F3:14",
  "chipId": "71F314",
  "capabilities": { "rgb": true, "white": false, "effects": 10, "speed": true, "ir": true,
                    "identify": true, "websocket": true, "maxLeds": 800, "ota": true },
  "wifi": {
    "mode": "sta",               // "ap" | "sta" | "ap+sta"
    "ssid": "HomeNet",
    "status": "connected",       // "not configured" | "connecting" | "connected" |
                                 // "wrong password" | "network not found" | "connection failed" | "idle"
    "connected": true,
    "ip": "192.168.1.50",        // only when connected
    "rssi": -58,                 // only when connected
    "apActive": false,
    "apSsid": "…", "apIp": "192.168.4.1"   // only while the setup AP is up
  },
  "leds": { "pin": 2, "count": 30, "colorOrder": "GRB",
            "estimatedMa": 312, "maxMa": 500, "powerLimited": false },
  "ir":   { "pin": 4, "enabled": true, "lastCode": "0xF720DF", "lastAgeMs": 5120 },
  "system": { "uptimeS": 3600, "freeHeap": 38000, "sketchSize": 433000,
              "freeSketchSpace": 610000, "flashSize": 4194304, "resetReason": "Power On" }
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
{"type":"state","state":{"on":true,"bri":128,"color":[255,147,41],"hex":"#ff9329","effect":9,"effectName":"Christmas","speed":128}}
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
  "ledPin": 2, "ledCount": 30, "colorOrder": "GRB", "irPin": 4, "maxMa": 500, "fixture": "custom",
  "setupSsid": "LoGiX-Setup-F314", "apiToken": "" }
```

### `POST /api/config`

Send any subset of fields. All of them are validated before anything is saved.

| Field | Values | Applies |
| --- | --- | --- |
| `name` | 1–32 chars. Hostname = lowercase, non-alphanumerics → `-` | reboot |
| `apPass` | `""` (open) or 8–63 chars | next time the AP starts |
| `ledPin` | 0, 1, 2, 3, 4, 5, 12, 13, 14, 15 | reboot |
| `ledCount` | 1–800 | reboot |
| `colorOrder` | `RGB` `RBG` `GRB` `GBR` `BRG` `BGR` | immediately |
| `irPin` | -1 (off), 0–5, 12–14 | reboot |
| `maxMa` | 0 (no limit) or 100–20000 | immediately |
| `fixture` | `custom`, `string` (String Lights), `snowflake91` (91 Pixel Snowflake; forces `ledCount` 91). Icon at `/icon/<fixture>.webp` | immediately (reboot if the LED count changes) |
| `apiToken` | `"generate"` (random 24-hex), a 16–64 char `[A-Za-z0-9_-]` string, or `""` (off) | immediately; reboots when turning auth on/off (mDNS `auth` flag) |

Response: `{"ok":true}`, or `{"ok":true,"reboot":true}` when the device is about to restart (about 1.5 s).
When the token changes, the response includes the new `apiToken`.

```bash
curl -X POST -H "Content-Type: application/json" \
     -d '{"name":"Kitchen Shelf","ledCount":60,"maxMa":1500}' http://fixture.local/api/config
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

