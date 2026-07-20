// NVS persistence (via Preferences) for the saved network list and custom fields.
#include "wp_internal.h"
#include <Preferences.h>
#include <string.h>

namespace wp {

static const char* kNs = "wifiprov";  // NVS namespace

void storeLoadNetworks(Context& ctx) {
    Preferences prefs;
    prefs.begin(kNs, true);  // read-only
    uint8_t cnt = prefs.getUChar("cnt", 0);
    if (cnt > WP_MAX_NETWORKS) cnt = WP_MAX_NETWORKS;
    size_t want = (size_t)cnt * sizeof(WPNetwork);
    ctx.netCount = 0;
    if (want > 0 && prefs.getBytesLength("nets") == want) {
        prefs.getBytes("nets", ctx.networks, want);
        // Guard against corrupt/unterminated strings.
        for (uint8_t i = 0; i < cnt; ++i) {
            ctx.networks[i].ssid[sizeof(ctx.networks[i].ssid) - 1] = '\0';
            ctx.networks[i].pass[sizeof(ctx.networks[i].pass) - 1] = '\0';
        }
        ctx.netCount = cnt;
    }
    prefs.end();
}

void storeSaveNetworks(Context& ctx) {
    Preferences prefs;
    prefs.begin(kNs, false);  // read-write
    prefs.putUChar("cnt", ctx.netCount);
    prefs.putBytes("nets", ctx.networks, (size_t)ctx.netCount * sizeof(WPNetwork));
    prefs.end();
}

void storeReset(Context& ctx) {
    Preferences prefs;
    prefs.begin(kNs, false);
    prefs.remove("nets");
    prefs.putUChar("cnt", 0);
    prefs.end();
    ctx.netCount = 0;
}

void storeLoadFields(Context& ctx) {
    Preferences prefs;
    prefs.begin(kNs, true);
    for (uint8_t i = 0; i < ctx.fieldCount; ++i) {
        String key = "cf_" + ctx.fields[i].key;
        ctx.fields[i].value = prefs.getString(key.c_str(), ctx.fields[i].value);
    }
    prefs.end();
}

void storeSaveFields(Context& ctx) {
    Preferences prefs;
    prefs.begin(kNs, false);
    for (uint8_t i = 0; i < ctx.fieldCount; ++i) {
        String key = "cf_" + ctx.fields[i].key;
        prefs.putString(key.c_str(), ctx.fields[i].value);
    }
    prefs.end();
}

// ── In-RAM list operations ───────────────────────────────────────────────────

int netAdd(Context& ctx, const char* ssid, const char* pass) {
    if (!ssid || !ssid[0]) return -1;
    // Update the password if this SSID is already saved (keeps its priority).
    for (uint8_t i = 0; i < ctx.netCount; ++i) {
        if (strncmp(ctx.networks[i].ssid, ssid, sizeof(ctx.networks[i].ssid)) == 0) {
            strlcpy(ctx.networks[i].pass, pass ? pass : "", sizeof(ctx.networks[i].pass));
            return i;
        }
    }
    if (ctx.netCount >= WP_MAX_NETWORKS) return -1;  // list full
    WPNetwork& n = ctx.networks[ctx.netCount];
    strlcpy(n.ssid, ssid, sizeof(n.ssid));
    strlcpy(n.pass, pass ? pass : "", sizeof(n.pass));
    return ctx.netCount++;
}

bool netDelete(Context& ctx, int index) {
    if (index < 0 || index >= ctx.netCount) return false;
    for (uint8_t i = index; i + 1 < ctx.netCount; ++i) {
        ctx.networks[i] = ctx.networks[i + 1];
    }
    ctx.netCount--;
    return true;
}

bool netMove(Context& ctx, int index, bool up) {
    if (index < 0 || index >= ctx.netCount) return false;
    int j = up ? index - 1 : index + 1;
    if (j < 0 || j >= ctx.netCount) return false;
    WPNetwork tmp = ctx.networks[index];
    ctx.networks[index] = ctx.networks[j];
    ctx.networks[j] = tmp;
    return true;
}

}  // namespace wp
