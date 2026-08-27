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
