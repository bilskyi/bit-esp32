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
// A trial connected, and when. Together they are what lets provisioning end on
// success rather than only on the idle timeout.
static volatile bool s_succeeded = false;
static volatile TickType_t s_succeeded_at = 0;
static volatile TickType_t s_last_request = 0;
static pl_unlock_t s_unlock;
static setup_screen_t s_screen;
static SemaphoreHandle_t s_screen_lock = NULL;

// The DNS task's own socket, closed by provision_stop() before the task is
// deleted. A task deleted without this leaks the fd at the lwip layer - the
// RTOS reclaims the task's stack, not lwip's ten-socket table.
static int s_dns_sock = -1;

// ---------------------------------------------------------------- the page
//
// One static string, in two pieces either side of the one thing on this
// page that comes from outside this file: the current server URI. Splitting
// it here, rather than building the whole page into one buffer, means there
// is no buffer sized "big enough" to get wrong - each piece is sent as its
// own chunk and the escaped value is bounded by its own destination size.
//
// maxlength on each field is pulled from provision_logic.h's bounds via the
// preprocessor rather than typed in twice, so the page cannot quietly drift
// from the buffers behind pl_field().

#define STR2(x) #x
#define STR(x) STR2(x)

static const char PAGE_HEAD[] =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>Voice setup</title>\n"
    "<style>\n"
    "body{font-family:sans-serif;max-width:26em;margin:2em auto;padding:0 1em}\n"
    "fieldset{margin-bottom:1em}\n"
    "label{display:block;margin:0.6em 0}\n"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box}\n"
    "</style></head><body>\n"
    "<h1>Voice setup</h1>\n"
    "<form method=\"POST\" action=\"/save\">\n"
    "<fieldset><legend>Network</legend>\n";

// Between the two halves the scan results are written directly into the page.
//
// No JavaScript and no second request: the first board test served a page
// whose network list was a static "No networks found yet.", because /scan
// existed as an endpoint and nothing ever called it. Rendering the list where
// the page is built cannot drift out of sync with itself that way, and it
// works in a captive-portal WebView with scripting off.
//
// The cost is that GET / now blocks for the length of an all-channel scan.
// Nothing else is happening at that moment, and a page that takes a moment to
// arrive complete beats one that arrives instantly and empty.
static const char PAGE_AFTER_LIST[] =
    "<p>Only 2.4&nbsp;GHz networks appear here - this device has no "
    "5&nbsp;GHz radio.</p>\n"
    "<p><a href=\"/\">Scan again</a></p>\n"
    "</fieldset>\n"
    "<label>Password<br>\n"
    "<input type=\"password\" name=\"pass\" maxlength=\"" STR(PL_PASS_MAX) "\">\n"
    "</label>\n"
    "<fieldset><legend>Server</legend>\n"
    "<label>Server URI<br>\n"
    "<input type=\"text\" name=\"uri\" maxlength=\"" STR(PL_URI_MAX) "\" disabled value=\"";

static const char PAGE_TAIL[] =
    "\">\n"
    "</label>\n"
    "<label>Code<br>\n"
    "<input type=\"text\" name=\"code\" maxlength=\"" STR(PL_CODE_LEN) "\" "
    "pattern=\"[0-9]{4}\" inputmode=\"numeric\" autocomplete=\"off\">\n"
    "</label>\n"
    "</fieldset>\n"
    "<button type=\"submit\">Save</button>\n"
    "</form>\n"
    "<p>If this page stops responding, the device's own screen has the "
    "answer.</p>\n"
    "</body></html>\n";

// Escapes into dst, stopping cleanly rather than emitting a partial entity
// if dst is too small. The only caller sizes dst at PL_URI_MAX * 6 + 1 -
// six being the longest entity, &quot; - so this never actually truncates
// for anything config_store could have handed it; the bound exists so a
// value from outside that guarantee still cannot overrun the buffer.
static void html_escape(const char *src, char *dst, size_t dst_size) {
    if (dst_size == 0) return;
    size_t w = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        const char c = src[i];
        const char *rep = NULL;
        switch (c) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        if (rep != NULL) {
            const size_t rl = strlen(rep);
            if (w + rl + 1 > dst_size) break;
            memcpy(dst + w, rep, rl);
            w += rl;
        } else {
            if (w + 1 + 1 > dst_size) break;
            dst[w++] = c;
        }
    }
    dst[w] = '\0';
}

