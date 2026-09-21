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
<input id=man placeholder="or type SSID">
<input id=pw type=password placeholder=password>
<button onclick=save()>Save &amp; connect</button>
<h3>Saved</h3><ul id=list></ul>
<p><button onclick=reset()>Factory reset</button></p>
<pre id=msg></pre>
<script>
var M=msg;function j(u,o){return fetch(u,o).then(r=>r.json())}
function scan(){fetch('/wifi/scan').then(r=>r.status==202?(setTimeout(scan,1800),null):r.json()).then(a=>{if(!a)return;a.sort((x,y)=>y.rssi-x.rssi);sel.innerHTML='<option value="">-- pick --</option>'+a.map(x=>`<option>${x.ssid}</option>`).join('')})}
function load(){j('/api/networks').then(a=>{list.innerHTML=a.map(n=>`<li><span>${n.i+1}. ${n.ssid}</span><button onclick="mv(${n.i},1)">▲</button><button onclick="mv(${n.i},0)">▼</button><button onclick="del(${n.i})">✕</button></li>`).join('')})}
function P(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(r=>r.text())}
function save(){var s=(man.value.trim()||sel.value).trim();if(!s){M.textContent='SSID required';return}M.textContent='Saving…';P('/wifi/save','ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(pw.value)).then(t=>M.textContent=t)}
function del(i){P('/wifi/delete','index='+i).then(load)}
function mv(i,u){P('/wifi/priority','index='+i+'&dir='+(u?'up':'down')).then(load)}
function reset(){if(confirm('Clear all saved networks?'))P('/reset','').then(t=>M.textContent=t)}
scan();load();
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

static void servePortal(Context& ctx) {
    if (LittleFS.exists("/portal.html.gz")) {
        File f = LittleFS.open("/portal.html.gz", "r");
        ctx.server.sendHeader("Content-Encoding", "gzip");
        ctx.server.sendHeader("Cache-Control", "no-cache");
        ctx.server.streamFile(f, "text/html");
        f.close();
    } else {
        ctx.server.send_P(200, "text/html", kFallbackHtml);
    }
}

static void captiveRedirect(Context& ctx) {
    ctx.server.sendHeader("Location", "http://4.3.2.1/", true);
    ctx.server.send(302, "text/plain", "");
}

void httpSetup(Context& ctx) {
    WebServer& s = ctx.server;

    // ── Portal page ──────────────────────────────────────────────────────────
    s.on("/", HTTP_GET, [&ctx]() { servePortal(ctx); });

    // ── OS captive-portal detection probes (force the login page to open) ────
    s.on("/generate_204",        HTTP_GET, [&ctx]() { captiveRedirect(ctx); });
    s.on("/redirect",            HTTP_GET, [&ctx]() { captiveRedirect(ctx); });
    s.on("/ncsi.txt",            HTTP_GET, [&ctx]() { captiveRedirect(ctx); });
    s.on("/hotspot-detect.html", HTTP_GET, [&ctx]() { servePortal(ctx); });
    s.on("/canonical.html",      HTTP_GET, [&ctx]() { servePortal(ctx); });
    s.on("/success.txt",         HTTP_GET, [&ctx]() { ctx.server.send(200, "text/plain", "success"); });
    s.on("/wpad.dat",            HTTP_GET, [&ctx]() { ctx.server.send(404, "text/plain", ""); });
    s.on("/connecttest.txt",     HTTP_GET, [&ctx]() {
        ctx.server.sendHeader("Location", "http://logout.net", true);
        ctx.server.send(302, "text/plain", "");
    });

    // ── WiFi scan (async; 202 while running, then a JSON array) ──────────────
    s.on("/wifi/scan", HTTP_GET, [&ctx]() {
        int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) { ctx.server.send(202, "application/json", "[]"); return; }
        if (n < 0) { WiFi.scanNetworks(true); ctx.server.send(202, "application/json", "[]"); return; }
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
        WiFi.scanNetworks(true);  // kick the next scan for the following poll
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

    // Catch-all → drive unknown hosts to the portal.
    s.onNotFound([&ctx]() { captiveRedirect(ctx); });
}

}  // namespace wp
