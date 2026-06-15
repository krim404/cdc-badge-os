# Host unit tests (`test/host/`)

Pure-logic unit tests that run on the host via the PlatformIO `native` environment, with **no
firmware flash and no hardware**. They cover portable logic only (KDFs, CRCs, base64, CBOR
encoders, framing, bounds, capability/GPIO policy, CP437 conversion).

Run them with:

```bash
~/.platformio/penv/bin/pio test -e native
```

Each test folder is named `test/host/<test_name>/` and is picked up only by `[env:native]`
(`test_filter = host/*`); the firmware env `cdc_badge_usb` ignores them (`test_ignore = host/*`).

Hardware/protocol behaviour that cannot run on the host (secure element, USB, BLE, e-paper) is
covered by the documented HIL test plans under `specs/001-current-system-spec/hil/` instead.