// Escapes into dst for embedding in a JSON string, stopping cleanly rather
// than emitting a partial escape if dst is too small. An SSID is just an
// octet string at the 802.11 layer - nothing stops one from carrying a
// quote, a backslash, a raw control byte, or a byte that is not valid UTF-8
// at all - so scan results need the same care html_escape() above gives the
// URI on the page. Bytes at or above 0x7F get the same \u00XX treatment as
// the control bytes below 0x20, rather than passing through raw: a JSON
// string is supposed to be UTF-8, and a lone high byte off the air is not
// guaranteed to be part of a valid sequence. A browser's decoder is
// non-fatal about that - it renders replacement characters, it does not
// throw - so this is display correctness, not a bound; the bound is what
// the two size checks below already give it.
static void json_escape(const char *src, char *dst, size_t dst_size) {
    if (dst_size == 0) return;
    static const char hex[] = "0123456789abcdef";
    size_t w = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (w + 2 + 1 > dst_size) break;
            dst[w++] = '\\';
            dst[w++] = (char)c;
        } else if (c < 0x20 || c >= 0x7F) {
            if (w + 6 + 1 > dst_size) break;
            dst[w++] = '\\'; dst[w++] = 'u'; dst[w++] = '0'; dst[w++] = '0';
            dst[w++] = hex[(c >> 4) & 0xF];
            dst[w++] = hex[c & 0xF];
        } else {
            if (w + 1 + 1 > dst_size) break;
            dst[w++] = (char)c;
        }
    }
    dst[w] = '\0';
}

// ------------------------------------------------------------- HTTP handlers
//
// Every handler's first statement is the same, and it is not decorative:
// provision_idle_expired() reads s_last_request, and this is the only place
// it is written. Miss it in one handler and that handler's traffic becomes
// invisible to the idle timer.

// Defined below, next to the scan it wraps: the page is assembled here but the
// list it contains comes from the same code /scan uses, so the two cannot
// disagree about what is in range.
static void send_network_list(httpd_req_t *req);

static esp_err_t index_get_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    device_config_t cfg;
    config_load(&cfg);

    char esc_uri[PL_URI_MAX * 6 + 1];
    html_escape(cfg.uri, esc_uri, sizeof(esc_uri));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, PAGE_HEAD, (ssize_t)(sizeof(PAGE_HEAD) - 1));
    send_network_list(req);
    httpd_resp_send_chunk(req, PAGE_AFTER_LIST, (ssize_t)(sizeof(PAGE_AFTER_LIST) - 1));
    httpd_resp_send_chunk(req, esc_uri, (ssize_t)strlen(esc_uri));
    httpd_resp_send_chunk(req, PAGE_TAIL, (ssize_t)(sizeof(PAGE_TAIL) - 1));
    httpd_resp_send_chunk(req, NULL, 0);  // ends the chunked response
    return ESP_OK;
}

// Reads the form, then trials the candidate before anything reaches NVS -
// the one mistake config_store.h was written to rule out. The URI is gated
// on the unlock code; the WiFi fields are not, so a wrong code still lets
// ssid/pass take effect. A client waiting on this response must get one
// either way, which is why the body is always read out in full first.
// PL_SSID_MAX/PL_PASS_MAX/PL_URI_MAX/PL_CODE_LEN are *decoded* field
// bounds, but the wire carries application/x-www-form-urlencoded, where one
// decoded byte (anything outside [A-Za-z0-9-._~]) can arrive as three
// (%XX). Sizing the buffer off the decoded bounds would reject a
// legitimate maximum-length password made of special characters with a
// 400 the user cannot diagnose. Field by field, worst case on the wire:
//   "ssid="  ( 5) + 3*PL_SSID_MAX ( 96)   =  101
// + "&pass=" ( 6) + 3*PL_PASS_MAX (192)   =  198
// + "&uri="  ( 5) + 3*PL_URI_MAX  (384)   =  389
// + "&code=" ( 6) + 3*PL_CODE_LEN ( 12)   =   18
// + 1 for the '\0' this handler appends after the read loop
// = 101 + 198 + 389 + 18 + 1 = 707. Do not shrink this back to the decoded
// sum - that arithmetic is for pl_field()'s output buffers, not this one.
#define PROV_BODY_MAX                                                       \
    (5 + 3 * PL_SSID_MAX + 6 + 3 * PL_PASS_MAX + 5 + 3 * PL_URI_MAX + 6 +    \
     3 * PL_CODE_LEN + 1)

// Bounds how long one POST /save can hold the httpd task hostage.
// esp_http_server runs every connection on that single task, so a client
// that stops sending mid-body - a phone walking out of range does this by
// accident, nothing malicious required - would otherwise spin the read
// loop below forever, and "/", "/status" and "/scan" all stall behind it
// until the device is power-cycled. Each httpd_req_recv() call already
// gives up after cfg.recv_wait_timeout (5s, the HTTPD_DEFAULT_CONFIG()
// default, not overridden in this file) and returns HTTPD_SOCK_ERR_TIMEOUT
// rather than blocking indefinitely; the cap here is two of those - one
// retry for a client that is merely slow on a noisy 2.4GHz link, then give
// up - so a wedged client costs at most ~10s, not the "forever" of the
// unbounded loop it replaces.
#define PROV_BODY_RECV_TIMEOUT_MS 10000

