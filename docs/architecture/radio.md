# Radio Abstraction Layer

```
 WASM App / Shell
       │
  akira_rf_api       ← enum-select, chip init/deinit, WASM boundary
       │
 radio_manager       ← registry, active handle, bus lock, RX daemon, data path
       │
  radio_handle_t     ← vtable (radio_ops_t) + capability bitmask
  ┌────┼──────────────────────────────────────────┐
lr2021 cc1121 lr1121 cc1101 nrf24l01  wifi  ble  …
```

## Layers

| Layer | Owns |
|---|---|
| `akira_rf_api` | enum→handle mapping, per-chip init-once tracking, WASM exports |
| `radio_manager` | registry, active handle, bus locking, RX daemon, `send/recv/freq/power/rssi` |
| driver | hardware ops vtable, static `radio_handle_t`, auto-registers at boot |

## Key types

| Type | Purpose |
|---|---|
| `radio_handle_t` | one chip: name, type, caps, `radio_ops_t*` |
| `radio_ops_t` | vtable: init/deinit/send/recv/set_frequency/set_power/… |
| `RADIO_CAP_*` | bitmask: TX, RX, MOD_LORA, MOD_FSK, BAND_SUBGHZ, … |
| `radio_type_t` | protocol family: WiFi / BLE / 802154 / SubGHz |

## Classification

`radio_type_t` = protocol family. All sub-GHz chips set `RADIO_TYPE_SUBGHZ`.

Multi-mode chips (LR1121, LR2021) are `RADIO_TYPE_SUBGHZ` — LoRa support expressed via `RADIO_CAP_MOD_LORA`. Use caps to query modulation; use type to query protocol family.

| Chip | Type | Key caps |
|---|---|---|
| CC1121 | `RADIO_TYPE_SUBGHZ` | MOD_FSK, BAND_SUBGHZ |
| CC1101 | `RADIO_TYPE_SUBGHZ` | MOD_FSK, MOD_OOK, BAND_SUBGHZ |
| LR2021 | `RADIO_TYPE_SUBGHZ` | MOD_FSK, MOD_LORA, BAND_SUBGHZ |
| LR1121 | `RADIO_TYPE_LORA` | MOD_LORA, BAND_SUBGHZ |
| NRF24L01 | `RADIO_TYPE_NONE` | MOD_FSK, BAND_2GHZ4 |

## Select a chip

```c
// by enum — shell / WASM
akira_rf_select(AKIRA_RF_CHIP_LR2021);

// by capability — protocol stacks
radio_handle_t *h = radio_manager_get_by_caps(RADIO_CAP_MOD_LORA | RADIO_CAP_BAND_SUBGHZ);
```

## RX daemon

When `CONFIG_AKIRA_RF_RX_DAEMON=y`, `radio_manager` spawns a background thread that polls the active handle and enqueues packets. Callers drain via `radio_manager_recv_pop()` / `akira_rf_recv_pop()`.

Daemon uses `K_NO_WAIT` on the handle lock — skips poll cycle if TX in progress.
