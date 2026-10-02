# WiFiProvision

A drop-in **ESP32 WiFi provisioning** library. Add it to a project and it brings up a
captive-portal soft-AP, scans for networks, accepts credentials (scanned or typed manually),
and stores **up to 10 networks** in NVS with a **user-editable priority order**. On boot it
connects to the **highest-priority network that is actually visible**. The portal always lets
you **reorder and delete** saved networks. The portal UI is a **gzipped file served from
LittleFS**.

- **Synchronous `WebServer`** — no external dependencies, builds identically on Arduino-ESP32
  core 2.x and 3.x.
- **Runs in its own FreeRTOS task** — your `loop()` stays free.
- **Whole ESP32 family** — classic, S3, C3, H2, … (single-core parts handled automatically).
- Uses only core libraries: `WiFi`, `WebServer`, `DNSServer`, `Preferences`, `LittleFS`, `ESPmDNS`.

## Quick start

```cpp
#include <WiFiProvision.h>
WiFiProvision wifi;

void setup() {
  Serial.begin(115200);
  wifi.begin("MyDevice");   // AP becomes "MyDevice-AABBCC"
}

void loop() {
  if (wifi.connected()) { /* your app */ }
}
```

First boot (no saved networks): connect to the **`MyDevice-AABBCC`** AP — the setup page opens
automatically. Pick or type a network, tap **Save & Connect**, done. It reconnects on later boots.

## PlatformIO setup

The portal UI ships as `web/` (the page, the `vovan-io_style` stylesheet and a favicon) and is
gzipped into your project's `data/` folder by a bundled pre-build hook, then flashed as the
LittleFS image. Drop your own pages or assets into a `web/` folder in your project and they are
packed and served the same way — that is how a device gets its control pages in the same style.

```ini
[env:esp32dev]
platform = espressif32
framework = arduino
board = esp32dev

board_build.filesystem = littlefs
board_build.partitions = default.csv          ; any scheme with a LittleFS/SPIFFS region

lib_deps = https://github.com/<you>/WiFiProvision.git   ; or symlink://... for local dev

; Gzip web/ -> data/*.gz before each filesystem build:
extra_scripts = pre:${platformio.libdeps_dir}/${this.__env__}/WiFiProvision/scripts/gzip_web.py
```

Build & flash (the filesystem step is needed once, and again whenever the UI changes):

```sh
pio run -t buildfs    # gzips the UI and builds the LittleFS image
pio run -t uploadfs   # flashes it
pio run -t upload     # flashes the firmware
```

> If you skip `uploadfs`, the device falls back to a compact built-in portal page so
> provisioning still works — just without the full styling.

See [`examples/Basic/platformio.ini`](examples/Basic/platformio.ini) for a ready env matrix
covering esp32dev / S3 / C3 / H2.

### Arduino IDE

Install the folder as a library, open an example, and use the **ESP32 Sketch Data Upload**
(LittleFS) tool with `web/portal.html` gzipped into `data/` — or rely on the built-in fallback
page (no data upload needed).

## API

```cpp
struct WPConfig {
  const char* deviceName        = "ESP32";  // AP SSID prefix -> "<name>-AABBCC"
  const char* apSsid            = nullptr;  // full AP SSID; overrides "<name>-AABBCC"
  const char* apPassword        = nullptr;  // null/"" = open AP; else WPA2 (>= 8 chars)
  uint32_t    connectTimeoutMs  = 15000;    // per-network STA attempt
  uint8_t     connectRounds     = 1;        // passes over the visible saved networks
  uint32_t    portalTimeoutMs   = 0;        // reserved
  bool        startPortalOnFail = true;     // raise portal if nothing connects at boot
  const char* hostname          = nullptr;  // mDNS/host name; default "<name>-aabbcc"
  uint32_t    reconnectIntervalMs = 30000;  // link lost: re-run the connect sequence after this
  bool        portalOnReconnectFail = true; // ...and raise the portal if that finds nothing
  bool        serverInSta       = true;     // false: HTTP server only while the portal is up
  bool        mdns              = true;     // announce hostname.local on connect
  bool        newestFirst       = false;    // new networks go to the top; a full list
                                            // drops its lowest-priority entry
  bool        importDriverConfig = false;   // empty list: adopt the driver's stored network
  UBaseType_t taskPriority      = 3;
};

void      begin(const char* deviceName);
void      begin(const WPConfig& cfg);
bool      connected() const;
WPState   state() const;                    // Connecting | Connected | Portal | Error
IPAddress ip() const;
String    apSsid() const;

void      requestPortal();                  // force the portal open at runtime (e.g. a button)
void      resetNetworks();                  // clear the saved list, then reboot

// Saved networks (index 0 = highest priority); thread-safe, persisted at once.
uint8_t   networkCount() const;
String    networkSsid(uint8_t index) const; // never exposes the password
int       addNetwork(const char* ssid, const char* pass);  // index, or -1
bool      removeNetwork(uint8_t index);
bool      moveNetwork(uint8_t index, bool up);
void      clearNetworks();                  // resetNetworks() without the reboot

void      end(uint32_t timeoutMs = 5000);   // stop task/portal/server/radio (before deep sleep)

// Custom portal fields (register before begin(); persisted to NVS):
void      addCustomField(const char* key, const char* defaultValue, const char* label = nullptr);
String    getCustomField(const char* key) const;
```

### Sharing port 80 with the application

With `serverInSta = false` the library's HTTP server (and its captive `onNotFound`)
runs only while the portal is up. Start your own server once `state() == Connected`
(the portal server is closed by then). Set `portalOnReconnectFail = false` as well, so a
lost link never raises the portal while your server holds the port. Manage the list
from your own UI through the network-list API above.

### Custom fields

```cpp
wifi.addCustomField("udp_port", "5005", "UDP Port");   // before begin()
wifi.begin("Sensor");
...
uint16_t port = wifi.getCustomField("udp_port").toInt();
```

They render in a **Device settings** section of the portal and survive reboots.

## How it works

- **AP + captive DNS** at `4.3.2.1` with the full set of OS probe endpoints
  (Apple/Android/Windows) so the login page pops automatically.
- **Async scan** endpoint (`/wifi/scan`) — the page polls and sorts by signal strength.
- **Storage:** a single NVS blob (`Preferences`, namespace `wifiprov`) holds the network list;
  array order *is* the priority, so reorder is a swap and delete is a splice.
- **Connect logic:** scan once, try visible saved networks in priority order
  (`connectRounds` passes), then the rest once; the first that connects wins. Falls back to
  the portal (configurable). A lost link gets `reconnectIntervalMs` for the driver's own
  auto-reconnect, then the whole sequence runs again.
- **UI:** [vovan-io_style](https://github.com/) design system, `data-theme="modern"` — the
  stylesheet is vendored verbatim into `web/vovan-io.css`, so components use its classes and
  tokens and nothing hardcodes a colour. Update it by re-copying the file from that repo.
- **Static files:** anything in `web/` is packed to `data/<name>.gz` and served from LittleFS;
  a request for `/x` resolves `/x.gz` first. The pre-build hook strips remote `@import` rules
  from CSS, since the AP has no internet and the font stacks fall back to system faces.
  A compact PROGMEM fallback page covers a missing filesystem image.

## Configuration macros

```cpp
-DWP_MAX_NETWORKS=10        // max saved networks (default 10)
-DWP_MAX_CUSTOM_FIELDS=8    // max custom fields (default 8)
```

## License

MIT.
