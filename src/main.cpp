// ====== Libraries (ESP32) ======
#include <WiFi.h>
#include <PubSubClient.h>
#include <NeoPixelBus.h>
#include <time.h>
#include <math.h>

// ====== WiFi & MQTT CONFIG ======
const char* WIFI_SSID     = "LatourNet";
const char* WIFI_PASSWORD = "Brandevoort11";

const char* MQTT_HOST     = "192.168.1.182";   // broker IP/host
const uint16_t MQTT_PORT  = 1883;
const char* MQTT_USER     = "addon";               // optional
const char* MQTT_PASS     = "addon";               // optional

// Base topics (change if you like)
const char* TOPIC_BASE              = "led-clock";
const char* TOPIC_CMD_SHOW_TEMP     = "led-clock/cmd/show_temperature";
const char* TOPIC_CMD_COLOR         = "led-clock/cmd/color";
const char* TOPIC_CMD_BRIGHTNESS    = "led-clock/cmd/brightness";
const char* TOPIC_TEMPERATURE       = "led-clock/temperature";

const char* TOPIC_STATE_ONLINE      = "led-clock/status";
const char* TOPIC_STATE_SHOW_TEMP   = "led-clock/show_temperature/state";
const char* LWT_TOPIC               = "led-clock/status";
const char* LWT_PAYLOAD             = "offline";

// New power + state topics
const char* TOPIC_CMD_POWER        = "led-clock/cmd/power";     // "ON"/"OFF"
const char* TOPIC_LIGHT_STATE      = "led-clock/light/state";   // "ON"/"OFF"
const char* TOPIC_BRIGHT_STATE     = "led-clock/light/brightness"; // "0..255"
const char* TOPIC_RGB_STATE        = "led-clock/light/rgb";     // "r,g,b"

// ===== MQTT Discovery topics (HA) =====
const char* DISC_LIGHT_CFG  = "homeassistant/light/led_clock_rgb/config";
const char* DISC_SWITCH_CFG = "homeassistant/switch/led_clock_showtemp/config";
const char* DISC_TEMP_CFG   = "homeassistant/sensor/led_clock_temperature/config";

// ====== LED STRIP CONFIG (ESP32) ======
static const uint16_t NUM_LEDS = 142;
static const uint8_t  PIN_DATA = 4; // GPIO4

// Prefer ESP32 RMT method for WS2812(x); fall back to generic 800kbps if needed
#if defined(ARDUINO_ARCH_ESP32)
  // Pick an RMT channel (0..7). Change 0 -> 1/2/... if you have conflicts.
  using NeoWsMethod = NeoEsp32Rmt0Ws2812xMethod;  // <- the correct WS2812(x) method on ESP32
#else
  using NeoWsMethod = Neo800KbpsMethod;
#endif

NeoPixelBus<NeoGrbFeature, NeoWsMethod> strip(NUM_LEDS, PIN_DATA);

// ====== RUNTIME STATE ======
bool showTemperature = false;
float lastTemperatureC = NAN;        // updated via MQTT
uint8_t brightness = 255;            // 0..255
RgbColor displayColor(255, 255, 255);// default white

// ====== MQTT/WIFI ======
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ====== TIMING ======
unsigned long lastRenderMs = 0;
const unsigned long RENDER_INTERVAL_MS = 50;

// ====== TIME (Europe/Amsterdam) ======
// TZ string: CET-1CEST,M3.5.0,M10.5.0/3 (standard EU DST rules)
const char* TZ_EU_AMS = "CET-1CEST,M3.5.0,M10.5.0/3";
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// ====== HELPERS ======
uint8_t lastNonZeroBrightness = 255;

void applyBrightness(uint8_t b) {
  if (b > 0) lastNonZeroBrightness = b;
  brightness = b;
}

