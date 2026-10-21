# Bridge protocol (version 1)

The bridge relays the pendant dongle's raw USB HID reports over one TCP
connection. It doesn't interpret them: decoding buttons and the jog wheel, and
building LCD updates, stays in the client (for example the Carvera Controller's
`whb04.py`). That means any client that already speaks the WHB04 HID protocol
through hidapi can be adapted by swapping the transport.

## Connection

- **The bridge is the TCP server** (default port **5104**) and the controller
  application is the client.
- **One client is served at a time.** A new connection replaces the current
  one, so a tablet that drops off WiFi and comes back can reconnect straight
  away, even while its old socket still looks alive on the bridge.
- **There is no authentication.** Anyone who can reach the port can use the
  pendant. Keep the bridge on a trusted network.
- **Both sides disable Nagle** (`TCP_NODELAY`) to keep jog-wheel latency low.

## Framing

Every message in both directions is a frame:

| Offset | Size     | Field                     |
|--------|----------|---------------------------|
| 0      | 1        | `type`                    |
| 1      | 1        | `length` of payload (0–255) |
| 2      | `length` | payload                   |

Receivers must ignore frame types they don't know. They can always skip them,
because the length is explicit.

## Bridge → client

| Type   | Name           | Payload | When |
|--------|----------------|---------|------|
| `0x01` | HELLO          | `[protocol_version]` followed by the firmware version in ASCII | First frame after connecting |
| `0x02` | STATUS         | `[dongle_attached]` (`0` or `1`) | Right after HELLO, then whenever the dongle is attached or removed |
| `0x03` | INPUT_REPORT   | One raw HID input report, **including the report ID** (as returned by hidapi `read()`). For the WHB04B: 8 bytes, starting with `0x04`. | For every report from the dongle |
| `0x04` | HEARTBEAT      | none | When nothing else has been sent for 500 ms |

## Client → bridge

| Type   | Name           | Payload | Notes |
|--------|----------------|---------|-------|
| `0x81` | FEATURE_REPORT | One raw HID feature report, **first byte = report ID** (hidapi `send_feature_report()` convention; `0` means the device uses no report IDs). For the WHB04B LCD: 8 bytes, starting with `0x06`. | The bridge sends it to the dongle as a `SET_REPORT(Feature)` control transfer |
| `0x82` | HEARTBEAT      | none | Send whenever nothing else has been sent for 500 ms |

## Timeouts

- **The bridge drops a client after 3 s** with nothing received from it.
- **The Carvera Controller treats 1.5 s of silence as a lost link.** It then
  stops any continuous jog in progress and reconnects.
- **TCP keepalive is enabled on the bridge** (5 s idle, 2 s interval, 3 probes)
  as a backstop.

## Example session

```
bridge → 01 07 01 76 31 2e 30 2e 30         HELLO  v1, "v1.0.0"
bridge → 02 01 01                           STATUS dongle attached
client → 81 08 06 fe fd fe 01 00 00 00      FEATURE (LCD packet 1/3)
client → 81 08 06 ...                       FEATURE (LCD packet 2/3)
client → 81 08 06 ...                       FEATURE (LCD packet 3/3)
bridge → 03 08 04 5a 00 00 0f 11 02 00      INPUT  axis X, step 0.1, jog +2
bridge → 04 00                              HEARTBEAT
client → 82 00                              HEARTBEAT
```

## WHB04B report formats (for reference)

These details live in the client, not the bridge. They match `whb04.py` in the
Carvera Controller and LinuxCNC's `xhc-whb04b-6` driver.

**Input report (8 bytes):**

`04 <random> <button1> <button2> <step_selector> <axis_selector> <jog_delta:int8> <checksum>`

**LCD update:** 21 bytes, split into three feature reports. Each report is
`0x06` followed by 7 data bytes. The data starts with the magic `FE FD FE`,
then a flags byte (step indicator, reset, and machine/work coordinates), three
coordinates, feed and spindle.
