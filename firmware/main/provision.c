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
    "<fieldset><legend>Network</legend>\n"
    "<p>No networks found yet.</p>\n"
    "<p>Only 2.4&nbsp;GHz networks appear here - this device has no "
    "5&nbsp;GHz radio.</p>\n"
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

// ------------------------------------------------------------- HTTP handlers
//
// Every handler's first statement is the same, and it is not decorative:
// provision_idle_expired() reads s_last_request, and this is the only place
// it is written. Miss it in one handler and that handler's traffic becomes
// invisible to the idle timer.

static esp_err_t index_get_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    device_config_t cfg;
    config_load(&cfg);

    char esc_uri[PL_URI_MAX * 6 + 1];
    html_escape(cfg.uri, esc_uri, sizeof(esc_uri));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, PAGE_HEAD, (ssize_t)(sizeof(PAGE_HEAD) - 1));
    httpd_resp_send_chunk(req, esc_uri, (ssize_t)strlen(esc_uri));
    httpd_resp_send_chunk(req, PAGE_TAIL, (ssize_t)(sizeof(PAGE_TAIL) - 1));
    httpd_resp_send_chunk(req, NULL, 0);  // ends the chunked response
    return ESP_OK;
}

// Parses the submitted form and says so. Trialling these credentials needs
// the station interface this task raised but must not use - that is Task
// 5's job - and persisting anything ahead of a successful trial would be
// the one mistake config_store.h was written to rule out. So this reads the
// body all the way out (a client waiting for a response must get one) and
// answers honestly: not yet.
static esp_err_t save_post_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    // Sized for what the four fields could add up to, including their
    // "name=" prefixes and the '&' separators between them - generous, not
    // exact, because percent-encoding can make a short value take more
    // bytes on the wire than it will after pl_field() decodes it.
    char body[PL_SSID_MAX + PL_PASS_MAX + PL_URI_MAX + PL_CODE_LEN + 64];

    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too large");
        return ESP_FAIL;
    }

    size_t got = 0;
    const size_t want = req->content_len;
    while (got < want) {
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
    pl_field(body, got, "uri", uri, sizeof(uri));
    pl_field(body, got, "code", code, sizeof(code));

    ESP_LOGI(TAG, "save: ssid \"%s\" received, not implemented yet", ssid);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "not implemented yet");
    return ESP_OK;
}

// Polled by the page while a future step (Task 6) waits on a trial that
// this task cannot start. For now it can only ever report what
// provision_start() put in s_screen: SS_STATUS_WAITING.
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

// A placeholder, on purpose: esp_wifi_scan_start() needs the station
// interface this task raised, and running an actual scan from it is Task
// 5's job. An empty list is the honest answer to "what have you seen" when
// nothing has looked yet.
static esp_err_t scan_get_handler(httpd_req_t *req) {
    s_last_request = xTaskGetTickCount();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"networks\":[]}");
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

static void register_or_warn(httpd_handle_t s, const httpd_uri_t *u) {
    const esp_err_t err = httpd_register_uri_handler(s, u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not register %s: %s", u->uri, esp_err_to_name(err));
    }
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

    // 2. Name the AP from the station MAC.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_name[16];
    snprintf(ap_name, sizeof(ap_name), "Voice-%02x%02x", mac[4], mac[5]);

    // 3. A fresh code every session, from esp_random() rather than the MAC
    // just used above - that value is public, broadcast in every beacon
    // this AP is about to send.
    char code[PL_CODE_LEN + 1];
    snprintf(code, sizeof(code), "%04u", (unsigned)(esp_random() % 10000u));
    pl_unlock_init(&s_unlock, code);

    // 4. Created once; provision_start() can be called again within the
    // same boot (idle timeout, then a hold gesture re-enters it) and the
    // netif does not need recreating each time.
    static bool s_ap_netif_ready = false;
    if (!s_ap_netif_ready) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_ready = true;
    }

    // 5. Open. WPA2 would mean reading a passphrase off a 0.96" panel and
    // typing it on a phone, and would make a device with no panel
    // unprovisionable. The lock is the code above, on the one field that
    // warrants it.
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, ap_name, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    // 6. APSTA throughout, and esp_wifi_connect() is deliberately not
    // called: the station interface exists here for Task 5's scan and
    // trial, not to join whatever was last configured at the exact moment
    // someone is trying to change it.
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }

    // 7. One phone needs nothing like the default seven sockets.
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
        return err;
    }

    // 8. Registered in this order on purpose: matching stops at the first
    // hit, so the specific handlers must be added before the wildcard.
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
    register_or_warn(s_httpd, &index_uri);
    register_or_warn(s_httpd, &save_uri);
    register_or_warn(s_httpd, &status_uri);
    register_or_warn(s_httpd, &scan_uri);
    register_or_warn(s_httpd, &redirect_uri);

    // 9. A phone's own captive-portal probe has somewhere to go before
    // anyone taps a notification. Not fatal if it fails to start - the
    // panel and the console carry the address either way.
    if (xTaskCreate(dns_task, "prov_dns", 3072, NULL, 3, &s_dns_task) != pdPASS) {
        ESP_LOGW(TAG, "dns task failed to start; captive-portal probes will 404");
        s_dns_task = NULL;
    }

    // 10. Tell the screen, and the log, because a device with no panel
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

    // 11.
    s_last_request = xTaskGetTickCount();
    s_active = true;
    return ESP_OK;
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

bool provision_idle_expired(void) {
    if (!s_active) return false;
    const uint32_t idle_ms = (uint32_t)(xTaskGetTickCount() - s_last_request) *
                              portTICK_PERIOD_MS;
    return idle_ms > PROV_AP_IDLE_MS;
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
