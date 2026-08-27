# WiFi provisioning — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the device be joined to a WiFi network from a phone's browser instead of from a compiled-in header, and let the server URI be changed the same way.

**Architecture:** Held button plus microphone silence puts the device into provisioning. It raises an open SoftAP in `WIFI_MODE_APSTA` — station side required for scanning and for trialling credentials — serves one page over `esp_http_server` with a wildcard DNS stub, trials the chosen network before saving anything, and writes to NVS only on success. The panel is the authoritative report because connecting the station can drop the phone. The two most bug-prone parts, the form parser and the screen layout, are pure C with no ESP-IDF dependency so they are tested on the laptop.

**Tech Stack:** ESP-IDF ≥ 5.0 on ESP32-C3, `esp_wifi`, `esp_http_server`, `nvs_flash`, plain C11 for the host-tested parts, `make` + `cc` for the host tests.

**Spec:** `docs/superpowers/specs/2026-08-27-wifi-provisioning-design.md`

## Global Constraints

- **Provisioning is `WIFI_MODE_APSTA`, never `WIFI_MODE_AP`.** `esp_wifi_scan_start()` is *"supported only in station or station/AP mode"* (`esp-idf/docs/en/api-guides/wifi.rst:504`). The station side is torn down to plain `WIFI_MODE_STA` before the websocket opens.
- **Do not touch the station radio settings.** `wc.sta.listen_interval = 1` and `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` in `wifi_start()` are measured values with a history of being made worse. `WIFI_PS_NONE` has been tried twice and is catastrophic on this board.
- **The access point is open.** No WPA2, no passphrase. The server URI field is what carries a lock.
- **The unlock code is four digits, random per session, and five wrong attempts end the attempt** for the rest of that provisioning session.
- **Credentials are written to NVS only after a trial connection succeeds.** Entering provisioning never erases anything.
- **`face_task` is the only writer to the framebuffer.** `setup_screen.c` renders into a buffer it is handed and touches no hardware.
- **`main/setup_screen.c` and `main/provision_logic.c` must include no ESP-IDF header, use no float, and allocate nothing** — the same rules `face.c` follows, because `host/` has to build them.
- **The device must still work with no panel attached.** `voice_main` probes the bus and carries on; provisioning must not become the first feature that requires a display.
- Host tests build with `-std=c11 -O2 -Wall -Wextra -Werror`. A warning is a failure.
- `DEVICE_TOKEN` stays compiled in and never appears in HTML.

---

### Task 1: The pure logic — form parsing, URI validation, unlock attempts, mode choice

Everything in this task is a pure function over bytes. No ESP-IDF, no network, no board. It is first because it is the only part of this feature where TDD works completely, and it is where the parsing bugs live.

**Files:**
- Create: `firmware/main/provision_logic.h`
- Create: `firmware/main/provision_logic.c`
- Create: `firmware/host/provision_logic_test.c`
- Modify: `firmware/host/Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces, used by Tasks 4–7:
  - `size_t pl_url_decode(const char *src, size_t src_len, char *dst, size_t dst_size)` — returns bytes written, always NUL-terminates when `dst_size > 0`.
  - `bool pl_field(const char *body, size_t body_len, const char *name, char *out, size_t out_size)` — finds `name=` in an `application/x-www-form-urlencoded` body, URL-decodes the value into `out`. Returns false if absent.
  - `bool pl_uri_valid(const char *uri)` — shape check only.
  - `pl_mode_t pl_decide(bool ssid_configured, bool hold_satisfied, bool idle_expired)`
  - `bool pl_unlock_check(pl_unlock_t *u, const char *entered)` — constant-time-ish compare, counts failures, locks at `PL_UNLOCK_MAX_TRIES`.

- [ ] **Step 1: Write the header**

Create `firmware/main/provision_logic.h`:

```c
// The parts of provisioning that are pure functions over bytes.
//
// Split out of provision.c for one reason: host/ can build this, and the form
// parser and the URI check are where the bugs actually are. Everything here
// follows face.c's rules - no ESP-IDF header, no float, no allocation - so the
// laptop can run it.

#pragma once

#include <stdbool.h>
#include <stddef.h>

// Bounds taken from wifi_config_t, so nothing here can overflow what it feeds.
#define PL_SSID_MAX 32
#define PL_PASS_MAX 64
#define PL_URI_MAX 128

// Four digits plus the terminator. Four is not what protects the field - see
// pl_unlock_check - it is what stops a single lucky guess.
#define PL_CODE_LEN 4
#define PL_UNLOCK_MAX_TRIES 5

typedef enum {
    PL_MODE_CONNECT = 0,   // join the configured network
    PL_MODE_PROVISION,     // raise the access point
} pl_mode_t;

typedef struct {
    char code[PL_CODE_LEN + 1];
    int tries_used;
    bool unlocked;
} pl_unlock_t;

// Percent-decoding for application/x-www-form-urlencoded, where '+' is a
// space. Writes at most dst_size-1 bytes and always terminates. Returns the
// number of bytes written, not counting the terminator.
size_t pl_url_decode(const char *src, size_t src_len, char *dst, size_t dst_size);

// Pulls one field out of a form body and decodes it. Returns false when the
// field is absent; an empty value present in the body returns true with an
// empty string, which is a different thing and the caller cares.
bool pl_field(const char *body, size_t body_len, const char *name, char *out, size_t out_size);

// Shape only: scheme ws:// or wss://, a non-empty host, under PL_URI_MAX.
// Whether the server answers cannot be known from provisioning - the device is
// not on the internet yet - so a valid-looking wrong URI will be accepted.
bool pl_uri_valid(const char *uri);

pl_mode_t pl_decide(bool ssid_configured, bool hold_satisfied, bool idle_expired);

// Initialises the attempt counter around a code. The code is generated by the
// caller, because randomness needs the platform.
void pl_unlock_init(pl_unlock_t *u, const char *code);

// True when the field is unlocked, whether by this call or a previous one.
// After PL_UNLOCK_MAX_TRIES failures every further call returns false, even if
// the right code arrives - the only way to get more attempts is to hold the
// button again, which requires standing next to the device.
bool pl_unlock_check(pl_unlock_t *u, const char *entered);
```

- [ ] **Step 2: Write the failing tests**

Create `firmware/host/provision_logic_test.c`:

```c
// The provisioning logic that needs no board.
//
//   cd firmware/host && make test

#include <stdio.h>
#include <string.h>

#include "../main/provision_logic.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d  ", __func__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static void test_url_decode_plain(void) {
    char out[32];
    pl_url_decode("hello", 5, out, sizeof(out));
    CHECK(strcmp(out, "hello") == 0, "got %s", out);
}

static void test_url_decode_plus_is_space(void) {
    char out[32];
    pl_url_decode("my+network", 10, out, sizeof(out));
    CHECK(strcmp(out, "my network") == 0, "got %s", out);
}

static void test_url_decode_percent(void) {
    char out[32];
    pl_url_decode("a%26b%3Dc", 9, out, sizeof(out));
    CHECK(strcmp(out, "a&b=c") == 0, "got %s", out);
}

static void test_url_decode_lowercase_hex(void) {
    char out[32];
    pl_url_decode("%2f%2F", 6, out, sizeof(out));
    CHECK(strcmp(out, "//") == 0, "got %s", out);
}

static void test_url_decode_truncates_rather_than_overflows(void) {
    char out[5];
    memset(out, 0x7f, sizeof(out));
    size_t n = pl_url_decode("abcdefghij", 10, out, sizeof(out));
    CHECK(n == 4, "wrote %zu", n);
    CHECK(out[4] == '\0', "not terminated");
    CHECK(strcmp(out, "abcd") == 0, "got %s", out);
}

static void test_url_decode_trailing_percent_is_not_read_past(void) {
    // "%" with nothing after it must not read off the end of the buffer.
    char out[8];
    pl_url_decode("ab%", 3, out, sizeof(out));
    CHECK(strcmp(out, "ab%") == 0, "got %s", out);
}

static void test_field_found(void) {
    const char *body = "ssid=home&pass=secret";
    char out[PL_SSID_MAX + 1];
    CHECK(pl_field(body, strlen(body), "ssid", out, sizeof(out)), "not found");
    CHECK(strcmp(out, "home") == 0, "got %s", out);
}

static void test_field_second_and_last(void) {
    const char *body = "ssid=home&pass=secret";
    char out[PL_PASS_MAX + 1];
    CHECK(pl_field(body, strlen(body), "pass", out, sizeof(out)), "not found");
    CHECK(strcmp(out, "secret") == 0, "got %s", out);
}

static void test_field_absent(void) {
    const char *body = "ssid=home";
    char out[8];
    CHECK(!pl_field(body, strlen(body), "pass", out, sizeof(out)), "found nothing");
}

