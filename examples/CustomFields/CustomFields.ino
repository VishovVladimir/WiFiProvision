// CustomFields example — add your own portal settings.
//
// Register text fields BEFORE begin(). They appear in a "Device settings"
// section of the captive portal, are persisted to NVS, and are readable at
// runtime with getCustomField(). Handy for app config like a server URL, a UDP
// port, an MQTT broker, etc. — the same pattern several projects re-implemented.
#include <WiFiProvision.h>

WiFiProvision wifi;

void setup() {
  Serial.begin(115200);

  wifi.addCustomField("udp_port", "5005", "UDP Port");
  wifi.addCustomField("mqtt", "mqtt://192.168.1.10", "MQTT Broker");

  wifi.begin("Sensor");
}

void loop() {
  static bool shown = false;
  if (wifi.connected() && !shown) {
    shown = true;
    Serial.printf("UDP port  : %s\n", wifi.getCustomField("udp_port").c_str());
    Serial.printf("MQTT broker: %s\n", wifi.getCustomField("mqtt").c_str());
  }
  delay(100);
}
