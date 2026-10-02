// WiFiProvision — task, connect-by-priority state machine, and public API.
#include "WiFiProvision.h"
#include "wp_internal.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <esp_wifi.h>

using namespace wp;

// ── Identity: AP SSID and hostname from the low 3 bytes of the eFuse MAC ─────
static void initIds(Context& ctx) {
    uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
    if (ctx.cfg.apSsid && ctx.cfg.apSsid[0]) {
        strlcpy(ctx.apSsid, ctx.cfg.apSsid, sizeof(ctx.apSsid));
    } else {
        snprintf(ctx.apSsid, sizeof(ctx.apSsid), "%s-%06X", ctx.cfg.deviceName, id);
    }
    if (ctx.cfg.hostname && ctx.cfg.hostname[0]) {
        strlcpy(ctx.hostname, ctx.cfg.hostname, sizeof(ctx.hostname));
    } else {
        snprintf(ctx.hostname, sizeof(ctx.hostname), "%s-%06x", ctx.cfg.deviceName, id);
    }
}

// ── HTTP server lifecycle ────────────────────────────────────────────────────
// Must only start after WiFi.mode(): that brings up the lwIP TCP/IP task the
// server binds to.
static void serverStart(Context& ctx) {
    if (ctx.started) return;
    if (!ctx.routes) { httpSetup(ctx); ctx.routes = true; }
    ctx.server.begin();
    ctx.started = true;
}

static void serverStop(Context& ctx) {
    if (!ctx.started) return;
    ctx.server.close();
    ctx.started = false;
}

// ── Portal lifecycle ─────────────────────────────────────────────────────────
static void startPortal(Context& ctx) {
    // AP_STA so the portal can scan for networks without dropping AP clients.
    WiFi.mode(WIFI_MODE_APSTA);
    // The 5th argument is the DNS server offered to DHCP clients. Leave it out
    // and the core skips the DHCP DNS option entirely (NetworkInterface::config
    // only sets it when it is non-zero): the phone then joins with no resolver,
    // never asks our DNSServer anything, and no captive portal ever opens.
    // The 4th (lease start) stays 0 = default, i.e. the address after the AP's.
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    WiFi.softAPConfig(kApIp, kApIp, kApSubnet, IPAddress((uint32_t)0), kApIp);
#else
    WiFi.softAPConfig(kApIp, kApIp, kApSubnet);
#endif
    bool ok = (ctx.cfg.apPassword && strlen(ctx.cfg.apPassword) >= 8)
                  ? WiFi.softAP(ctx.apSsid, ctx.cfg.apPassword)
                  : WiFi.softAP(ctx.apSsid);
    if (!ok) { ctx.state = WPState::Error; return; }

#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
    // RFC 8910 DHCP option 114: hands the portal URL to the client directly.
    // Recent iOS/Android open the portal from this alone, without a probe.
    WiFi.AP.enableDhcpCaptivePortal();
#endif

    ctx.dns.setErrorReplyCode(DNSReplyCode::NoError);
    ctx.dns.setTTL(0);  // do not let the phone cache our hijacked answers
    ctx.dns.start(kDnsPort, "*", kApIp);
    serverStart(ctx);
    // No scan here: it would take the radio off the AP channel exactly while the
    // phone is loading the page. The portal asks for one over /wifi/scan.
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
    if (!ctx.cfg.serverInSta) serverStop(ctx);  // hand port 80 back to the app
}