static void test_field_present_but_empty(void) {
    // Different from absent, and the caller cares: an empty password is legal
    // for an open network, a missing one is a malformed form.
    const char *body = "ssid=home&pass=";
    char out[8];
    CHECK(pl_field(body, strlen(body), "pass", out, sizeof(out)), "not found");
    CHECK(out[0] == '\0', "got %s", out);
}

static void test_field_does_not_match_a_suffix(void) {
    // "pass" must not be found inside "userpass".
    const char *body = "userpass=x&pass=y";
    char out[8];
    CHECK(pl_field(body, strlen(body), "pass", out, sizeof(out)), "not found");
    CHECK(strcmp(out, "y") == 0, "got %s", out);
}

static void test_field_decodes_the_value(void) {
    const char *body = "pass=a%26b+c";
    char out[16];
    CHECK(pl_field(body, strlen(body), "pass", out, sizeof(out)), "not found");
    CHECK(strcmp(out, "a&b c") == 0, "got %s", out);
}

static void test_field_truncates_an_overlong_ssid(void) {
    char body[128];
    // 40 characters, where wifi_config_t allows 32.
    snprintf(body, sizeof(body), "ssid=%s", "0123456789012345678901234567890123456789");
    char out[PL_SSID_MAX + 1];
    CHECK(pl_field(body, strlen(body), "ssid", out, sizeof(out)), "not found");
    CHECK(strlen(out) == PL_SSID_MAX, "kept %zu bytes", strlen(out));
}

static void test_uri_accepts_both_schemes(void) {
    CHECK(pl_uri_valid("ws://192.168.31.214:8000/ws"), "plain ws rejected");
    CHECK(pl_uri_valid("wss://voice.example.com/ws"), "wss rejected");
}

static void test_uri_rejects_other_schemes(void) {
    CHECK(!pl_uri_valid("http://example.com/"), "http accepted");
    CHECK(!pl_uri_valid("wsss://example.com/"), "wsss accepted");
    CHECK(!pl_uri_valid("example.com"), "bare host accepted");
    CHECK(!pl_uri_valid(""), "empty accepted");
}

static void test_uri_rejects_an_empty_host(void) {
    CHECK(!pl_uri_valid("ws://"), "empty host accepted");
    CHECK(!pl_uri_valid("ws:///ws"), "empty host with path accepted");
}

static void test_uri_rejects_overlong(void) {
    char uri[PL_URI_MAX + 32];
    memset(uri, 'a', sizeof(uri));
    memcpy(uri, "ws://", 5);
    uri[sizeof(uri) - 1] = '\0';
    CHECK(!pl_uri_valid(uri), "overlong accepted");
}

static void test_decide_unconfigured_provisions(void) {
    CHECK(pl_decide(false, false, false) == PL_MODE_PROVISION, "not provisioning");
}

static void test_decide_configured_connects(void) {
    CHECK(pl_decide(true, false, false) == PL_MODE_CONNECT, "not connecting");
}

static void test_decide_hold_wins_over_configured(void) {
    CHECK(pl_decide(true, true, false) == PL_MODE_PROVISION, "hold ignored");
}

static void test_decide_idle_expiry_returns_to_the_network(void) {
    // The access point is not a one-way door: a timed-out session goes back to
    // the saved network even though the hold that started it happened.
    CHECK(pl_decide(true, true, true) == PL_MODE_CONNECT, "stuck in provisioning");
}

static void test_decide_idle_expiry_cannot_strand_an_unconfigured_device(void) {
    // With nothing to go back to, timing out must not produce CONNECT - there
    // is no network to connect to and it would wait forever with no way in.
    CHECK(pl_decide(false, false, true) == PL_MODE_PROVISION, "stranded");
}

static void test_unlock_accepts_the_right_code(void) {
    pl_unlock_t u;
    pl_unlock_init(&u, "4271");
    CHECK(pl_unlock_check(&u, "4271"), "right code rejected");
    CHECK(u.unlocked, "not marked unlocked");
}

static void test_unlock_rejects_the_wrong_code(void) {
    pl_unlock_t u;
    pl_unlock_init(&u, "4271");
    CHECK(!pl_unlock_check(&u, "1234"), "wrong code accepted");
    CHECK(!u.unlocked, "marked unlocked");
}

static void test_unlock_stays_unlocked(void) {
    pl_unlock_t u;
    pl_unlock_init(&u, "4271");
    pl_unlock_check(&u, "4271");
    CHECK(pl_unlock_check(&u, ""), "lost the unlock");
}

static void test_unlock_locks_out_after_five_failures(void) {
    // The security property. Four digits are scriptable in under a minute over
    // HTTP, so the attempt limit is what actually protects the field.
    pl_unlock_t u;
    pl_unlock_init(&u, "4271");
    for (int i = 0; i < PL_UNLOCK_MAX_TRIES; i++) {
        CHECK(!pl_unlock_check(&u, "0000"), "wrong code accepted on try %d", i);
    }
    CHECK(!pl_unlock_check(&u, "4271"), "right code accepted after lockout");
    CHECK(!u.unlocked, "unlocked after lockout");
}

static void test_unlock_rejects_wrong_length(void) {
    pl_unlock_t u;
    pl_unlock_init(&u, "4271");
    CHECK(!pl_unlock_check(&u, "427"), "short code accepted");
    CHECK(!pl_unlock_check(&u, "42710"), "long code accepted");
}

