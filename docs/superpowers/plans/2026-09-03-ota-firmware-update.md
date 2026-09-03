# Firmware over the air — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace the USB cable before the enclosure is sealed, with two
independent update paths — down the WebSocket the device already holds, and
through a form on the provisioning access point.

**Architecture:** All new decisions live in two new firmware files. `ota_logic.c`
is pure (no ESP-IDF, no allocation, host-testable) and owns the image-header
check, control-frame field extraction and the transfer state machine. `ota.c`
owns `esp_ota_*`, the rollback deadline and the running version. `voice_main.c`
and `provision.c` gain routing only. On the server side `firmware.py` adds a
registry of live device sockets and a relay that stores nothing.

**Tech Stack:** ESP-IDF 5.3.2 / ESP32-C3, FreeRTOS, `esp_websocket_client`,
`esp_http_server`, FastAPI, React + Vite, pytest, vitest, plain-C host tests
under `firmware/host/`.

**Spec:** `docs/superpowers/specs/2026-09-03-ota-firmware-update-design.md`.
Read it first. Every "why" is there; this document is the "how".

## Global Constraints

- **Target:** ESP32-C3, 4 MB flash, ESP-IDF 5.3.2. `chip_id` of a valid image
  is `5`.
- **Project name in every image:** `voice_capture` (from `project()` in
  `firmware/CMakeLists.txt`).
- **Header bytes that decide validity:** `0x00` = `0xE9`, `0x0C` = `5`,
  `0x20` = `0xABCD5432`, `0x50` = `"voice_capture"`. 144 bytes are enough.
- **`ota_logic.c` follows `face.c`'s rules:** no ESP-IDF header, no float, no
  allocation. It must compile with
  `cc -std=c11 -O2 -Wall -Wextra -Werror` and under
  `-fsanitize=address,undefined`.
- **WebSocket frame size:** 4096 bytes. `ws_start()` sets
  `.buffer_size = 4096` (`voice_main.c:819`); the server must not exceed it.
- **`nvs` never moves:** offset `0x9000`, size `0x6000`.
- **Footprint rule:** no new logic in `voice_main.c` or `provision.c`. They get
  routing and nothing else, because
  `docs/superpowers/specs/2026-09-03-two-button-settings-design.md` is rewriting
  the button apparatus in the same two files.
- **Never run `idf.py flash`.** Building is verification; flashing is a
  decision for the person holding the board.

---

## File structure

| File | Responsibility | New? |
|---|---|---|
| `firmware/partitions.csv` | two OTA slots, `nvs` unmoved | modify |
| `firmware/sdkconfig.defaults` | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` | modify |
| `firmware/main/ota_logic.h` / `.c` | pure: header check, frame fields, transfer state machine | create |
| `firmware/main/ota.h` / `.c` | `esp_ota_*`, rollback deadline, running version | create |
| `firmware/host/ota_logic_test.c` | host tests for the above | create |
| `firmware/host/Makefile` | wire the new test in | modify |
| `firmware/main/CMakeLists.txt` | add the two sources to `voice` | modify |
| `firmware/main/setup_screen.h` / `.c` | one more status for the panel | modify |
| `firmware/host/setup_screen_test.c` | cover it | modify |
| `firmware/main/voice_main.c` | route `ota_*` and binary-during-transfer; boot guard; hello | modify |
| `firmware/main/provision.c` | `POST /firmware`, one form section | modify |
| `server/firmware.py` | live-socket registry, relay, header check | create |
| `server/main.py` | wire routes, intercept `hello` / `ota_*` | modify |
| `tests/test_firmware.py` | server tests | create |
| `scripts/fake_device.py` | accept an update, so the path is testable with no board | modify |
| `web/src/firmwareApi.ts` | GitHub release query, upload with progress, device info | create |
| `web/src/firmwareApi.test.ts` | cover it | create |
| `web/src/Devices.tsx` | the panel, replacing the empty health block | modify |

---

## Task 1: The partition table and the rollback switch

**Files:**
- Modify: `firmware/partitions.csv`
- Modify: `firmware/sdkconfig.defaults`

**Interfaces:**
- Consumes: nothing.
- Produces: partitions named `ota_0` / `ota_1`, each `0x1F0000` bytes, and a
  bootloader built with rollback enabled. Task 3 reads the partition size at
  runtime via `esp_ota_get_next_update_partition()`.

- [ ] **Step 1: Rewrite the table**

`firmware/partitions.csv` — keep the explanatory header, replace the rows:

```
nvs,        data, nvs,      0x9000,   0x6000
otadata,    data, ota,      0xf000,   0x2000
phy_init,   data, phy,      0x11000,  0x1000
ota_0,      app,  ota_0,    0x20000,  0x1F0000
ota_1,      app,  ota_1,    0x210000, 0x1F0000
```

- [ ] **Step 2: Add the rollback switch**

Append to `firmware/sdkconfig.defaults`:

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

- [ ] **Step 3: Verify the arithmetic before trusting the build**

Run:

```bash
python3 - <<'PY'
rows = [("nvs",0x9000,0x6000),("otadata",0xf000,0x2000),
        ("phy_init",0x11000,0x1000),("ota_0",0x20000,0x1F0000),
        ("ota_1",0x210000,0x1F0000)]
