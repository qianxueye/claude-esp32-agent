# Network link on the board

How the firmware implements the Wi-Fi and tunnel links of `protocol.md`. The protocol is the contract; this page explains the board side: tasks, locks, order of attempts, buffers and time bounds. Every constant lives in `firmware/main/app_config.h` and is plain configuration; none is a mathematical model, a run-time judgement or a console setting.

Code: `net.c` (task, Wi-Fi, both transports, inner TLS, PAIR/FORGET/SET_REMOTE), `netstore.c` (NVS), `transport.c` (dispatch and routing). The rules that can be tested without IDF are pure modules with host tests: `netcfg` (configuration text, WIFI_STATUS text), `linkstate` (active link), `netplan` (network pick, try order, backoffs, WIFI_STATUS state), `wsmap` (tunnel BIO results), `netwrite` (TX queue).

## Task model

- One network task (`net`, `STACK_NET` 12288 bytes, priority `PRIO_NET`) owns the Wi-Fi station, the LAN socket, the WebSocket transport and both mbedTLS contexts. No other task touches them.
- The task starts after the boot HELLO, so the first WIFI_STATUS follows it. While unpaired it does not initialise Wi-Fi; after PAIR it initialises the driver and netif once and starts the station without a reboot. FORGET stops the station; the driver stays initialised.
- Wi-Fi and IP events go from the default event loop into the task's queue (`NET_EVENT_QUEUE_DEPTH`). The task waits in slices of at most 100 ms, so it notices a configuration change, a remote URL change and USB loss within one slice outside a blocking call.
- Session loop: read with a `NET_POLL_MS` wait, feed every received byte to a stream-mode frame parser, dispatch each frame, then write queued frames until the queue is empty or the transport would block.
- A failure in Wi-Fi, netif, event loop, TLS or WebSocket set-up is logged and shows as `wifi` `failed`; the board keeps running on USB, FORGET included. The network code has no `ESP_ERROR_CHECK` and no `abort`.

## Dispatch and locks

- USB rx and the network task feed received frames to one dispatch function under one mutex. The active-link rules and the frame handler run under it, so handlers never run concurrently.
- The route for device-to-host frames (USB, Wi-Fi or tunnel) is an atomic, readable without the dispatch mutex, because handlers and the log hook send while it is held.
- No handler waits on the network task. PAIR, FORGET and SET_REMOTE validate, write NVS, update the shared configuration and return; the task applies the change on its own time. Their replies (PAIR_RESULT, FORGET_RESULT) always go out on USB.
- `transport_send_reliable` waits for queue room only when the caller is neither the network task nor the dispatch holder; otherwise a NET frame is queued without waiting. The network task is the only consumer of its queue, so waiting there would only time out.
- The shared configuration has its own short mutex, never held across a blocking call. The task copies it on every generation change and works on the copy.
- Lock order is dispatch, then configuration or TX queue; no path takes the dispatch mutex while holding either.

## Generation counter

- PAIR and FORGET store the new configuration (or erase it), then increment the generation. The task compares its copy's generation between blocking steps; on a change it ends the session or the attempt in progress, stops Wi-Fi, clears the station configuration in the driver, copies the new configuration and starts again.
- SET_REMOTE does not change the generation. It updates the URL and its own counter; the next tunnel attempt uses the new URL, an open session is kept, and the relay backoff resets to 1 s.
- Worst case from FORGET to the radio stopping is the longest blocking step in progress (see Bounds), typically well under a second outside a tunnel connect.

## Link rules

- `linkstate` decides per frame whether it is handled and whether it counts for liveness, as `protocol.md` "Active link" states. A network HOST_HELLO makes its session active only when no valid USB frame has arrived for 5000 ms; a board that has heard nothing on USB since boot counts as silent.
- The board does not close its network session when USB HOST_HELLO arrives; the relay does that.
- A HOST_HELLO with another protocol version is dropped before these rules, so it neither changes the active link nor counts as a USB frame.
- Every end of a network session (relay close, silence close, read or write error, Wi-Fi loss, generation change, FORGET) runs one exit path: close_notify, then the active link drops from the network to none, then the session's queued frames are discarded. Only then can the next session start, and it cannot become active before its own HOST_HELLO.
- The board closes a session on which no valid frame arrived for 5000 ms (`PROTO_HOST_SILENCE_TIMEOUT_MS`), active or not. A session ignored under the 5000 ms USB rule therefore ends within that time, and the board retries under the backoff.
- Only a HOST_HELLO that made the session active resets the relay backoff. A session that ends without one counts as a failed attempt.

