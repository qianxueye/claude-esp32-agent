# Device ↔ host wire protocol (version 3)

Single source of truth for `firmware/` and `host/`. Change both sides and this file together. Both sides refuse a peer whose `proto` differs from theirs.

## Links

The same frames run over USB and over two network links, Wi-Fi on the LAN and the tunnel. Only one link is active at a time (see Active link).

### USB

- USB-Serial-JTAG CDC of the ESP32-S3 (`/dev/cu.usbmodem*`, VID `0x303A`, PID `0x1001`). Baud rate is ignored.
- Find the board by its USB serial number (its MAC address), never by port name. With one Espressif USB-JTAG device attached, tools may pick it automatically; a specific serial is set in local, untracked configuration.
- The firmware console (`ESP_LOG`, `printf`) is on UART0 (GPIO43/44), not on USB. The app forwards its log lines to the host as `LOG` frames.
- Opening the port on macOS resets the board. The host opens it with DTR and RTS deasserted and then expects ROM text followed by HELLO (`reason` `boot`) about 2.5 s later. Receivers skip the ROM text by resynchronising.
- A receiver abandons a partial frame when its remaining bytes do not arrive within 200 ms.

### Wi-Fi

- TCP carrying TLS 1.2 with the single cipher suite `ECDHE-PSK-CHACHA20-POLY1305` (no fallback, no other version). Pre-shared key: 32 random bytes. PSK identity: `clawd-` followed by the USB serial number without colons, in uppercase.
- The board is the TLS client; the relay is the server on TCP port 47801 on all LAN interfaces. Both sides set `TCP_NODELAY`. The board turns Wi-Fi power saving off while a relay session is open.
- The board tries the relay addresses saved at pairing in order: the relay's `.local` host name (resolved by mDNS), then each saved IPv4 address. It retries with backoff (1 s doubling to 30 s).
- The relay keeps one board session; a new authenticated session replaces the old one.
- The board closes a network session, Wi-Fi or tunnel, active or not, on which no valid frame has arrived for 5000 ms, and reconnects with the backoff above.
- Over TCP a frame is dropped only before its first byte is written; the 200 ms partial-frame rule does not apply.
- A sleeping Mac or a stopped relay shows on the board as OFFLINE.

### Tunnel

- The board opens `wss://<host>/clawd/v1`, the remote URL from PAIR or SET_REMOTE. The outer TLS ends at the tunnel provider's edge, which forwards the WebSocket connection to the relay on `127.0.0.1` (TCP port 47802). The relay does not listen for the tunnel on any other interface.
- The relay answers only `GET /clawd/v1` with a WebSocket upgrade; any other request gets 404 with an empty body. No WebSocket extension is negotiated, so there is no compression.
- Inside the WebSocket connection runs the same TLS session as on Wi-Fi: TLS 1.2, `ECDHE-PSK-CHACHA20-POLY1305`, the same PSK identity and key, no session tickets, no resumption. Binary messages carry the inner TLS byte stream; neither side relies on message boundaries. A text message closes the connection.
- The tunnel provider sees the upgrade request, message sizes and timing, and the inner ClientHello with the PSK identity. It sees no frame content and cannot inject or change frames.
- The relay closes a tunnel connection that has not finished the upgrade within 10 s, has not finished the inner handshake 10 s after the upgrade, sends a message larger than 32 768 bytes, or sends more than 32 768 bytes before the inner session is authenticated.
- Liveness is the PING and PONG frames of the session. Neither side sends WebSocket pings; both answer them.
- A frame is dropped only whole and before encryption; an encrypted record is never dropped. The 200 ms partial-frame rule does not apply.
- Try order: on a network saved with `lan` true, the board tries the Wi-Fi relay addresses first and then the remote URL; on any other saved network it tries only the remote URL.

### Pairing

Pairing happens once over USB and carries everything the Wi-Fi link needs.

- The user runs `claude-voice pair` in their own terminal and types the Wi-Fi name and password there. The relay generates the PSK, stores it in the macOS Keychain (generic password, service `claude-voice`, account = PSK identity) and sends PAIR.
- The device accepts PAIR and FORGET only on the USB link.
- The PAIR and SET_REMOTE payloads are never logged: the relay log, `devctl` and LOG frames show at most the SSID and the remote URL.
- The device stores the Wi-Fi credentials, PSK, relay addresses, remote URL and Access credentials in NVS, in a flash partition of their own. Flash encryption is not enabled, so anyone with physical access to the board can read them.
- FORGET erases that partition's flash. A pairing replaced by a later PAIR can stay in flash until FORGET, because NVS marks replaced entries erased without erasing them.

### Revocation