static esp_err_t save_post_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    char body[PROV_BODY_MAX];

    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too large");
        return ESP_FAIL;
    }

    size_t got = 0;
    const size_t want = req->content_len;
    const TickType_t recv_start = xTaskGetTickCount();
    while (got < want) {
        // Elapsed-since-start, not an absolute deadline compared with >=:
        // the same wraparound-safe subtraction provision_idle_expired()
        // already uses on s_last_request, for the same reason - tick
        // counts roll over and a direct comparison would not survive it.
        const uint32_t elapsed_ms =
            (uint32_t)(xTaskGetTickCount() - recv_start) * portTICK_PERIOD_MS;
        if (elapsed_ms >= PROV_BODY_RECV_TIMEOUT_MS) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "body took too long");
            // ESP_OK, not ESP_FAIL: this is a client that stopped sending,
            // not a handler fault, and ESP_OK is what tells httpd to close
            // the socket cleanly instead of tearing down the connection as
            // an error.
            return ESP_OK;
        }
        const int r = httpd_req_recv(req, body + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    body[got] = '\0';

    char ssid[PL_SSID_MAX + 1];
    char pass[PL_PASS_MAX + 1];
    char uri[PL_URI_MAX + 1];
    char code[PL_CODE_LEN + 1];
    pl_field(body, got, "ssid", ssid, sizeof(ssid));
    pl_field(body, got, "pass", pass, sizeof(pass));
    const bool have_uri = pl_field(body, got, "uri", uri, sizeof(uri));
    pl_field(body, got, "code", code, sizeof(code));

    // pl_unlock_check() only runs when uri was actually submitted, so a
    // WiFi-only save cannot burn one of pl_unlock_t's PL_UNLOCK_MAX_TRIES
    // attempts just by not mentioning the URI at all. When it is submitted,
    // both checks must pass - a wrong code must leave the URI untouched
    // without stopping ssid/pass from being trialled below.
    const bool uri_ok = have_uri && pl_unlock_check(&s_unlock, code) && pl_uri_valid(uri);

    ESP_LOGI(TAG, "save: trialling \"%s\"%s", ssid, uri_ok ? " (uri unlocked)" : "");

    xSemaphoreTake(s_screen_lock, portMAX_DELAY);
    s_screen.status = SS_STATUS_TRYING;
    xSemaphoreGive(s_screen_lock);

    // From here until provision_set_trial_mode(false) below, wifi_event()
    // in voice_main.c ends a disconnect at the reason code instead of
    // retrying it - see s_trial_in_progress there for why that split has to
    // exist at all.
    provision_set_trial_mode(true);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    esp_err_t werr = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (werr == ESP_OK) werr = esp_wifi_connect();
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "could not start connecting: %s", esp_err_to_name(werr));
    }

    uint8_t reason = 0;
    // A failure above never posts WIFI_EVENT_STA_DISCONNECTED, so waiting on
    // it here would just burn the full PROV_TRIAL_MS before landing on the
    // same answer as a real timeout would.
    const provision_trial_outcome_t outcome =
        (werr == ESP_OK) ? provision_wifi_trial_wait(&reason) : PROV_TRIAL_TIMED_OUT;

    // A timed-out trial means provision_wifi_trial_wait() gave up, not that
    // esp_wifi_connect() did - the attempt it started is still in flight. If
    // it resolves after this point, the resulting WIFI_EVENT_STA_DISCONNECTED
    // or IP_EVENT_STA_GOT_IP must still land on wifi_event()'s trial branch,
    // not the normal one: outside a trial that branch reconnects
    // unconditionally, forever, with the candidate the user was just told
    // had failed, and on a late success it sets WIFI_CONNECTED_BIT with
    // nobody left waiting on it.
    //
    // Calling esp_wifi_disconnect() here, textually before
    // provision_set_trial_mode(false) below, is not enough on its own to
    // guarantee the resulting event lands on the trial branch:
    // esp_wifi_disconnect() only *requests* the disconnect, and the event it
    // produces has to cross WiFi-driver teardown and then dispatch on the
    // default event-loop task - a different task from this one - before
    // wifi_event() ever sees it, while clearing s_trial_in_progress is a
    // same-thread write that finishes essentially instantly. The likely
    // ordering is the wrong one. provision_wifi_trial_cancel() closes that
    // gap by waiting, bounded, for wifi_event()'s trial branch to actually
    // confirm the disconnect before trial mode comes off - see its comment
    // in voice_main.c for the sequence and the timeout. A timeout there
    // does not make this race impossible, only smaller: the remaining
    // window is whatever a few hundred milliseconds does not cover, not the
    // "guaranteed on the common path" this used to be.
    // Also correct, and harmless, on the other two outcomes: a disconnect
    // already delivered its event and left nothing in flight, and a
    // synchronous esp_wifi_set_config()/esp_wifi_connect() failure above
    // never started an attempt at all - provision_wifi_trial_cancel() finds
    // esp_wifi_disconnect() failing on an already-idle station in both
    // cases and returns without waiting.
    if (outcome != PROV_TRIAL_CONNECTED) {
        provision_wifi_trial_cancel();
    }

    provision_set_trial_mode(false);

    ss_status_t status;
    if (outcome == PROV_TRIAL_CONNECTED) {
        // Nothing reaches NVS on any other path out of this function.
        if (config_save_wifi(ssid, pass) != ESP_OK) {
            ESP_LOGW(TAG, "config_save_wifi failed; connected this session only");
        }
        if (uri_ok && config_save_uri(uri) != ESP_OK) {
            ESP_LOGW(TAG, "config_save_uri failed");
        }
        status = SS_STATUS_CONNECTED;
        // Starts the grace window. Until this, provisioning had no way to end
        // except the five-minute idle timeout, so a device that had just been
        // configured successfully sat in its own access point for five more
        // minutes before going to work. Found on the first board test.
        s_succeeded_at = xTaskGetTickCount();
        s_succeeded = true;
    } else if (outcome == PROV_TRIAL_DISCONNECTED) {
        // NO_AP_FOUND and everything else are different next actions for
        // whoever is holding the phone - "check the network name" versus
        // "check the password" - which is most of the point of trialling.
        status = (reason == WIFI_REASON_NO_AP_FOUND) ? SS_STATUS_NOT_FOUND : SS_STATUS_BAD_PASSWORD;
    } else {
        status = SS_STATUS_TIMED_OUT;
    }

    xSemaphoreTake(s_screen_lock, portMAX_DELAY);
    s_screen.status = status;
    xSemaphoreGive(s_screen_lock);

    // Not torn down here even on success. esp_wifi_connect() just forced the
    // AP onto the home network's channel - AP and STA share one radio
    // (wifi.rst:1660) - which can drop whatever phone is mid-request right
    // now, so this response may never arrive. PROV_GRACE_MS is the window
    // for a phone that reassociates afterwards to poll GET /status and get
    // the answer instead; tearing the AP down the instant this function
    // returns would close that window before it opens. Nothing in this file
    // calls provision_stop() on a timer, so that holds by construction -
    // whatever eventually does drive it after a successful save must still
    // wait out PROV_GRACE_MS first.
    char out[64];
    snprintf(out, sizeof(out), "{\"status\":\"%s\"}", ss_status_text(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// Polled by the page for a live status: whatever save_post_handler last put
// in s_screen, from SS_STATUS_WAITING before any submission through
// SS_STATUS_TRYING and on to the outcome. May never be reached mid-trial -
// see the note on the AP hold in save_post_handler - which is why this
// exists as a second way in rather than the only one.
static esp_err_t status_get_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    setup_screen_t snap;
    provision_screen(&snap);

    // ss_status_text() returns one of a fixed set of lowercase words with no
    // quote or backslash in any of them, so it is safe to embed directly -
    // unlike the URI on the page above, nothing here came from a form.
    char out[64];
    snprintf(out, sizeof(out), "{\"status\":\"%s\"}", ss_status_text(snap.status));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// Capped well under what a crowded building could return, so the reply and
// the static record buffer below both stay small and fixed-size.
#define PROV_SCAN_MAX 20

// GET /scan: a blocking, all-channel active scan, returned strongest signal
// first. esp_wifi_scan_start() needs the station interface provision_start()
// raised - "supported only in station or station/AP mode"
// (esp-idf/docs/en/api-guides/wifi.rst:504) - and, blocking, ties up the one
// httpd task for the scan's duration. That is fine: there is nothing else
// this device should be doing with that task while someone is provisioning.
// What a scan produced, so both the page and /scan can say the same thing.
typedef enum {
    SCAN_OK = 0,
    SCAN_BUSY,   // a trial is running; esp_wifi_scan_start says ESP_ERR_WIFI_STATE
    SCAN_FAILED,
} scan_result_t;

// Runs a blocking all-channel scan into `out`, strongest first, and reports
// how many landed there. Owns the driver's record list on every path: it is
// dynamically allocated and this device has 55 KB of heap during a
// conversation, so a leak here is not survivable.
//
// static storage in the caller: ~20 wifi_ap_record_t would be a few KB on the
// httpd task's stack, and these handlers run one at a time on that one task.
static scan_result_t scan_networks(wifi_ap_record_t *out, uint16_t *count) {
    *count = 0;

    wifi_scan_config_t sc = {0};  // NULL SSID, all channels, active
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_ERR_WIFI_STATE) return SCAN_BUSY;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(err));
        return SCAN_FAILED;
    }

    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_num: %s", esp_err_to_name(err));
        // The scan completed and its results are queued in the driver; nothing
        // has claimed them, so this path must free them itself. The BUSY path
        // above must not - there, the scan never started.
        esp_wifi_clear_ap_list();
        return SCAN_FAILED;
    }

    uint16_t n = (found > PROV_SCAN_MAX) ? PROV_SCAN_MAX : found;
    err = esp_wifi_scan_get_ap_records(&n, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records: %s", esp_err_to_name(err));
        // Frees the whole list itself on success; on failure it may not have.
        esp_wifi_clear_ap_list();
        return SCAN_FAILED;
    }

    // Strongest first. n is capped at PROV_SCAN_MAX, so a plain insertion sort
    // costs nothing worth measuring.
    for (uint16_t i = 1; i < n; i++) {
        const wifi_ap_record_t key = out[i];
        uint16_t j = i;
        while (j > 0 && out[j - 1].rssi < key.rssi) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }

    *count = n;
    return SCAN_OK;
}