// ── Single blocking STA attempt (runs inside the task) ───────────────────────
static bool connectOne(Context& ctx, const WPNetwork& net) {
    Serial.printf("[WiFiProv] Trying \"%s\"…\n", net.ssid);
    WiFi.begin(net.ssid, net.pass[0] ? net.pass : nullptr);  // "" = open network
    uint32_t t0 = millis();
    while (millis() - t0 < ctx.cfg.connectTimeoutMs) {
        if (WiFi.status() == WL_CONNECTED) return true;
        if (ctx.stopReq) break;
        if (ctx.started) ctx.server.handleClient();  // stay responsive during the wait
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    WiFi.disconnect();  // abandon this one cleanly before begin() on the next
    return false;
}

static void onConnected(Context& ctx) {
    stopPortal(ctx);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // low-latency RX; every reference project did this
    if (ctx.cfg.mdns) {
        MDNS.end();
        if (MDNS.begin(ctx.hostname)) MDNS.addService("http", "tcp", 80);
    }
    // Last: an app watching state() may take port 80 as soon as it reads Connected.
    ctx.state = WPState::Connected;
    Serial.printf("[WiFiProv] Connected: %s  IP %s  (%s.local)\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), ctx.hostname);
}

// Try saved networks by priority. Pass 1 (repeated connectRounds times): the ones
// visible in a fresh scan. Pass 2 (once): the rest — hidden SSIDs, or missed by
// the scan. Works on a copy, so the list may change meanwhile.
static bool connectSaved(Context& ctx) {
    static WPNetwork nets[WP_MAX_NETWORKS];  // only ever used by this task
    uint8_t n;
    {
        Lock l(ctx);
        n = ctx.netCount;
        memcpy(nets, ctx.networks, sizeof(WPNetwork) * n);
    }
    if (n == 0) return false;

    ctx.state = WPState::Connecting;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    bool autoRc = WiFi.getAutoReconnect();
    WiFi.setAutoReconnect(false);  // this loop decides what to try next
    WiFi.disconnect();             // a pending reconnect would stall the scan

    bool visible[WP_MAX_NETWORKS] = {false};
    int found = WiFi.scanNetworks();  // synchronous; fine inside the task
    for (int k = 0; k < found; ++k) {
        String s = WiFi.SSID(k);
        for (uint8_t i = 0; i < n; ++i) {
            if (s == nets[i].ssid) visible[i] = true;
        }
    }
    WiFi.scanDelete();

    bool ok = false;
    uint8_t rounds = ctx.cfg.connectRounds ? ctx.cfg.connectRounds : 1;
    for (uint8_t r = 0; r < rounds && !ok && !ctx.stopReq; ++r) {
        for (uint8_t i = 0; i < n && !ok && !ctx.stopReq; ++i) {
            if (visible[i]) ok = connectOne(ctx, nets[i]);
        }
    }
    for (uint8_t i = 0; i < n && !ok && !ctx.stopReq; ++i) {
        if (!visible[i]) ok = connectOne(ctx, nets[i]);
    }
    memset(nets, 0, sizeof(nets));  // do not keep passwords around
    WiFi.setAutoReconnect(autoRc);
    return ok;
}

// Opens the portal if nothing connects and portalOnFail is set.
static void connectOrPortal(Context& ctx, bool portalOnFail) {
    if (connectSaved(ctx)) { onConnected(ctx); return; }
    if (ctx.stopReq) return;
    if (portalOnFail) { startPortal(ctx); }
    else { ctx.state = WPState::Error; Serial.println("[WiFiProv] No network connected."); }
}

// Empty list at boot: adopt the network the WiFi driver still holds in NVS (a
// previous firmware's credentials). esp_wifi_init() loaded it, so the driver
// must be up (WiFi.mode) before this runs.
static void importDriverConfig(Context& ctx) {
    Lock l(ctx);
    if (ctx.netCount) return;
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || !conf.sta.ssid[0]) return;
    char ssid[33] = {0}, pass[65] = {0};
    memcpy(ssid, conf.sta.ssid, 32);
    memcpy(pass, conf.sta.password, 64);
    if (netAdd(ctx, ssid, pass) >= 0) {
        storeSaveNetworks(ctx);
        Serial.printf("[WiFiProv] Imported \"%s\" from the driver config\n", ssid);
    }
}

// end(): release everything the task owns. The radio itself is stopped by end().
static void teardown(Context& ctx) {
    serverStop(ctx);
    if (ctx.cfg.mdns) MDNS.end();
    WiFi.setAutoReconnect(false);  // a disconnect must not trigger a new connect
    if (ctx.portalActive) {
        ctx.dns.stop();
        WiFi.softAPdisconnect(false);
        ctx.portalActive = false;
    } else {
        WiFi.disconnect(false);
    }
}

// ── Background task ──────────────────────────────────────────────────────────
static void netTask(void* arg) {
    Context& ctx = *static_cast<Context*>(arg);

    WiFi.persistent(false);
    WiFi.setHostname(ctx.hostname);

    if (!LittleFS.begin(true)) {
        Serial.println("[WiFiProv] LittleFS mount failed — serving built-in portal page.");
    }

    WiFi.mode(WIFI_STA);
    if (ctx.cfg.importDriverConfig) importDriverConfig(ctx);
    // persistent(false) only takes effect if this task initialised the driver;
    // the app may have done so already. Our own list is the source of truth, so
    // keep begin() from rewriting the driver's NVS copy on every attempt.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    connectOrPortal(ctx, ctx.cfg.startPortalOnFail);
    if (ctx.cfg.serverInSta) serverStart(ctx);

    uint32_t lastAttempt = millis();
    for (;;) {
        if (ctx.stopReq) {
            teardown(ctx);
            ctx.stopped = true;
            vTaskDelete(nullptr);
        }
        if (ctx.pendingRestart) { vTaskDelay(pdMS_TO_TICKS(1200)); esp_restart(); }

        if (ctx.started) ctx.server.handleClient();
        if (ctx.portalActive) ctx.dns.processNextRequest();

        if (ctx.requestPortalReq) {
            ctx.requestPortalReq = false;
            if (!ctx.portalActive) startPortal(ctx);
        }
        if (ctx.applyConnectReq) {
            ctx.applyConnectReq = false;
            stopPortal(ctx);
            connectOrPortal(ctx, ctx.cfg.startPortalOnFail);
            lastAttempt = millis();
        }

        // Auto-recovery when running as STA (not while the portal is up).
        if (!ctx.portalActive) {
            if (WiFi.status() == WL_CONNECTED) {
                if (ctx.state != WPState::Connected) onConnected(ctx);
            } else {
                if (ctx.state == WPState::Connected) {
                    ctx.state = WPState::Connecting;
                    lastAttempt = millis();  // the driver's auto-reconnect goes first
                    Serial.println("[WiFiProv] Connection lost — will retry.");
                }
                if (millis() - lastAttempt > ctx.cfg.reconnectIntervalMs) {
                    connectOrPortal(ctx, ctx.cfg.portalOnReconnectFail);
                    lastAttempt = millis();
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
    if (_ctx->task) return;
    _ctx->cfg = cfg;
    initIds(*_ctx);
    // Loaded here rather than in the task, so the list API is valid (and cannot
    // be overwritten by a late load) as soon as begin() returns.
    {
        Lock l(*_ctx);
        storeLoadNetworks(*_ctx);
    }
    storeLoadFields(*_ctx);
    // Single-core parts (C3/H2) have no core 1 to pin to.
#if defined(CONFIG_FREERTOS_UNICORE) || (portNUM_PROCESSORS == 1)
    xTaskCreate(netTask, "wifiprov", 8192, _ctx, cfg.taskPriority, &_ctx->task);
#else
    xTaskCreatePinnedToCore(netTask, "wifiprov", 8192, _ctx, cfg.taskPriority, &_ctx->task, 0);
#endif
}

bool WiFiProvision::connected() const { return _ctx->state == WPState::Connected; }
WPState WiFiProvision::state() const { return _ctx->state; }
IPAddress WiFiProvision::ip() const { return WiFi.localIP(); }
String WiFiProvision::apSsid() const { return String(_ctx->apSsid); }

void WiFiProvision::requestPortal() { _ctx->requestPortalReq = true; }

void WiFiProvision::resetNetworks() {
    clearNetworks();
    _ctx->pendingRestart = true;
}

uint8_t WiFiProvision::networkCount() const {
    Lock l(*_ctx);
    return _ctx->netCount;
}

String WiFiProvision::networkSsid(uint8_t index) const {
    Lock l(*_ctx);
    return index < _ctx->netCount ? String(_ctx->networks[index].ssid) : String();
}

int WiFiProvision::addNetwork(const char* ssid, const char* pass) {
    Lock l(*_ctx);
    int idx = netAdd(*_ctx, ssid, pass);
    if (idx >= 0) storeSaveNetworks(*_ctx);
    return idx;
}

bool WiFiProvision::removeNetwork(uint8_t index) {
    Lock l(*_ctx);
    if (!netDelete(*_ctx, index)) return false;
    storeSaveNetworks(*_ctx);
    return true;
}

bool WiFiProvision::moveNetwork(uint8_t index, bool up) {
    Lock l(*_ctx);
    if (!netMove(*_ctx, index, up)) return false;
    storeSaveNetworks(*_ctx);
    return true;
}

void WiFiProvision::clearNetworks() {
    Lock l(*_ctx);
    storeReset(*_ctx);
}

void WiFiProvision::end(uint32_t timeoutMs) {
    if (!_ctx->task) return;
    if (!_ctx->stopped) {
        _ctx->stopReq = true;
        uint32_t t0 = millis();
        while (!_ctx->stopped && millis() - t0 < timeoutMs) vTaskDelay(pdMS_TO_TICKS(10));
        if (!_ctx->stopped) {
            // Stuck in a driver call. Freeze it so it cannot touch WiFi after the
            // stop below; end() is a one-way street (deep sleep / restart) anyway.
            vTaskSuspend(_ctx->task);
            Serial.println("[WiFiProv] end(): task did not stop in time, suspended");
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // let the tcpip thread drain its backlog
    esp_wifi_stop();
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