end = 0
for name, off, size in rows:
    assert off >= end, f"{name} overlaps"
    if name.startswith("ota_") and name != "otadata":
        assert off % 0x10000 == 0, f"{name} not 64K aligned"
    end = off + size
assert end == 0x400000, f"ends at {end:#x}, not 4MB"
print("table ok, app slot", 0x1F0000, "bytes")
PY
```

Expected: `table ok, app slot 2031616 bytes`

- [ ] **Step 4: Commit**

```bash
git add firmware/partitions.csv firmware/sdkconfig.defaults
git commit -m "Give the app two slots, and turn rollback on"
```

---

## Task 2: `ota_logic` — everything decidable without a board

**Files:**
- Create: `firmware/main/ota_logic.h`, `firmware/main/ota_logic.c`
- Create: `firmware/host/ota_logic_test.c`
- Modify: `firmware/host/Makefile`

**Interfaces:**
- Consumes: nothing. C stdlib only.
- Produces, and every later task uses these exact names:

```c
#define OL_HEADER_MIN 144
#define OL_PROJECT_NAME "voice_capture"
#define OL_CHIP_ID_ESP32C3 5

typedef enum {
    OL_IMAGE_OK = 0,
    OL_IMAGE_TOO_SHORT,
    OL_IMAGE_NOT_ESP,
    OL_IMAGE_WRONG_CHIP,
    OL_IMAGE_NO_APP_DESC,
    OL_IMAGE_WRONG_PROJECT,
} ol_image_verdict_t;

ol_image_verdict_t ol_check_image(const unsigned char *head, size_t len);
const char *ol_verdict_text(ol_image_verdict_t v);
bool ol_image_version(const unsigned char *head, size_t len,
                      char *out, size_t out_size);

typedef enum {
    OL_FRAME_OTHER = 0,
    OL_FRAME_BEGIN,
    OL_FRAME_END,
    OL_FRAME_ABORT,
} ol_frame_t;

ol_frame_t ol_frame_type(const char *body, size_t len);
bool ol_field_u32(const char *body, size_t len, const char *name, uint32_t *out);

typedef enum {
    OL_XFER_IDLE = 0,
    OL_XFER_ACTIVE,
    OL_XFER_DONE,
} ol_xfer_state_t;

typedef struct {
    ol_xfer_state_t state;
    uint32_t expected;
    uint32_t received;
} ol_xfer_t;

typedef enum {
    OL_STEP_OK = 0,
    OL_STEP_BUSY,
    OL_STEP_NOT_ACTIVE,
    OL_STEP_TOO_BIG,
    OL_STEP_EMPTY,
    OL_STEP_OVERRUN,
    OL_STEP_SHORT,
} ol_step_t;