int main(void) {
    test_url_decode_plain();
    test_url_decode_plus_is_space();
    test_url_decode_percent();
    test_url_decode_lowercase_hex();
    test_url_decode_truncates_rather_than_overflows();
    test_url_decode_trailing_percent_is_not_read_past();
    test_field_found();
    test_field_second_and_last();
    test_field_absent();
    test_field_present_but_empty();
    test_field_does_not_match_a_suffix();
    test_field_decodes_the_value();
    test_field_truncates_an_overlong_ssid();
    test_uri_accepts_both_schemes();
    test_uri_rejects_other_schemes();
    test_uri_rejects_an_empty_host();
    test_uri_rejects_overlong();
    test_decide_unconfigured_provisions();
    test_decide_configured_connects();
    test_decide_hold_wins_over_configured();
    test_decide_idle_expiry_returns_to_the_network();
    test_decide_idle_expiry_cannot_strand_an_unconfigured_device();
    test_unlock_accepts_the_right_code();
    test_unlock_rejects_the_wrong_code();
    test_unlock_stays_unlocked();
    test_unlock_locks_out_after_five_failures();
    test_unlock_rejects_wrong_length();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 3: Add the target to the host Makefile**

In `firmware/host/Makefile`, add a build rule and hook it into `test`. Replace the `test:` target and add the new rule:

```make
$(BUILD)/provision_logic_test: ../main/provision_logic.c ../main/provision_logic.h provision_logic_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/provision_logic.c provision_logic_test.c

test: $(BUILD)/face_test $(BUILD)/provision_logic_test
	@./$(BUILD)/face_test
	@./$(BUILD)/provision_logic_test
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cd firmware/host && make test`
Expected: the build fails — `provision_logic.c` does not exist. That is the red state; do not proceed until you have seen it.

- [ ] **Step 5: Implement**

Create `firmware/main/provision_logic.c`:

```c
#include "provision_logic.h"

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t pl_url_decode(const char *src, size_t src_len, char *dst, size_t dst_size) {
    if (dst_size == 0) return 0;
    size_t w = 0;
    for (size_t i = 0; i < src_len && w + 1 < dst_size; i++) {
        char c = src[i];
        if (c == '+') {
            dst[w++] = ' ';
        } else if (c == '%' && i + 2 < src_len) {
            const int hi = hex_digit(src[i + 1]);
            const int lo = hex_digit(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[w++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[w++] = c;  // not an escape; pass it through
            }
        } else {
            dst[w++] = c;
        }
    }
    dst[w] = '\0';
    return w;
}

bool pl_field(const char *body, size_t body_len, const char *name, char *out, size_t out_size) {
    if (out_size == 0) return false;
    out[0] = '\0';

    size_t name_len = 0;
    while (name[name_len] != '\0') name_len++;

    for (size_t i = 0; i < body_len;) {
        // A field starts at the beginning of the body or just after '&', which
        // is what stops "pass" matching inside "userpass".
        const size_t start = i;
        size_t end = start;
        while (end < body_len && body[end] != '&') end++;

        if (end - start > name_len && body[start + name_len] == '=') {
            size_t k = 0;
            while (k < name_len && body[start + k] == name[k]) k++;
            if (k == name_len) {
                const size_t vstart = start + name_len + 1;
                pl_url_decode(body + vstart, end - vstart, out, out_size);
                return true;
            }
        }
        i = end + 1;
    }
    return false;
}

bool pl_uri_valid(const char *uri) {
    if (uri == NULL) return false;

    size_t len = 0;
    while (uri[len] != '\0') {
        len++;
        if (len >= PL_URI_MAX) return false;
    }
    if (len == 0) return false;

    const char *host = NULL;
    if (len > 5 && uri[0] == 'w' && uri[1] == 's' && uri[2] == ':' && uri[3] == '/' && uri[4] == '/') {
        host = uri + 5;
    } else if (len > 6 && uri[0] == 'w' && uri[1] == 's' && uri[2] == 's' &&
               uri[3] == ':' && uri[4] == '/' && uri[5] == '/') {
        host = uri + 6;
    } else {
        return false;
    }

    // A host has to be there and cannot start with the path separator.
    return host[0] != '\0' && host[0] != '/';
}

pl_mode_t pl_decide(bool ssid_configured, bool hold_satisfied, bool idle_expired) {
    // Nothing to go back to: an unconfigured device must never be told to
    // connect, because it would wait forever with no way to reach it.
    if (!ssid_configured) return PL_MODE_PROVISION;
    if (idle_expired) return PL_MODE_CONNECT;
    return hold_satisfied ? PL_MODE_PROVISION : PL_MODE_CONNECT;
}

void pl_unlock_init(pl_unlock_t *u, const char *code) {
    for (int i = 0; i < PL_CODE_LEN; i++) u->code[i] = code[i];
    u->code[PL_CODE_LEN] = '\0';
    u->tries_used = 0;
    u->unlocked = false;
}

bool pl_unlock_check(pl_unlock_t *u, const char *entered) {
    if (u->unlocked) return true;
    if (u->tries_used >= PL_UNLOCK_MAX_TRIES) return false;

    // Find how far entered actually goes before comparing against it, capped
    // at PL_CODE_LEN + 1 so a caller passing something enormous costs nothing.
    // The walk stops at the terminator, so it only ever reads a byte once
    // every byte before it is known to exist.
    size_t len = 0;
    while (len <= PL_CODE_LEN && entered[len] != '\0') len++;

    // Compare the whole code every time rather than returning early, so the
    // time taken says nothing about how many digits were right. A wrong
    // length folds into diff via len rather than being checked by indexing
    // past where entered was just shown to end.
    int diff = (len == PL_CODE_LEN) ? 0 : 1;
    for (int i = 0; i < PL_CODE_LEN; i++) {
        unsigned char e = ((size_t)i < len) ? (unsigned char)entered[i] : 0;
        diff |= e ^ (unsigned char)u->code[i];
    }

    u->tries_used++;
    if (diff == 0) {
        u->unlocked = true;
        return true;
    }
    return false;
}
```

**This function was wrong in the first draft of this plan and the bug shipped
before it was caught.** The original broke out of the comparison loop on the
terminator and then read `entered[PL_CODE_LEN]` anyway, so a short code read one
byte past its allocation — confirmed under AddressSanitizer as a
`heap-buffer-overflow` at this line. The existing wrong-length test did not
catch it because it passes a string literal, and a literal has enough bytes
after it that the read lands inside the same page. That is why the host
harness now builds everything under `-fsanitize=address,undefined` as well as
plain, and why the regression test allocates on the heap.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd firmware/host && make test`
Expected: `face_test` reports 487 checks, 0 failures; `provision_logic_test` reports 40-odd checks, 0 failures. Zero compiler warnings — `-Werror` is on.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/provision_logic.c firmware/main/provision_logic.h \
        firmware/host/provision_logic_test.c firmware/host/Makefile
git commit -m "$(cat <<'EOF'
Put the parsing where the laptop can test it

Form decoding and URI validation are where provisioning bugs live, and
neither needs a radio. They go in a file that follows face.c's rules - no
ESP-IDF header, no float, no allocation - so host/ builds them and a
mis-parsed password is caught by make test rather than by a phone.

Three of these tests exist because of specific ways this goes wrong:
"pass" must not match inside "userpass", a field that is present but
empty is not the same as an absent one, and an over-length SSID has to be
truncated rather than overflow the 32 bytes wifi_config_t allows.

The lockout has its own test because it, not the four digits, is what
protects the server URI - ten thousand codes are scriptable in under a
minute, five attempts are not.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: The 5×7 font and the setup screen

**Files:**
- Create: `firmware/main/setup_screen.h`
- Create: `firmware/main/setup_screen.c`
- Create: `firmware/host/setup_screen_test.c`
- Modify: `firmware/host/Makefile`

**Interfaces:**
- Consumes: `FACE_W`, `FACE_H`, `FACE_FB_BYTES` from `face.h` — the framebuffer layout, not the eyes.
- Produces, used by Task 7: `void setup_screen_render(uint8_t *fb, const setup_screen_t *s)`.

**On the font table.** The glyphs are the standard 5×7 ASCII set, columns
left-to-right, one byte per column, bit 0 the top row. This plan does not
transcribe 95 glyphs by hand — that would be 475 bytes of hand-typed hex and
would contain errors. Copy a public-domain 5×7 table (the one in the Adafruit
GFX `glcdfont` is the usual source; take columns 0–4 of each 6-column entry).
The tests below are written to catch a mis-transcribed table: every printable
character must produce ink, space must produce none, and the exact bytes of
`A`, `0` and `:` are pinned.

- [ ] **Step 1: Write the header**

Create `firmware/main/setup_screen.h`:

```c
// The provisioning screen: four lines of 5x7 text on the 128x64 panel.
//
// Same rules as face.c - no ESP-IDF header, no float, no allocation - so
// host/setup_screen_test.c can build it and the layout can be checked without
// a board. It renders into a framebuffer it is handed and knows nothing about
// I2C; face_task owns the panel and calls this instead of the eyes while
// provisioning is up.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "face.h"  // FACE_W, FACE_H, FACE_FB_BYTES only

#define SS_GLYPH_W 5
#define SS_GLYPH_H 7
#define SS_ADVANCE 6  // one blank column between glyphs
#define SS_COLS (FACE_W / SS_ADVANCE)  // 21 characters per line

#define SS_LINE_MAX (SS_COLS + 1)

typedef enum {
    SS_STATUS_WAITING = 0,   // access point up, nobody has submitted
    SS_STATUS_TRYING,        // trialling the credentials
    SS_STATUS_CONNECTED,
    SS_STATUS_BAD_PASSWORD,
    SS_STATUS_NOT_FOUND,
    SS_STATUS_TIMED_OUT,
    SS_STATUS_COUNT
} ss_status_t;

typedef struct {
    char ap_name[SS_LINE_MAX];  // e.g. "Voice-3f2a"
    char address[SS_LINE_MAX];  // "192.168.4.1"
    char code[SS_LINE_MAX];     // the four-digit unlock code
    ss_status_t status;
} setup_screen_t;

// Clears fb and draws the screen. Text longer than a line is truncated, never
// wrapped and never drawn past the edge.
void setup_screen_render(uint8_t *fb, const setup_screen_t *s);

// Exposed for the host test, and useful on its own.
void ss_draw_text(uint8_t *fb, int x, int y, const char *text);
const char *ss_status_text(ss_status_t status);
```

- [ ] **Step 2: Write the failing tests**

Create `firmware/host/setup_screen_test.c`:

```c
// Layout invariants for the provisioning screen.
//
// The panel is 128x64 and unforgiving: a line one character too long does not
// wrap, it disappears off the edge, and the difference is invisible until
// someone is standing in front of the device trying to read a code.
//
//   cd firmware/host && make test

#include <stdio.h>
#include <string.h>

#include "../main/setup_screen.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d  ", __func__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static bool lit_at(const uint8_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return false;
    return (fb[(y / 8) * FACE_W + x] >> (y % 8)) & 1;
}

static int lit_count(const uint8_t *fb) {
    int n = 0;
    for (int i = 0; i < FACE_FB_BYTES; i++) {
        uint8_t b = fb[i];
        while (b) { n += b & 1; b >>= 1; }
    }
    return n;
}

static int lit_in_column(const uint8_t *fb, int x) {
    int n = 0;
    for (int y = 0; y < FACE_H; y++) if (lit_at(fb, x, y)) n++;
    return n;
}

static setup_screen_t sample(void) {
    setup_screen_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.ap_name, sizeof(s.ap_name), "Voice-3f2a");
    snprintf(s.address, sizeof(s.address), "192.168.4.1");
    snprintf(s.code, sizeof(s.code), "4271");
    s.status = SS_STATUS_WAITING;
    return s;
}

static void test_space_draws_nothing(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "   ");
    CHECK(lit_count(fb) == 0, "space drew %d pixels", lit_count(fb));
}

static void test_every_printable_character_draws_something(void) {
    // A mis-transcribed font table shows up here as a blank glyph long before
    // anyone notices a missing character in a code on the panel.
    for (int c = '!'; c <= '~'; c++) {
        uint8_t fb[FACE_FB_BYTES];
        memset(fb, 0, sizeof(fb));
        const char text[2] = {(char)c, '\0'};
        ss_draw_text(fb, 0, 0, text);
        CHECK(lit_count(fb) > 0, "'%c' (0x%02x) drew nothing", c, c);
    }
}

