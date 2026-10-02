// HTTP routes for the captive portal: page serving, WiFi scan, network-list
// management (add / delete / reorder), custom fields, status, and the OS
// captive-portal probe endpoints. Synchronous WebServer (core built-in).
#include "wp_internal.h"
#include <WiFi.h>
#include <LittleFS.h>

namespace wp {

// Minimal built-in page used when /portal.html.gz is missing from LittleFS
// (e.g. the filesystem image was never uploaded). Talks to the same endpoints
// so provisioning still works. The full styled UI lives in web/portal.html.
static const char kFallbackHtml[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1"><title>WiFi Setup</title>
<style>body{font-family:sans-serif;max-width:460px;margin:14px auto;padding:0 12px}
input,select,button{width:100%;padding:.5em;margin:.25em 0;box-sizing:border-box}
li{display:flex;gap:6px;align-items:center;margin:.2em 0}li span{flex:1}</style></head><body>
<h3>Add network</h3>
<select id=sel><option value="">scanning…</option></select>
<button onclick="scan(1)">Rescan</button>
<input id=man placeholder="or type SSID">
<input id=pw type=password placeholder=password>
<button onclick=save()>Save &amp; connect</button>
<h3>Saved</h3><ul id=list></ul>
<p><button onclick=reset()>Factory reset</button></p>
<pre id=msg></pre>
<script>
var M=msg;function j(u,o){return fetch(u,o).then(r=>r.json())}
function scan(r){fetch('/wifi/scan'+(r?'?refresh=1':'')).then(r=>r.status==202?(setTimeout(scan,1500),null):r.json()).then(a=>{if(!a)return;a.sort((x,y)=>y.rssi-x.rssi);sel.innerHTML='<option value="">-- pick --</option>'+a.map(x=>`<option>${x.ssid}</option>`).join('')})}
function load(){j('/api/networks').then(a=>{list.innerHTML=a.map(n=>`<li><span>${n.i+1}. ${n.ssid}</span><button onclick="mv(${n.i},1)">▲</button><button onclick="mv(${n.i},0)">▼</button><button onclick="del(${n.i})">✕</button></li>`).join('')})}
function P(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(r=>r.text())}
function save(){var s=(man.value.trim()||sel.value).trim();if(!s){M.textContent='SSID required';return}M.textContent='Saving…';P('/wifi/save','ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(pw.value)).then(t=>M.textContent=t)}
function del(i){P('/wifi/delete','index='+i).then(load)}
function mv(i,u){P('/wifi/priority','index='+i+'&dir='+(u?'up':'down')).then(load)}
function reset(){if(confirm('Clear all saved networks?'))P('/reset','').then(t=>M.textContent=t)}
load();setTimeout(scan,500);
</script></body></html>)HTML";

static String jsonEscape(const String& s) {
    String r;
    r.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c == '\n')        { r += "\\n"; }
        else if ((uint8_t)c < 0x20) { /* skip other control chars */ }
        else                       { r += c; }
    }
    return r;
}

static const char* stateName(WPState s) {
    switch (s) {
        case WPState::Connected:  return "connected";
        case WPState::Portal:     return "portal";
        case WPState::Error:      return "error";
        default:                  return "connecting";
    }
}

// Content type from the extension. Only what a portal page can reference.
static const char* mimeFor(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".woff2"))return "font/woff2";
    return "text/plain";
}

// Serves anything scripts/gzip_web.py packed into LittleFS: "/x" is looked up as
// "/x.gz" first, then "/x". This is what lets a project drop extra pages and
// assets (its own stylesheet, a control page) into web/ and have them served
// without touching the library.
static bool serveStatic(Context& ctx, const String& path) {
    if (path.isEmpty() || path[0] != '/' || path.indexOf("..") >= 0) return false;
    String gz = path + ".gz";
    String f  = LittleFS.exists(gz) ? gz : (LittleFS.exists(path) ? path : String());
    if (f.isEmpty()) return false;
    File file = LittleFS.open(f, "r");
    if (!file || file.isDirectory()) return false;
    // Assets are immutable between filesystem uploads; the page itself is not.
    ctx.server.sendHeader("Cache-Control", path.endsWith(".html") ? "no-cache" : "max-age=86400");
    // No Content-Encoding here: streamFile() adds it for a ".gz" name by itself.
    ctx.server.streamFile(file, mimeFor(path));
    file.close();
    return true;
}