- `claude-voice revoke` deletes a board's PSK from the Keychain without USB. The running relay re-reads the PSK of every identity it serves every 10 s (`key_reload_interval_s`). Only an item the Keychain reports as not found, or a different key, ends a PSK; a read error keeps it.
- When a PSK ends, the relay refuses new handshakes with it and closes every Wi-Fi and tunnel session, because its TLS library does not tell which identity a connection used. USB sessions stay open.
- The relay counts PSK removals. A connection notes the count when it is admitted and opens no session if the count changed before its session starts: the PSK is looked up at ClientKeyExchange, and a client may hold its Finished for up to the 10 s handshake timeout.
- The board keeps its saved Wi-Fi passwords, which can be read from its flash; change them after a board is lost.

### Active link

USB takes precedence. A HOST_HELLO on USB makes USB the active link and closes the Wi-Fi session. Wi-Fi becomes the active link when an authenticated TLS session sends HOST_HELLO and no valid frame has arrived on USB for 5000 ms. Device-to-host frames go to the active link only; host-to-device frames from the inactive link are ignored, except PAIR and FORGET on USB.

- Before the first HOST_HELLO after boot, no link is active and the device sends its frames on USB.
- The tunnel is a network link like Wi-Fi: USB takes precedence over both, and between a Wi-Fi and a tunnel session the newer authenticated one replaces the older.
- When paired, the device connects to the relay whether or not USB is active. The relay may close that session while USB is active; the device then retries with the usual backoff.

## Frame

| Field   | Size | Value |
|---------|------|-------|
| SYNC    | 2    | `0xC1 0xAD` |
| TYPE    | 1    | see tables below |
| LEN     | 2    | payload length, little-endian, 0–4096 |
| PAYLOAD | LEN  | |
| CRC     | 2    | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection, no final xor) over TYPE, LEN and PAYLOAD; little-endian |

Receiver: scan for SYNC; if LEN > 4096 or the CRC fails, drop the candidate and rescan from the byte after the first SYNC byte. Check value: CRC of ASCII `123456789` is `0x29B1`.

Both sides write whole frames or drop whole frames; a frame is never cut by a full buffer. Device TX never blocks when no host is reading: frames are dropped instead.

Integers are little-endian. JSON payloads are UTF-8 without a trailing NUL.

## Audio format

Signed 16-bit little-endian PCM, mono, 16 000 Hz, both directions.

## Device → host

| Type   | Name            | Payload |
|--------|-----------------|---------|
| `0x01` | HELLO           | JSON `{"proto":3,"fw":"<version>","rate":16000,"reason":"boot"|"reply"|"resume","link":"usb"|"wifi","paired":true|false,"vol":0-100,"vol_set":true|false}`. `vol` is the user volume; `vol_set` says a user volume exists (from storage, the knob, or a persist or seed SET_VOLUME). |
| `0x02` | LOG             | UTF-8 log line. |
| `0x10` | MIC_AUDIO       | 640 bytes of PCM (20 ms). Sent only while the state is LISTENING. |
| `0x20` | INPUT           | `u8` code: `1` TAP, `2` LONG_PRESS, `3` BOOT_BUTTON (BOOT or PWR key). |
| `0x21` | SPK_STATUS      | `u32` bytes queued, `u8` flags (bit0 playing, bit1 drained after SPK_END), `u32` consumed total. |
| `0x22` | VOLUME          | `u8` user volume. Sent when a knob release changed it and after a persist or applied seed SET_VOLUME. Best effort; HELLO resynchronises. Never sent during a drag or for a transient set. |
| `0x30` | PONG            | `u32` nonce copied from PING. |
| `0x31` | SELFTEST_RESULT | JSON, see SELFTEST. |
| `0x32` | SNAP_BEGIN      | JSON `{"w":466,"h":466,"fmt":"RGB565"}` (RGB565 little-endian, row-major). |
| `0x33` | SNAP_DATA       | Next bytes of the snapshot, ≤ 4096 per frame, in order. |
| `0x34` | SNAP_END        | `u32` total byte count. |
| `0x35` | PAIR_RESULT     | JSON `{"ok":true}` or `{"ok":false,"error":"<reason>"}`. Reasons: `too long`, `bad json`, `bad field <name>` (a required field is missing or has the wrong type, or `networks` is malformed), `bad psk`, `identity mismatch`, `bad ssid`, `bad password`, `too many networks`, `bad relay`, `bad remote_url`, `storage error`. A refused PAIR keeps the previous pairing; after `storage error` only until the board restarts, and the next boot uses what the flash then holds. |
| `0x36` | WIFI_STATUS     | JSON `{"wifi":"off"|"connecting"|"connected"|"failed","ip":"…","rssi":-55,"relay":"none"|"connecting"|"connected"|"auth_failed"|"unreachable"|"captive"|"tls_failed"}`. `captive`: the network intercepts the tunnel (name lookup fails, or the upgrade gets a redirect or another answer than 101); `tls_failed`: the outer TLS certificate check failed. Sent on every change and in reply to HOST_HELLO. |
| `0x37` | FORGET_RESULT   | JSON `{"ok":true}` or `{"ok":false,"error":"<reason>"}`. Reason: `storage error` (the pairing is dropped from RAM and Wi-Fi stops anyway). |

