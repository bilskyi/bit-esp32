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