void ol_xfer_reset(ol_xfer_t *x);
ol_step_t ol_xfer_begin(ol_xfer_t *x, uint32_t size, uint32_t capacity);
ol_step_t ol_xfer_write(ol_xfer_t *x, uint32_t n);
ol_step_t ol_xfer_end(ol_xfer_t *x);
const char *ol_step_text(ol_step_t s);
```

- [ ] **Step 1: Write the failing test**

`firmware/host/ota_logic_test.c`, in the shape of
`firmware/host/provision_logic_test.c` — same `CHECK` macro, same `main()`
that calls every test and prints a count. Build a valid header in a helper so
each test mutates one field:

```c
static void good_header(unsigned char *h) {
    memset(h, 0, OL_HEADER_MIN);
    h[0] = 0xE9;                 // esp_image_header_t.magic
    h[12] = OL_CHIP_ID_ESP32C3;  // .chip_id, little-endian u16
    h[13] = 0;
    h[0x20] = 0x32; h[0x21] = 0x54; h[0x22] = 0xCD; h[0x23] = 0xAB;
    memcpy(h + 0x30, "v0.2.1", 7);          // esp_app_desc_t.version
    memcpy(h + 0x50, OL_PROJECT_NAME, sizeof(OL_PROJECT_NAME));
}
```

Tests to write, all of them:

| Test | Asserts |
|---|---|
| `test_image_accepts_a_real_header` | `OL_IMAGE_OK` |
| `test_image_rejects_a_short_buffer` | 143 bytes → `OL_IMAGE_TOO_SHORT` |
| `test_image_rejects_a_zip` | `h[0]='P'` → `OL_IMAGE_NOT_ESP` |
| `test_image_rejects_another_chip` | `h[12]=9` → `OL_IMAGE_WRONG_CHIP` |
| `test_image_rejects_a_missing_app_desc` | `h[0x20]=0` → `OL_IMAGE_NO_APP_DESC` |
| `test_image_rejects_another_project` | `"other_app"` at `0x50` → `OL_IMAGE_WRONG_PROJECT` |
| `test_image_version_is_read_out` | `"v0.2.1"` |
| `test_image_version_truncates_rather_than_overflows` | `out[4]`, no overflow, terminated |
| `test_image_version_rejects_an_unterminated_field` | 32 non-zero bytes at `0x30` → returns false |
| `test_frame_type_recognises_all_three` | begin / end / abort |
| `test_frame_type_ignores_a_state_frame` | `{"type":"state",...}` → `OL_FRAME_OTHER` |
| `test_frame_type_does_not_match_a_substring_elsewhere` | `{"type":"text","value":"ota_begin"}` → `OL_FRAME_OTHER` |
| `test_field_u32_reads_a_size` | `{"type":"ota_begin","size":1099920}` → `1099920` |
| `test_field_u32_absent_returns_false` | no `size` → false |
| `test_field_u32_rejects_a_non_number` | `"size":"big"` → false |
| `test_field_u32_rejects_an_overflowing_number` | 20 digits → false |
| `test_field_u32_does_not_match_a_suffix` | `"mysize":7` alone → false |
| `test_xfer_accepts_a_clean_run` | begin, three writes, end → `OL_STEP_OK`, `state == OL_XFER_DONE` |
| `test_xfer_rejects_a_second_begin` | `OL_STEP_BUSY` |
| `test_xfer_rejects_a_write_with_no_begin` | `OL_STEP_NOT_ACTIVE` |
| `test_xfer_rejects_an_end_with_no_begin` | `OL_STEP_NOT_ACTIVE` |
| `test_xfer_rejects_a_size_past_the_partition` | size > capacity → `OL_STEP_TOO_BIG` |
| `test_xfer_rejects_a_zero_size` | `OL_STEP_EMPTY` |
| `test_xfer_rejects_more_bytes_than_promised` | `OL_STEP_OVERRUN`, and state leaves `ACTIVE` |
| `test_xfer_rejects_an_early_end` | `OL_STEP_SHORT` |
| `test_xfer_reset_allows_a_retry` | after a failure, begin works again |
| `test_step_and_verdict_text_cover_every_value` | no enum value returns NULL or `"?"` |

- [ ] **Step 2: Wire it into the Makefile**

Four additions to `firmware/host/Makefile`, mirroring `provision_logic_test`
exactly: a `$(BUILD)/ota_logic_test` rule, a `$(ASAN_BUILD)/ota_logic_test`
rule, and the binary appended to both the `test:` and `asan:` prerequisite
lists and their run lines.

- [ ] **Step 3: Run it and watch it fail**

Run: `cd firmware/host && make test ASAN=0`
Expected: compile error — `ota_logic.h: No such file or directory`

- [ ] **Step 4: Implement `ota_logic.h` and `ota_logic.c`**

Header exactly as the Interfaces block above, with the comment header
explaining the split (follow `provision_logic.h`'s voice).

`ol_check_image` reads the four fixed offsets in order and returns the first
failure. `ol_image_version` requires a terminator inside the 32-byte field —
an unterminated field is a malformed image, not a long version. `ol_frame_type`
matches `"ota_begin"` / `"ota_end"` / `"ota_abort"` **only as the value of
`"type"`**, which is what stops `{"type":"text","value":"ota_begin"}` from
triggering an update. `ol_field_u32` finds `"<name>"` preceded by `"` and
followed by `"` then `:`, then parses digits with an overflow guard.

The state machine's one non-obvious rule: any rejection except `BUSY` leaves
the transfer unusable, so `ol_xfer_write`/`ol_xfer_end` set `state` back to
`OL_XFER_IDLE` on failure. A caller that ignores a return value must not be
able to continue a broken transfer.

- [ ] **Step 5: Run the tests, both builds**

Run: `cd firmware/host && make test`
Expected: `N checks, 0 failures` twice — once plain, once sanitized.

- [ ] **Step 6: Prove the header check against the real binary**

Run:

```bash
cd firmware/host && cat > /tmp/ol_real.c <<'EOF'
#include <stdio.h>
#include "../main/ota_logic.h"
int main(int argc, char **argv) {
    unsigned char h[OL_HEADER_MIN];
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(h, 1, sizeof(h), f) != sizeof(h)) return 2;
    char v[33] = {0};
    ol_image_version(h, sizeof(h), v, sizeof(v));
    printf("%s version=%s\n", ol_verdict_text(ol_check_image(h, sizeof(h))), v);
    return 0;
}
EOF
cc -std=c11 -Wall -Wextra -Werror -I../main -o /tmp/ol_real /tmp/ol_real.c ../main/ota_logic.c
/tmp/ol_real ../build/voice_capture.bin
```

Expected: `ok version=372f5d5-dirty` (or whatever `git describe` says now).
A verdict other than `ok` means the offsets are wrong and nothing later can be
trusted.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/ota_logic.h firmware/main/ota_logic.c \
        firmware/host/ota_logic_test.c firmware/host/Makefile
git commit -m "Decide what a firmware image is, on the laptop"
```

---

## Task 3: `ota` — the flash side and the rollback deadline

**Files:**
- Create: `firmware/main/ota.h`, `firmware/main/ota.c`
- Modify: `firmware/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `ota_logic.h` from Task 2.
- Produces:

```c
#define OTA_DEADLINE_MS (10 * 60 * 1000)

void ota_boot_guard(void);            // FIRST statement of app_main
bool ota_pending_verify(void);
void ota_mark_valid(const char *why);
void ota_deadline_suspend(bool on);

const char *ota_running_version(void);
uint32_t ota_capacity(void);

esp_err_t ota_begin(uint32_t size);
esp_err_t ota_write(const void *data, size_t len);
esp_err_t ota_end(void);
void ota_abort(void);
bool ota_active(void);
uint32_t ota_received(void);
uint32_t ota_expected(void);
const char *ota_last_error(void);
```

- [ ] **Step 1: Write `ota.h`**

With the comment header carrying the two constraints that are easy to lose:
`ota_boot_guard()` must be the first statement in `app_main`, and
`ota_deadline_suspend(true)` is what provisioning calls — provisioning must
never call `ota_mark_valid`.

- [ ] **Step 2: Implement `ota.c`**

`ota_boot_guard()`: read `esp_ota_get_state_partition()` on the running
partition. If it is `ESP_OTA_IMG_PENDING_VERIFY`, set `s_pending = true` and
create a one-shot `esp_timer` for `OTA_DEADLINE_MS` whose callback logs and
calls `esp_ota_mark_app_invalid_rollback_and_reboot()`. If it is anything else,
do nothing at all — a cable-flashed image has no otadata entry and must never
be rolled back.

`ota_mark_valid(why)`: only when `s_pending`. Stop the timer,
`esp_ota_mark_app_valid_cancel_rollback()`, log `why`, clear `s_pending`.
Idempotent — it will be called on every frame from the server.

`ota_deadline_suspend(on)`: stops or restarts the timer, leaving `s_pending`
alone. Restarting gives a full fresh `OTA_DEADLINE_MS`, because someone who
just finished provisioning deserves the whole window to get online.

`ota_begin(size)`: `esp_ota_get_next_update_partition(NULL)`,
`ol_xfer_begin(&s_xfer, size, part->size)`, then `esp_ota_begin(part, size,
&s_handle)`. Reset the 144-byte header staging buffer.

`ota_write(data, len)`: `ol_xfer_write` first. While fewer than
`OL_HEADER_MIN` bytes have been staged, copy into the staging buffer; the
moment it is full, run `ol_check_image` and fail the transfer on anything but
`OL_IMAGE_OK`, before the first `esp_ota_write`. Then write everything staged,
and stream the remainder straight through.

`ota_end()`: `ol_xfer_end`, `esp_ota_end`, `esp_ota_set_boot_partition`. Both
of those verify the image's appended SHA-256, which is why there is no hash in
the protocol.

`ota_abort()`: `esp_ota_abort` if a handle is open, `ol_xfer_reset`.

Every failure path records a short reason in a static buffer for
`ota_last_error()`, and calls `ota_abort()` so a failed transfer cannot be
continued.

- [ ] **Step 3: Add the sources to the `voice` sketch**

`firmware/main/CMakeLists.txt`, in the `if(SKETCH STREQUAL "voice")` block,
append `"ota_logic.c" "ota.c"` to `SKETCH_SRCS`.

- [ ] **Step 4: Build the firmware**

Run:

```bash
cd firmware && unset VIRTUAL_ENV && . ~/esp/esp-idf/export.sh >/dev/null \
  && idf.py -DSKETCH=voice build 2>&1 | tail -20
```

Expected: `Project build complete.`, and a partition-table line showing
`ota_0` at `0x20000`. **Do not flash.**