static void servePortal(Context& ctx) {
    if (serveStatic(ctx, "/portal.html")) return;
    // Nothing in LittleFS (no filesystem image uploaded) — built-in page.
    ctx.server.sendHeader("Cache-Control", "no-cache");
    ctx.server.send_P(200, "text/html", kFallbackHtml);
}

static void captiveRedirect(Context& ctx) {
    // no-store: a cached 302 for a probe URL keeps the phone in "portal" state
    // long after it has joined a real network.
    ctx.server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    ctx.server.sendHeader("Location", "http://" + kApIp.toString() + "/", true);
    ctx.server.send(302, "text/plain", "");
}

// A scan takes the radio off the AP channel for a couple of seconds: while it
// runs the soft-AP answers nothing, so the phone's page load stalls and its TCP
// connections reset. Scans are therefore on demand only and rate-limited, and
// they use a shorter dwell time than the 300 ms/channel default (~1.7 s total
// instead of ~4 s). Nothing rescans in the background.
static const uint32_t kScanMinIntervalMs = 10000;

static void startScan(Context& ctx) {
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
    uint32_t now = millis();
    if (ctx.lastScanMs && now - ctx.lastScanMs < kScanMinIntervalMs) return;
    ctx.lastScanMs = now;
    WiFi.scanNetworks(true, false, false, 120);
}

