# Updating over the air, on the board — what the hardware said

The spec listed five things that could only be judged on the board
(`2026-09-03-ota-firmware-update-design.md`, "What is tested where"). This is
what they turned out to be. Two of the five contradicted the design, and one
of those had already been named in the spec as an assumption rather than a
conclusion.

**Provenance.** One ESP32-C3 on `/dev/cu.usbmodem101`, serial captured with a
raw pyserial reader (`idf.py monitor` needs a TTY). Server is the real Railway
deployment; the device is on a home 2.4 GHz network, RTT to the gateway
measured at 58–92 ms with `WIFI_PS_MIN_MODEM` in force. Firmware `6b9e9bb`.

---

## 1. The cable flash preserved NVS, as designed

The partition table moved the app from `0x10000` to `0x20000` and inserted
`otadata` at `0xf000`. `nvs` kept its offset and size, and the device read its
own configuration back afterwards without being provisioned again:

```
I (242) config: ssid "Xiaomi_CFB8", uri "wss://…/ws", vol 5, bright 3, eyes 0
```

The bootloader reads the table exactly as written, and `phy_init` moving from
`0xf000` to `0x11000` is harmless because
`CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` is unset — the data is compiled into
the app and that partition is never read. `phy_init: saving new calibration
data because of checksum failure` appears once after the move and is the
calibration blob in NVS being rewritten, not a fault.

## 2. A cable-flashed image is never put on trial

`ota_data_initial.bin` is 8192 bytes of `0xFF`. With no recorded state
`esp_ota_get_state_partition()` reports nothing, `ota_boot_guard()` returns
early, and the `ota` tag prints nothing at all. Silence on that tag after a
cable flash is the correct output, which is worth writing down because it
looks like nothing happening.

## 3. TCP backpressure alone does not survive a megabyte

**This is the one the spec got wrong, and said it might.** The design left
application-level flow control out, with the note that this was "an assumption
to measure, not a settled question" and that the fallback was a window of
frames acknowledged by the device.

Unpaced, the transfer died:

```
W (121093) ota: update started: 1121184 bytes into ota_0 (capacity 2031616)
E (152497) voice: socket error … heap 44236: esp_transport_read() failed with -1 … errno=11
W (152499) voice: socket disconnected: sent 0 B, 0 send failures
```

**31.4 seconds in**, no `ota_end`, no commit. The mechanism is in the client's
own source: `esp_websocket_client.c:1108` dispatches `WEBSOCKET_EVENT_DATA` to
the application, and lines 1118–1143 answer `PING` — from one task. While the
application's handler writes flash, no `PONG` leaves. uvicorn runs with its
default keepalive (ping every 20 s, close 20 s after silence), so a ping
landing anywhere in the first 20 s of the transfer kills the socket between
41 s and 61 s after it starts. Observed 31.4 s after `ota_begin`, which sits
inside that window once the ping's own phase is accounted for.

Not proven: the server-side close line had already rolled out of Railway's
retained logs, so the keepalive is the leading explanation rather than a
confirmed one. The fix does not depend on which it was — anything that keeps
the client's task from answering for tens of seconds is fatal, and the window
addresses all of them.

**Safe failure, and this part the design got right.** The running image was
untouched, `otadata` still selected the good slot, and the device reconnected
on its own. A transfer that dies costs nothing.

## 4. With a 32 KB window it completes — at 7.5 KB/s

```
34 windows logged, first ack 32768 B at 118.3 s, last 1114112 B at 259.6 s
1056 KB in 141.3 s  ->  7.5 KB/s
W (260577) ota: update committed to ota_1
```

So a megabyte takes about **145 seconds**, and the commit adds ~1 s for
`esp_ota_end` to hash the image.

7.5 KB/s is far below what the link can carry, and the reason is the shape of
the window rather than the flash: it is lock-step, so the pipe empties
completely on every round. Each of the 34 rounds pays an RTT, a radio wake-up
out of modem sleep, and TCP restarting its congestion window on an idle
connection — about 4.2 s per 32 KB, against roughly 0.5 s of actual work.

**The obvious improvement is a larger window, not a deeper one.** Doubling
`OL_ACK_EVERY`/`ACK_EVERY` to 64 KB halves the number of round trips while
keeping the same pause pattern that lets the client answer `PING`. Allowing
several windows in flight would instead shrink the idle time the window exists
to create, which is how the original failure happened. Not done: 145 seconds
for something updated rarely is tolerable, and correctness came first.

## 5. Rollback fires, on the deadline, and the previous image comes back

Driven with an image built with a deliberately wrong `DEVICE_TOKEN`, so
everything works except the one thing that counts as proof.