HELLO `link` is `"tunnel"` when the board sends it over the tunnel.

## Host → device

| Type   | Name       | Payload |
|--------|------------|---------|
| `0x81` | HOST_HELLO | JSON `{"proto":3}`. |
| `0x82` | SET_STATE  | `u8` state (table below). |
| `0x83` | SET_VOLUME | `u8` volume (values above 100 are clamped to 100), then an optional `u8` mode: `0` transient (also when absent), `1` persist, `2` seed. Other modes and other lengths are ignored, as is any set while the knob is armed. See Volume. |
| `0x84` | PAIR       | USB only. JSON `{"ssid":"…","password":"…","psk_identity":"…","psk":"<64 hex>","relay_names":["…local"],"relay_addrs":["10.0.0.2"],"relay_port":47801}`. Optional: `networks`, up to 4 `{"ssid":"…","password":"…","lan":true|false}` that replace all saved networks (`lan` true where the relay addresses are reachable); `remote_url` (`"wss://…/clawd/v1"`); `access_id` and `access_secret` (tunnel provider credentials sent as headers on the upgrade). The relay sends `networks` with the main network first. At most 4 `relay_names` of at most 63 bytes each and at most 8 `relay_addrs` (dotted-quad IPv4). Payload at most 1023 bytes (the device keeps 1024 bytes for the text and its terminator); the relay refuses a longer PAIR before sending it. |
| `0x85` | FORGET     | USB only. Empty. Erase the saved networks and Wi-Fi credentials, PSK, relay addresses, remote URL and Access credentials, then send FORGET_RESULT. Wi-Fi and any network session stop when the network step in progress ends (bounds in `network.md`). |
| `0x86` | SET_REMOTE | JSON `{"url":"wss://…/clawd/v1"}`, optionally with `access_id` and `access_secret`. Sent by the relay after HELLO on any authenticated link; the device stores the values only when they changed. Payload at most 511 bytes; the device ignores a longer one. |
| `0x90` | SPK_AUDIO  | PCM, ≤ 4096 bytes, even length. Appended to the playback queue. |
| `0x91` | SPK_FLUSH  | Empty. Stop playback now and clear the queue. |
| `0x92` | SPK_END    | Empty. No more audio for this reply. |
| `0xA0` | PING       | `u32` nonce. |
| `0xA2` | SELFTEST   | Empty. Device plays a 1 kHz tone for 1 s while recording, then sends SELFTEST_RESULT `{"mic_rms_before":…,"mic_rms_during":…,"spk_bytes_played":…}`. |
| `0xA3` | SNAPSHOT   | Empty. Device sends SNAP_BEGIN, SNAP_DATA…, SNAP_END with the current LVGL screen. |
| `0xF0` | DEBUG      | Test tools only; the relay never sends it. JSON `{"angle_override":<degrees>|null}` fixes the UI rotation angle (null returns control to the IMU). Other keys: `imu_dump` seconds (logs the raw accelerometer counts, five samples per line as `imu_dump i=<first index> x,y,z;…`, then `imu_dump done n=<count>`; `devctl imu-dump` rejects a trace whose indices have a gap), `bench` (frame-rate test mode), `disp_scale` N (display power timeline N times faster), `disp_force` 0 active / 1 dim / 2 off / null, `orbit` lattice index / null, `fill` grey 0–255 / null, `arc` 0–100 / null (shows the volume arc at that level without changing the volume). Payload at most 127 bytes (the device keeps 128 bytes for the text and its terminator). See `display-power.md`. |

## Handshake

- The host sends HOST_HELLO only when it opens a link. Opening the USB port resets the board and loses what was sent before the reset, so on USB the host repeats HOST_HELLO every 1000 ms until the first HELLO arrives, and sends it once more after the first HELLO with `reason` `boot` that follows the open.
- The device sends HELLO at boot (`reason` `boot`), when it leaves OFFLINE (`resume`) and in reply to HOST_HELLO (`reply`).
- On any HELLO the host sends SET_STATE with its current state, and SET_VOLUME with mode `2` (seed) only when the HELLO has `vol_set` false. Apart from the USB case above, it never answers HELLO with HOST_HELLO. On `boot` it also resets its speaker byte counter.

