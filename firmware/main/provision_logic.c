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
    // at PL_CODE_LEN + 1 so a caller passing something enormous costs
    // nothing. The walk stops the moment it sees the terminator, so it only
    // ever reads a byte once every byte before it is known - from having
    // been read as non-NUL - to exist; unlike a plain index into entered, it
    // can never land past the allocation a short entered points at.
    size_t len = 0;
    while (len <= PL_CODE_LEN && entered[len] != '\0') len++;

    // Compare the whole code every time rather than returning early, so the
    // time taken says nothing about how many digits were right. A code of
    // the wrong length is folded into diff via len instead of being checked
    // by reading past where entered was just shown to end.
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
