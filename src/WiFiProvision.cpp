// WiFiProvision — task, connect-by-priority state machine, and public API.
#include "WiFiProvision.h"
#include "wp_internal.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_system.h>

using namespace wp;

// ── Identity: AP SSID and hostname from the low 3 bytes of the eFuse MAC ─────
static void initIds(Context& ctx) {
    uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
    snprintf(ctx.apSsid, sizeof(ctx.apSsid), "%s-%06X", ctx.cfg.deviceName, id);
    if (ctx.cfg.hostname && ctx.cfg.hostname[0]) {
        strlcpy(ctx.hostname, ctx.cfg.hostname, sizeof(ctx.hostname));
    } else {
        snprintf(ctx.hostname, sizeof(ctx.hostname), "%s-%06x", ctx.cfg.deviceName, id);
    }
}

// ── Portal lifecycle ─────────────────────────────────────────────────────────
static void startPortal(Context& ctx) {
    // AP_STA so the portal can scan for networks without dropping AP clients.
    WiFi.mode(WIFI_MODE_APSTA);
    WiFi.softAPConfig(kApIp, kApIp, kApSubnet);
    bool ok = (ctx.cfg.apPassword && strlen(ctx.cfg.apPassword) >= 8)
                  ? WiFi.softAP(ctx.apSsid, ctx.cfg.apPassword)
                  : WiFi.softAP(ctx.apSsid);
    if (!ok) { ctx.state = WPState::Error; return; }

    ctx.dns.setErrorReplyCode(DNSReplyCode::NoError);
    ctx.dns.start(kDnsPort, "*", kApIp);
    WiFi.scanNetworks(true);  // start an async scan for the portal's dropdown
    ctx.portalActive = true;
    ctx.state = WPState::Portal;
    Serial.printf("[WiFiProv] Portal up: SSID \"%s\"  http://%s\n",
                  ctx.apSsid, kApIp.toString().c_str());
}

static void stopPortal(Context& ctx) {
    if (!ctx.portalActive) return;
    ctx.dns.stop();
    WiFi.softAPdisconnect(true);
    ctx.portalActive = false;
}