void publishLightState() {
  mqtt.publish(TOPIC_LIGHT_STATE, (brightness == 0) ? "OFF" : "ON", true);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", brightness);
  mqtt.publish(TOPIC_BRIGHT_STATE, buf, true);
  char rgb[16];
  snprintf(rgb, sizeof(rgb), "%u,%u,%u", displayColor.R, displayColor.G, displayColor.B);
  mqtt.publish(TOPIC_RGB_STATE, rgb, true);
}

void handlePowerPayload(char* payload)
{
  if (!strcasecmp(payload, "ON")) {
    if (brightness == 0) applyBrightness(lastNonZeroBrightness == 0 ? 255 : lastNonZeroBrightness);
  } else if (!strcasecmp(payload, "OFF")) {
    applyBrightness(0);
  }
  publishLightState();
}



inline void safeSet(uint16_t idx, const RgbColor& c)
{
  if (idx < NUM_LEDS) {
    RgbColor scaled(
      (uint8_t)((uint16_t)c.R * brightness / 255),
      (uint8_t)((uint16_t)c.G * brightness / 255),
      (uint8_t)((uint16_t)c.B * brightness / 255)
    );
    strip.SetPixelColor(idx, scaled);
  }
}

// Build a stable device/unique_id from MAC
String uniqueIdBase() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[17];
  snprintf(buf, sizeof(buf), "%04X%08X", (uint32_t)(mac>>32), (uint32_t)mac);
  return String("led_clock_") + buf;
}

void publishDiscovery() {
  String uid = uniqueIdBase();

  // ---- LIGHT (optimistic; uses your existing command topics) ----
  // Uses rgb_command_topic with "R,G,B" and brightness_command_topic 0..255
  String lightPayload = "{"
      "\"name\":\"LED Clock\","
      "\"unique_id\":\"" + uid + "_light\","
      "\"command_topic\":\"" + String(TOPIC_CMD_POWER) + "\","
      "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
      "\"state_topic\":\"" + String(TOPIC_LIGHT_STATE) + "\","
      "\"brightness_command_topic\":\"" + String(TOPIC_CMD_BRIGHTNESS) + "\","
      "\"brightness_state_topic\":\"" + String(TOPIC_BRIGHT_STATE) + "\","
      "\"brightness_scale\":255,"
      "\"rgb_command_topic\":\"" + String(TOPIC_CMD_COLOR) + "\","
      "\"rgb_state_topic\":\"" + String(TOPIC_RGB_STATE) + "\","
      "\"availability_topic\":\"" + String(TOPIC_STATE_ONLINE) + "\","
      "\"payload_available\":\"online\","
      "\"payload_not_available\":\"offline\","
      "\"optimistic\":false,"
      "\"device\":{"
        "\"identifiers\":[\"" + uid + "\"],"
        "\"name\":\"LED Clock\",\"model\":\"ESP32 LED 7-seg Clock\",\"manufacturer\":\"Custom\"}"
    "}";

  // ---- SWITCH (show temperature) ----
  String switchPayload = "{"
    "\"name\":\"Show Temperature\","
    "\"unique_id\":\"" + uid + "_showtemp\","
    "\"command_topic\":\"" + String(TOPIC_CMD_SHOW_TEMP) + "\","
    "\"state_topic\":\"" + String(TOPIC_STATE_SHOW_TEMP) + "\","
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
    "\"availability_topic\":\"" + String(TOPIC_STATE_ONLINE) + "\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\","
    "\"device\":{"
      "\"identifiers\":[\"" + uid + "\"],"
      "\"name\":\"LED Clock\","
      "\"model\":\"ESP32 LED 7-seg Clock\","
      "\"manufacturer\":\"Custom\""
    "}"
  "}";

  // ---- SENSOR (temperature °C) ----
String numberPayload = "{"
  "\"name\":\"Clock Temperature\","
  "\"unique_id\":\"" + uid + "_temp_set\","
  "\"command_topic\":\"" + String(TOPIC_TEMPERATURE) + "\","
  "\"state_topic\":\"" + String(TOPIC_TEMPERATURE) + "\","
  "\"min\":-20,\"max\":50,\"step\":0.1,"
  "\"unit_of_measurement\":\"°C\","
  "\"device_class\":\"temperature\","
  "\"mode\":\"box\","
  "\"availability_topic\":\"" + String(TOPIC_STATE_ONLINE) + "\","
  "\"payload_available\":\"online\","
  "\"payload_not_available\":\"offline\","
  "\"device\":{"
    "\"identifiers\":[\"" + uid + "\"],"
    "\"name\":\"LED Clock\",\"model\":\"ESP32 LED 7-seg Clock\",\"manufacturer\":\"Custom\"}"
"}";

  mqtt.publish(DISC_LIGHT_CFG,  lightPayload.c_str(),  true);
  mqtt.publish(DISC_SWITCH_CFG, switchPayload.c_str(), true);
  mqtt.publish("homeassistant/number/led_clock_temperature/config", numberPayload.c_str(), true);
}

