#include "ota_logic.h"

#include <string.h>

// ---------------------------------------------------------- the image

ol_image_verdict_t ol_check_image(const unsigned char *head, size_t len) {
    if (head == NULL || len < OL_HEADER_MIN) return OL_IMAGE_TOO_SHORT;

    if (head[OL_OFF_IMAGE_MAGIC] != 0xE9) return OL_IMAGE_NOT_ESP;

    // Both bytes of chip_id, not just the low one: a part whose id needs the
    // high byte must not pass by having the right remainder.
    const uint16_t chip = (uint16_t)(head[OL_OFF_CHIP_ID] |
                                     ((uint16_t)head[OL_OFF_CHIP_ID + 1] << 8));
    if (chip != OL_CHIP_ID_ESP32C3) return OL_IMAGE_WRONG_CHIP;

    const uint32_t desc = (uint32_t)head[OL_OFF_APP_DESC] |
                          ((uint32_t)head[OL_OFF_APP_DESC + 1] << 8) |
                          ((uint32_t)head[OL_OFF_APP_DESC + 2] << 16) |
                          ((uint32_t)head[OL_OFF_APP_DESC + 3] << 24);
    if (desc != 0xABCD5432u) return OL_IMAGE_NO_APP_DESC;

    // memcmp over the terminator, so a name that merely starts with ours -
    // "voice_capture2" is a different project - does not pass.
    if (memcmp(head + OL_OFF_PROJECT, OL_PROJECT_NAME, sizeof(OL_PROJECT_NAME)) != 0) {
        return OL_IMAGE_WRONG_PROJECT;
    }

    return OL_IMAGE_OK;
}

const char *ol_verdict_text(ol_image_verdict_t v) {
    switch (v) {
        case OL_IMAGE_OK: return "ok";
        case OL_IMAGE_TOO_SHORT: return "too short to be firmware";
        case OL_IMAGE_NOT_ESP: return "not an ESP firmware image";
        case OL_IMAGE_WRONG_CHIP: return "built for another chip";
        case OL_IMAGE_NO_APP_DESC: return "no application descriptor";
        case OL_IMAGE_WRONG_PROJECT: return "firmware for another project";
    }
    return "unknown";
}

bool ol_image_version(const unsigned char *head, size_t len, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return false;
    out[0] = '\0';
    if (head == NULL || len < OL_HEADER_MIN) return false;

    const char *field = (const char *)head + OL_OFF_VERSION;
    size_t n = 0;
    while (n < OL_DESC_FIELD && field[n] != '\0') n++;
    // esp_app_desc_t declares version[32] and the string in it is terminated.
    // Thirty-two non-zero bytes is a malformed field, not a long version.
    if (n == OL_DESC_FIELD) return false;

    const size_t room = out_size - 1;
    const size_t take = n < room ? n : room;
    memcpy(out, field, take);
    out[take] = '\0';
    return true;
}

// ---------------------------------------------------------- the frames

// Points just past the colon that follows "name", or NULL. Both quotes are
// part of the search, which is what stops "size" from matching inside
// "mysize". Never reads past end.
static const char *value_of(const char *body, size_t len, const char *name) {
    if (body == NULL || name == NULL) return NULL;

    const size_t name_len = strlen(name);
    if (name_len == 0 || name_len + 2 > len) return NULL;

    const char *const end = body + len;
    for (const char *p = body; (size_t)(end - p) >= name_len + 2; p++) {
        if (p[0] != '"') continue;
        if (memcmp(p + 1, name, name_len) != 0) continue;
        if (p[1 + name_len] != '"') continue;

        const char *q = p + 2 + name_len;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != ':') continue;  // "name" used as a value, not a key
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        return q;
    }
    return NULL;
}

// True when the quoted string starting at v is exactly `want`.
static bool quoted_equals(const char *v, const char *end, const char *want) {
    if (v >= end || *v != '"') return false;
    v++;
    const size_t want_len = strlen(want);
    if ((size_t)(end - v) < want_len + 1) return false;
    if (memcmp(v, want, want_len) != 0) return false;
    return v[want_len] == '"';  // exact, so "ota_beginning" does not match
}

ol_frame_t ol_frame_type(const char *body, size_t len) {
    const char *v = value_of(body, len, "type");
    if (v == NULL) return OL_FRAME_OTHER;
    const char *const end = body + len;

    if (quoted_equals(v, end, "ota_begin")) return OL_FRAME_BEGIN;
    if (quoted_equals(v, end, "ota_end")) return OL_FRAME_END;
    if (quoted_equals(v, end, "ota_abort")) return OL_FRAME_ABORT;
    return OL_FRAME_OTHER;
}

bool ol_field_u32(const char *body, size_t len, const char *name, uint32_t *out) {
    if (out == NULL) return false;

    const char *v = value_of(body, len, name);
    if (v == NULL) return false;
    const char *const end = body + len;

    uint32_t acc = 0;
    size_t digits = 0;
    while (v < end && *v >= '0' && *v <= '9') {
        const uint32_t d = (uint32_t)(*v - '0');
        // Checked before multiplying rather than after, because after is
        // already wrong.
        if (acc > (UINT32_MAX - d) / 10u) return false;
        acc = acc * 10u + d;
        digits++;
        v++;
    }
    if (digits == 0) return false;  // a string, null, or nothing at all

    *out = acc;
    return true;
}

// ---------------------------------------------------- the transfer itself

void ol_xfer_reset(ol_xfer_t *x) {
    x->state = OL_XFER_IDLE;
    x->expected = 0;
    x->received = 0;
}

ol_step_t ol_xfer_begin(ol_xfer_t *x, uint32_t size, uint32_t capacity) {
    // Checked before anything is cleared: a stray second begin must leave a
    // running transfer exactly as it was.
    if (x->state == OL_XFER_ACTIVE) return OL_STEP_BUSY;

    if (size == 0) {
        ol_xfer_reset(x);
        return OL_STEP_EMPTY;
    }
    if (size > capacity) {
        ol_xfer_reset(x);
        return OL_STEP_TOO_BIG;
    }

    x->state = OL_XFER_ACTIVE;
    x->expected = size;
    x->received = 0;
    return OL_STEP_OK;
}

ol_step_t ol_xfer_write(ol_xfer_t *x, uint32_t n) {
    if (x->state != OL_XFER_ACTIVE) return OL_STEP_NOT_ACTIVE;

    // Subtraction, not addition: received + n can wrap and would then look
    // like a perfectly ordinary write.
    if (n > x->expected - x->received) {
        ol_xfer_reset(x);
        return OL_STEP_OVERRUN;
    }

    x->received += n;
    return OL_STEP_OK;
}

ol_step_t ol_xfer_end(ol_xfer_t *x) {
    if (x->state != OL_XFER_ACTIVE) return OL_STEP_NOT_ACTIVE;

    if (x->received != x->expected) {
        ol_xfer_reset(x);
        return OL_STEP_SHORT;
    }

    x->state = OL_XFER_DONE;
    return OL_STEP_OK;
}

const char *ol_step_text(ol_step_t s) {
    switch (s) {
        case OL_STEP_OK: return "ok";
        case OL_STEP_BUSY: return "an update is already running";
        case OL_STEP_NOT_ACTIVE: return "no update is running";
        case OL_STEP_TOO_BIG: return "larger than the partition";
        case OL_STEP_EMPTY: return "empty image";
        case OL_STEP_OVERRUN: return "more bytes than promised";
        case OL_STEP_SHORT: return "fewer bytes than promised";
    }
    return "unknown";
}
