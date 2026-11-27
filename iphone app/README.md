# LoGiX iPhone app

A native SwiftUI app (iOS 17+) for the LoGiX Base Fixture firmware. It finds fixtures on your Wi-Fi
and controls them live.

## Features

- **Discovery.** Bonjour `_logix._tcp`, keyed on the TXT `id` (MAC), so a device keeps its identity when its IP or name changes. Other LoGiX devices (outlets, plugs, vents) appear in the list too, with an *Open Web UI* link.
- **Multiple outputs (firmware 1.10+).** Fixtures with several outputs (WT32-ETH01: up to 4) show an output picker. *All* controls the whole fixture: master power, master brightness, and the look of every output. Picking one output controls just that output. A hint appears when the outputs are showing different things.
- **Three colors per output.** Color 1–3 slots; the ones the current effect doesn't use are dimmed. Picking a color while an effect with its own palette is running (Rainbow, Christmas, team colors…) switches to Solid.
- **Fixture models.** Each output's model (string lights, 91-pixel snowflake, 4 arches, 81-pixel light tree…) is shown with the icon the fixture serves at `/icon/<model>.webp`, and can be changed in settings.
- **Live control.** Power, brightness, color wheel and swatches, effects (grouped into Basic, Themes and Your Colors), and speed. State comes in over the WebSocket on port 81, so changes from the IR remote or the web UI show up right away. If the socket drops, the app falls back to HTTP polling. Slider drags are coalesced (about 60 ms) so the ESP8266 isn't flooded.
- **New fixture setup.** The app joins `LoGiX-Setup-XXXX` with `NEHotspotConfiguration`, lists the networks the fixture can see, sends the credentials, waits for `wifi.connected`, then returns the phone to your home Wi-Fi.
- **Settings.** Name; per output: GPIO (or *Not used*), name, fixture model, LED count (fixed for some models), color order; IR pin and power limit with the right GPIO lists for the ESP-12F or WT32-ETH01; Ethernet and Wi-Fi status; change or forget Wi-Fi; setup-hotspot password; API token on/off/regenerate; device info, restart, factory reset.
- **Firmware updates.** The app checks the latest GitHub release of `logichousepcb/logix-fixture-firmware` (cached 6 h), picks the right file for the board (`-eth01.bin`, `-4mb.bin` or `-1mb.bin`), downloads it and installs it on the fixture. You can also install a local `.bin`. Files built for the other chip are refused before upload.
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
LoGiX/Services/FirmwareReleases.swift  GitHub release check + download
LoGiX/Services/Keychain.swift     API token storage
LoGiX/Views/…                     device list, control screen, color wheel, settings, onboarding, pairing
```

The project uses Xcode's synchronized folders, so new files under `LoGiX/` join the target
automatically.

### Debug launch arguments (DEBUG builds only)

`-openDevice <id>` opens a device's control screen at launch. Add `-openOutput <n>` to select an output,
or `-openSettings YES` to go straight to its settings. These are handy for screenshots.

Compatibility: older firmware (before 1.10, one output and one color) still works; the app hides the
output picker and sends the single-strip settings fields.
