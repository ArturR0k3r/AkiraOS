---
layout: default
title: Akira & the Seal
parent: Architecture
nav_order: 8
---

# Akira & the Seal

AkiraConsole's identity has two parts:

- **Akira** is the console's crystal. It grows from how the console is used.
- **The Seal** is the moment of consent. Before a restricted power runs, the
  owner holds **A+B** while a ring charges.

Each seal is signed by the SE050 secure element (when fitted), appended to a
hash-chained ledger, and grows a shard on Akira.

Both need `CONFIG_AKIRA_CRYSTAL` (default on with the OS shell).

## Akira

Akira's anatomy mirrors the AkiraOS logo, which is what a fully grown Akira looks like.

| Part | What grows it |
|---|---|
| Core (bright centre) | days the console has been used |
| Shards (spikes) | one per (app, power) ever sealed, in that power's domain colour; longer with use |
| Glints (floating fragments) | achievements awarded by games |
| Cracks | an app tried a power it never declared and was refused; shown until mended |

Growth is logarithmic and never decays. Each console has its own shape, seeded
from the SE050 seal key, so no two are alike.

### Domains

| Domain | Capabilities | Colour |
|---|---|---|
| Radio | `rf.transceive`, `mesh`, `nfc` | violet |
| Air | `ble`, `ble.scan`, `ble.spam`, `wifi.inject` | blue |
| Net | `network`, `matter`, `ota`; app installs | cyan |
| Wire | `hid`, GPIO, UART, I2C, PWM, ADC; USB SD sharing | amber |
| Vault | `crypto` (keys in the SE050) | ice white |
| Play | sealed game records | magenta |

Everyday capabilities (display, input, storage, sensors) grow no shards.

### Where Akira appears

Akira is part of the OS, not an app:

- **Boot splash:** Akira grows from its core.
- **Wait screen:** Akira turns one notch per minute.
- **Settings → Akira & Seals:** the crystal turning, with three plain facts:
  how many powers are allowed (each one a shard), glints won in games, and
  cracks. Below them:
  - **How it works:** the four-page intro, with a practice Seal.
  - **Seal history:** every seal, each with its own sigil.
  - **Always allowed:** "always" grants. Revoking one makes the app ask again.
  - **Cracks:** mend them, or remove the app. When there are cracks, the
    cursor starts here.
- **Toasts:** when an app is refused a power it never declared, the shell
  shows "Blocked *app*" with the domain and where to look.

### First run

About six seconds after the first boot (on the home screen), the console
opens **How it works** once:

1. **Meet Akira:** what the crystal is.
2. **You stay in charge:** a practice ring. Hold A+B for one lap (this time)
   or two (always); B on its own says no. You can't skip this page with A.
3. **What grows Akira:** the six domains and their shard counts.
4. **Cracks:** what they mean and how to mend them.

Finishing it stores `akira/intro` in settings. It can be reopened from
**Settings → Akira & Seals → How it works**.

## The Seal

A restricted power runs only under a grant:

- **This time** lasts until the app stops. System actions get this scope only.
- **Always** lasts for this app *build* and this power until revoked. Updating
  or reinstalling the app asks again.

To grant, hold A+B. One lap of the ring seals *this time*; keep holding for
a second lap to seal *always*. A quick tap of **B** refuses. HOME refuses and
goes home. An unanswered seal is refused after `CONFIG_AKIRA_SEAL_TIMEOUT_S`.

The prompt says in one sentence what the power can do (for example "It can
type and click on the computer."), shows the keys to press, and counts down
the seconds left. A refusal doesn't stop the app; the call just fails.

If the asking app is stopped (HOME, crash, or `app stop`) while its seal is
on screen, the prompt closes and counts as refused. Other apps waiting for
the Seal give up after `CONFIG_AKIRA_SEAL_TIMEOUT_S` + 30 s instead of
waiting forever.

| Sealed power | Native calls gated |
|---|---|
| Transmit on radio | `rf_send`, `rf_tx_cw_start`, `rf_raw_replay` |
| Inject Wi-Fi frames | `wifi_deauth`, `wifi_capture_pmkid` |
| Broadcast BLE spam | `ble_spam_start` |
| Act as keyboard & mouse | HID key, type, mouse, gamepad, consumer and raw-report sends |
| Sign with a vault key | `crypto_p256_sign_se050` |
| Install an app (system) | AkiraPlay installs |
| Share SD card over USB (system) | the USB trust prompt |

Enforcement sits inside the native calls, right after the capability check, so
an app cannot skip the prompt. When a seal is needed while an app is running,
the shell borrows the display, keeps the app's frame, shows the seal, puts the
frame back, and only then lets the app continue.

### For app developers

```c
#include "akira_api.h"

/* Ask first, with a reason; returns at once if already granted. */
if (seal_request(AKIRA_SEAL_RF_TX, "replay 433.92 MHz capture") == 1) {
    rf_raw_replay(buf, len, rate, 1);
}

akira_glint(ACHIEVEMENT_FIRST_WIN);           /* a glint on Akira, no prompt */
seal_record("High score", score);             /* owner confirms; the record is signed */
```

`akira_ui_confirm_dialog()` in the SDK's
[`akira_ui.h`](https://github.com/ArturR0k3r/AkiraSDK/blob/ConsoleSDK/wasm_apps/common/akira_ui.h)
already routes the restricted capabilities (`RF_RAW_TX`, `WIFI_DEAUTH`,
`WIFI_PMKID`, `BLE_SPAM`, `USB_HID`, `VAULT_SIGN`) to the Seal. Apps that use
these imports need firmware with `CONFIG_AKIRA_CRYSTAL`, so set
`min_akiraos_version` accordingly.

## The ledger

Each entry is an `akira_seal_record_t` and holds:

- operation and scope (including refusals and timeouts)
- a monotonic counter
- wall-clock time and uptime
- app name and app-build identity
- the reason given
- the first 16 bytes of the previous record's SHA-256, so the ledger is a hash chain

With an SE050, the record's SHA-256 is signed on-chip with a P-256 key that
never leaves it (object `CONFIG_AKIRA_CRYSTAL_SE050_KEY_OBJID`).

Files live in `/lfs/akira/`:
- `ledger.bin` holds up to 256 entries; older ones rotate into `ledger.old`.
- `crystal.bin` is Akira's growth state.
- `grants.bin` holds the "always" grants.
- `cracks.bin` holds unmended cracks.

Akira's growth is also mirrored into an SE050 object, so a flash wipe doesn't
erase it (the newer copy wins on load). Grants and cracks don't survive a wipe.

Shell commands (`crystal …`):

| Command | Does |
|---|---|
| `crystal status` | growth per domain, cracks, kept seals |
| `crystal ledger [n]` | newest seals |
| `crystal pubkey` | the seal key, to verify signatures off-device |
| `crystal seal [op] [detail]` | test the Seal prompt |
| `crystal mend [i]` / `crystal revoke <app> [op]` | manage cracks and grants |

`CONFIG_AKIRA_CRYSTAL_DEBUG` adds `grow`, `day`, `glints`, `crack`, `ask` and
`reset` for UI work. Never enable it in a release build.

Sources: [`src/crystal/`](https://github.com/ArturR0k3r/AkiraOS/tree/main/src/crystal),
[`seal_screen.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/console_shell/seal_screen.c),
[`crystal_render.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/console_shell/ui/crystal_render.c).