const bool seg_on[10][7] = {
  { true,  true,  true,  true,  true,  true,  false},  // 0
  { true,  false, false, false, false, true,  false},  // 1
  { false, true,  true,  false, true,  true,  true },  // 2
  { true,  true,  false, false, true,  true,  true },  // 3
  { true,  false, false, true,  false, true,  true },  // 4
  { true,  true,  false, true,  true,  false, true },  // 5
  { true,  true,  true,  true,  true,  false, true },  // 6
  { true,  false, false, false, true,  true,  false},  // 7
  { true,  true,  true,  true,  true,  true,  true },  // 8
  { true,  true,  false, true,  true,  true,  true }   // 9
};

// each segment is a block of 5 LEDs; segments are laid out consecutively
void drawSegmentBlock(int start, const RgbColor& col)
{
  for (int i = start; i < start + 5; i++) safeSet(i, col);
}

void drawDigit(int digit, int offset, const RgbColor& col)
{
  if (digit < 0 || digit > 9) return;
  for (int seg = 0; seg < 7; seg++) {
    if (seg_on[digit][seg]) drawSegmentBlock(offset + seg * 5, col);
  }
}

void drawMinus(int offset, const RgbColor& col)
{
  // "g" segment only (7th, index 6)
  drawSegmentBlock(offset + 6 * 5, col);
}

void clearAll()
{
  for (uint16_t i = 0; i < NUM_LEDS; i++) strip.SetPixelColor(i, 0);
}

// returns local time components; if time not set yet, returns false
bool getLocalTimeSafe(struct tm* info)
{
  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) return false; // crude "time not set" guard
  localtime_r(&now, info);
  return true;
}

// ====== MQTT CALLBACK ======
void handleColorPayload(char* payload)
{
  int r=0,g=0,b=0;
  if (sscanf(payload, "%d,%d,%d", &r,&g,&b) == 3) {
    displayColor = RgbColor(constrain(r,0,255), constrain(g,0,255), constrain(b,0,255));
    publishLightState();
  }
}

void handleBrightnessPayload(char* payload)
{
  long val = strtol(payload, nullptr, 10);
  val = constrain(val, 0, 255);
  applyBrightness((uint8_t)val);
  publishLightState();
}

void handleShowTempPayload(char* payload)
{
  // ON / OFF
  if (!strcasecmp(payload, "ON"))  showTemperature = true;
  if (!strcasecmp(payload, "OFF")) showTemperature = false;

  // publish new state
  mqtt.publish(TOPIC_STATE_SHOW_TEMP, showTemperature ? "ON" : "OFF", true);
}