## Network pick and try order

- After each scan the board joins the strongest visible network saved with `lan` true, else the strongest visible saved network. None visible, or an association failure, sets `wifi` `failed` and waits for the association backoff (1 s doubling to 30 s). Hidden networks are not found, because the scan does not name an SSID.
- Station settings: authentication threshold OPEN for a network without a password, otherwise WPA2-PSK; PMF capable, not required; Wi-Fi settings stay in RAM (`WIFI_STORAGE_RAM`, `CONFIG_ESP_WIFI_NVS_ENABLED` off). Power saving is `WIFI_PS_NONE` while a session is open and `WIFI_PS_MIN_MODEM` otherwise.
- Attempts on a `lan` network: each relay name in order (mDNS), then each relay address, then the remote URL if one is set. On any other network: the remote URL only. One pass is a round; a failed round waits for the relay backoff (1 s doubling to 30 s).
- The relay backoff resets on an activating HOST_HELLO, on PAIR, FORGET or a changed SET_REMOTE, and when USB stops being connected while USB is the active link. A reset cuts the current wait to at most 1 s.
- A session is kept until it drops; a LAN relay becoming reachable never preempts a tunnel session.
- Failure mapping for `relay`: inner fatal alert or MAC failure `auth_failed`; tunnel name lookup failure or an upgrade answer other than 101 `captive`; outer certificate failure `tls_failed`; anything else `unreachable`. The last failure of a round stays until a session authenticates, the configuration changes or Wi-Fi drops.

### Expected takeover after USB unplug

With the relay reachable and the board paired: the cut backoff (up to 1 s), connect and inner handshake (LAN about 0.3 s, tunnel about 1-3 s), then the relay's HOST_HELLO. If it arrives within 5000 ms of the last USB frame the board ignores it, the session ends by silence, and the board retries after about 1 s. Expected: about 8-9 s on the LAN and about 10-11 s over the tunnel; under 2 s when the board was already more than 5 s without USB frames.

## Configuration text

- PAIR and SET_REMOTE are read by a strict reader for their fixed schemas, not a general JSON parser. It accepts RFC 8259 syntax with whitespace (space, tab, CR, LF) between tokens, UTF-8 strings and escapes (`\uXXXX` with surrogate pairs; an escaped NUL is refused), and skips unknown keys with values nested at most three levels.
- PAIR text is at most 1023 bytes, SET_REMOTE text at most 511; each has its own static buffer, so neither runs on a task stack.
- Checks and their order produce the PAIR_RESULT codes listed in `protocol.md`. The first failing check names the code; a refused PAIR keeps the previous configuration.
- The PSK identity must equal `clawd-` plus the factory MAC (`esp_read_mac(…, ESP_MAC_EFUSE_FACTORY)`) in uppercase hex. The USB-Serial-JTAG peripheral is fixed-function hardware whose serial number is expected to be this MAC; IDF has no software source for the descriptor, so the board test confirms that both match.
- Access credentials (`access_id`, `access_secret`) are skipped by the reader and ignored in SET_REMOTE.

## Storage

- NVS namespace `clawdnet`: key `pair` holds the PAIR text exactly as received, key `remote` the remote URL.
- PAIR writes in this order: validate, erase `remote`, set `pair`, set `remote` if the PAIR has one, commit, reply. A later SET_REMOTE sets `remote` only when the URL changed.
- At boot the stored text goes through the same reader with the identity check. A failure leaves the board unpaired and logs one line with the error code only.
- FORGET erases the namespace and commits. If that fails, the board still drops the configuration from RAM, stops Wi-Fi and replies `storage error`.
- Flash encryption is off, so the stored text can be read with physical access.

## TX

- Frames for a network link go into one ring of `NET_TX_RING_BYTES` (32 768) bytes in PSRAM. Producers take a short mutex (`NET_TX_LOCK_WAIT_MS`); the network task is the only consumer.
- A frame is admitted whole or not at all; a full ring drops it before any byte is encrypted and counts it in `net drop` on the stats line. The ring holds at least `2n − 1` bytes for an `n`-byte frame at any position.
- Once `mbedtls_ssl_write` has taken a frame's first byte, the task finishes that frame: a partial write continues at the next offset, WANT_WRITE retries the same pointer and length. An encrypted record is never dropped.
- When a session ends its queued frames are discarded, so the next session starts at a frame boundary.