- [ ] **Step 5: Commit**

```bash
git add firmware/main/ota.h firmware/main/ota.c firmware/main/CMakeLists.txt
git commit -m "Write the flash side, and give a new image ten minutes to prove itself"
```

---

## Task 4: Route it in `voice_main.c`

**Files:**
- Modify: `firmware/main/voice_main.c`

**Interfaces:**
- Consumes: everything from Tasks 2 and 3.
- Produces: a device that accepts `ota_begin` / binary / `ota_end` over the
  socket, announces itself with `hello`, and cannot be left in
  `PENDING_VERIFY` by `link_task`'s reboot.

Five edits, and no more than five — see the footprint rule.

- [ ] **Step 1: Include the headers**

Add `#include "ota.h"` and `#include "ota_logic.h"` to the include block.

- [ ] **Step 2: The boot guard goes first**

The very first statement of `app_main()`:

```c
    // Before anything that can hang: see ota.h. A new image that never
    // reaches the server has to be rolled back by something, and this
    // project's task watchdog does not panic.
    ota_boot_guard();
```

- [ ] **Step 3: Route the frames**

In `ws_event`, `case WEBSOCKET_EVENT_DATA`, immediately after
`s_last_activity = xTaskGetTickCount();`:

```c
            // Any frame from the server proves radio, TLS, token and
            // protocol at once, which is the whole definition of a good
            // image. Idempotent, so calling it on every frame is free.
            ota_mark_valid("frame from the server");

            if (e->op_code == 0x02 && ota_active()) {
                // Firmware, not speech. Blocking here is the backpressure:
                // this task stops draining the socket while flash is busy.
                if (ota_write(e->data_ptr, (size_t)e->data_len) != ESP_OK) {
                    s_ota_outcome = OTA_OUTCOME_FAILED;
                }
                break;
            }
```

And at the top of the `op_code == 0x01` text branch, before the `emotion`
check:

```c
                const ol_frame_t of = ol_frame_type(e->data_ptr, e->data_len);
                if (of != OL_FRAME_OTHER) {
                    handle_ota_frame(of, e->data_ptr, e->data_len);
                    break;
                }
```

`handle_ota_frame` is a small static function placed just above `ws_event`. It
calls `ota_begin` / `ota_end` / `ota_abort` and sets `s_ota_outcome`. It sends
nothing — see the next step for why.

- [ ] **Step 4: All sends stay in `link_task`**

Add near the other statics:

```c
// Sends never happen from ws_event. The websocket client dispatches events
// from its own task, and calling esp_websocket_client_send_text() from
// inside that callback would have that task take a lock it may already
// hold. link_task runs at 1 Hz and already owns the socket's health, so it
// carries the two frames this feature has to send.
typedef enum { OTA_OUTCOME_NONE = 0, OTA_OUTCOME_READY, OTA_OUTCOME_FAILED } ota_outcome_t;
static volatile ota_outcome_t s_ota_outcome = OTA_OUTCOME_NONE;
static volatile bool s_hello_sent = false;
```

In `link_task`, inside the `esp_websocket_client_is_connected()` branch,
before `continue`: send `hello` once with `ota_running_version()`; then act on
`s_ota_outcome` — `READY` sends `{"type":"ota_ready"}`, waits 200 ms for it to
leave, and calls `esp_restart()`; `FAILED` sends
`{"type":"ota_failed","reason":"..."}` from `ota_last_error()`. Clear
`s_hello_sent` in the disconnected path so a reconnect re-announces.

- [ ] **Step 5: Suppress the 90-second reboot while unproven**

Change the reboot condition in `link_task`:

```c
        if (down_ms > REBOOT_AFTER_MS && !ota_pending_verify()) {
```

with the comment explaining that in `PENDING_VERIFY` this reboot *is* a
rollback, and 90 seconds is too short a leash for a router that is merely
slow — `OTA_DEADLINE_MS` governs instead.

- [ ] **Step 6: Suspend the deadline while provisioning is up**

Where `app_main` enters provisioning, add `ota_deadline_suspend(true)` before
the provisioning loop and `ota_deadline_suspend(false)` after it. Provisioning
must not mark the image valid — only suspend the clock.

- [ ] **Step 7: Build**

Run:

```bash
cd firmware && unset VIRTUAL_ENV && . ~/esp/esp-idf/export.sh >/dev/null \
  && idf.py -DSKETCH=voice build 2>&1 | tail -8
```

