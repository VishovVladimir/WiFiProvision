// Internal shared state for WiFiProvision. Not part of the public API.
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "WiFiProvision.h"

namespace wp {

// The captive-portal soft-AP always uses this address (classic captive IP).
static const IPAddress kApIp     (4, 3, 2, 1);
static const IPAddress kApSubnet (255, 255, 255, 0);
static const uint8_t   kDnsPort  = 53;

// One saved network. Fixed-size char buffers so the whole list is a flat NVS
// blob. Named WPNetwork to avoid clashing with the core's global `Network`.
struct WPNetwork {
    char ssid[33];   // 32 chars + NUL
    char pass[65];   // 64 chars + NUL
};

// One sketch-registered portal field, persisted under NVS key "cf_<key>".
struct CustomField {
    String key;
    String label;
    String value;
};

struct Context {
    WPConfig cfg{};
    char apSsid[33]   = {0};
    char hostname[33] = {0};

    // The list is changed from the app's tasks (public API) as well as this
    // library's task (portal routes) -> every access holds `lock`.
    WPNetwork networks[WP_MAX_NETWORKS];
    uint8_t   netCount = 0;
    StaticSemaphore_t lockBuf;
    SemaphoreHandle_t lock = xSemaphoreCreateMutexStatic(&lockBuf);

    CustomField fields[WP_MAX_CUSTOM_FIELDS];
    uint8_t fieldCount = 0;

    volatile WPState state            = WPState::Connecting;
    volatile bool    portalActive     = false;
    volatile bool    pendingRestart   = false;  // deferred reboot after a destructive save
    volatile bool    requestPortalReq = false;  // portal requested at runtime
    volatile bool    applyConnectReq  = false;  // re-run the connect sequence
    volatile bool    stopReq          = false;  // end(): wind the task down
    volatile bool    stopped          = false;  // the task has finished its teardown
    TaskHandle_t     task             = nullptr;

    WebServer server{80};
    DNSServer dns;
    bool      started = false;   // server listening
    bool      routes  = false;   // routes registered (once)
};

// RAII holder for Context::lock.
struct Lock {
    explicit Lock(Context& c) : m(c.lock) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m); }
    SemaphoreHandle_t m;
};

// ── Store (WPStore.cpp) ──────────────────────────────────────────────────────
void storeLoadNetworks(Context& ctx);   // fills networks[]/netCount from NVS
void storeSaveNetworks(Context& ctx);    // persists networks[]/netCount to NVS
void storeLoadFields(Context& ctx);      // overrides field values from NVS
void storeSaveFields(Context& ctx);      // persists all field values to NVS
void storeReset(Context& ctx);           // clears the saved network list in NVS

// Network-list operations (in-RAM; caller holds ctx.lock and persists via
// storeSaveNetworks).
int  netAdd(Context& ctx, const char* ssid, const char* pass);  // add/update, returns index or -1 if full
bool netDelete(Context& ctx, int index);
bool netMove(Context& ctx, int index, bool up);

// ── HTTP (WPHttp.cpp) ────────────────────────────────────────────────────────
void httpSetup(Context& ctx);   // registers all routes on ctx.server

}  // namespace wp