static void test_a_glyph_stays_inside_its_five_columns(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "A");
    CHECK(lit_in_column(fb, SS_GLYPH_W) == 0, "column %d has ink", SS_GLYPH_W);
    for (int x = SS_GLYPH_W; x < FACE_W; x++) {
        CHECK(lit_in_column(fb, x) == 0, "ink at x=%d from one glyph", x);
    }
}

static void test_a_glyph_stays_inside_its_seven_rows(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "A");
    for (int y = SS_GLYPH_H; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x++) {
            CHECK(!lit_at(fb, x, y), "ink at y=%d from one glyph", y);
        }
    }
}

static void test_text_is_truncated_not_wrapped(void) {
    // 30 characters into a 21-column panel. The tail must be dropped, and
    // nothing may appear on the row below.
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0, sizeof(fb));
    ss_draw_text(fb, 0, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123");
    for (int y = SS_GLYPH_H; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x++) {
            CHECK(!lit_at(fb, x, y), "wrapped: ink at (%d,%d)", x, y);
        }
    }
}

static void test_nothing_is_drawn_outside_the_framebuffer(void) {
    // Negative and past-the-edge origins must clip rather than corrupt. The
    // guard bytes catch a write outside fb entirely.
    uint8_t buf[FACE_FB_BYTES + 16];
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *fb = buf + 8;
    memset(fb, 0, FACE_FB_BYTES);

    ss_draw_text(fb, -20, 0, "EDGE");
    ss_draw_text(fb, FACE_W - 2, 0, "EDGE");
    ss_draw_text(fb, 0, FACE_H - 2, "EDGE");

    for (int i = 0; i < 8; i++) {
        CHECK(buf[i] == 0xAA, "wrote %d bytes before fb", 8 - i);
        CHECK(buf[FACE_FB_BYTES + 8 + i] == 0xAA, "wrote past the end of fb");
    }
}

static void test_the_screen_shows_all_four_lines(void) {
    uint8_t fb[FACE_FB_BYTES];
    setup_screen_t s = sample();
    setup_screen_render(fb, &s);

    // Four rows of 7 pixels with gaps must all carry ink, and none may sit
    // below the panel. Checked as four disjoint horizontal bands.
    int bands_with_ink = 0;
    for (int band = 0; band < 4; band++) {
        int n = 0;
        for (int y = band * 16; y < band * 16 + SS_GLYPH_H && y < FACE_H; y++) {
            for (int x = 0; x < FACE_W; x++) if (lit_at(fb, x, y)) n++;
        }
        if (n > 0) bands_with_ink++;
    }
    CHECK(bands_with_ink == 4, "only %d of 4 lines drew", bands_with_ink);
}

static void test_render_clears_what_was_there(void) {
    uint8_t fb[FACE_FB_BYTES];
    memset(fb, 0xFF, sizeof(fb));
    setup_screen_t s = sample();
    setup_screen_render(fb, &s);
    CHECK(lit_count(fb) < FACE_FB_BYTES * 8 / 2, "did not clear: %d lit", lit_count(fb));
}

static void test_a_maximum_length_ap_name_does_not_overrun(void) {
    uint8_t buf[FACE_FB_BYTES + 16];
    memset(buf, 0xAA, sizeof(buf));
    uint8_t *fb = buf + 8;

    setup_screen_t s = sample();
    memset(s.ap_name, 'W', SS_LINE_MAX - 1);
    s.ap_name[SS_LINE_MAX - 1] = '\0';
    setup_screen_render(fb, &s);

    for (int i = 0; i < 8; i++) {
        CHECK(buf[i] == 0xAA, "overran before fb");
        CHECK(buf[FACE_FB_BYTES + 8 + i] == 0xAA, "overran after fb");
    }
}

static void test_every_status_has_distinct_text(void) {
    // A status the panel renders as an empty string is a device that has
    // stopped explaining itself at the moment it most needs to.
    for (int a = 0; a < SS_STATUS_COUNT; a++) {
        const char *ta = ss_status_text((ss_status_t)a);
        CHECK(ta != NULL && ta[0] != '\0', "status %d has no text", a);
        CHECK(strlen(ta) <= SS_COLS, "status %d is %zu cols, panel fits %d",
              a, strlen(ta), SS_COLS);
        for (int b = a + 1; b < SS_STATUS_COUNT; b++) {
            CHECK(strcmp(ta, ss_status_text((ss_status_t)b)) != 0,
                  "status %d and %d read the same", a, b);
        }
    }
}

int main(void) {
    test_space_draws_nothing();
    test_every_printable_character_draws_something();
    test_a_glyph_stays_inside_its_five_columns();
    test_a_glyph_stays_inside_its_seven_rows();
    test_text_is_truncated_not_wrapped();
    test_nothing_is_drawn_outside_the_framebuffer();
    test_the_screen_shows_all_four_lines();
    test_render_clears_what_was_there();
    test_a_maximum_length_ap_name_does_not_overrun();
    test_every_status_has_distinct_text();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 3: Add the target to the host Makefile**

```make
$(BUILD)/setup_screen_test: ../main/setup_screen.c ../main/setup_screen.h setup_screen_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../main/setup_screen.c setup_screen_test.c

test: $(BUILD)/face_test $(BUILD)/provision_logic_test $(BUILD)/setup_screen_test
	@./$(BUILD)/face_test
	@./$(BUILD)/provision_logic_test
	@./$(BUILD)/setup_screen_test
```

- [ ] **Step 4: Run to verify failure**

Run: `cd firmware/host && make test`
Expected: build failure, `setup_screen.c` missing.

- [ ] **Step 5: Implement**

Create `firmware/main/setup_screen.c` with:

1. `static const uint8_t FONT5X7[95][5]` — ASCII 32 to 126, columns
   left-to-right, bit 0 the top row. Transcribe from a public-domain 5×7
   table. Index with `c - 32`; anything outside 32–126 renders as `?`.
2. `ss_draw_text(fb, x, y, text)` — for each character, for each of the 5
   columns, for each of the 7 rows, set the bit when the font bit is set.
   Clip every pixel against `0 <= x < FACE_W` and `0 <= y < FACE_H` **before**
   touching `fb`; do not clip only the starting position, because a glyph that
   starts on the panel can still end off it. Stop when `x` passes `FACE_W`.
3. `ss_status_text()` — one short string per status, each ≤ 21 characters:
   `"waiting"`, `"connecting..."`, `"connected"`, `"wrong password"`,
   `"network not found"`, `"timed out"`.
4. `setup_screen_render(fb, s)` — `memset(fb, 0, FACE_FB_BYTES)`, then four
   lines at `y = 0, 16, 32, 48`: the AP name, the address, `"code "` followed
   by `s->code`, and `ss_status_text(s->status)`.

The pixel write, given the page layout in `face.h`:

```c
static void set_pixel(uint8_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return;
    fb[(y / 8) * FACE_W + x] |= (uint8_t)(1u << (y % 8));
}
```

- [ ] **Step 6: Run to verify pass**

Run: `cd firmware/host && make test`
Expected: all three test binaries pass, zero warnings.

If `test_every_printable_character_draws_something` fails for a specific
character, that entry of the font table was transcribed wrong — fix the table,
not the test.

- [ ] **Step 7: Look at it**

Run: `cd firmware/host && make preview && open build/face-preview.html`
Expected: the existing preview still builds and runs. This step is a guard: the
new file must not have broken `face.c`'s build.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/setup_screen.c firmware/main/setup_screen.h \
        firmware/host/setup_screen_test.c firmware/host/Makefile
git commit -m "$(cat <<'EOF'
Teach the panel to write, in its own file

There was no font anywhere: face.c draws eyes and ssd1306.c moves bytes.
Provisioning needs to show an access point name, an address and a code,
so a 5x7 table and a text routine had to exist.

It is not in face.c. That file's stated discipline is eyes and nothing
else, and more usefully a separate file keeps the property that made the
eyes work at all - it builds on the laptop, so the layout is checked by
make test rather than by squinting at a 0.96" screen.

The tests are about the two ways this fails invisibly: a line one
character too long does not wrap, it walks off the edge, and a
mis-transcribed font entry is a blank glyph nobody notices until it is
in a code someone is trying to read. Guard bytes either side of the
framebuffer catch a write that leaves it entirely.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `config_store` — NVS, with `secrets.h` as the fallback

**Files:**
- Create: `firmware/main/config_store.h`
- Create: `firmware/main/config_store.c`
- Modify: `firmware/main/secrets.h.example`
- Modify: `firmware/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `PL_SSID_MAX`, `PL_PASS_MAX`, `PL_URI_MAX` from `provision_logic.h`.
- Produces, used by Tasks 5–7:
  - `void config_load(device_config_t *out)` — NVS first, `secrets.h` where NVS is empty. Never fails; an unreadable NVS yields the compiled values.
  - `esp_err_t config_save_wifi(const char *ssid, const char *pass)`
  - `esp_err_t config_save_uri(const char *uri)`
  - `bool config_is_provisioned(const device_config_t *c)` — true when `ssid[0] != '\0'`.

- [ ] **Step 1: Write the header**

Create `firmware/main/config_store.h`:

```c
// What the device is configured with, and where it came from.
//
// NVS is the source; secrets.h is the fallback for whatever NVS does not have.
// That ordering is what keeps the bench working unchanged - a board flashed
// with a filled-in secrets.h never has to be provisioned - while letting a
// board with an empty one configure itself from a phone.

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "provision_logic.h"  // PL_SSID_MAX, PL_PASS_MAX, PL_URI_MAX

typedef struct {
    char ssid[PL_SSID_MAX + 1];
    char pass[PL_PASS_MAX + 1];
    char uri[PL_URI_MAX + 1];
} device_config_t;

// Reads NVS, filling anything absent from secrets.h. Never fails: a corrupt or
// unreadable NVS is indistinguishable to the caller from an empty one, and the
// compiled values are a working answer in both cases.
void config_load(device_config_t *out);

// Written only after a trial connection succeeds. Nothing about entering
// provisioning may erase a working network.
esp_err_t config_save_wifi(const char *ssid, const char *pass);
esp_err_t config_save_uri(const char *uri);

bool config_is_provisioned(const device_config_t *c);
```

- [ ] **Step 2: Implement**

Create `firmware/main/config_store.c`:

```c
#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "secrets.h"

static const char *TAG = "config";
static const char *NS = "cfg";

static void load_one(nvs_handle_t h, const char *key, const char *fallback,
                     char *out, size_t out_size) {
    size_t len = out_size;
    if (h != 0 && nvs_get_str(h, key, out, &len) == ESP_OK && out[0] != '\0') {
        return;
    }
    // Absent, empty, or NVS unavailable: the compiled value is the answer.
    strncpy(out, fallback, out_size - 1);
    out[out_size - 1] = '\0';
}

void config_load(device_config_t *out) {
    memset(out, 0, sizeof(*out));

    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        h = 0;  // never opened; load_one falls back for every field
    }

    load_one(h, "ssid", WIFI_SSID, out->ssid, sizeof(out->ssid));
    load_one(h, "pass", WIFI_PASSWORD, out->pass, sizeof(out->pass));
    load_one(h, "server_uri", SERVER_URI, out->uri, sizeof(out->uri));

    if (h != 0) nvs_close(h);

    ESP_LOGI(TAG, "ssid \"%s\", uri \"%s\"", out->ssid, out->uri);
}