Expected: `Project build complete.` and a binary under 1.94 MB.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/voice_main.c
git commit -m "Take firmware down the socket that is already open"
```

---

## Task 5: The access-point path

**Files:**
- Modify: `firmware/main/setup_screen.h`, `firmware/main/setup_screen.c`
- Modify: `firmware/host/setup_screen_test.c`
- Modify: `firmware/main/provision.c`

**Interfaces:**
- Consumes: `ota.h`, `ota_logic.h`.
- Produces: `POST /firmware?code=NNNN` on the provisioning access point,
  accepting `application/octet-stream`. Adds `SS_STATUS_FLASHING` to
  `ss_status_t` (appended before `SS_STATUS_COUNT`, so no existing value
  changes).

- [ ] **Step 1: Add the status, test first**

In `firmware/host/setup_screen_test.c`, extend the existing status-text test
so every value including `SS_STATUS_FLASHING` returns a non-empty string
shorter than `SS_COLS`. Run `cd firmware/host && make test` and watch it fail
to compile.

- [ ] **Step 2: Add it**

`SS_STATUS_FLASHING` before `SS_STATUS_COUNT` in the enum, and `"updating..."`
in `ss_status_text()`. Run `make test` — passes.

- [ ] **Step 3: The upload handler**

In `provision.c`, `firmware_post_handler`:

- `pl_unlock_check(&s_unlock, code)` on the `code` query parameter
  (`httpd_req_get_url_query_str` then `httpd_query_key_value`). Wrong code →
  403, and no partition touched. This is the same gate as the server URI,
  because uploading firmware is more dangerous, not less.
- `ota_begin(req->content_len)`, then a loop of `httpd_req_recv` into a
  4096-byte static buffer, each chunk through `ota_write`.
- **`s_last_request = xTaskGetTickCount();` inside the loop, on every chunk.**
  Without it `provision_idle_expired()` tears the access point down in the
  middle of a slow upload — a single long POST produces no requests. This is
  the bug this step exists to avoid.
- Set `s_screen.status = SS_STATUS_FLASHING` under `s_screen_lock` at the
  start, so the panel says what is happening.
- On success: `ota_end()`, reply `200 rebooting`, `vTaskDelay(500 ms)`,
  `esp_restart()`.
- On any failure: `ota_abort()`, an error response naming
  `ota_last_error()`, and the screen back to `SS_STATUS_WAITING`.

- [ ] **Step 4: The form and its uploader**

A new `<fieldset><legend>Firmware</legend>` section in `PAGE_TAIL`, with a
file input, a four-digit code input, a progress element, and an
`XMLHttpRequest` that POSTs the raw file as the body and shows
`upload.onprogress`. Raw body, not multipart — parsing multipart in C for one
field would be more code than the whole handler.

- [ ] **Step 5: Register it**

Alongside the other handlers, `register_or_warn` (not `_fail`): a device whose
firmware form failed to register is still provisionable, and the AP path is a
fallback, not the reason provisioning exists.

- [ ] **Step 6: Build and commit**

```bash
cd firmware && unset VIRTUAL_ENV && . ~/esp/esp-idf/export.sh >/dev/null \
  && idf.py -DSKETCH=voice build 2>&1 | tail -8
cd .. && git add firmware/main/provision.c firmware/main/setup_screen.h \
        firmware/main/setup_screen.c firmware/host/setup_screen_test.c
git commit -m "Let the setup page take a firmware file, when no server can"
```

---

## Task 6: The server relay

**Files:**
- Create: `server/firmware.py`
- Modify: `server/main.py`
- Create: `tests/test_firmware.py`

**Interfaces:**
- Consumes: nothing from the firmware tasks — the protocol is the contract.
- Produces:

```python
OTA_CHUNK = 4096          # must not exceed the device's .buffer_size
HEADER_MIN = 144
PROJECT_NAME = "voice_capture"
CHIP_ID_ESP32C3 = 5

