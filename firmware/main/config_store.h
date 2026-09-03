// What the device is configured with, and where it came from.
//
// NVS is the source; secrets.h is the fallback for whatever NVS does not have.
// That ordering is what keeps the bench working unchanged - a board flashed
// with a filled-in secrets.h never has to be provisioned - while letting a
// board with an empty one configure itself from a phone.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "provision_logic.h"  // PL_SSID_MAX, PL_PASS_MAX, PL_URI_MAX

typedef struct {
    char ssid[PL_SSID_MAX + 1];
    char pass[PL_PASS_MAX + 1];
    char uri[PL_URI_MAX + 1];

    // The settings the two buttons change. Step indices, not values: the
    // lookup tables in settings_menu.c can be retuned without migrating what
    // is already written here. Absent keys read as the defaults below, so a
    // device flashed with this firmware behaves exactly like the last one.
    uint8_t volume;  // 0-5, default 5 (unity, today's behaviour)
    uint8_t screen;  // 0-3, default 3 (0xcf, what ssd1306.c writes at init)
    uint8_t eyes;    // 0-3, default 0 (calm)
} device_config_t;

// Reads NVS, filling anything absent from secrets.h. Never fails: a corrupt or
// unreadable NVS is indistinguishable to the caller from an empty one, and the
// compiled values are a working answer in both cases.
void config_load(device_config_t *out);

// Written only after a trial connection succeeds. Nothing about entering
// provisioning may erase a working network.
esp_err_t config_save_wifi(const char *ssid, const char *pass);
esp_err_t config_save_uri(const char *uri);

// Written once when the settings menu closes, not on every press: walking the
// volume page in a circle is six presses and would otherwise be six writes.
esp_err_t config_save_settings(uint8_t volume, uint8_t screen, uint8_t eyes);

bool config_is_provisioned(const device_config_t *c);

// A request to provision, surviving one reboot.
//
// The hold-to-reset gesture can complete while the radio and the socket are
// already up, and taking those down in place is not something this firmware
// can do safely: wifi_start() performs one-time initialisation - esp_netif_init,
// the default event loop, esp_wifi_init - that must not run twice, so there is
// no second call to make. Rebooting is how this firmware already gets a clean
// slate; link_task does it after ninety seconds offline.
//
// So the gesture sets this and restarts, and the next boot sees it. Taking it
// clears it, so a device that reboots for any other reason afterwards comes up
// normally rather than provisioning again.
void config_request_provisioning(void);
bool config_take_provisioning_request(void);
