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
    const char* apPassword       = nullptr;  // nullptr/"" = open AP; else WPA2 (>= 8 chars)
    uint32_t    connectTimeoutMs = 15000;    // per-network STA attempt
    uint32_t    portalTimeoutMs  = 0;        // reserved; 0 = portal stays up
    bool        startPortalOnFail = true;    // raise portal if nothing connects
    const char* hostname         = nullptr;  // mDNS/host name; default "<name>-aabbcc"
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

    // Register a custom text field shown in the portal and persisted to NVS.
    // Call before begin(). label defaults to key when null.
    void   addCustomField(const char* key, const char* defaultValue, const char* label = nullptr);
    String getCustomField(const char* key) const;

private:
    wp::Context* _ctx;
};