def check_image(head: bytes) -> str | None    # None means valid
class DeviceLink                              # ws + a queue of device frames
class DeviceRegistry                          # register / get / drop by surface
def register_firmware_routes(app, settings) -> None
```

Routes: `POST /firmware/push` (login), `GET /firmware/device` (login).

- [ ] **Step 1: Write the failing tests**

`tests/test_firmware.py`, using `client()` copied from `tests/test_main.py`:

| Test | Asserts |
|---|---|
| `test_check_image_accepts_a_real_header` | built from the same offsets as the C test → `None` |
| `test_check_image_rejects_a_short_buffer` | names the reason |
| `test_check_image_rejects_another_chip` | ditto |
| `test_check_image_rejects_another_project` | ditto |
| `test_push_requires_a_login` | 401 |
| `test_push_with_no_device_connected_is_a_409` | and says so in the body |
| `test_push_sends_begin_bytes_and_end` | device is a `websocket_connect`; asserts `ota_begin` with the right `size`, the bytes reassemble to the file, then `ota_end` |
| `test_push_refuses_a_file_that_is_not_firmware` | 400, and the device receives nothing at all |
| `test_push_streams_progress` | response body carries increasing `sent` counts and a final `done` |
| `test_device_endpoint_reports_what_hello_said` | after a `hello`, `GET /firmware/device` names that version |
| `test_a_second_push_while_one_is_running_is_a_409` | one at a time |

The push tests run `c.post(...)` on a `threading.Thread` while the test thread
services the device websocket — the server sends into the socket and the test
must be reading it, and both are synchronous.

- [ ] **Step 2: Run them and watch them fail**

Run: `.venv/bin/python -m pytest tests/test_firmware.py -q`
Expected: `ModuleNotFoundError: No module named 'server.firmware'`

- [ ] **Step 3: Implement `server/firmware.py`**

`check_image` is the same four offsets as `ol_check_image`, returning a human
reason or `None`.

`DeviceRegistry` holds one `DeviceLink` per surface. `DeviceLink` carries the
`WebSocket`, the last `hello` version, and an `asyncio.Queue` the ws loop puts
`ota_*` frames on. A single `asyncio.Lock` makes one push at a time.

`POST /firmware/push` reads the body in chunks, holds back the first
`HEADER_MIN` bytes to validate before sending anything, then
`send_text(ota_begin)`, `send_bytes` in `OTA_CHUNK` slices, `send_text(ota_end)`,
and finally waits up to 30 s on the queue for `ota_ready` / `ota_failed`. The
response is a `StreamingResponse` of JSON lines: `{"sent": N, "total": M}` as
it goes, then `{"done": true, "outcome": "..."}`. Nothing is ever written to
disk.

- [ ] **Step 4: Wire `main.py`**

`create_app` builds a `DeviceRegistry` onto `app.state` and calls
`register_firmware_routes`. In `ws_endpoint`: register the link after
`accept()`, drop it in the `finally`, and in the control-frame loop intercept
`hello` and any `type` starting with `ota_` **before** the `session.on_*`
dispatch — today they would fall into `log.debug("ignoring control message")`.

- [ ] **Step 5: Run the tests**

Run: `.venv/bin/python -m pytest tests/test_firmware.py -q`
Expected: all pass. Then the whole suite:
`.venv/bin/python -m pytest -q` — no regressions.

- [ ] **Step 6: Commit**

```bash
git add server/firmware.py server/main.py tests/test_firmware.py
git commit -m "Relay firmware to the device, and keep not a byte of it"
```

---

## Task 7: Teach the fake device to be updated

**Files:**
- Modify: `scripts/fake_device.py`

**Interfaces:**
- Consumes: the protocol.
- Produces: `uv run python scripts/fake_device.py --await-ota` — connects,
  sends `hello`, accepts an update, verifies the bytes against the real
  `.bin`, and reports. This is how the whole path gets exercised against a
  running server with no board in the room.

- [ ] **Step 1: Send `hello` on connect**

Right after the socket opens, in every mode:
`{"type":"hello","version":"fake-0.0.0"}`.

- [ ] **Step 2: Add the `--await-ota` mode**

Waits for `ota_begin`, collects binary frames, checks the count against
`size`, runs the same header check, replies `ota_ready`, and prints the
received size, the elapsed time and the throughput. `--expect FILE` compares
the bytes to a file and prints whether they match.

- [ ] **Step 3: Verify against a real server, end to end**

Run, in two terminals:

```bash
.venv/bin/python -m uvicorn server.main:app --port 8000
uv run python scripts/fake_device.py --await-ota --expect firmware/build/voice_capture.bin
```

then push the file through the API with a logged-in cookie. Expected: the fake
device prints a byte count equal to the file's size and `bytes match`.

- [ ] **Step 4: Commit**

```bash
git add scripts/fake_device.py
git commit -m "Let the stand-in device be updated, so the path is testable indoors"
```

---

## Task 8: The panel in the web app

**Files:**
- Create: `web/src/firmwareApi.ts`, `web/src/firmwareApi.test.ts`
- Modify: `web/src/Devices.tsx`

**Interfaces:**
- Consumes: `POST /firmware/push`, `GET /firmware/device`.
- Produces:

```ts
export interface DeviceFirmware { version: string | null; online: boolean }
export interface LatestRelease { tag: string; url: string; published: string }

export const firmwareApi = {
  device(): Promise<DeviceFirmware>,
  latest(repo: string): Promise<LatestRelease | null>,
  push(file: File, onProgress: (sent: number, total: number) => void): Promise<string>,
}

