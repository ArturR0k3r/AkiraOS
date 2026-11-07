---
layout: default
title: Flash your board
nav_order: 2
permalink: /flash
---

# Flash AkiraOS from your browser

Connect an ESP32 board over USB and flash AkiraOS directly from this page — no toolchain,
no `esptool`. This uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) over Web
Serial, so it needs **Chrome, Edge or another Chromium browser** (Web Serial is not
available in Firefox or Safari).

<div id="flash-ui" style="border:1px solid #444;border-radius:8px;padding:1rem;margin:1rem 0;">
  <label for="board" style="font-weight:600;">Board:</label>
  <select id="board" style="margin:0 .5rem;padding:.3rem;">
    <option value="akiraconsole">AkiraConsole (ESP32-S3)</option>
    <option value="akiraconsole_prod">AkiraConsole Prod (ESP32-S3)</option>
    <option value="esp32s3_devkitm">ESP32-S3 DevKitM</option>
  </select>
  <p style="margin:.75rem 0;">
    <esp-web-install-button id="btn" manifest="/flash/akiraconsole.json">
      <button slot="activate" style="padding:.5rem 1rem;border-radius:6px;border:1px solid #7f5af0;background:#7f5af0;color:#fff;cursor:pointer;">⚡ Connect &amp; flash</button>
      <span slot="unsupported">Your browser does not support Web Serial — use Chrome or Edge.</span>
      <span slot="not-allowed">Serial access was blocked. Allow it and retry.</span>
    </esp-web-install-button>
  </p>
  <p style="font-size:.9em;color:#aaa;margin:0;">Flashing erases the whole chip and writes MCUboot + AkiraOS. After it finishes, the device reboots into AkiraOS.</p>
</div>

<script type="module" src="https://unpkg.com/esp-web-tools@10.0.3/dist/web/install-button.js?module"></script>
<script>
  (function () {
    var sel = document.getElementById('board');
    var btn = document.getElementById('btn');
    if (sel && btn) {
      sel.addEventListener('change', function () {
        btn.manifest = '/flash/' + sel.value + '.json';
      });
    }
  })();
</script>

## After flashing — Wi-Fi provisioning

AkiraOS boots a serial shell at **115200 baud**. In the browser serial console (or any
terminal), set your network and the device reconnects on reboot:

```
wifi connect "<ssid>" "<password>"
wifi status
```

Once it has an IP, install apps over Wi-Fi (no reflash) or use the
[app catalogue](https://console.app.akiraos.dev):

```
curl -X POST --data-binary @app.wasm \
  "http://<device-ip>/api/apps/install?name=app"
```

## Notes & limits

- **ESP32 only.** nRF54L15 flashes with `west flash` / `nrfutil`; RP2040/RP2350 mount as a
  USB drive — copy the `.uf2` from the [release](https://github.com/ArturR0k3r/AkiraOS/releases).
- **AkiraConsole Prod has no auto-download-reset:** start the flash, then during
  "Connecting…" hold **BOOT** and tap **RESET**, keeping BOOT held until writing begins.
- Each board's manifest points at the `akiraos-<board>-flash.bin` asset of the
  [latest release](https://github.com/ArturR0k3r/AkiraOS/releases/latest) — a full-flash
  image (MCUboot + app). If flashing reports a missing file, that release has not published
  the merged image yet.
- Prefer the command line? See [QUICKSTART.md](https://github.com/ArturR0k3r/AkiraOS/blob/main/QUICKSTART.md).