// The radio buttons, sent as chunks so no single buffer has to hold an escaped
// SSID twice over. html_escape can turn one byte into six.
static void send_network_list(httpd_req_t *req) {
    static wifi_ap_record_t recs[PROV_SCAN_MAX];
    uint16_t n = 0;

    switch (scan_networks(recs, &n)) {
        case SCAN_BUSY:
            httpd_resp_sendstr_chunk(req, "<p>Busy trying the last network. Reload in a moment.</p>\n");
            return;
        case SCAN_FAILED:
            httpd_resp_sendstr_chunk(req, "<p>The scan failed. Reload to try again.</p>\n");
            return;
        case SCAN_OK:
            break;
    }

    if (n == 0) {
        httpd_resp_sendstr_chunk(req, "<p>No networks in range.</p>\n");
        return;
    }

    char esc[PL_SSID_MAX * 6 + 1];
    char rssi[16];
    for (uint16_t i = 0; i < n; i++) {
        // ssid[33] is null-terminated by the driver even at the full 32 bytes.
        html_escape((const char *)recs[i].ssid, esc, sizeof(esc));
        httpd_resp_sendstr_chunk(req, "<label><input type=\"radio\" name=\"ssid\" value=\"");
        httpd_resp_sendstr_chunk(req, esc);
        httpd_resp_sendstr_chunk(req, "\" required> ");
        httpd_resp_sendstr_chunk(req, esc);
        const int len = snprintf(rssi, sizeof(rssi), " (%d dBm)", (int)recs[i].rssi);
        httpd_resp_send_chunk(req, rssi, len);
        httpd_resp_sendstr_chunk(req, "</label>\n");
    }
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    wifi_scan_config_t sc = {0};  // NULL SSID, all channels, active
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_ERR_WIFI_STATE) {
        // "wifi still connecting when invoke esp_wifi_scan_start"
        // (esp_wifi.h:514). A trial is running; say so rather than failing
        // silently.
        //
        // Not httpd_resp_send_err(req, HTTPD_409_CONFLICT, ...): this
        // esp-idf (v5.3.2) has no 409 in httpd_err_code_t, only 400-405,
        // 408, 411, 414, 431 and 500/501/505 - none of which mean "come
        // back later". httpd_resp_send_custom_err() is the same header's
        // way to put an arbitrary status on the wire, the same way
        // redirect_get_handler() below hand-writes its own 302.
        return httpd_resp_send_custom_err(req, "409 Conflict", "trial in progress");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_num: %s", esp_err_to_name(err));
        // The scan completed and its results are still queued in the
        // driver; nothing has claimed them yet, so this path - unlike the
        // ESP_ERR_WIFI_STATE one above, where the scan never started - must
        // free them itself.
        esp_wifi_clear_ap_list();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    uint16_t n = (found > PROV_SCAN_MAX) ? PROV_SCAN_MAX : found;
    // static: ~20 wifi_ap_record_t would be a few KB on the httpd task's
    // stack, and index_get_handler's chunked-send pattern below already
    // establishes that these handlers run one at a time on that one task.
    static wifi_ap_record_t recs[PROV_SCAN_MAX];
    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records: %s", esp_err_to_name(err));
        // This call frees the whole list itself on success; on failure it
        // may not have, so the same cleanup as above applies.
        esp_wifi_clear_ap_list();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    // Strongest first. n is capped at PROV_SCAN_MAX, so a plain insertion
    // sort costs nothing worth measuring.
    for (uint16_t i = 1; i < n; i++) {
        const wifi_ap_record_t key = recs[i];
        uint16_t j = i;
        while (j > 0 && recs[j - 1].rssi < key.rssi) {
            recs[j] = recs[j - 1];
            j--;
        }
        recs[j] = key;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"networks\":[", 13);
    // Worst case, one entry: leading "," (1) + {"ssid":" (9) + an escaped
    // SSID where every one of PL_SSID_MAX bytes became a \u00XX control
    // escape (PL_SSID_MAX * 6) + ","rssi": (9) + "-128" (4) + ,"open": (8) +
    // "false" (5) + } (1) + '\0' (1). An SSID is attacker-controlled - it
    // arrives over the air from whatever is broadcasting nearby, no
    // association required - so this has to hold the worst case exactly,
    // not the common one: snprintf() below reports how long the full string
    // would have been even when it truncates, and httpd_resp_send_chunk()
    // would then read that many bytes out of entry regardless of how much
    // of it snprintf() actually wrote.
    char entry[1 + 9 + PL_SSID_MAX * 6 + 9 + 4 + 8 + 5 + 1 + 1];
    char esc_ssid[PL_SSID_MAX * 6 + 1];
    for (uint16_t i = 0; i < n; i++) {
        // ssid[33] is null-terminated by the driver even at the full 32
        // bytes - the 33rd byte exists for exactly that.
        json_escape((const char *)recs[i].ssid, esc_ssid, sizeof(esc_ssid));
        const int len = snprintf(entry, sizeof(entry),
                                  "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                                  i == 0 ? "" : ",", esc_ssid, (int)recs[i].rssi,
                                  recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_send_chunk(req, entry, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);  // ends the chunked response
    return ESP_OK;
}

// Every other path - a phone's captive-portal probe among them - lands back
// on the page. Nothing is relied on to launch that probe automatically; the
// panel and the console already say where to go.
static esp_err_t redirect_get_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ------------------------------------------------------------------ the DNS
//
// One task, one UDP socket, one job: whatever name was asked for, answer
// with 192.168.4.1. This is not relied on - the panel and the console carry
// the address regardless of whether a phone's captive-portal detection ever
// fires - so it is kept to the minimum that makes that detection possible.

#define DNS_PORT 53
#define DNS_BUF_LEN 512  // a captive-portal probe; anything bigger is not one
// header (12) + the question copied verbatim (bounded by DNS_BUF_LEN) + one
// answer record (2 name pointer + 2 type + 2 class + 4 ttl + 2 rdlength + 4
// rdata = 16 bytes).
#define DNS_RESP_LEN (DNS_BUF_LEN + 16)

static void dns_task(void *arg) {
    (void)arg;

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGW(TAG, "dns: socket() failed");
        // Clear the handle before deleting self: provision_stop() checks
        // s_dns_task != NULL and, if this were left set, would later hand
        // vTaskDelete() a handle to a task that no longer exists.
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DNS_PORT);
    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "dns: bind() on :%d failed", DNS_PORT);
        close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task = NULL;  // see the comment above
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "dns stub up on :%d", DNS_PORT);

    static uint8_t req[DNS_BUF_LEN];
    static uint8_t resp[DNS_RESP_LEN];

    while (true) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        const int len = recvfrom(s_dns_sock, req, sizeof(req), 0,
                                  (struct sockaddr *)&src, &src_len);
        if (len <= 0) {
            // Also how provision_stop() gets this task's attention: closing
            // s_dns_sock from another task unblocks this recvfrom with an
            // error. Either way there is nothing to parse.
            continue;
        }

        // A query is at least a 12-byte header plus one question. Walking
        // the question below is bounded by len at every step, so a
        // truncated or hostile packet can move pos but never past
        // req[len-1] - there is nothing past DNS_BUF_LEN to protect
        // against, because len can never exceed sizeof(req) in the first
        // place, but a malformed length byte inside the packet must not be
        // allowed to walk pos beyond what was actually received either.
        if ((size_t)len < 12) continue;

        size_t pos = 12;
        bool ok = true;
        while (true) {
            if (pos >= (size_t)len) { ok = false; break; }
            const uint8_t label_len = req[pos];
            if (label_len == 0) { pos++; break; }
            if ((label_len & 0xC0) != 0) { ok = false; break; }  // no
            // compression pointers in a question; a real client does not
            // send one, and honouring one here would mean chasing an
            // offset instead of just declining to answer.
            pos += 1u + label_len;
            if (pos >= (size_t)len) { ok = false; break; }
        }
        if (!ok || pos + 4 > (size_t)len) continue;  // QTYPE + QCLASS
        pos += 4;

        const size_t question_len = pos - 12;

        size_t w = 0;
        resp[w++] = req[0]; resp[w++] = req[1];  // ID, copied as asked
        resp[w++] = 0x81; resp[w++] = 0x80;      // QR=1 AA=0 RD=1 RA=1 RCODE=0
        resp[w++] = req[4]; resp[w++] = req[5];  // QDCOUNT, copied (== 1)
        resp[w++] = 0x00; resp[w++] = 0x01;      // ANCOUNT = 1
        resp[w++] = 0x00; resp[w++] = 0x00;      // NSCOUNT = 0
        resp[w++] = 0x00; resp[w++] = 0x00;      // ARCOUNT = 0

        memcpy(resp + w, req + 12, question_len);  // the question, verbatim
        w += question_len;

        resp[w++] = 0xC0; resp[w++] = 0x0C;  // NAME: pointer to offset 12
        resp[w++] = 0x00; resp[w++] = 0x01;  // TYPE = A
        resp[w++] = 0x00; resp[w++] = 0x01;  // CLASS = IN
        resp[w++] = 0x00; resp[w++] = 0x00; resp[w++] = 0x00; resp[w++] = 0x3C;  // TTL 60s
        resp[w++] = 0x00; resp[w++] = 0x04;  // RDLENGTH = 4
        resp[w++] = 192; resp[w++] = 168; resp[w++] = 4; resp[w++] = 1;  // 192.168.4.1

        sendto(s_dns_sock, resp, w, 0, (struct sockaddr *)&src, src_len);
    }
}