// The same four offsets as ol_check_image() and check_image(), so a wrong
// file is refused before a megabyte goes up the wire. Returns null when the
// file is a valid image for this project on this chip.
export function checkImage(head: Uint8Array): string | null
```

- [ ] **Step 1: Write the failing tests**

`web/src/firmwareApi.test.ts`, in the shape of `settingsApi.test.tsx`:

| Test | Asserts |
|---|---|
| `device() reports the version the server has` | parses the payload |
| `device() surfaces an offline device` | `online: false`, `version: null` |
| `latest() returns the newest tag and its asset url` | picks the `.bin` asset |
| `latest() returns null when the repo has no releases` | 404 → `null`, not a throw |
| `latest() returns null when the release carries no .bin` | no asset → `null` |
| `push() reports progress and resolves with the outcome` | `onProgress` called with increasing values, resolves `"ota_ready"` |
| `push() rejects with the server's message on a 400` | the reason reaches the caller |
| `checkImage() accepts a real header` | built from the same offsets as the C and Python tests → `null` |
| `checkImage() rejects a zip, another chip, another project` | each names its reason |

- [ ] **Step 2: Run them and watch them fail**

Run: `cd web && npx vitest run src/firmwareApi.test.ts`
Expected: `Failed to resolve import "./firmwareApi"`

- [ ] **Step 3: Implement `firmwareApi.ts`**

`push()` uses `XMLHttpRequest`, not `fetch` — `upload.onprogress` is the only
way to get real upload progress in a browser, and this is a megabyte over a
home connection. `latest()` calls
`https://api.github.com/repos/<repo>/releases/latest` and never throws on a
missing release: a fresh fork has no releases, and that is not an error.

- [ ] **Step 4: Replace the empty health block**

In `Devices.tsx`, the `Здоровʼя пристрою` group (around line 196) currently
says nothing is reported. Replace it with a panel that shows:

- the running version, from `device()`, as a string;
- the latest release tag beside it, as a string, **with no comparison between
  them** — `git describe` output has no total order and a comparison would lie
  about dirty builds;
- a file input and an `Оновити` button, always enabled;
- the two phases, separately: `Передано` from `onProgress`, and `Запустилось`
  only once `device()` reports a *different* version than before the push.

Keep the copy Ukrainian, like the rest of the page.

- [ ] **Step 5: Verify**

Run:

```bash
cd web && npx vitest run && npx tsc -b && npm run build
```

Expected: tests pass, no type errors, build succeeds.

- [ ] **Step 6: Commit**

```bash
git add web/src/firmwareApi.ts web/src/firmwareApi.test.ts web/src/Devices.tsx
git commit -m "Make the device health block real, and put the update behind it"
```

---

## Task 9: Say so in the documentation

**Files:**
- Modify: `firmware/README.md`
- Modify: `README.md`

- [ ] **Step 1: Replace `firmware/README.md`'s "Not yet implemented"**

It currently ends with step 4 as unimplemented. Add an over-the-air section:
the one-time cable flash and why it cannot be skipped, the two paths, what
rollback does, and the failure signatures table this file uses everywhere else
— at minimum: an image rejected as not-firmware, a rollback that fired, a
transfer that stalled, and an AP upload that died at five minutes.

- [ ] **Step 2: Add the protocol frames to the root `README.md`**

The protocol section documents the control frames. Add `hello`, `ota_begin`,
`ota_end`, `ota_abort`, `ota_ready`, `ota_failed`, and note that binary frames
mean firmware while a transfer is active and reply audio otherwise.

- [ ] **Step 3: Commit**

```bash
git add README.md firmware/README.md
git commit -m "Write down the update path, and how it fails"
```

- [ ] **Step 4: Tag a first release — not an implementer's decision**

The repository has no tags, so `git describe` falls back to a bare commit
hash and the version on the panel is `372f5d5-dirty` rather than a version.
One tag fixes it for good:

```bash
git tag v0.1.0
```

**Left undone deliberately.** Which number this is, and whether the first tag
lands before or after the enclosure closes, is a decision for whoever owns the
release — not something to infer from a plan. Everything else here works
without it; only the string shown on the panel and in the web app is worse.

---

## What no amount of this plan can verify

Everything below needs the board, and the spec says so too. It is listed here
so it is not mistaken for done:

- that a transfer survives a megabyte with flash writes in the socket's
  receive path, or whether the 8-frame window is needed after all;
- `esp_ota_write` timing and the real transfer duration over WiFi;
- free heap during a transfer with the socket open;
- `httpd` receiving a megabyte: `recv_wait_timeout`, handler stack, and the
  idle-marker refresh actually holding the access point up;
- that rollback fires, checked with a deliberately broken image.

The order in the spec's "The order of operations" section is not negotiable,
and steps 4 and 5 there happen on an **open** board.