void httpSetup(Context& ctx) {
    WebServer& s = ctx.server;

    // ── Portal page ──────────────────────────────────────────────────────────
    s.on("/", HTTP_GET, [&ctx]() { servePortal(ctx); });

    // ── OS captive-portal detection probes (force the login page to open) ────
    s.on("/generate_204",        HTTP_GET, [&ctx]() { captiveRedirect(ctx); });
    s.on("/gen_204",             HTTP_GET, [&ctx]() { captiveRedirect(ctx); });
    s.on("/redirect",            HTTP_GET, [&ctx]() { captiveRedirect(ctx); });
    s.on("/ncsi.txt",            HTTP_GET, [&ctx]() { captiveRedirect(ctx); });
    s.on("/library/test/success.html", HTTP_GET, [&ctx]() { servePortal(ctx); });
    s.on("/favicon.ico",         HTTP_GET, [&ctx]() {
        if (!serveStatic(ctx, "/favicon.svg")) ctx.server.send(404, "text/plain", "");
    });
    s.on("/hotspot-detect.html", HTTP_GET, [&ctx]() { servePortal(ctx); });
    s.on("/canonical.html",      HTTP_GET, [&ctx]() { servePortal(ctx); });
    s.on("/success.txt",         HTTP_GET, [&ctx]() { ctx.server.send(200, "text/plain", "success"); });
    s.on("/wpad.dat",            HTTP_GET, [&ctx]() { ctx.server.send(404, "text/plain", ""); });
    s.on("/connecttest.txt",     HTTP_GET, [&ctx]() {
        ctx.server.sendHeader("Location", "http://logout.net", true);
        ctx.server.send(302, "text/plain", "");
    });

    // ── WiFi scan (async; 202 while running, then a JSON array) ──────────────
    // The last result set stays cached and is re-served for free; "?refresh=1"
    // asks for a new scan. See startScan() for why this is not a background poll.
    s.on("/wifi/scan", HTTP_GET, [&ctx]() {
        if (ctx.server.hasArg("refresh")) startScan(ctx);
        int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) { ctx.server.send(202, "application/json", "[]"); return; }
        if (n < 0) { startScan(ctx); ctx.server.send(202, "application/json", "[]"); return; }
        String j = "[";
        for (int16_t i = 0; i < n; ++i) {
            if (i) j += ',';
            j += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":";
            j += WiFi.RSSI(i);
            j += ",\"sec\":";
            j += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
            j += '}';
        }
        j += ']';
        ctx.server.send(200, "application/json", j);
    });

    // ── Saved network list (priority order, no passwords) ────────────────────
    s.on("/api/networks", HTTP_GET, [&ctx]() {
        Lock l(ctx);
        String j = "[";
        for (uint8_t i = 0; i < ctx.netCount; ++i) {
            if (i) j += ',';
            j += "{\"i\":" + String(i) + ",\"ssid\":\"" + jsonEscape(ctx.networks[i].ssid) + "\"}";
        }
        j += ']';
        ctx.server.send(200, "application/json", j);
    });

    // ── Add / update a network, then try to connect with the new list ────────
    s.on("/wifi/save", HTTP_POST, [&ctx]() {
        String ssid = ctx.server.arg("ssid");
        String pass = ctx.server.arg("pass");
        if (ssid.isEmpty()) { ctx.server.send(400, "text/plain", "SSID is required"); return; }
        int idx;
        {
            Lock l(ctx);
            idx = netAdd(ctx, ssid.c_str(), pass.c_str());
            if (idx >= 0) storeSaveNetworks(ctx);
        }
        if (idx < 0) {
            ctx.server.send(507, "text/plain",
                            "Network list is full (max " + String(WP_MAX_NETWORKS) + ")");
            return;
        }
        ctx.server.send(200, "text/plain", "Saved. Connecting to \"" + ssid + "\"…");
        ctx.applyConnectReq = true;  // reconnect using the updated list (drops the portal)
    });

    // ── Delete a saved network ───────────────────────────────────────────────
    s.on("/wifi/delete", HTTP_POST, [&ctx]() {
        int idx = ctx.server.arg("index").toInt();
        Lock l(ctx);
        if (!netDelete(ctx, idx)) { ctx.server.send(400, "text/plain", "Bad index"); return; }
        storeSaveNetworks(ctx);
        ctx.server.send(200, "text/plain", "Deleted");
    });

    // ── Reorder priority (dir = up|down) ─────────────────────────────────────
    s.on("/wifi/priority", HTTP_POST, [&ctx]() {
        int  idx = ctx.server.arg("index").toInt();
        bool up  = ctx.server.arg("dir") == "up";
        Lock l(ctx);
        if (!netMove(ctx, idx, up)) { ctx.server.send(400, "text/plain", "Cannot move"); return; }
        storeSaveNetworks(ctx);
        ctx.server.send(200, "text/plain", "Reordered");
    });

    // ── Custom fields ────────────────────────────────────────────────────────
    s.on("/api/fields", HTTP_GET, [&ctx]() {
        String j = "[";
        for (uint8_t i = 0; i < ctx.fieldCount; ++i) {
            if (i) j += ',';
            j += "{\"key\":\"" + jsonEscape(ctx.fields[i].key) +
                 "\",\"label\":\"" + jsonEscape(ctx.fields[i].label) +
                 "\",\"value\":\"" + jsonEscape(ctx.fields[i].value) + "\"}";
        }
        j += ']';
        ctx.server.send(200, "application/json", j);
    });
    s.on("/api/fields", HTTP_POST, [&ctx]() {
        for (uint8_t i = 0; i < ctx.fieldCount; ++i) {
            String arg = "cf_" + ctx.fields[i].key;
            if (ctx.server.hasArg(arg)) ctx.fields[i].value = ctx.server.arg(arg);
        }
        storeSaveFields(ctx);
        ctx.server.send(200, "text/plain", "Settings saved");
    });

    // ── Status ───────────────────────────────────────────────────────────────
    s.on("/api/status", HTTP_GET, [&ctx]() {
        String j = "{\"state\":\"";
        j += stateName(ctx.state);
        j += "\",\"connected\":";
        j += (ctx.state == WPState::Connected) ? "true" : "false";
        j += ",\"ip\":\"" + WiFi.localIP().toString() +
             "\",\"ssid\":\"" + jsonEscape(WiFi.SSID()) +
             "\",\"ap\":\"" + jsonEscape(ctx.apSsid) +
             "\",\"count\":" + String(ctx.netCount) + "}";
        ctx.server.send(200, "application/json", j);
    });

    // ── Factory reset (clears the list, then reboots) ────────────────────────
    s.on("/reset", HTTP_POST, [&ctx]() {
        { Lock l(ctx); storeReset(ctx); }
        ctx.server.send(200, "text/plain", "Cleared. Rebooting…");
        ctx.pendingRestart = true;
    });

    // Catch-all → serve a packed asset if we have one, else drive the client to
    // the portal. The asset lookup must come first: the stylesheet and any extra
    // page a project ships live in LittleFS, not in a route.
    s.onNotFound([&ctx]() {
        if (serveStatic(ctx, ctx.server.uri())) return;
        captiveRedirect(ctx);
    });
}

}  // namespace wp