static esp_err_t save_pair(const char *k1, const char *v1, const char *k2, const char *v2) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, k1, v1);
    if (err == ESP_OK && k2 != NULL) err = nvs_set_str(h, k2, v2);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_save_wifi(const char *ssid, const char *pass) {
    return save_pair("ssid", ssid, "pass", pass);
}

esp_err_t config_save_uri(const char *uri) {
    return save_pair("server_uri", uri, NULL, NULL);
}

bool config_is_provisioned(const device_config_t *c) {
    return c->ssid[0] != '\0';
}
```

- [ ] **Step 3: Empty the template, and say why**

Replace `firmware/main/secrets.h.example` with:

```c
// Copy to secrets.h and fill in. secrets.h is git-ignored.
//
//     cp firmware/main/secrets.h.example firmware/main/secrets.h
//
// These are the *fallback*. NVS wins wherever it has a value, and NVS is what
// the phone writes. Leaving WIFI_SSID empty is how you get a device that
// raises its own access point on first boot and asks - which is the normal
// path. Filling it in is the bench shortcut: a board flashed this way joins
// the network immediately and never needs provisioning.
//
// The ESP32-C3 has no 5 GHz radio: WIFI_SSID must name a 2.4 GHz network.
#pragma once

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Where the server lives. Also settable from the phone, behind the unlock code.
#define SERVER_URI "wss://voice-server-production-e023.up.railway.app/ws"

// Must match DEVICE_TOKEN in the server's .env. Empty disables the check.
// Not settable from the phone: it is a secret and does not belong in HTML.
#define DEVICE_TOKEN ""
```

- [ ] **Step 4: Add the sources that exist to the build**

In `firmware/main/CMakeLists.txt`, extend the `voice` branch. **Do not list
`provision.c` yet** — it does not exist until Task 4, and a commit that does not
build is not worth the convenience of editing this file once:

```cmake
if(SKETCH STREQUAL "face" OR SKETCH STREQUAL "voice")
    list(APPEND SKETCH_SRCS "face.c" "ssd1306.c")
endif()

# Provisioning is part of the real device only. The bring-up sketches exist to
# isolate one piece of hardware each, and a WiFi stack in them would defeat it.
if(SKETCH STREQUAL "voice")
    list(APPEND SKETCH_SRCS "provision_logic.c" "setup_screen.c" "config_store.c")
endif()
```

Task 4 adds `provision.c` to that list in the same commit that creates the file.

- [ ] **Step 5: Build, so the commit is known good**

Run:
```bash
deactivate 2>/dev/null; unset VIRTUAL_ENV
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v "Documents/BIT/.venv" | paste -sd: -)
. ~/esp/esp-idf/export.sh
cd firmware && idf.py -DSKETCH=voice build
```

Expected: builds with zero warnings. `config_store.c` is compiled but nothing
calls it yet, which is fine — Task 7 wires it up. Do not pipe this through
`tail`: the exit status would be `tail`'s, and that has already once reported a
failed build as a clean pass.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/config_store.c firmware/main/config_store.h \
        firmware/main/secrets.h.example firmware/main/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Read the network from NVS, and fall back to what was compiled in

secrets.h stops being the source and becomes the fallback, which is what
lets a phone change the network without taking the bench away: a board
flashed with a filled-in header joins immediately and is never asked
anything.

The template now ships empty strings, and that is the load-bearing part.
It used to carry "your-2.4GHz-network", which is a non-empty string and
would have counted as configured - so a board built from the template
unchanged would have waited forever for a network that does not exist
instead of raising an access point and asking.

An unreadable NVS is deliberately indistinguishable from an empty one.
Both mean "use what was compiled in", which is a working answer, and
neither is worth a failure path the caller would have to handle.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: The access point, the page, and the DNS stub

No credential trial yet — this task ends with a device that raises an open AP,
serves the form over HTTP, and can be reached from a phone. Trialling is Task 5.

**Files:**
- Create: `firmware/main/provision.h`
- Create: `firmware/main/provision.c`
- Modify: `firmware/main/CMakeLists.txt` — add `provision.c` to the `voice` list, in this commit, because that is the commit where the file starts existing

**Interfaces:**
- Consumes: everything from Tasks 1–3.
- Produces, used by Tasks 6–7:
  - `esp_err_t provision_start(void)` — raises the AP, starts httpd and DNS, returns immediately.
  - `void provision_stop(void)`
  - `bool provision_is_active(void)`
  - `bool provision_idle_expired(void)` — true when `AP_IDLE_MS` has passed since the last HTTP request.
  - `void provision_screen(setup_screen_t *out)` — fills the current screen contents for `face_task`.

- [ ] **Step 1: Write the header**

Create `firmware/main/provision.h`:

```c
// Provisioning mode: an open access point, one page, and a DNS stub.
//
// The mode is WIFI_MODE_APSTA throughout and this is not a preference. Both
// scanning for networks and trialling credentials need the station interface:
// esp_wifi_scan_start() is "supported only in station or station/AP mode"
// (esp-idf/docs/en/api-guides/wifi.rst:504). The station side is torn back
// down to plain STA before the websocket opens - an access point must not
// share the radio with a conversation.

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "setup_screen.h"

#define PROV_AP_IDLE_MS 300000  // since the last HTTP request, not since start
#define PROV_TRIAL_MS 20000
#define PROV_GRACE_MS 3000      // AP holds this long after success, so a
                                // surviving page can collect the result

esp_err_t provision_start(void);
void provision_stop(void);
bool provision_is_active(void);

// True when PROV_AP_IDLE_MS has passed with no HTTP request. A phone that
// joins and sits there does not hold the session open; a person part-way
// through typing does, because typing produces requests.
bool provision_idle_expired(void);

// What face_task should draw. Safe to call from another task: it copies.
void provision_screen(setup_screen_t *out);
```

- [ ] **Step 2: Implement the access point and the server**

Create `firmware/main/provision.c`. The pieces, in order:

```c
#include "provision.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

#include "config_store.h"
#include "provision_logic.h"