void handleTemperaturePayload(char* payload)
{
  // numeric °C
  lastTemperatureC = atof(payload);
}

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  static char buf[64];
  unsigned int len = min(length, (unsigned int)(sizeof(buf)-1));
  memcpy(buf, payload, len); buf[len] = '\0';

  if (!strcmp(topic, TOPIC_CMD_POWER))          handlePowerPayload(buf);
  else if (!strcmp(topic, TOPIC_CMD_COLOR))     handleColorPayload(buf);
  else if (!strcmp(topic, TOPIC_CMD_BRIGHTNESS))handleBrightnessPayload(buf);
  else if (!strcmp(topic, TOPIC_CMD_SHOW_TEMP)) handleShowTempPayload(buf);
  else if (!strcmp(topic, TOPIC_TEMPERATURE))   handleTemperaturePayload(buf);
}

// ====== CONNECT WIFI & MQTT ======
void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

String makeClientId()
{
  uint64_t mac = ESP.getEfuseMac(); // unique per ESP32
  char buf[32];
  snprintf(buf, sizeof(buf), "led-clock-%08X%08X",
           (uint32_t)(mac >> 32), (uint32_t)mac);
  return String(buf);
}

void connectMQTT()
{
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  while (!mqtt.connected()) {
    String clientId = makeClientId();
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                     LWT_TOPIC, 0, true, LWT_PAYLOAD)) {
      mqtt.publish(TOPIC_STATE_ONLINE, "online", true);
      mqtt.publish(TOPIC_STATE_SHOW_TEMP, showTemperature ? "ON" : "OFF", true);

      mqtt.subscribe(TOPIC_CMD_COLOR);
      mqtt.subscribe(TOPIC_CMD_BRIGHTNESS);
      mqtt.subscribe(TOPIC_CMD_SHOW_TEMP);
      mqtt.subscribe(TOPIC_TEMPERATURE);

      publishDiscovery(); // <--- add this
    } else {
      delay(1000);
    }
  }
}


// ====== SETUP/LOOP ======
void setup()
{
  // LEDs
  strip.Begin();
  strip.Show();

  mqtt.setBufferSize(1024);

  // WiFi + MQTT
  connectWiFi();
  connectMQTT();

  mqtt.subscribe(TOPIC_CMD_POWER);
  // Time (ESP32 uses configTzTime)
  configTzTime(TZ_EU_AMS, NTP1, NTP2);

  delay(500); // let things settle
  mqtt.publish(TOPIC_STATE_ONLINE, "online", true);
  publishLightState();
  mqtt.publish(TOPIC_STATE_SHOW_TEMP, showTemperature ? "ON" : "OFF", true);
}

void renderClock()
{
  clearAll();

  struct tm lt;
  if (!getLocalTimeSafe(&lt)) {
    // time not ready: just keep blank
    strip.Show();
    return;
  }

  int hour = lt.tm_hour;
  int minute = lt.tm_min;
  int second = lt.tm_sec;

  // Offsets matching your ESPHome logic
  drawDigit(hour   / 10, 107, displayColor);
  drawDigit(hour   % 10,  72, displayColor);
  drawDigit(minute / 10,  35, displayColor);
  drawDigit(minute % 10,   0, displayColor);

  // Blinking colon (LEDs 70, 71)
  RgbColor col = (second % 2 == 0) ? displayColor : RgbColor(0,0,0);
  safeSet(70, col);
  safeSet(71, col);

  strip.Show();
}

void renderTemperature()
{
  clearAll();

  // If we don't yet have a value, keep display blank
  if (isnan(lastTemperatureC)) {
    strip.Show();
    return;
  }

  int t = (int)lroundf(lastTemperatureC);

  // Handle negative: draw minus at tens position if needed
  if (t < 0) {
    t = -t;
    drawMinus(35, displayColor); // minus in tens slot
    drawDigit(t % 10, 0, displayColor);
  } else {
    // two digits: tens and ones
    drawDigit((t / 10) % 10, 35, displayColor);
    drawDigit(t % 10, 0, displayColor);
  }

  strip.Show();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastRenderMs >= RENDER_INTERVAL_MS) {
    lastRenderMs = now;
    if (showTemperature) {
      renderTemperature();
    } else {
      renderClock();
    }
  }
}
