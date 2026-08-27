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