static const char *TAG = "prov";

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_dns_task = NULL;
static volatile bool s_active = false;
static volatile TickType_t s_last_request = 0;
static pl_unlock_t s_unlock;
static setup_screen_t s_screen;
static SemaphoreHandle_t s_screen_lock = NULL;
```

`provision_start()` does, in this order:

1. `esp_wifi_stop()` if the stack is running.
2. Build the AP name from the station MAC:
   ```c
   uint8_t mac[6];
   esp_read_mac(mac, ESP_MAC_WIFI_STA);
   char ap_name[16];
   snprintf(ap_name, sizeof(ap_name), "Voice-%02x%02x", mac[4], mac[5]);
   ```
3. Generate the session code and initialise the unlock state:
   ```c
   char code[PL_CODE_LEN + 1];
   snprintf(code, sizeof(code), "%04u", (unsigned)(esp_random() % 10000u));
   pl_unlock_init(&s_unlock, code);
   ```
   Random per session, not derived from the MAC — the MAC is broadcast in every
   frame, so anything derived from it is public.
4. `esp_netif_create_default_wifi_ap()` if not already created.
5. Configure the AP as **open**:
   ```c
   wifi_config_t ap = {0};
   strncpy((char *)ap.ap.ssid, ap_name, sizeof(ap.ap.ssid) - 1);
   ap.ap.ssid_len = strlen(ap_name);
   ap.ap.channel = 1;
   ap.ap.max_connection = 2;
   ap.ap.authmode = WIFI_AUTH_OPEN;
   ```
6. `esp_wifi_set_mode(WIFI_MODE_APSTA)`, `esp_wifi_set_config(WIFI_IF_AP, &ap)`,
   `esp_wifi_start()`. **Do not** call `esp_wifi_connect()` — the station
   interface exists for scanning and trialling, not for joining anything yet.
7. Start httpd with the socket count cut down, because one phone needs nothing
   like the default:
   ```c
   httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
   cfg.max_open_sockets = 2;   // default is 7, of which 3 are reserved
   cfg.lru_purge_enable = true;
   ```
8. Register handlers: `GET /` (the page), `POST /save`, `GET /status`,
   `GET /scan`, and a wildcard `GET /*` that redirects to `/`.
9. Start the DNS task.
10. Fill `s_screen` with the AP name, `"192.168.4.1"`, the code, and
    `SS_STATUS_WAITING`. Log all three, so a device with no panel still tells
    someone watching the USB console.
11. `s_last_request = xTaskGetTickCount(); s_active = true;`

**The DNS stub** is one task on UDP port 53. For every query received it copies
the request ID and question, sets the response flags, and appends an A record
pointing at `192.168.4.1`. A 512-byte buffer is enough — anything larger is not
a captive-portal probe. Whether the phone's own captive-portal detection pops
the page up is not relied on; the panel shows the address.

**Every handler calls** `s_last_request = xTaskGetTickCount();` as its first
statement. That is what makes `provision_idle_expired()` measure inactivity
rather than uptime, and it is the difference between dropping a user who is
typing at 4:50 and not.

**The page** is one static HTML string with the network list substituted in. It
contains:
- the scanned networks as radio buttons, strongest first
- a line saying only 2.4 GHz networks appear, because the device has no 5 GHz
  radio — without it, a user whose phone shows a 5 GHz network hunts for it
- a password field
- the server URI, `disabled`, with the current value shown and a four-digit
  code field beside it
- a line saying that if the page stops responding, the device's own screen has
  the answer

- [ ] **Step 3: Build it**

Run:
```bash
deactivate 2>/dev/null; unset VIRTUAL_ENV
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v "Documents/BIT/.venv" | paste -sd: -)
. ~/esp/esp-idf/export.sh
cd firmware && idf.py -DSKETCH=voice build
```

Expected: builds with **zero warnings**. Do not pipe this through `tail` — the
exit status would be `tail`'s, and that has already once reported a failed
build as a clean pass.

- [ ] **Step 4: Commit**

```bash
git add firmware/main/provision.c firmware/main/provision.h
git commit -m "$(cat <<'EOF'
Raise an open access point with one page on it

The mode is APSTA and that is forced, not chosen: scanning needs the
station interface up - "supported only in station or station/AP mode",
wifi.rst:504 - and so does trialling credentials later. The ban on an
access point sharing the radio is about conversations, and the station
side is torn back down before the websocket opens.

The access point takes no password. WPA2 would mean reading eight hex
digits off a 0.96" panel and typing them on a phone every time, to stop a
passerby inside a five-minute window - and it would make a device with no
display impossible to provision at all. The lock is on the one field that
warrants it, the server URI, behind a code that is random per session
because the MAC it would otherwise derive from is broadcast in every
frame.

httpd runs with two sockets rather than the default seven. One phone
needs nothing like that, and this device has 55 KB of heap during a
conversation.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Scanning, the credential trial, and the event handler that would sabotage it

**Files:**
- Modify: `firmware/main/provision.c`
- Modify: `firmware/main/voice_main.c` — the `wifi_event` handler at lines 409–419

**Interfaces:**
- Produces: `void provision_set_trial_mode(bool on)` in `provision.h`, called by
  the trial; and a hook the event handler uses to decide whether a disconnect
  means "retry" or "the trial failed".

- [ ] **Step 1: Teach the event handler about trials**

`voice_main.c:412` currently reconnects unconditionally:

```c
} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    ESP_LOGW(TAG, "wifi dropped, reconnecting");
    esp_wifi_connect();
}
```

That is right for normal operation and fatal during a trial: a wrong password
disconnects, this reconnects with the same wrong password, and the trial can
only ever end in a timeout — reporting "timed out" for something the disconnect
reason names exactly. Replace with:

```c
} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
    if (s_trial_in_progress) {
        // A trial asks a question, so a disconnect is the answer, not a fault
        // to recover from. The reason code is what separates "wrong password"
        // from "that network is not here", and telling them apart is most of
        // the value of trialling before saving.
        s_trial_reason = d->reason;
        xEventGroupSetBits(s_wifi_events, WIFI_TRIAL_DONE_BIT);
        return;
    }
    ESP_LOGW(TAG, "wifi dropped, reconnecting (reason %d)", (int)d->reason);
    esp_wifi_connect();
}
```

with, near the other volatiles:

```c
// Set while a provisioning trial is running. Outside a trial the handler
// behaves exactly as it always has, including reconnecting forever through a
// router reboot, which is what keeps the device alive.
static volatile bool s_trial_in_progress = false;
static volatile uint8_t s_trial_reason = 0;
#define WIFI_TRIAL_DONE_BIT BIT1
```

- [ ] **Step 2: Implement the scan**

In `provision.c`, `GET /scan` runs a blocking all-channel scan and returns the
results as JSON, strongest first:

```c
wifi_scan_config_t sc = {0};  // NULL SSID, all channels, active
esp_err_t err = esp_wifi_scan_start(&sc, true);
if (err == ESP_ERR_WIFI_STATE) {
    // "wifi still connecting when invoke esp_wifi_scan_start" (esp_wifi.h:514).
    // A trial is running; say so rather than failing silently.
    return httpd_resp_send_err(req, HTTPD_409_CONFLICT, "trial in progress");
}
```

Then `esp_wifi_scan_get_ap_num()` / `esp_wifi_scan_get_ap_records()`, capped at
20 records. Records are dynamically allocated by the driver and **must** be
drained or freed — `esp_wifi_scan_get_ap_records()` frees them; if you bail out
early, call `esp_wifi_clear_ap_list()`.

- [ ] **Step 3: Implement the trial**

`POST /save`:

1. Read the body, `pl_field()` out `ssid`, `pass`, and optionally `uri` and
   `code`.
2. If `uri` is present: `pl_unlock_check(&s_unlock, code)` must pass **and**
   `pl_uri_valid(uri)` must pass. A wrong code leaves the URI untouched and the
   WiFi fields still take effect — locking one field must not block the other.
3. Set the screen to `SS_STATUS_TRYING`.
4. `s_trial_in_progress = true`, configure `WIFI_IF_STA` with the candidate,
   `esp_wifi_connect()`.
5. Wait up to `PROV_TRIAL_MS` on `WIFI_CONNECTED_BIT | WIFI_TRIAL_DONE_BIT`.
6. Outcome:
   - connected → `config_save_wifi()`, and `config_save_uri()` if unlocked;
     screen to `SS_STATUS_CONNECTED`
   - `WIFI_REASON_NO_AP_FOUND` → `SS_STATUS_NOT_FOUND`, nothing saved
   - any other disconnect reason → `SS_STATUS_BAD_PASSWORD`, nothing saved
   - neither bit inside `PROV_TRIAL_MS` → `SS_STATUS_TIMED_OUT`, nothing saved
7. `s_trial_in_progress = false`.
8. Reply with the outcome. On success, hold the AP for `PROV_GRACE_MS` before
   anything tears it down — tearing it down the instant the trial succeeds
   would guarantee the page never gets its answer even for a phone that would
   have survived the channel switch.

`GET /status` returns the current status as JSON so the page can poll. It may
never be reached: connecting the station forces the AP onto the home network's
channel (`wifi.rst:1660`), which can drop the phone at the exact moment of
success. That is why the panel carries the same states and why the page says to
look at the device.

- [ ] **Step 4: Build**

Run the build from Task 4 Step 3.
Expected: zero warnings.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/provision.c firmware/main/voice_main.c
git commit -m "$(cat <<'EOF'
Trial the network before saving it, and stop the handler fighting that

voice_main.c reconnected unconditionally on STA_DISCONNECTED. That is
right for a dropped link and fatal for a credential trial: a wrong
password disconnects, the handler retries the same wrong password, and
the trial can only ever end in a timeout - reporting "timed out" for
something the disconnect reason names exactly.

It now ends the trial instead of retrying, and only while a trial is
running. Outside one it behaves as it always has, including reconnecting
forever through a router reboot, which is what keeps this device alive on
an unreliable link.

The reason code earns its keep: NO_AP_FOUND and an auth failure send the
user to different next actions, and telling them apart is most of the
value of trialling at all. Nothing reaches NVS until the trial succeeds.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: The countdown, and which holds arm it

**Files:**
- Modify: `firmware/main/face.c`, `firmware/main/face.h`
- Modify: `firmware/host/face_test.c`

**Interfaces:**
- Produces: `void face_set_reset_progress(face_t *f, uint8_t percent, uint32_t now_ms)` — 0 means not counting.

- [ ] **Step 1: Write the failing tests**

Append to `firmware/host/face_test.c`, and call them from `main()`:

```c
static void test_the_reset_countdown_looks_nothing_like_listening(void) {
    // The whole point is that a user holding the button sees it coming and
    // can let go. If it renders like listening, it cannot be noticed.
    face_t a, b;
    uint32_t t = 1000;

    face_init(&a, t);
    face_set_state(&a, FACE_ST_LISTENING, t);
    face_set_reset_progress(&a, 0, t);

    face_init(&b, t);
    face_set_state(&b, FACE_ST_LISTENING, t);
    face_set_reset_progress(&b, 90, t);

    for (int i = 0; i < 10; i++) { t += TICK_MS; face_tick(&a, t); face_tick(&b, t); }

    const int la = lit_count(face_framebuffer(&a));
    const int lb = lit_count(face_framebuffer(&b));
    const int diff = la > lb ? la - lb : lb - la;
    CHECK(diff > 200, "countdown differs from listening by only %d pixels", diff);
}

static void test_the_countdown_is_monotonic(void) {
    // Progress must read as progress: more of the gesture done means more of
    // whatever the eyes are doing. A wobble reads as a glitch.
    int prev = -1;
    for (int pct = 0; pct <= 100; pct += 10) {
        face_t f;
        uint32_t t = 1000;
        face_init(&f, t);
        face_set_state(&f, FACE_ST_LISTENING, t);
        face_set_reset_progress(&f, (uint8_t)pct, t);
        for (int i = 0; i < 5; i++) { t += TICK_MS; face_tick(&f, t); }
        const int n = lit_count(face_framebuffer(&f));
        if (prev >= 0) CHECK(n <= prev, "pct %d lit %d, previous %d", pct, n, prev);
        prev = n;
    }
}

static void test_zero_progress_restores_the_ordinary_face(void) {
    // Releasing cancels with nothing lost, so zero must be indistinguishable
    // from never having started.
    face_t a, b;
    uint32_t t = 1000;

    face_init(&a, t);
    face_set_state(&a, FACE_ST_LISTENING, t);

    face_init(&b, t);
    face_set_state(&b, FACE_ST_LISTENING, t);
    face_set_reset_progress(&b, 60, t);
    face_set_reset_progress(&b, 0, t);

    for (int i = 0; i < 5; i++) { t += TICK_MS; face_tick(&a, t); face_tick(&b, t); }
    CHECK(memcmp(face_framebuffer(&a), face_framebuffer(&b), FACE_FB_BYTES) == 0,
          "cancel did not restore the face");
}
```

**There is no `face_render(face_t *, uint8_t *)`, and assuming one is the
easiest mistake to make here.** `face_tick()` renders into the face's own
buffer — *"Advance the animation to now_ms and render into f->fb"*
(`face.h:161`) — and `face_framebuffer(&f)` returns it
(`face.h:164`, a `static inline` returning `const uint8_t *`). `face_render_pose()`
is a different thing: it rasterises one static pose and is what the emotion
tests use. The tests above use the real API.

`lit_count()` already exists in `face_test.c` and takes a `const uint8_t *`.

- [ ] **Step 2: Run to verify failure**

Run: `cd firmware/host && make test`
Expected: build failure, `face_set_reset_progress` undeclared.

- [ ] **Step 3: Implement**

Add to `face.h`:

```c
// How far through the hold-to-reset gesture the user is, 0-100. Zero means
// not counting, and setting zero must leave no trace - releasing the button
// cancels and nothing was lost.
void face_set_reset_progress(face_t *f, uint8_t percent, uint32_t now_ms);
```

In `face.c`, store the percentage and apply it as a modulation after the state
pose is computed, the way `startle` and loudness already work. The shape is
chosen in the preview; the tests above only require that it is far from
listening, monotonic, and cancels cleanly.

- [ ] **Step 4: Run to verify pass, then look at it**

Run: `cd firmware/host && make test`
Expected: all four binaries pass, zero warnings.

Run: `cd firmware/host && make preview && open build/face-preview.html`
Expected: the countdown is visible in the preview and reads as a warning rather
than as a mood. Adjust the shape here, not on the board.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/face.c firmware/main/face.h firmware/host/face_test.c
git commit -m "$(cat <<'EOF'
Show the reset coming, so it can be called off

A hold that silently reconfigures the device is a trap. The eyes now
carry how far through the gesture the hold is, and the tests pin the
three properties that make it safe rather than decorative: it must look
nothing like listening, or nobody notices; progress must be monotonic, or
it reads as a glitch; and cancelling must restore the previous face
exactly, because releasing is supposed to cost nothing.

The shape itself was chosen in the preview on the laptop, the way every
other expression in this project was.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Wiring — boot order, the hold detector, the panel handover

**Files:**
- Modify: `firmware/main/voice_main.c`

**Interfaces:**
- Consumes: everything from Tasks 1–6.

- [ ] **Step 1: Move `button_task` ahead of `wifi_start()`**

In `app_main`, `button_task` is created after `wifi_start()`, which blocks. So
while the device is stuck connecting, nothing samples the button — the press is
undetectable in exactly the situation that needs it. Move the
`xTaskCreate(button_task, ...)` line to just after the panel probe and before
`wifi_start()`. It touches only GPIO and its own debounce state, so it has no
dependency on the radio.

- [ ] **Step 2: Make `wifi_start()` interruptible without making it wait less**

Replace the final wait:

```c
xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
```

with a wait on either bit:

```c
// Still no timeout: the device waits for its network exactly as long as it
// always has. It just stops being unreachable while it does.
xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_PROVISION_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);
```

with `#define WIFI_PROVISION_BIT BIT2` near the other bits.

- [ ] **Step 3: Add the hold detector**

In `net_task`, alongside the existing button handling, count silent held time:

```c
// Armed in ST_LISTENING and when the socket is down, and nowhere else.
// During a reply a hold means "interrupt", which is the commoner intent by a
// wide margin, and the two must not compete.
const bool armed = (s_state == ST_LISTENING) || !esp_websocket_client_is_connected(s_ws);
if (s_button_down && armed && s_audio_level < RESET_SILENCE_LEVEL) {
    silent_ticks++;
} else {
    silent_ticks = 0;  // speech, release, or a state that does not arm it
}
const uint32_t held_ms = silent_ticks * NET_TASK_PERIOD_MS;
s_face_reset_pct = (uint8_t)((held_ms * 100) / RESET_HOLD_MS);
if (held_ms >= RESET_HOLD_MS) { /* enter provisioning, below */ }
```

`RESET_SILENCE_LEVEL` is a **bench number**. Put a placeholder of 400 in and a
comment saying it is unmeasured, then take the measurement in Step 6 and change
it in its own commit. Do not present an unmeasured value as a measured one.

Entering provisioning from `ST_LISTENING` abandons the utterance in flight, the
same way the interrupt path already does:

```c
if (esp_websocket_client_is_connected(s_ws)) {
    esp_websocket_client_send_text(s_ws, "{\"type\":\"cancel\"}", 17, SEND_TIMEOUT);
}
xStreamBufferReset(s_mic_buf);
s_state = ST_IDLE;
xEventGroupSetBits(s_wifi_events, WIFI_PROVISION_BIT);
```

- [ ] **Step 4: Hand the panel over**

`face_task` today advances the face and flushes the face's own buffer
(`voice_main.c:702,708`):

```c
face_tick(&s_face, t);
const esp_err_t ferr = ssd1306_flush(&s_panel, face_framebuffer(&s_face));
```

The setup screen needs somewhere to draw that is not inside `face_t` — writing
into the face's private buffer would make `setup_screen.c` reach into another
module's state, which is exactly what its no-dependency rule exists to prevent.
Give it a static buffer of its own; 1 KB of BSS, not heap, on a device with
55 KB free during a conversation:

```c
static uint8_t s_setup_fb[FACE_FB_BYTES];
```

Then in `face_task`:

```c
const uint8_t *fb;
if (provision_is_active()) {
    setup_screen_t s;
    provision_screen(&s);
    setup_screen_render(s_setup_fb, &s);
    fb = s_setup_fb;
} else {
    face_set_reset_progress(&s_face, s_face_reset_pct, t);
    face_tick(&s_face, t);
    fb = face_framebuffer(&s_face);
}
const esp_err_t ferr = ssd1306_flush(&s_panel, fb);
```

One task writes, one task flushes, no lock. `ssd1306_flush` diffs against its
own shadow, so alternating between two buffers costs nothing beyond the pages
that actually changed — which is the whole frame on the transition and very
little after it.

- [ ] **Step 5: Wire the top-level flow**

In `app_main`, after the panel probe:

```c
device_config_t cfg;
config_load(&cfg);

