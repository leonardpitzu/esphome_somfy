# ESPSomfy

This repository provides an **external ESPHome component** to control Somfy motorised covers (and future device types). It uses a **hub architecture** where radio hardware is configured once in a `somfy:` hub block, and individual devices reference it via `somfy_id`. This makes it easy to add multiple covers sharing a single radio, and to extend with new platforms (switches, lights, etc.) in the future.

Two protocols are supported:

| Type | Protocol | Frequency | Modulation | Use case |
|------|----------|-----------|------------|----------|
| `rts` | Somfy RTS | 433.42 MHz | OOK (Manchester) | Standard Somfy remotes/motors |
| `iohc` 1W | io-homecontrol 1W | 868.95 MHz | 2-FSK, 38.4 kbaud | Newer Somfy motors (one-way control) |
| `iohc` 2W | io-homecontrol 2W | 868.25/868.95/869.85 MHz | 2-FSK, 38.4 kbaud | Bidirectional with status feedback |

## Architecture

```
somfy:              ← Hub (owns radio hardware)
  type: rts/iohc

cover:              ← Device (references hub)
  platform: somfy
  somfy_id: <hub>
```

The hub handles all radio TX/RX and protocol framing. Device classes (covers today, more in the future) handle device-specific logic (rolling codes, position tracking, pairing) and delegate radio operations to their hub.

## Required hardware

- **ESP32** (any variant)
- **For RTS:** CC1101 433 MHz module + antenna (via ESPHome `remote_transmitter` / `remote_receiver`)
- **For iohc:** CC1101 868 MHz module + antenna (via ESPHome native `cc1101` component in packet mode)

## Installation

```yaml
external_components:
  source: github://leonardpitzu/esphome_somfy@main
  components: [somfy]
  refresh: 600s
```

---

<details>
<summary><h2>RTS configuration</h2></summary>

### Minimal (TX only)

```yaml
remote_transmitter:
  id: transmitter
  pin: GPIO04
  carrier_duty_percent: 100%

somfy:
  - id: rts_radio
    type: rts
    remote_transmitter: transmitter

button:
  - platform: template
    id: program_livingroom_door
    name: "Prog Living Room Door"
    entity_category: config

cover:
  - platform: somfy
    type: rts
    id: livingroom_door
    name: "Living Room Door"
    device_class: shutter
    open_duration: 40s
    close_duration: 40s
    storage_key: KeyLivingDoor
    remote_code: 0x088331
    prog_button: program_livingroom_door
    somfy_id: rts_radio
```

### With receiver (RX — sync with physical remotes)

```yaml
remote_receiver:
  id: receiver
  pin: GPIO03

somfy:
  - id: rts_radio
    type: rts
    remote_transmitter: transmitter
    remote_receiver: receiver

text_sensor:
  - platform: template
    id: somfy_rx_last
    name: "Detected Somfy Remote"

cover:
  - platform: somfy
    type: rts
    id: livingroom_door
    name: "Living Room Door"
    device_class: shutter
    open_duration: 40s
    close_duration: 40s
    storage_key: KeyLivingDoor
    remote_code: 0x088331
    prog_button: program_livingroom_door
    somfy_id: rts_radio
    detected_remote: somfy_rx_last
    allowed_remotes:
      - 0x92FB39
```

### Pairing (PROG)

The ESPHome device acts like a new Somfy RTS remote identified by `remote_code`. Before it can control a motor, that motor must be paired:

1. Put the motor into *programming mode* using an **already paired** physical remote (hold PROG ~2 s until the motor jogs).
2. Within the programming window, press the **Prog …** button in Home Assistant.
3. The motor jogs again to confirm pairing.

### Detecting remote IDs

1. Add a `text_sensor` (as above) and set `detected_remote` on the cover.
2. Press a button on the physical remote.
3. Read the decoded `0x......` ID from the text sensor.
4. Add it to `allowed_remotes` and recompile.

</details>

---

<details>
<summary><h2>iohc configuration (1W — one-way)</h2></summary>

Requires ESPHome 2026.4+ with the native `cc1101` component in packet mode.

```yaml
spi:
  clk_pin: GPIO07
  mosi_pin: GPIO09
  miso_pin: GPIO08

cc1101:
  id: cc1101_radio
  cs_pin: GPIO6
  gdo0_pin: GPIO04
  frequency: 868.95MHz
  modulation_type: 2-FSK
  symbol_rate: 38400
  fsk_deviation: 19.2kHz
  filter_bandwidth: 100kHz
  packet_mode: true
  packet_length: 0
  crc_enable: false
  sync_mode: "16/16"
  sync1: 0xFF
  sync0: 0x33
  num_preamble: 4

button:
  - platform: template
    id: program_bedroom_blind
    name: "Prog Bedroom Blind"
    entity_category: config

somfy:
  - id: iohc_radio
    type: iohc
    cc1101_id: cc1101_radio

cover:
  - platform: somfy
    type: iohc
    id: bedroom_blind
    name: "Bedroom Blind"
    device_class: shutter
    open_duration: 30s
    close_duration: 30s
    storage_key: KeyBedBlind
    remote_code: 0xAABBCC
    prog_button: program_bedroom_blind
    somfy_id: iohc_radio
    # encryption_key: "34C3466ED88F4E8E16AA473949884373"  # optional: custom key
```

### Notes

- 1W commands (open/close/stop) are sent on **868.95 MHz** with the public io-homecontrol transfer key.
- The component handles CRC-16 (Kermit) and AES-128 HMAC in software.
- For bidirectional (2W) support, see the next section.

