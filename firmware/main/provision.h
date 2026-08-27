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
#include <stdint.h>

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

// ---------------------------------------------------------- the WiFi trial
//
// A credential trial needs the station connection state that wifi_event()
// in voice_main.c already tracks, and that state is not safe to reach into
// from another translation unit directly - hence these two, declared here
// and defined in voice_main.c, next to what they touch.

// Turns trial mode on or off. While on, a disconnect ends the trial instead
// of being retried - see s_trial_in_progress next to wifi_event() in
// voice_main.c for why that split exists.
void provision_set_trial_mode(bool on);

typedef enum {
    PROV_TRIAL_CONNECTED,     // IP_EVENT_STA_GOT_IP arrived
    PROV_TRIAL_DISCONNECTED,  // WIFI_EVENT_STA_DISCONNECTED arrived; *out_reason is set
    PROV_TRIAL_TIMED_OUT,     // neither arrived within PROV_TRIAL_MS
} provision_trial_outcome_t;

// Waits up to PROV_TRIAL_MS for the connection attempt started by
// esp_wifi_connect() to resolve. Call only while trial mode is on.
provision_trial_outcome_t provision_wifi_trial_wait(uint8_t *out_reason);

// Cancels the connection attempt a trial started and waits, bounded, for
// wifi_event() to confirm the resulting disconnect landed on the trial
// branch rather than the normal one - see provision_set_trial_mode() above
// for why that split exists. Textual order relative to
// provision_set_trial_mode(false) is not synchronisation:
// esp_wifi_disconnect() only requests the disconnect, and the event it
// produces crosses WiFi-driver teardown and the default event-loop task
// before wifi_event() sees it, while clearing trial mode is a same-thread
// write that finishes essentially instantly. This waits for the trial
// branch's own confirmation instead of assuming that ordering. Call only
// while trial mode is still on, after a trial outcome other than
// PROV_TRIAL_CONNECTED. Safe to call when there is nothing to cancel -
// esp_wifi_disconnect() then fails synchronously and this returns without
// waiting.
void provision_wifi_trial_cancel(void);