## Tunnel BIO

- The outer link is `esp_transport_ssl` with the certificate bundle (SNI and host name checked) under `esp_transport_ws`, path from the remote URL. A redirect is never followed. `TCP_NODELAY` is set on the socket under both layers.
- Send: mbedTLS's buffer is copied into a separate `NET_WS_TX_BYTES` buffer first, because the WebSocket layer masks the payload in place. A write is offered only when the socket is writable; WANT_WRITE is returned only when no byte of the message went out. A timeout or short write after the header went out ends the session.
- Receive: a short poll; once the socket is readable, a read with `NET_WS_READ_TIMEOUT_MS` for the rest of the message. Binary data goes to mbedTLS; PING is answered by the WebSocket layer itself and returns no data; a close frame ends the stream; a text message or lost framing ends the session.
- The WebSocket layer's poll checks only the socket and the outer TLS buffer, not bytes kept from the upgrade response. The relay sends nothing before the board's ClientHello, so that buffer is empty when the session starts.
- The WebSocket layer logs the full upgrade request on a write error, so its log tag `transport_ws` is set to `ESP_LOG_NONE`.

## Inner TLS

TLS 1.2 only, the single suite `TLS-ECDHE-PSK-WITH-CHACHA20-POLY1305-SHA256`, no session tickets, no renegotiation, CTR-DRBG seeded from the hardware RNG. Each session uses fresh contexts, freed at the end, which also wipes the PSK copy in the configuration. The build enables only the ECDHE-PSK key exchange among the PSK modes.

## Bounds

The task checks the generation, Wi-Fi and the deadlines between blocking calls. Each blocking call has its own limit:

| Step | Limit |
|---|---|
| Scan | `NET_SCAN_TIMEOUT_MS` 10 s, event driven |
| Association and DHCP | `NET_ASSOC_TIMEOUT_MS` 20 s, event driven |
| Relay name lookup (`.local`, mDNS) | lwIP retry schedule, about 7 s; not limited by the 1.5 s connect timeout |
| LAN TCP connect | `LAN_CONNECT_TIMEOUT_MS` 1.5 s in 100 ms slices |
| Tunnel host lookup alone | about 7 s per DNS server, at most two from DHCP, so about 14 s |
| Tunnel host lookup, TCP connect and outer handshake | `REMOTE_STEP_TIMEOUT_MS` 5 s in total, checked after each step, so up to about 10 s; a slow lookup ends the attempt at its first check |
| Upgrade request write | 5 s |
| Upgrade response read | 5 s per read; a peer that sends a few bytes per read can stretch it, at most one read per byte of the 2048-byte buffer |
| Inner handshake | `NET_INNER_HANDSHAKE_MS` 10 s, checked between steps; one step on the tunnel can add up to 5 s |
| Session read | `NET_POLL_MS` 20 ms; on the tunnel up to 5 s for the rest of a message |
| Session write | LAN non-blocking; tunnel 20 ms poll, then up to 5 s for the message |

The three tunnel steps add up to `REMOTE_CONNECT_TIMEOUT_MS` 15 s. A slow lookup or a peer that answers just inside each step limit can exceed it, by the overrun shown above.

## Memory

- PSRAM: the TX ring, the tunnel send copy, the stream parser buffer, mbedTLS allocations (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`) and Wi-Fi/lwIP buffers where IDF allows it (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`).
- Internal RAM: the network task stack (it writes NVS for SET_REMOTE, and flash writes disable the cache that PSRAM sits behind), the PAIR and SET_REMOTE buffers (static), and the 1024-byte read chunk.
- `CONFIG_WS_BUFFER_SIZE` 2048 holds the upgrade request and response.

## Stacks

- The rx task (`STACK_RX` 6144) runs PAIR parsing and its NVS write.
- The log hook formats each line into a 200-byte stack buffer in the task that logs. The system event task runs the Wi-Fi and IP handlers that log, so `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` is 4096.
- The stats line reports free stack bytes of `rx`, `net`, `sys_evt`, `wifi` and `tiT` (lwIP), and the network TX drops.

## Logging

PAIR and SET_REMOTE payloads, the PSK and Wi-Fi passwords are never logged. Logs show at most the SSID, the counts of networks, names and addresses, the remote URL and its host, and error codes. Buffers that held PAIR text or the parsed configuration are wiped with `mbedtls_platform_zeroize`.
