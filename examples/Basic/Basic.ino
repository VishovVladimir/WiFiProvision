// Basic WiFiProvision example.
//
// On first boot (no saved networks) the device raises a captive-portal AP named
// "MyDevice-XXXXXX". Connect a phone/laptop to it, the setup page opens
// automatically; pick or type a network, save, and the device connects as STA.
// Saved networks persist and reconnect on the next boot.
//
// Build & flash (PlatformIO):
//   pio run -t buildfs && pio run -t uploadfs   # portal UI into LittleFS (once)
//   pio run -t upload                            # firmware
#include <WiFiProvision.h>

WiFiProvision wifi;

void setup() {
  Serial.begin(115200);

  // Simplest form — AP/host name only, everything else default.
  wifi.begin("MyDevice");

  // Or configure explicitly:
  // WPConfig cfg;
  // cfg.deviceName = "MyDevice";
  // cfg.apPassword = "setup1234";   // >= 8 chars, or leave null for an open AP
  // cfg.connectTimeoutMs = 15000;
  // wifi.begin(cfg);
}

void loop() {
  // The library runs in its own task — loop() is free for your app.
  static bool announced = false;
  if (wifi.connected() && !announced) {
    announced = true;
    Serial.printf("Online! IP: %s\n", wifi.ip().toString().c_str());
  }
  delay(100);
}