// ----------------------------------------------------------------- lifecycle

// For handlers whose absence still leaves the device usable - right now
// just the wildcard redirect, which only helps a phone's captive-portal
// probe find the page a little sooner. GET / and POST /save are not this:
// see register_or_fail() below.
static void register_or_warn(httpd_handle_t s, const httpd_uri_t *u) {
    const esp_err_t err = httpd_register_uri_handler(s, u);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not register %s: %s", u->uri, esp_err_to_name(err));
    }
}

// For handlers the page cannot work without. If "/" or "/save" fail to
// register, the AP is up and broadcasting but there is nothing a phone can
// actually do with it - that is a failed start, not a log line the caller
// never sees, so the return value here feeds provision_start()'s goto
// fail ladder instead of being swallowed the way register_or_warn's is.
static esp_err_t register_or_fail(httpd_handle_t s, const httpd_uri_t *u) {
    const esp_err_t err = httpd_register_uri_handler(s, u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not register %s: %s", u->uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t provision_start(void) {
    // Calling this while already active would overwrite s_httpd and
    // s_dns_task with fresh handles and leak whatever they pointed to -
    // a running server and a bound socket, neither reachable again. A
    // caller re-entering provisioning must see a clean restart.
    if (s_active) provision_stop();

    if (s_screen_lock == NULL) {
        s_screen_lock = xSemaphoreCreateMutex();
        if (s_screen_lock == NULL) return ESP_ERR_NO_MEM;
    }

    // 1. esp_wifi_stop() on a stack that was never started, or already
    // stopped, just reports ESP_ERR_WIFI_NOT_INIT - nothing to stop, not a
    // failure of this call.
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }

    // 2. RAM-backed storage for whatever esp_wifi_set_config() touches from
    // here on, for the rest of this device's boot - there is no call
    // anywhere in this codebase that ever sets it back. Without this,
    // esp_wifi_set_config() defaults to WIFI_STORAGE_FLASH for both station
    // and soft-AP (esp_wifi.h:963,1038), which means the candidate that
    // save_post_handler() below hands to esp_wifi_set_config(WIFI_IF_STA,
    // ...) lands in flash the instant it is set - before esp_wifi_connect()
    // is even called, let alone before a trial decides whether the
    // credential was any good. That candidate comes from a POST to an open
    // access point with no login of any kind, so this is not a theoretical
    // path: it is every /save, including one submitted by a stranger with a
    // garbage password. config_store.c's own writes are already gated on a
    // successful trial; this is the driver's second, independent copy, and
    // it was never gated on anything. Safe to make RAM-only here because
    // wifi_start() in voice_main.c unconditionally calls
    // esp_wifi_set_config() with its own values - currently
    // WIFI_SSID/WIFI_PASSWORD from secrets.h; config_load() exists but
    // nothing in the boot path calls it yet - before esp_wifi_start() on
    // every boot. The driver's persisted copy is therefore never relied on
    // regardless of where those values came from, so the driver's
    // flash-backed copy is pure duplication and removing it costs nothing.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage: %s", esp_err_to_name(err));
        goto fail;
    }

    // 3. Name the AP from the station MAC. esp_read_mac() can fail (the
    // eFuse block it reads is not guaranteed present on every variant);
    // unchecked, mac[] stays whatever was on the stack and that garbage
    // becomes the network name broadcast in every beacon. A fixed fallback
    // is at least a name someone can recognise and connect to.
    uint8_t mac[6];
    char ap_name[16];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(ap_name, sizeof(ap_name), "Voice-%02x%02x", mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "esp_read_mac failed; using fixed AP name");
        snprintf(ap_name, sizeof(ap_name), "Voice-Setup");
    }

    // 4. A fresh code every session, from esp_random() rather than the MAC
    // just used above - that value is public, broadcast in every beacon
    // this AP is about to send.
    char code[PL_CODE_LEN + 1];
    snprintf(code, sizeof(code), "%04u", (unsigned)(esp_random() % 10000u));
    pl_unlock_init(&s_unlock, code);

    // 5. Created once; provision_start() can be called again within the
    // same boot (idle timeout, then a hold gesture re-enters it) and the
    // netif does not need recreating each time.
    static bool s_ap_netif_ready = false;
    if (!s_ap_netif_ready) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_ready = true;
    }

    // 6. Open. WPA2 would mean reading a passphrase off a 0.96" panel and
    // typing it on a phone, and would make a device with no panel
    // unprovisionable. The lock is the code above, on the one field that
    // warrants it.
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, ap_name, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    // 7. APSTA throughout, and esp_wifi_connect() is deliberately not
    // called: the station interface exists here for Task 5's scan and
    // trial, not to join whatever was last configured at the exact moment
    // someone is trying to change it.
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        goto fail;
    }

    // 8. One phone needs nothing like the default seven sockets.
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    // Without this, "/*" matches only the literal two characters after the
    // slash - the four-character string "/*" - and every path a phone
    // actually probes falls through to httpd's default 404 instead of
    // reaching redirect_get_handler.
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        goto fail;
    }

    // 9. Registered in this order on purpose: matching stops at the first
    // hit, so the specific handlers must be added before the wildcard. All
    // five are attempted regardless of earlier failures, so the log shows
    // the full picture in one shot; index/save's results are what decide
    // whether this start succeeds.
    static const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_get_handler};
    static const httpd_uri_t save_uri = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    static const httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler};
    static const httpd_uri_t scan_uri = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler};
    static const httpd_uri_t redirect_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = redirect_get_handler};
    bool required_handlers_ok = true;
    if (register_or_fail(s_httpd, &index_uri) != ESP_OK) required_handlers_ok = false;
    if (register_or_fail(s_httpd, &save_uri) != ESP_OK) required_handlers_ok = false;
    register_or_warn(s_httpd, &status_uri);
    register_or_warn(s_httpd, &scan_uri);
    register_or_warn(s_httpd, &redirect_uri);
    if (!required_handlers_ok) {
        // The AP would be up and the page unreachable behind it - the
        // exact failure this task exists to rule out - so this is a
        // failed start, not a warning the caller never sees.
        err = ESP_FAIL;
        goto fail;
    }

    // 10. A phone's own captive-portal probe has somewhere to go before
    // anyone taps a notification. Not fatal if it fails to start - the
    // panel and the console carry the address either way.
    if (xTaskCreate(dns_task, "prov_dns", 3072, NULL, 3, &s_dns_task) != pdPASS) {
        ESP_LOGW(TAG, "dns task failed to start; captive-portal probes will 404");
        s_dns_task = NULL;
    }

    // 11. Tell the screen, and the log, because a device with no panel
    // attached must still be provisionable by someone watching the console.
    xSemaphoreTake(s_screen_lock, portMAX_DELAY);
    strncpy(s_screen.ap_name, ap_name, sizeof(s_screen.ap_name) - 1);
    s_screen.ap_name[sizeof(s_screen.ap_name) - 1] = '\0';
    strncpy(s_screen.address, "192.168.4.1", sizeof(s_screen.address) - 1);
    s_screen.address[sizeof(s_screen.address) - 1] = '\0';
    strncpy(s_screen.code, code, sizeof(s_screen.code) - 1);
    s_screen.code[sizeof(s_screen.code) - 1] = '\0';
    s_screen.status = SS_STATUS_WAITING;
    xSemaphoreGive(s_screen_lock);

    ESP_LOGI(TAG, "provisioning: join \"%s\" (open), browse to http://192.168.4.1/, code %s",
             ap_name, code);

    // 12.
    s_last_request = xTaskGetTickCount();
    s_active = true;
    return ESP_OK;