### RX state-sync (sync with physical remotes)

Just like RTS, the iohc hub can keep Home Assistant in sync when a motor is
driven by an **original io-homecontrol remote**. The CC1101 is always listening,
so no extra receiver hardware is needed — just add a text sensor and/or an
allow-list to the cover:

```yaml
text_sensor:
  - platform: template
    id: somfy_iohc_rx_last
    name: "Detected iohc Remote"

cover:
  - platform: somfy
    type: iohc
    id: bedroom_blind
    # ... other options ...
    somfy_id: iohc_radio
    detected_remote: somfy_iohc_rx_last
    allowed_remotes:
      - 0x112233
```

1. Set `detected_remote` and press a button on the physical remote.
2. Read the decoded `0x......` node ID from the text sensor.
3. Add it to `allowed_remotes` and recompile.

When a listed remote sends open/close/stop, the HA cover animates to match using
the configured `open_duration`/`close_duration` (assumed-state, time-based). An
empty `allowed_remotes` means *accept all* (discovery mode). RX-sync code is only
compiled in when `detected_remote` or `allowed_remotes` is configured.

</details>

---

<details>
<summary><h2>iohc configuration (2W — bidirectional)</h2></summary>

2W mode enables authenticated bidirectional communication with io-homecontrol actuators. The controller sends a command, the actuator replies with a cryptographic challenge, and the controller responds to prove it holds the system key. This provides command acknowledgement and status feedback.

### Requirements

- ESPHome 2026.4+ with native `cc1101` component in packet mode
- The **system key** (per-installation AES-128 key) — obtained during initial pairing or from a paired controller backup
- The actuator's **node address** (3-byte, e.g. `0xABCDEF`)

### Protocol overview

1. Controller sends CMD_EXECUTE (0x00) on 868.95 MHz
2. Actuator replies with challenge (CMD 0x3C, 6 random bytes) on one of 3 RX channels (868.25 / 868.95 / 869.85 MHz)
3. Controller computes AES-128-ECB response from IV (frame data + rolling checksum + challenge) and sends CMD 0x3D
4. Actuator validates and executes; sends status (CMD 0xFE) back

### Example configuration

```yaml
somfy:
  - id: iohc_radio
    type: iohc
    cc1101_id: cc1101_radio

cover:
  - platform: somfy
    type: iohc
    id: living_room_blind
    name: "Living Room Blind"
    device_class: shutter
    open_duration: 25s
    close_duration: 25s
    storage_key: KeyLivBlind
    remote_code: 0xAABBCC
    prog_button: program_living_room
    somfy_id: iohc_radio
    mode: 2w
    target_node: 0xDEADBE
    encryption_key: "YOUR_SYSTEM_KEY_HEX_32_CHARS_HERE"
```

### Additional cover options (2W)

| Option | Required | Description |
|--------|----------|-------------|
| `mode` | no | `1w` (default) or `2w` |
| `target_node` | 2W only | 3-byte hex address of the target actuator |
| `encryption_key` | 2W only | System key (32-char hex AES-128 key) |

### Notes

- TX is always on **868.95 MHz**; RX hops across 3 channels (868.25 / 868.95 / 869.85 MHz) with 2.7 ms dwell time.
- The session has a 3-second timeout with up to 2 retries.
- Unlike 1W, 2W frames do not use a rolling code — authentication is challenge/response based.
- The system key is specific to your installation. It is **not** the public transfer key used by 1W.
- RX state-sync (`detected_remote` / `allowed_remotes`) also works in 2W: foreign remote → motor commands are heard on the 868.95 MHz TX channel while the radio idles there. Verify decoding against your actuators on first use.

</details>

---

## Common options

### Hub (`somfy:`)

| Option | Required | Description |
|--------|----------|-------------|
| `type` | yes | `rts` or `iohc` |
| `remote_transmitter` | RTS only | ESPHome `remote_transmitter` ID |
| `remote_receiver` | no | ESPHome `remote_receiver` ID (enables RX decode) |
| `cc1101_id` | iohc only | ESPHome `cc1101` component ID |

### Cover (`platform: somfy`)

| Option | Required | Description |
|--------|----------|-------------|
| `type` | yes | `rts` or `iohc` |
| `somfy_id` | yes | Reference to the `somfy:` hub |
| `open_duration` | yes | Time for a full open travel |
| `close_duration` | yes | Time for a full close travel |
| `storage_key` | yes | NVS key for rolling code persistence (max 15 chars) |
| `remote_code` | yes | Hex address of this virtual remote |
| `prog_button` | yes | Button entity to trigger PROG pairing |
| `detected_remote` | no | Text sensor for decoded remote IDs (RTS & iohc) |
| `allowed_remotes` | no | List of physical remote IDs to accept for RX state-sync (RTS & iohc) |
| `encryption_key` | no | Custom AES key hex string (iohc 1W: defaults to transfer key; iohc 2W: system key, required) |
| `mode` | no | iohc only: `1w` (default) or `2w` |
| `target_node` | 2W only | 3-byte hex address of target actuator |

## Credits

This project builds on prior work:
- https://github.com/HarmEllis/esphome-somfy-cover-remote (basic rts implementation)
- https://github.com/fawick/somfy_cover_2025.12 (same basic rts implementation but using esphome's standard cc1101 component)
- https://github.com/rstrouse/ESPSomfy-RTS (rts rx decoder reference)
- https://github.com/Velocet/iown-homecontrol (io-homecontrol protocol documentation)
- https://github.com/rspaargaren/iohomecontrol (io-homecontrol implementation reference)

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