// ── Single blocking STA attempt (runs inside the task) ───────────────────────
static bool connectOne(Context& ctx, const WPNetwork& net) {
    Serial.printf("[WiFiProv] Trying \"%s\"…\n", net.ssid);
    WiFi.begin(net.ssid, net.pass);
    uint32_t t0 = millis();
    while (millis() - t0 < ctx.cfg.connectTimeoutMs) {
        if (WiFi.status() == WL_CONNECTED) return true;
        ctx.server.handleClient();  // stay responsive during the wait
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static bool ssidVisible(const String& ssid, int scanCount) {
    for (int i = 0; i < scanCount; ++i) {
        if (WiFi.SSID(i) == ssid) return true;
    }
    return false;
}

static void onConnected(Context& ctx) {
    stopPortal(ctx);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // low-latency RX; every reference project did this
    ctx.state = WPState::Connected;
    MDNS.end();
    if (MDNS.begin(ctx.hostname)) MDNS.addService("http", "tcp", 80);
    Serial.printf("[WiFiProv] Connected: %s  IP %s  (%s.local)\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), ctx.hostname);
}

// Try saved networks by priority, preferring ones visible in a fresh scan.
// Opens the portal if nothing connects (unless disabled).
static void connectOrPortal(Context& ctx) {
    if (ctx.netCount == 0) { startPortal(ctx); return; }

    ctx.state = WPState::Connecting;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    int n = WiFi.scanNetworks();  // synchronous; fine inside the task

    bool tried[WP_MAX_NETWORKS] = {false};
    // Pass 1: saved networks that are currently visible, in priority order.
    for (uint8_t i = 0; i < ctx.netCount; ++i) {
        if (ssidVisible(ctx.networks[i].ssid, n)) {
            tried[i] = true;
            if (connectOne(ctx, ctx.networks[i])) { WiFi.scanDelete(); onConnected(ctx); return; }
        }
    }
    // Pass 2: the rest (hidden SSIDs or missed by the scan), in priority order.
    for (uint8_t i = 0; i < ctx.netCount; ++i) {
        if (!tried[i] && connectOne(ctx, ctx.networks[i])) {
            WiFi.scanDelete(); onConnected(ctx); return;
        }
    }
    WiFi.scanDelete();

    if (ctx.cfg.startPortalOnFail) { startPortal(ctx); }
    else { ctx.state = WPState::Error; Serial.println("[WiFiProv] No network connected."); }
}

// ── Background task ──────────────────────────────────────────────────────────
static const uint32_t kReconnectIntervalMs = 30000;

static void netTask(void* arg) {
    Context& ctx = *static_cast<Context*>(arg);

    initIds(ctx);
    WiFi.persistent(false);
    WiFi.setHostname(ctx.hostname);

    if (!LittleFS.begin(true)) {
        Serial.println("[WiFiProv] LittleFS mount failed — serving built-in portal page.");
    }
    storeLoadNetworks(ctx);
    storeLoadFields(ctx);

    // WiFi.mode() (inside connectOrPortal/startPortal) must run before
    // server.begin(): it brings up the lwIP TCP/IP task the server binds to.
    connectOrPortal(ctx);

    httpSetup(ctx);
    ctx.server.begin();
    ctx.started = true;

    uint32_t lastReconnect = millis();
    for (;;) {
        if (ctx.pendingRestart) { vTaskDelay(pdMS_TO_TICKS(1200)); esp_restart(); }

        ctx.server.handleClient();
        if (ctx.portalActive) ctx.dns.processNextRequest();

        if (ctx.requestPortalReq) {
            ctx.requestPortalReq = false;
            if (!ctx.portalActive) startPortal(ctx);
        }
        if (ctx.applyConnectReq) {
            ctx.applyConnectReq = false;
            stopPortal(ctx);
            connectOrPortal(ctx);
            lastReconnect = millis();
        }

        // Auto-recovery when running as STA (not while the portal is up).
        if (!ctx.portalActive) {
            if (WiFi.status() == WL_CONNECTED) {
                if (ctx.state != WPState::Connected) onConnected(ctx);
            } else {
                if (ctx.state == WPState::Connected) {
                    ctx.state = WPState::Connecting;
                    Serial.println("[WiFiProv] Connection lost — will retry.");
                }
                if (millis() - lastReconnect > kReconnectIntervalMs) {
                    lastReconnect = millis();
                    connectOrPortal(ctx);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ── Public API ───────────────────────────────────────────────────────────────
WiFiProvision::WiFiProvision() : _ctx(new wp::Context()) {}
WiFiProvision::~WiFiProvision() { delete _ctx; }

void WiFiProvision::begin(const char* deviceName) {
    WPConfig cfg;
    cfg.deviceName = deviceName;
    begin(cfg);
}

void WiFiProvision::begin(const WPConfig& cfg) {
    _ctx->cfg = cfg;
    // Single-core parts (C3/H2) have no core 1 to pin to.
#if defined(CONFIG_FREERTOS_UNICORE) || (portNUM_PROCESSORS == 1)
    xTaskCreate(netTask, "wifiprov", 8192, _ctx, 3, nullptr);
#else
    xTaskCreatePinnedToCore(netTask, "wifiprov", 8192, _ctx, 3, nullptr, 0);
#endif
}

bool WiFiProvision::connected() const { return _ctx->state == WPState::Connected; }
WPState WiFiProvision::state() const { return _ctx->state; }
IPAddress WiFiProvision::ip() const { return WiFi.localIP(); }
String WiFiProvision::apSsid() const { return String(_ctx->apSsid); }

void WiFiProvision::requestPortal() { _ctx->requestPortalReq = true; }

void WiFiProvision::resetNetworks() {
    storeReset(*_ctx);
    _ctx->pendingRestart = true;
}

void WiFiProvision::addCustomField(const char* key, const char* defaultValue, const char* label) {
    if (_ctx->fieldCount >= WP_MAX_CUSTOM_FIELDS || !key || !key[0]) return;
    CustomField& f = _ctx->fields[_ctx->fieldCount++];
    f.key   = key;
    f.label = (label && label[0]) ? label : key;
    f.value = defaultValue ? defaultValue : "";
}

String WiFiProvision::getCustomField(const char* key) const {
    for (uint8_t i = 0; i < _ctx->fieldCount; ++i) {
        if (_ctx->fields[i].key == key) return _ctx->fields[i].value;
    }
    return String();
}