fail:
    // Single unwind path for every failure from here on: whichever of the
    // radio and httpd came up before the failure goes back down, so a
    // failed start never leaves an open, unsecured AP broadcasting with
    // nothing - or nothing reachable - behind it. s_active is untouched:
    // it is only ever set true on the success path above, so it is still
    // false here, which matches reality on every one of these paths.
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    // esp_wifi_stop() is safe to call even when esp_wifi_start() never
    // ran or esp_wifi_set_mode() failed outright: per the comment on the
    // esp_wifi_stop() call at the top of this function, its only error is
    // ESP_ERR_WIFI_NOT_INIT, and the wifi driver was already initialised
    // long before provision_start() was ever called.
    {
        const esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(stop_err));
        }
    }
    return err;
}

void provision_stop(void) {
    // First, so nothing that checks provision_is_active() mid-teardown -
    // face_task deciding whether to draw the setup screen, say - sees a
    // session that looks active but is already being pulled down.
    s_active = false;

    if (s_dns_task != NULL) {
        // Close before delete: deleting the task reclaims its stack, not
        // the socket it held open at the lwip layer, and this is the only
        // task that ever touches s_dns_sock.
        if (s_dns_sock >= 0) {
            close(s_dns_sock);
            s_dns_sock = -1;
        }
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }

    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    const esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
}

