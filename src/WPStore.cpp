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
    if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(WPNetwork::ssid)) return -1;
    if (pass && strlen(pass) >= sizeof(WPNetwork::pass)) return -1;
    int found = -1;
    for (uint8_t i = 0; i < ctx.netCount; ++i) {
        if (strncmp(ctx.networks[i].ssid, ssid, sizeof(ctx.networks[i].ssid)) == 0) { found = i; break; }
    }

    if (!ctx.cfg.newestFirst) {
        // Update the password if this SSID is already saved (keeps its priority).
        if (found >= 0) {
            strlcpy(ctx.networks[found].pass, pass ? pass : "", sizeof(ctx.networks[found].pass));
            return found;
        }
        if (ctx.netCount >= WP_MAX_NETWORKS) return -1;  // list full
        WPNetwork& n = ctx.networks[ctx.netCount];
        strlcpy(n.ssid, ssid, sizeof(n.ssid));
        strlcpy(n.pass, pass ? pass : "", sizeof(n.pass));
        return ctx.netCount++;
    }

    // newestFirst: the entry (new or re-saved) goes to index 0 and the rest shift
    // down one; when the list is full the last (lowest-priority) entry falls off.
    int from = found;
    if (from < 0) {
        if (ctx.netCount < WP_MAX_NETWORKS) ctx.netCount++;
        from = ctx.netCount - 1;
    }
    memmove(&ctx.networks[1], &ctx.networks[0], sizeof(WPNetwork) * from);
    memset(&ctx.networks[0], 0, sizeof(WPNetwork));
    strlcpy(ctx.networks[0].ssid, ssid, sizeof(ctx.networks[0].ssid));
    strlcpy(ctx.networks[0].pass, pass ? pass : "", sizeof(ctx.networks[0].pass));
    return 0;
}

bool netDelete(Context& ctx, int index) {
    if (index < 0 || index >= ctx.netCount) return false;
    for (uint8_t i = index; i + 1 < ctx.netCount; ++i) {
        ctx.networks[i] = ctx.networks[i + 1];
    }
    ctx.netCount--;
    memset(&ctx.networks[ctx.netCount], 0, sizeof(WPNetwork));  // no stale password in RAM
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
