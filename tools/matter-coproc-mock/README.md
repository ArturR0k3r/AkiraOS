# Mock Matter Co-processor

Host-side mock of the AkiraOS Matter/Thread co-processor. It answers the
co-processor IPC frames so the AkiraOS-side **accessory** (device-as-endpoint)
path can be exercised without a real esp-matter co-processor.

This script is also the **executable protocol contract** the real co-processor
firmware (`tools/matter-coproc/`, a separate track) must implement.

## Usage

Against a real UART link (AkiraOS on hardware, co-proc UART wired to a USB
serial adapter on your host):

```sh
python3 mock_coproc.py /dev/ttyUSB1
pip install pyserial   # if not already present
```

Against a PTY (useful when driving AkiraOS `native_sim` UART):

```sh
python3 mock_coproc.py --pty
# prints e.g. "[mock] PTY ready: /dev/pts/7" — point the sim's
# matter-coproc-uart at that path
```

## What it does

- Answers `STATUS`, `EP_ADD`, `ATTR_REPORT`, `PAIR_OPEN`, `QR_GET`
  (and the controller-direction `COMMISSION`/`SEND`/`SUBSCRIBE`).
- On the first `EP_ADD`, injects an unsolicited `ACC_CMD_EVENT` (OnOff / On)
  so `matter_cmd_poll` in the app immediately receives a command.
- Returns a fixed mock onboarding payload
  (QR `MT:MOCK.AKIRA000MATTER01`, manual `3497-011-2332`).

> For a purely on-target test with no host tooling, build the firmware with
> `CONFIG_AKIRA_MATTER_COPROC_MOCK=y` instead — that compiles the equivalent
> responder into the firmware itself (`src/connectivity/matter/matter_coproc_mock.c`).