bool provision_is_active(void) { return s_active; }

bool provision_complete(void) {
    if (!s_active || !s_succeeded) return false;
    const uint32_t since_ms = (uint32_t)(xTaskGetTickCount() - s_succeeded_at) *
                               portTICK_PERIOD_MS;
    // The wait is the point, not an afterthought: connecting the station just
    // forced the access point onto the home network's channel, which can drop
    // the phone mid-request, and this window is what lets one that
    // reassociates poll GET /status and learn it worked.
    return since_ms >= PROV_GRACE_MS;
}

bool provision_idle_expired(void) {
    if (!s_active) return false;
    const uint32_t idle_ms = (uint32_t)(xTaskGetTickCount() - s_last_request) *
                              portTICK_PERIOD_MS;
    return idle_ms > PROV_AP_IDLE_MS;
}

void provision_set_status(ss_status_t status) {
    if (s_screen_lock == NULL) return;
    xSemaphoreTake(s_screen_lock, portMAX_DELAY);
    s_screen.status = status;
    xSemaphoreGive(s_screen_lock);
}

void provision_screen(setup_screen_t *out) {
    if (s_screen_lock != NULL) {
        xSemaphoreTake(s_screen_lock, portMAX_DELAY);
        *out = s_screen;
        xSemaphoreGive(s_screen_lock);
    } else {
        // provision_start() has never run: s_screen is still its all-zero
        // initial value, and there is no writer to race with.
        *out = s_screen;
    }
}