if (pl_decide(config_is_provisioned(&cfg), false, false) == PL_MODE_PROVISION) {
    provision_start();
    while (provision_is_active() && !provision_idle_expired()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    provision_stop();
    config_load(&cfg);   // pick up whatever was just saved
}

wifi_start(cfg.ssid, cfg.pass);
ws_start(cfg.uri);
```

`wifi_start()` and `ws_start()` take their values as arguments now rather than
reading the `WIFI_SSID` and `SERVER_URI` macros. **`ws_start()` reading the
macro is the one line that would make the whole server-URI half of this feature
configure a value nothing reads.**

- [ ] **Step 6: Measure the silence threshold**

Flash, hold the button in a quiet room, and log `s_audio_level` once a second
for thirty seconds. Record the range. Repeat while speaking normally at
conversational distance. Put the threshold between them, nearer the quiet
figure, and commit that number with both ranges in the message.

If the two ranges overlap, **stop and say so**: the silence gate does not work
in that room and the spec's premise is wrong. Do not pick a number that splits
the difference.

- [ ] **Step 7: Bench check, in this order**

1. **Does the access point come up and stay up on the intended supply?** The
   radio cannot sleep while beaconing (`wifi.rst:1773`, `:1795`), and this board
   has already failed twice in that condition — a 1 KB send taking 32 seconds
   and then failing to associate at all. If the AP will not hold, that is a
   supply problem and no code will fix it. **Everything below is moot if this
   fails.**
2. Join from a phone, load the page, pick a network, submit. Watch the panel.
3. Check whether the phone survived the channel switch. It changes nothing —
   the panel is authoritative — but it is worth recording which phones get the
   nice path.
4. Reboot; the device joins the saved network with no prompting.
5. Hold the button silently; the countdown appears and completes.
6. Hold the button and talk; the countdown never completes.
7. Enter provisioning and walk away; it returns to the saved network after
   five minutes.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/voice_main.c
git commit -m "$(cat <<'EOF'
Wire provisioning into the boot, and make the wait interruptible

Three changes that had to happen together. button_task moved ahead of
wifi_start(), which blocks forever - so until now nothing sampled the
button in exactly the situation that needs it, a device stuck connecting.
wifi_start() waits on two bits instead of one, with no timeout still: the
device waits for its network as long as it always has and merely stops
being unreachable while it does. And ws_start() takes the URI as an
argument rather than reading the macro, without which the whole
server-URI half of this configures a value nothing reads.

The hold arms in ST_LISTENING and when the socket is down, never during a
reply, where it would compete with interrupting - the commoner intent by
a wide margin. A hold in ST_LISTENING is also an utterance in flight, so
entering provisioning cancels it the way the interrupt path already does.

face_task stays the only writer to the framebuffer and picks which to
draw, which is why setup_screen.c never touches the panel and no lock is
needed.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Record it in the handoff

**Files:**
- Modify: `RESUME.md`

- [ ] **Step 1: Add what was measured**

The silence threshold with both ranges, the heap the access point and httpd
actually cost, and whether the AP held up on the supply. Real figures from Task
7 Step 6 and Step 7 — not the plan's placeholders.

- [ ] **Step 2: Replace the LAN/cloud item under "Agreed next, in order"**

Item 1 is a build-time `-DSERVER=lan` / `-DSERVER=cloud` switch. It is retired:
the URI is settable from the phone without reflashing. Say that rather than
deleting the item, so the next reader knows it was answered and not forgotten.

- [ ] **Step 3: Add to "Traps already paid for"**

The one worth carrying forward: an access point cannot use modem sleep, so
provisioning holds the radio awake — the state that produced a 32-second 1 KB
send the two times `WIFI_PS_NONE` was tried. It is survivable during
provisioning because the amplifier is silent and nothing is streaming, and that
distinction is the whole reason it works.

- [ ] **Step 4: Update the test count and commit**

Run `cd firmware/host && make test` for the real number.

```bash
git add RESUME.md
git commit -m "$(cat <<'EOF'
Record what provisioning cost and what it retired

The handoff carried a build-time LAN/cloud switch as the next thing to
do. It is answered rather than dropped: the server URI is settable from a
phone, so there is nothing to switch at build time.

Also recorded: the silence threshold with the two ranges it came from,
what the access point and httpd actually cost in heap, and the trap worth
carrying - an access point cannot use modem sleep, so provisioning holds
the radio in the state that broke this board twice. It survives because
the amplifier is silent and nothing is streaming, and that distinction is
the whole reason it works.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

**Spec coverage**

| Spec requirement | Task |
|---|---|
| Open AP, no WPA2 | 4 |
| Server URI locked behind a 4-digit code, 5 attempts | 1 (logic + tests), 4 (generation), 5 (enforcement) |
| Code random per session, not MAC-derived | 4 Step 2 |
| `WIFI_MODE_APSTA`, station torn down before the socket | 4, 7 |
| Scan requires station mode; refused during a trial | 5 Step 2 |
| Credentials trialled before saving; nothing erased on entry | 5 Step 3 |
| Disconnect reason separates bad password / not found | 5 Step 1 |
| The event handler must not retry during a trial | 5 Step 1 |
| Panel authoritative, page best-effort, `GRACE_MS` | 5 Step 3 |
| `AP_IDLE_MS` measured from the last HTTP request | 4 Step 2 |
| One writer to the framebuffer | 7 Step 4 |
| `face_task` learns the mode from a volatile | 7 Step 4 |
| `ws_start()` from `config_store`, not the macro | 7 Step 5 |
| httpd sockets configured, not defaulted | 4 Step 2 |
| `button_task` before `wifi_start()` | 7 Step 1 |
| `wifi_start()` interruptible, still no timeout | 7 Step 2 |
| Countdown armed only in `ST_LISTENING` / socket down | 7 Step 3 |
| In-flight utterance cancelled on entry | 7 Step 3 |
| Silence threshold measured, with the overlap escape | 7 Step 6 |
| AP-on-supply checked first | 7 Step 7 |
| `secrets.h` as fallback; template emptied | 3 |
| Works with no panel | 4 Step 2 (states logged), 7 Step 4 |
| Hidden networks out of scope | not implemented, by design |
| 5 GHz note on the page | 4 Step 2 |
| `DEVICE_TOKEN` never in HTML | 3 Step 3 |

**Type consistency.** `pl_*` names are defined in Task 1 and used unchanged in
4, 5 and 7. `setup_screen_t` and `ss_status_t` are defined in Task 2 and used in
4 and 7. `device_config_t` is defined in Task 3 and used in 7.
`face_set_reset_progress(face_t *, uint8_t, uint32_t)` is defined in Task 6 and
called in Task 7 Step 4 with that signature.

One type error was caught by this review and is worth naming, because it is the
kind that survives into code: the first draft of Tasks 6 and 7 called a
`face_render(face_t *, uint8_t *)` that **does not exist**. `face_tick()`
renders into `f->fb` and `face_framebuffer()` hands it out; `face_render_pose()`
is a different function for static poses. Both tasks now use the real API, and
Task 7 gives the setup screen a buffer of its own rather than borrowing the
face's private one.

**Known softness, stated rather than hidden.** Tasks 4 and 5 describe the
ESP-IDF sequence and the exact API calls but do not transcribe every line of
`provision.c`, because the HTML string and the DNS responder are long and
mechanical, and neither is unit-testable. The decisions are all in Task 1 where
they are tested; what remains in `provision.c` is sequencing. Task 2's font
table is likewise specified by format and pinned by tests rather than
transcribed — 475 hand-typed bytes would contain errors that the tests are
written to catch.