```
W (260577) ota: update committed to ota_1; the next boot runs it unproven
I    (237) boot: Loaded app from partition at offset 0x210000
W    (252) ota: this image is unproven: 10 minutes to reach the server
             … esp_ws_handshake_status_code=403, repeatedly …
E (601670) ota: unproven after 600 s with no frame from the server; rolling back
I    (216) boot: Loaded app from partition at offset 0x20000
I   (5314) voice: socket connected
```

601.7 s against a 600 s deadline. The server logged
`rejected unauthorised connection` / `connection rejected (403 Forbidden)`
throughout, which is what "the socket opened but the token is wrong" looks
like from both ends — and exactly the case the definition of "good" was
written for: the image booted, joined WiFi and reached TLS, and none of that
counted.

After the rollback the device came back on the previous image in 5.3 s with
NVS intact.

## 6. A committed update reported itself as a failure

Found while doing the above, and it is a real defect rather than a
measurement.

```
W (260577) ota: update committed to ota_1
W (260741) voice: restarting into the new image      <- 164 ms later
```

`esp_websocket_client_send_text()` returns when the bytes reach the transport,
not when they reach the wire. With 58–92 ms RTT and modem power save, the
200 ms delay before `esp_restart()` was not enough for a 20-byte frame, so the
server waited its 30 s and reported `timeout` for an update that had
committed. The web panel then said the device refused an update it had in fact
taken.

Fixed by closing the socket before restarting — the closing handshake waits
for what went before it — rather than by lengthening the delay, which would
only move the same race.

## 7. The happy path, end to end, with both fixes in

Pushed from the web app, over the Railway deployment, onto a device
running `438956f`:

```
I    (216) boot: Loaded app from partition at offset 0x20000
W (142279) ota: update started: 1122288 bytes into ota_1 (capacity 2031616)
             … 34 windows, heap 43940-47616 …
W (263709) ota: update committed to ota_1; the next boot runs it unproven
W (264436) voice: restarting into the new image
I    (237) boot: Loaded app from partition at offset 0x210000
W    (252) ota: this image is unproven: 10 minutes to reach the server
I   (5940) voice: socket connected
W  (23187) ota: image accepted (frame from the server)
I  (23252) ota: image proved good; the deadline is off
```

1,122,288 bytes in 121.4 s — **9.0 KB/s**, a little better than the 7.5 KB/s
of the earlier run and the same shape of cost. **Free heap held between 43,940
and 47,616 bytes across all 34 windows**, which is the number the spec asked
for and could not get from a laptop. No downward trend across the transfer, so
nothing is leaking per window.

The panel reported `ota_ready` rather than `timeout`, which is the closing
handshake from item 6 doing its job.

**The frame that proves an image is usually a PONG.** `image accepted` landed
17 seconds after the socket opened, not immediately: nothing
application-level arrives spontaneously, so the first inbound frame is the
answer to the device's own 20-second keepalive ping. That still satisfies the
definition — a WebSocket frame of any kind can only arrive after a handshake
the server accepted, and the handshake is where the token is checked, as the
403s in item 5 show. Worth writing down because "waited 17 s doing nothing
visible" looks like a fault and is not one, and because it is 17 seconds
against a 600-second deadline.

## Two bugs found by using it, not by testing it

Neither showed up in 446 server tests or 12 host runs. Both are recorded in
`438956f`.

**A failed update came out of the speaker.** The binary path was guarded on
`ota_active()` — whether a transfer is *healthy* — rather than on whether the
sender is sending firmware at all. When a write fails, `ota_write()` aborts
the transfer, that guard goes false, and every remaining frame of the image
falls into the branch that treats binary as reply audio. About five seconds of
noise came out of the amplifier, which is exactly the play buffer's depth.

**Two pushes interleaved into one partition.** The server's one-push-at-a-time
slot lives as long as its HTTP request and no longer. A browser that reloads
cancels the request and frees the slot while the device, told nothing, is
still receiving; the next push then writes into the first transfer's byte
stream. Two different images into one partition, an overrun, and the abort
above. Every push now opens with `ota_abort` rather than assuming the device
is idle.

The two together are one lesson: **the device's transfer state and the
sender's idea of it are separate things**, and every place that assumed
otherwise was wrong.

## What this leaves undone

- **Versions are still not distinguishable.** Both the good and the
  deliberately-broken image reported `6b9e9bb`, because there are no git tags
  and `secrets.h` is untracked so `--dirty` does not fire. The panel's second
  phase — "запустилось", the one that proves rollback did not fire — cannot
  work until a release is tagged.
- **The access-point path has no measurements.** It carried the first
  successful update, but joining the device's own access point cuts the
  laptop off the internet, so nothing could be observed while it ran. Its
  timings, and whether its idle-marker refresh behaves under a slow upload,
  are still unmeasured.
