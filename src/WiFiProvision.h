// WiFiProvision — a drop-in ESP32 WiFi provisioning library.
//
// Brings up a captive-portal soft-AP, scans for networks, accepts credentials
// (scanned or typed manually), and stores up to WP_MAX_NETWORKS networks in NVS
// with a user-editable priority order. On boot it connects to the highest
// priority network that is actually visible. The portal always lets you reorder
// and delete saved networks. The portal UI is a gzipped file served from
// LittleFS. Everything runs in its own FreeRTOS task; the sketch only calls
// begin().
//
// Targets the whole ESP32 family (classic / S3 / C3 / H2 / …). Uses only libs
// bundled with the Arduino-ESP32 core (WiFi, WebServer, DNSServer, Preferences,
// LittleFS, ESPmDNS) — no external dependencies, no core 2.x/3.x fork split.
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// Maximum number of saved networks. Override with -DWP_MAX_NETWORKS=N.
#ifndef WP_MAX_NETWORKS
#define WP_MAX_NETWORKS 10
#endif

// Maximum number of custom fields a sketch may register.
#ifndef WP_MAX_CUSTOM_FIELDS
#define WP_MAX_CUSTOM_FIELDS 8
#endif

enum class WPState : uint8_t {
    Connecting,  // trying saved networks
    Connected,   // STA connected, portal down
    Portal,      // captive portal AP is up
    Error        // no network connected and portal disabled
};

struct WPConfig {
    const char* deviceName       = "ESP32";  // AP SSID prefix; AP is "<name>-AABBCC"
    const char* apSsid           = nullptr;  // full AP SSID; overrides "<name>-AABBCC"
    const char* apPassword       = nullptr;  // nullptr/"" = open AP; else WPA2 (>= 8 chars)
    uint32_t    connectTimeoutMs = 15000;    // per-network STA attempt
    uint8_t     connectRounds    = 1;        // passes over the visible saved networks
    uint32_t    portalTimeoutMs  = 0;        // reserved; 0 = portal stays up
    bool        startPortalOnFail = true;    // raise portal if nothing connects at boot
    const char* hostname         = nullptr;  // mDNS/host name; default "<name>-aabbcc"

    // Link lost while running: wait this long (the driver's own auto-reconnect gets
    // the first go), then re-run the connect sequence over the whole list.
    uint32_t    reconnectIntervalMs = 30000;
    bool        portalOnReconnectFail = true;  // raise the portal if that finds nothing

    // false = the HTTP server runs only while the portal is up, and port 80 is free
    // for the application once connected (it must then keep its own server off
    // while state() == Portal).
    bool        serverInSta      = true;
    bool        mdns             = true;     // announce hostname.local on connect
    bool        newestFirst      = false;    // new networks go to the top; a full list
                                             // drops its lowest-priority entry
    bool        importDriverConfig = false;  // empty list at boot -> seed it with the
                                             // network the WiFi driver kept in NVS
    UBaseType_t taskPriority     = 3;
};

namespace wp { struct Context; }

class WiFiProvision {
public:
    WiFiProvision();
    ~WiFiProvision();

    // Start provisioning. Spawns the background task and returns immediately.
    void begin(const char* deviceName);
    void begin(const WPConfig& cfg);

    bool      connected() const;
    WPState   state() const;
    IPAddress ip() const;
    String    apSsid() const;   // the soft-AP name ("<deviceName>-AABBCC")

    void requestPortal();   // force the portal open at runtime (e.g. a button)
    void resetNetworks();   // factory-clear the saved network list, then reboot

    // Saved networks, index 0 = highest priority. Safe to call from any task;
    // changes are persisted at once and used by the next connect sequence.
    uint8_t networkCount() const;
    String  networkSsid(uint8_t index) const;   // "" if out of range; never the password
    int     addNetwork(const char* ssid, const char* pass);  // index, or -1 (bad / full)
    bool    removeNetwork(uint8_t index);
    bool    moveNetwork(uint8_t index, bool up);
    void    clearNetworks();                    // like resetNetworks(), without the reboot

    // Stop everything before deep sleep: the task (cutting short any connect attempt),
    // portal, server, mDNS and the radio (esp_wifi_stop). Blocks up to timeoutMs for
    // the task to wind down. Not restartable.
    void end(uint32_t timeoutMs = 5000);

    // Register a custom text field shown in the portal and persisted to NVS.
    // Call before begin(). label defaults to key when null.
    void   addCustomField(const char* key, const char* defaultValue, const char* label = nullptr);
    String getCustomField(const char* key) const;

private:
    wp::Context* _ctx;
};
