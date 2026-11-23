# LoGiX iPhone app

A native SwiftUI app (iOS 17+) for the LoGiX Base Fixture firmware. It finds fixtures on your Wi-Fi
and controls them live.

## Features

- **Discovery.** Bonjour `_logix._tcp`, keyed on the TXT `id` (MAC), so a device keeps its identity when its IP or name changes. Other LoGiX devices (outlets, plugs, vents) appear in the list too, with an *Open Web UI* link.
- **Live control.** Power, brightness, color wheel and swatches, effects, and speed. State comes in over the WebSocket on port 81, so changes from the IR remote or the web UI show up right away. If the socket drops, the app falls back to HTTP polling. Slider drags are coalesced (about 60 ms) so the ESP8266 isn't flooded.
- **New fixture setup.** The app joins `LoGiX-Setup-XXXX` with `NEHotspotConfiguration`, lists the networks the fixture can see, sends the credentials, waits for `wifi.connected`, then returns the phone to your home Wi-Fi.
- **Settings.** Name, LED count, color order, power limit, LED and IR pins, change or forget Wi-Fi, setup-hotspot password, and API token on/off/regenerate. Also firmware upload of a `.bin` with progress, device info, restart, and factory reset.
- **API tokens.** Stored per device in the Keychain. You can paste a token or scan the pairing QR code from the web UI (*Device › API token*).
- **Add by address.** For fixtures that don't show up over Bonjour, for example on another subnet or VLAN.

## Build and run

1. Open `LoGiX.xcodeproj` in Xcode.
2. Select the **LoGiX** target, then *Signing & Capabilities*, and choose your Team. Change the bundle id (`com.logix.fixtures`) if Xcode says it's taken.
3. Run on your iPhone. On first launch, allow **Local Network** access, which discovery needs.

### Hotspot Configuration capability

In-app setup joins the fixture's hotspot automatically. That needs the *Hotspot Configuration*
entitlement (`LoGiX/LoGiX.entitlements`), and a **free (Personal Team) Apple ID can't sign it**. If
signing fails for that reason, delete the entitlement from *Signing & Capabilities*. Setup still works:
when the automatic join fails, the app asks you to join `LoGiX-Setup-XXXX` in Settings › Wi-Fi and then
continues from there.

## Layout

```
LoGiX/LoGiXApp.swift              app entry; starts/stops discovery with the app lifecycle
LoGiX/Models/Models.swift         API types (state, info, config, Wi-Fi), DeviceRecord, RGB/HSV, theme
LoGiX/Services/BonjourBrowser.swift  NWBrowser for _logix._tcp + IPv4 resolution
LoGiX/Services/DeviceStore.swift  known devices: discovered + manual + QR pairing, persisted in UserDefaults
LoGiX/Services/Device.swift       per-device connection (WebSocket/HTTP), coalesced commands
LoGiX/Services/FixtureAPI.swift   HTTP client for API.md (incl. multipart firmware upload)
LoGiX/Services/Hotspot.swift      NEHotspotConfiguration join/leave
LoGiX/Services/Keychain.swift     API token storage
LoGiX/Views/…                     device list, control screen, color wheel, settings, onboarding, pairing
```

The project uses Xcode's synchronized folders, so new files under `LoGiX/` join the target
automatically.

### Debug launch arguments (DEBUG builds only)

`-openDevice <id>` opens a device's control screen at launch. Add `-openSettings YES` to go straight to
its settings. These are handy for screenshots.
