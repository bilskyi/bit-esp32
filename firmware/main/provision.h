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
