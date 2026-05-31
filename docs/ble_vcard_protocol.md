# BLE vCard Protocol (Badge2Badge)

Protocol specification for BLE-based contact exchange between CDC Badges.

This document defines the BLE beacon (mini-card) and the BLE vCard exchange (full vCard), and explains how to implement it on your own device.

Notes:
- BLE exchange is intentionally **badge‑to‑badge only** (custom GATT). The standards‑compliant interop path is QR.
- BLE and WiFi cannot run in parallel. BLE UART must be disabled when this feature is active.
- **Exchange mode is separate** from mini‑card broadcast/scan. Both sides must enable exchange mode to discover and swap vCards.

## 1) Terms
- **Mini‑card**: short version (badge "username" + slogan (info1)) via BLE advertising.
- **vCard**: full version (vCard 4.0) via custom GATT exchange.
- **Initiator (Client)**: GATT Central, starts the connection/exchange.
- **Responder (Server)**: GATT Peripheral, provides its vCard and accepts one.

## 2) BLE Advertising (Mini‑card)

### 2.1 Goals
- Very short, passive identification in close range (~5 m).
- Mini‑cards are **not stored**.
- Optional toast/backlight alert, with blacklist/TTL to avoid spam.

### 2.2 Data layout
We use standard BLE Advertising fields. Short info is stored in standard AD types:

- **Complete Local Name (AD Type 0x09)**: username
- **Manufacturer Specific Data (AD Type 0xFF)**: slogan (max 12 bytes)

Suggested lengths:
- Name: max 29 bytes (fits 31‑byte ADV payload)
- Slogan: max 12 bytes

Example (simplified):
- AD Structure 1: `0x02 0x01 0x06` (Flags)
- AD Structure 2: `0x0A 0x09 "MAX MUST"`
- AD Structure 3: `0x0D 0xFF "SLOGAN TXT"`

### 2.3 Advertising interval
- Default: 60s (once per minute)
- Configurable via UI
- Must run in standby (cyclic)

### 2.4 Scanning
- Active scan recommended
- RSSI filter (default −75 dBm, approx. 5 m)
- Mini‑card is not stored

### 2.5 Blacklist / Notification
- If RSSI > threshold, show a nearby notification.
- Blacklist by BLE address with TTL = 1h (avoid repeated blinking for same person).

Pseudocode:
```
if rssi >= threshold and !blacklist.contains(addr):
  show_nearby_alert(name)
  backlight_blink(3)
  blacklist.mark(addr, ttl=1h)
```

## 3) BLE vCard Exchange (Custom GATT)

### 3.0 Exchange mode gating
- Exchange mode must be enabled on **both** devices.
- When exchange mode is ON, devices add the vCard service UUID to the **scan response** so peers can detect availability.
- When exchange mode is OFF, the GATT server **rejects** exchange control/RX writes.

### 3.1 Service and UUIDs
Custom 128‑bit service + 4 characteristics:

Service UUID:
- `8E2F1F20-8B5D-4D7A-9A6E-4C9D6A8B1A01`

Characteristics:
- **Data** (Indicate): `8E2F1F21-8B5D-4D7A-9A6E-4C9D6A8B1A01`
- **RX** (Write w/ Response): `8E2F1F22-8B5D-4D7A-9A6E-4C9D6A8B1A01`
- **Control** (Write): `8E2F1F23-8B5D-4D7A-9A6E-4C9D6A8B1A01`
- **Status** (Indicate + Read): `8E2F1F24-8B5D-4D7A-9A6E-4C9D6A8B1A01`

Security:
- LE Secure Connections + MITM + Bonding
- IO Capabilities: Display + Yes/No (Numeric Comparison)
- Passkey input as fallback
- **No auto‑confirm**; user must confirm

### 3.2 Transport frames
Chunked transfer with a small header:

**Client -> Server (RX / Write)**
- `0x01`: WRITE_START, followed by 16‑bit length (LE)
- `0x02`: WRITE_CONT
- `0x03`: WRITE_END

**Server -> Client (Data / Indicate)**
- `0x81`: DATA_START, followed by 16‑bit length (LE)
- `0x82`: DATA_CONT
- `0x83`: DATA_END

Payload follows immediately after the opcode (START has an extra 2‑byte length).

### 3.3 vCard format
- vCard 4.0 (text)
- Max 768 bytes (overall limit)
- Duplicates are discarded (hash or full compare)

### 3.4 Exchange flow (two‑way)

**Initiator (Client)**
1) Scan, user selects peer
2) Connect + Pairing (Numeric Comparison, Passkey if needed)
3) Service discovery
4) Enable indications for Data + Status (CCCD = `0x0002`)
5) Control write: `0x01` (request remote vCard)
6) Receive Data indications and reassemble remote vCard
7) Store if OK
8) Send own vCard via RX (Write) in chunks
9) Wait for Status notification (`0x01` ACCEPTED, `0x02` DECLINED, `0x03` BUSY)
10) Disconnect

**Responder (Server)**
1) Advertising active
2) Client connects + pairs
3) On Control=0x01: send own vCard via Data indications
4) On RX writes: reassemble and store
5) Send Status notification: `0x01` ACCEPTED, `0x02` DECLINED, `0x03` BUSY

### 3.5 State machine (Client)
- CONNECTING
- DISCOVERING
- ENABLING_NOTIFY
- REQUESTING_REMOTE
- RECEIVING_REMOTE
- SENDING_LOCAL
- WAITING_ACK
- DONE / FAIL (timeout)

### 3.6 Errors / Retry
- Timeout (default 30s) -> FAIL
- Status `0x02` (DECLINED) -> FAIL
- Retry is manual (user starts again)

### 3.7 Receive without active exchange
- RX‑Write only allowed when **Receive toggle** is enabled.
- During active exchange, RX is temporarily allowed even if Receive is OFF.

## 4) Device implementation details

### 4.1 BLE GAP
- Device name = username
- Manufacturer data = slogan (<= 12B)
- Advertising interval configurable via UI
- Scan interval (window + pause) configurable via UI

### 4.2 Pairing UI
- Numeric comparison: show code, user confirms Y/N
- Passkey display: show code for phone entry
- Passkey input: PIN view, Y=OK, N=Cancel

### 4.3 Storage
- Own vCard in NVS (e.g., key `vcard/own`)
- Received vCards in slots (max 100)
- Sort by last name
- Discard duplicates

### 4.4 Mini‑card UI
- Mini‑card is display/notification only
- Do not store

## 5) Example pseudocode (Client exchange)
```
connect(peer)
request_mtu()
secure_pairing()
find_service_and_chars()
write_cccd(data, 0x0002)
write_cccd(status, 0x0002)
write_char(control, 0x01)
recv_data_chunks() -> remote_vcard
store(remote_vcard)
send_local_vcard_chunks(rx)
wait_status()
```

## 6) Example pseudocode (Server handler)
```
on_control_write(0x01):
  if have_own_vcard:
    indicate_data_chunks()

on_rx_write(chunk):
  reassemble()
  if complete:
    if validate/store OK:
      notify_status(0x01)   # ACCEPTED
    else:
      notify_status(0x02)   # DECLINED
```

## 7) Compatibility and limits
- Without an app, there is no OS‑native vCard‑over‑BLE path on iOS/Android.
- QR code is the standard interop path.
- RSSI is noisy; threshold must be calibrated on real hardware.