## States

| Value | State     | Device behaviour |
|-------|-----------|------------------|
| 0 | IDLE      | Mascot idles. |
| 1 | LISTENING | Mic capture on; MIC_AUDIO streams. Leaving this state stops capture. |
| 2 | THINKING  | Thinking animation. |
| 3 | SPEAKING  | Speaking animation driven by the playback level. |
| 4 | ERROR     | Error animation. |
| 5 | OFFLINE   | Set by the device itself when the host is silent (see liveness); the host may also send it. |

The host owns the state. The device changes state only on SET_STATE, except for entering and leaving OFFLINE. Input events never change the state on the device; the host decides.

## Input

- A touch is classified when it starts: on a dark screen it is dead; starting 150 px or more from the centre it is an edge touch; otherwise a centre touch. A touch that moves more than 13 px is neither a TAP nor a LONG_PRESS.
- TAP: touch released within 600 ms, anywhere on the screen, without moving more than 13 px. A touch that began while the screen was dim or off only wakes it and sends no TAP, LONG_PRESS or volume change.
- LONG_PRESS: a centre touch held for 1500 ms; sent once when the threshold is reached. An edge touch never sends it.
- The volume knob sends no INPUT.
- BOOT_BUTTON: the BOOT key (GPIO0) or the PWR key (IO expander pin 4) pressed; both send the same code.

## Volume

- The device owns the user volume and keeps it in non-volatile storage (namespace `clawd`, key `vol`). Without a stored value it uses 70 and reports `vol_set` false.
- Knob: an edge touch arms after 15° of net travel along the edge; the first 15° never change the volume. Then 270° of travel is 0–100, clockwise raises, in 1 % steps; the ends saturate and do not wrap. A touch that jumps farther than the plausible finger speed cancels the knob and restores the volume it armed with. Before arming, a sample inside radius 150 px means the touch can no longer arm, though it can still be a TAP; after arming, samples inside radius 100 px are skipped.
- Jump limit: between two samples whose point changed, the touch angle may change by at most 10° plus 720°/s times the sample gap (the gap capped at 50 ms); a larger change cancels the knob.
- A transient SET_VOLUME changes the codec only. It ends, restoring the user volume, at the next valid HOST_HELLO, on entering OFFLINE, at reboot, when the knob arms, and at a persist set. A tool that needs it to last keeps its link alive.
- A persist set becomes the user volume, is stored and reported by VOLUME. A seed does the same only when `vol_set` is false and is otherwise ignored, so repeated HELLOs do not repeat it.
- Writes: only a knob release or a persist or applied seed set marks the volume for writing; the write waits until the amplifier is off and the state is not LISTENING, and at least 5 s after the previous write. A reset before the write loses that change.
- Wear: a save is one 32-byte entry, 126 entries fill a 4096-byte page and the six pages rotate, so each sector is erased once per 756 saves. At one save per 5 s without pause that is 17 280 saves a day, about 12 years at 100 000 erase cycles per sector.
- Storage: if the partition is full or from a newer version, it is erased once and reinitialised, which also removes anything else kept there; any other storage error, including a failed read of the stored volume, leaves the volume in RAM for that boot: nothing is written, so a stored value that could not be read is kept, and `vol_set` becomes true after a knob release or a persist or applied seed set.
- The relay keeps the last `vol` or VOLUME value in memory only.

## Playback and flow control

- Playback works in every state. A state change does not flush the queue.
- The device playback queue holds at least 192 000 bytes (6 s).
- Consumed total in SPK_STATUS counts every byte that has left the queue, played or flushed, since boot; it wraps at 2^32. The host counts the bytes it has sent since the device's last `boot` HELLO and keeps `(sent − consumed) mod 2^32` at or below 64 000 bytes (2 s).
- The device sends SPK_STATUS every 100 ms while the queue is non-empty or audio is playing.
- Drained (bit1) is set in the first SPK_STATUS after the queue empties, provided SPK_END arrived after the last SPK_AUDIO. SPK_AUDIO after SPK_END is allowed and starts a new segment that needs its own SPK_END.
- SPK_FLUSH clears the queue and any pending SPK_END; the device then sends SPK_STATUS immediately with 0 queued and flags 0.

## Liveness

- The host sends PING every 1000 ms.
- The device enters OFFLINE when no valid frame has arrived on the active link for 5000 ms.
- In OFFLINE, the first valid host frame other than SNAPSHOT ends OFFLINE (to IDLE) and makes the device send HELLO with `reason` `resume`. SNAPSHOT leaves OFFLINE in place so it can be captured.

All numbers in this file are plain configuration constants of protocol version 3.
