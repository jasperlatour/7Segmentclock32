// ====== Libraries (ESP32) ======
#include <WiFi.h>
#include <PubSubClient.h>
#include <NeoPixelBus.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

// ====== WiFi & MQTT CONFIG ======
const char* WIFI_SSID     = "ssid";
const char* WIFI_PASSWORD = "password";

const char* MQTT_HOST     = "192.168.1.182";   // broker IP/host
const uint16_t MQTT_PORT  = 1883;
const char* MQTT_USER     = "addon";               // optional
const char* MQTT_PASS     = "addon";               // optional

// Base topics (change if you like)
const char* TOPIC_BASE              = "led-clock";
const char* TOPIC_CMD_COLOR         = "led-clock/cmd/color";
const char* TOPIC_CMD_BRIGHTNESS    = "led-clock/cmd/brightness";

const char* TOPIC_STATE_ONLINE      = "led-clock/status";
const char* LWT_TOPIC               = "led-clock/status";
const char* LWT_PAYLOAD             = "offline";

// New power + state topics
const char* TOPIC_CMD_POWER        = "led-clock/cmd/power";     // "ON"/"OFF"
const char* TOPIC_LIGHT_STATE      = "led-clock/light/state";   // "ON"/"OFF"
const char* TOPIC_BRIGHT_STATE     = "led-clock/light/brightness"; // "0..255"
const char* TOPIC_RGB_STATE        = "led-clock/light/rgb";     // "r,g,b"

// ---- Custom text/number (MQTT) ----
// Send up to 4 characters (0-9, A-Z, '-', space). Empty payload reverts
// the display back to the clock/temperature mode.
const char* TOPIC_CMD_TEXT         = "led-clock/cmd/text";
const char* TOPIC_TEXT_STATE       = "led-clock/text/state";

// ===== MQTT Discovery topics (HA) =====
const char* DISC_LIGHT_CFG  = "homeassistant/light/led_clock_rgb/config";
const char* DISC_TEXT_CFG   = "homeassistant/text/led_clock_text/config";

// Old entities removed from the firmware - published empty (retained) once
// on connect so Home Assistant drops any leftover discovered entities/state.
const char* DISC_REMOVED_CFGS[] = {
  "homeassistant/switch/led_clock_showtemp/config",
  "homeassistant/number/led_clock_temperature/config",
};
const char* RETAINED_REMOVED_TOPICS[] = {
  "led-clock/show_temperature/state",
  "led-clock/temperature",
};

// ---- Effects (MQTT) ----
const char* TOPIC_CMD_EFFECT   = "led-clock/cmd/effect";     // payload: effect name (see effectName())
const char* TOPIC_EFFECT_STATE = "led-clock/light/effect";   // current effect name


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
uint8_t brightness = 150;            // 0..255 (target, ramped smoothly - see smBrightness)
RgbColor displayColor(255, 255, 255);// default white (target, ramped smoothly - see smR/smG/smB)

char customText[5] = "";             // raw (unpadded) text, up to 4 chars + null
bool useCustomText = false;

// ====== MQTT/WIFI ======
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ====== TIMING ======
unsigned long lastRenderMs = 0;
const unsigned long RENDER_INTERVAL_MS = 20; // ~50 fps - smooth animation without overloading RMT

// ====== TIME (Europe/Amsterdam) ======
// TZ string: CET-1CEST,M3.5.0,M10.5.0/3 (standard EU DST rules)
const char* TZ_EU_AMS = "CET-1CEST,M3.5.0,M10.5.0/3";
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

enum EffectType : uint8_t {
  EFFECT_SOLID,
  EFFECT_PULSE,
  EFFECT_BREATHE,
  EFFECT_STROBE,
  EFFECT_COLOR_WIPE,
  EFFECT_COMET,
  EFFECT_THEATER_CHASE,
  EFFECT_TWINKLE,
  EFFECT_CONFETTI,
  EFFECT_FIRE,
  EFFECT_RAINBOW,
  EFFECT_RAINBOW_FADE,
  EFFECT_SNAKE
};
volatile EffectType currentEffect = EFFECT_SOLID;

uint32_t effectStartMs = 0;
uint32_t effectLastMs  = 0;
float effectMul = 1.0f;       // brightness multiplier for pulse/breathe/strobe
int   wipePos   = 0;          // position for color wipe (0..NUM_LEDS-1)
float cometPos  = 0.0f;       // position for comet (0..NUM_LEDS)

const uint32_t PULSE_PERIOD_MS        = 1800;
const uint32_t BREATHE_PERIOD_MS      = 4200;
const uint32_t STROBE_PERIOD_MS       = 1000;
const uint32_t STROBE_ON_MS           = 60;
const uint16_t WIPE_SPEED_MS          = 12;    // lower = faster
const float    COMET_SPEED_PPS        = 60.0f; // pixels/second
const float    COMET_TAIL             = 18.0f; // tail length in pixels
const uint16_t CHASE_INTERVAL_MS      = 100;
const uint32_t TWINKLE_BUCKET_MS      = 220;   // how often twinkle/confetti re-roll sparkles
const float    TWINKLE_DENSITY        = 0.12f; // fraction of lit pixels sparkling per bucket
const uint16_t RAINBOW_SPEED          = 30;    // hue step per frame
const uint32_t RAINBOW_FADE_PERIOD_MS = 30000;

// Snake: display sits static (like Solid) except for a brief moment right
// when the displayed minute changes, when a bright trail sweeps once across
// the whole strip. Unlike the other effects it does NOT run continuously.
const uint32_t SNAKE_DURATION_MS = 900;  // time for the trail to cross the strip
const float    SNAKE_TAIL        = 20.0f; // trail length in pixels
int      lastClockValue = -1;   // hour*60+minute last shown, -1 = not yet initialized
bool     snakeActive    = false;
uint32_t snakeStartMs   = 0;

// Map effect enum <-> name
const char* effectName(EffectType e) {
  switch (e) {
    case EFFECT_SOLID:         return "Solid";
    case EFFECT_PULSE:         return "Pulse";
    case EFFECT_BREATHE:       return "Breathe";
    case EFFECT_STROBE:        return "Strobe";
    case EFFECT_COLOR_WIPE:    return "Color Wipe";
    case EFFECT_COMET:         return "Comet";
    case EFFECT_THEATER_CHASE: return "Theater Chase";
    case EFFECT_TWINKLE:       return "Twinkle";
    case EFFECT_CONFETTI:      return "Confetti";
    case EFFECT_FIRE:          return "Fire";
    case EFFECT_RAINBOW:       return "Rainbow";
    case EFFECT_RAINBOW_FADE:  return "Rainbow Fade";
    case EFFECT_SNAKE:         return "Snake";
  }
  return "Solid";
}

EffectType effectFromName(const char* s) {
  if (!strcasecmp(s, "Pulse"))         return EFFECT_PULSE;
  if (!strcasecmp(s, "Breathe"))       return EFFECT_BREATHE;
  if (!strcasecmp(s, "Strobe"))        return EFFECT_STROBE;
  if (!strcasecmp(s, "Color Wipe"))    return EFFECT_COLOR_WIPE;
  if (!strcasecmp(s, "Comet"))         return EFFECT_COMET;
  if (!strcasecmp(s, "Theater Chase")) return EFFECT_THEATER_CHASE;
  if (!strcasecmp(s, "Twinkle"))       return EFFECT_TWINKLE;
  if (!strcasecmp(s, "Confetti"))      return EFFECT_CONFETTI;
  if (!strcasecmp(s, "Fire"))          return EFFECT_FIRE;
  if (!strcasecmp(s, "Rainbow"))       return EFFECT_RAINBOW;
  if (!strcasecmp(s, "Rainbow Fade"))  return EFFECT_RAINBOW_FADE;
  if (!strcasecmp(s, "Snake"))         return EFFECT_SNAKE;
  return EFFECT_SOLID;
}

void publishEffectState() {
  mqtt.publish(TOPIC_EFFECT_STATE, effectName(currentEffect), true);
}

// ====== SMOOTHING (color/brightness ramps so power/color/brightness changes fade, not snap) ======
float smR = 255.0f, smG = 255.0f, smB = 255.0f; // smoothed color, eases toward displayColor
float smBrightness = 150.0f;                    // smoothed brightness, eases toward `brightness`
const float SMOOTH_RATE = 0.15f;                // per-tick ease factor (~650ms to settle at 20ms/tick)

void updateSmoothing() {
  smR += ((float)displayColor.R - smR) * SMOOTH_RATE;
  smG += ((float)displayColor.G - smG) * SMOOTH_RATE;
  smB += ((float)displayColor.B - smB) * SMOOTH_RATE;
  smBrightness += ((float)brightness - smBrightness) * SMOOTH_RATE;
}

// ====== FRAME BUFFER + CROSSFADE ======
// Effects/render functions write into frameBuf; commitFrame() pushes it to the
// strip. When the displayed content changes in a way that would otherwise look
// like a hard "jump" (a real effect switch, or a display-mode switch), we
// crossfade from whatever was last actually shown into the new frame instead
// of a jump. This is also what keeps the animation phase continuous rather
// than restarting the effect from the beginning every time a redundant MQTT
// command re-arrives (e.g. a Home Assistant presence automation).
RgbColor frameBuf[NUM_LEDS];
RgbColor lastShown[NUM_LEDS];
RgbColor crossfadeFrom[NUM_LEDS];
bool crossfadeActive = false;
uint32_t crossfadeStartMs = 0;
const uint16_t CROSSFADE_MS = 350;

inline uint8_t lerp8(uint8_t a, uint8_t b, float t) {
  return (uint8_t)((float)a + ((float)b - (float)a) * t);
}

void startCrossfade() {
  memcpy(crossfadeFrom, lastShown, sizeof(lastShown));
  crossfadeActive = true;
  crossfadeStartMs = millis();
}

void commitFrame() {
  if (crossfadeActive) {
    uint32_t now = millis();
    float t = (float)(now - crossfadeStartMs) / (float)CROSSFADE_MS;
    if (t >= 1.0f) { t = 1.0f; crossfadeActive = false; }
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
      RgbColor mixed(
        lerp8(crossfadeFrom[i].R, frameBuf[i].R, t),
        lerp8(crossfadeFrom[i].G, frameBuf[i].G, t),
        lerp8(crossfadeFrom[i].B, frameBuf[i].B, t)
      );
      strip.SetPixelColor(i, mixed);
      lastShown[i] = mixed;
    }
  } else {
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
      strip.SetPixelColor(i, frameBuf[i]);
      lastShown[i] = frameBuf[i];
    }
  }
  strip.Show();
}

// Draws the Snake sweep (if currently mid-flight) as a bright trail added on
// top of whatever renderClock() already drew into frameBuf this frame. Only
// called while snakeActive is true, which renderClock() sets for
// SNAKE_DURATION_MS right after it notices the displayed minute changed.
void applySnakeOverlay(uint32_t nowMs) {
  if (!snakeActive) return;
  uint32_t elapsed = nowMs - snakeStartMs;
  if (elapsed >= SNAKE_DURATION_MS) { snakeActive = false; return; }

  // headPos runs from -SNAKE_TAIL to NUM_LEDS so the trail fully enters and
  // clears the strip within the animation - a clean one-shot sweep.
  float headPos = ((float)elapsed / (float)SNAKE_DURATION_MS) * (NUM_LEDS + SNAKE_TAIL) - SNAKE_TAIL;
  uint8_t br = (uint8_t)smBrightness;

  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    float dist = headPos - (float)i;
    if (dist < 0.0f || dist > SNAKE_TAIL) continue;
    float env = (1.0f - dist / SNAKE_TAIL) * ((float)br / 255.0f);
    uint16_t nr = frameBuf[i].R + (uint16_t)(255 * env);
    uint16_t ng = frameBuf[i].G + (uint16_t)(255 * env);
    uint16_t nb = frameBuf[i].B + (uint16_t)(255 * env);
    frameBuf[i] = RgbColor((uint8_t)min(nr, (uint16_t)255),
                            (uint8_t)min(ng, (uint16_t)255),
                            (uint8_t)min(nb, (uint16_t)255));
  }
}

// ====== HELPERS ======
uint8_t lastNonZeroBrightness = 255;

void applyBrightness(uint8_t b) {
  if (b > 0) lastNonZeroBrightness = b;
  brightness = b;
}

inline RgbColor hueToRgb(uint16_t hueDeg)
{
  // hueDeg 0..359
  float h = (hueDeg % 360) / 60.0f;   // sector 0..5
  int i = (int)floor(h);
  float f = h - i;
  float p = 0.0f;     // saturation=1, value=1 -> p=0
  float q = 1.0f - f;
  float t = f;

  float r_f, g_f, b_f;
  switch (i % 6) {
    case 0: r_f = 1.0f; g_f = t;     b_f = p; break;
    case 1: r_f = q;    g_f = 1.0f;  b_f = p; break;
    case 2: r_f = p;    g_f = 1.0f;  b_f = t; break;
    case 3: r_f = p;    g_f = q;     b_f = 1.0f; break;
    case 4: r_f = t;    g_f = p;     b_f = 1.0f; break;
    case 5: default: r_f = 1.0f; g_f = p; b_f = q; break;
  }

  return RgbColor((uint8_t)round(r_f * 255.0f),
                  (uint8_t)round(g_f * 255.0f),
                  (uint8_t)round(b_f * 255.0f));
}

// Deterministic per-pixel pseudo-random (no persistent arrays needed for
// twinkle/confetti/fire - keeps effects stateless and jitter-free).
inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
inline float hashf(uint32_t a, uint32_t b = 0) {
  return (hash32(a * 2654435761u + b * 40503u) & 0xFFFFFFu) / (float)0xFFFFFFu;
}

// Triangular attack/decay envelope within a twinkle "bucket" (0..1..0).
inline float twinkleEnvelope(uint32_t nowRel) {
  uint32_t within = nowRel % TWINKLE_BUCKET_MS;
  float frac = (float)within / (float)TWINKLE_BUCKET_MS;
  return 1.0f - fabsf(2.0f * frac - 1.0f);
}

// black -> red -> orange/yellow -> white palette for the Fire effect
inline RgbColor heatToColor(float h) {
  if (h < 0.0f) h = 0.0f; if (h > 1.0f) h = 1.0f;
  uint8_t r, g, b;
  if (h < 0.5f) {
    float t = h / 0.5f;
    r = (uint8_t)(255 * t); g = 0; b = 0;
  } else if (h < 0.8f) {
    float t = (h - 0.5f) / 0.3f;
    r = 255; g = (uint8_t)(180 * t); b = 0;
  } else {
    float t = (h - 0.8f) / 0.2f;
    r = 255; g = (uint8_t)(180 + 75 * t); b = (uint8_t)(80 * t);
  }
  return RgbColor(r, g, b);
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

void handleEffectPayload(char* payload)
{
  EffectType e = effectFromName(payload);
  // Only reset the animation phase (and crossfade) on a REAL change.
  // Home Assistant re-sends the current effect on every light.turn_on call
  // (e.g. a presence-triggered automation), which used to restart the
  // animation from the beginning every time and caused a visible jitter.
  if (e != currentEffect) {
    currentEffect = e;
    effectStartMs = millis();
    effectLastMs  = effectStartMs;
    wipePos = 0;
    // Re-arm Snake so switching into it doesn't immediately fire a sweep -
    // it should wait for the next real minute change.
    lastClockValue = -1;
    snakeActive = false;
    startCrossfade();
  }
  publishEffectState();
}

inline void safeSet(uint16_t idx, const RgbColor& base)
{
  if (idx >= NUM_LEDS) return;

  uint32_t now = millis();
  RgbColor out;

  switch (currentEffect) {
    case EFFECT_RAINBOW: {
      uint32_t phase = ((now - effectStartMs) / RAINBOW_SPEED) % 360u;
      out = hueToRgb((idx * 3u + phase) % 360u);
      break;
    }
    case EFFECT_RAINBOW_FADE: {
      uint32_t phase = (uint32_t)(((now - effectStartMs) % RAINBOW_FADE_PERIOD_MS) * 360ULL / RAINBOW_FADE_PERIOD_MS);
      out = hueToRgb((uint16_t)phase);
      break;
    }
    case EFFECT_COMET: {
      float dist = cometPos - (float)idx;
      if (dist < 0) dist += NUM_LEDS;
      float env = (dist <= COMET_TAIL) ? (1.0f - dist / COMET_TAIL) : 0.0f;
      out = RgbColor((uint8_t)(base.R * env), (uint8_t)(base.G * env), (uint8_t)(base.B * env));
      break;
    }
    case EFFECT_THEATER_CHASE: {
      uint32_t nowRel = now - effectStartMs;
      int phase = (int)((nowRel / CHASE_INTERVAL_MS) % 3);
      out = (((int)idx + phase) % 3 == 0) ? base : RgbColor(0, 0, 0);
      break;
    }
    case EFFECT_TWINKLE: {
      uint32_t nowRel = now - effectStartMs;
      uint32_t bucket = nowRel / TWINKLE_BUCKET_MS;
      float r = hashf(idx, bucket);
      if (r < TWINKLE_DENSITY) {
        float env = twinkleEnvelope(nowRel);
        out = RgbColor((uint8_t)(base.R * env), (uint8_t)(base.G * env), (uint8_t)(base.B * env));
      } else {
        out = RgbColor(0, 0, 0);
      }
      break;
    }
    case EFFECT_CONFETTI: {
      uint32_t nowRel = now - effectStartMs;
      uint32_t bucket = nowRel / TWINKLE_BUCKET_MS;
      float r = hashf(idx, bucket * 7u + 1u);
      if (r < TWINKLE_DENSITY) {
        float env = twinkleEnvelope(nowRel);
        uint16_t hue = (uint16_t)(hashf(idx, bucket * 13u + 3u) * 360.0f);
        RgbColor hc = hueToRgb(hue);
        out = RgbColor((uint8_t)(hc.R * env), (uint8_t)(hc.G * env), (uint8_t)(hc.B * env));
      } else {
        out = RgbColor(0, 0, 0);
      }
      break;
    }
    case EFFECT_FIRE: {
      float t = (now - effectStartMs) / 1000.0f;
      float n = 0.5f * (sinf(idx * 0.35f + t * 2.2f) + sinf(idx * 0.09f - t * 3.7f));
      n = (n + 1.0f) * 0.5f;
      float flick = hashf(idx, (uint32_t)(t * 8.0f)) * 0.35f;
      float heatv = n * 0.65f + flick;
      out = heatToColor(heatv);
      break;
    }
    default: {
      // Solid, Pulse, Breathe, Strobe, Color Wipe (base color scaled by effectMul)
      uint16_t r = (uint16_t)base.R * effectMul;
      uint16_t g = (uint16_t)base.G * effectMul;
      uint16_t b = (uint16_t)base.B * effectMul;
      out = RgbColor((uint8_t)r, (uint8_t)g, (uint8_t)b);
      break;
    }
  }

  // global smoothed brightness (0..255)
  uint8_t br = (uint8_t)smBrightness;
  RgbColor scaled(
    (uint8_t)((uint16_t)out.R * br / 255),
    (uint8_t)((uint16_t)out.G * br / 255),
    (uint8_t)((uint16_t)out.B * br / 255)
  );

  // Color Wipe mask
  if (currentEffect == EFFECT_COLOR_WIPE && (int)idx > wipePos) {
    scaled = RgbColor(0);
  }

  frameBuf[idx] = scaled;
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
    "\"effect_command_topic\":\"" + String(TOPIC_CMD_EFFECT) + "\","
    "\"effect_state_topic\":\"" + String(TOPIC_EFFECT_STATE) + "\","
    "\"effect_list\":[\"Solid\",\"Pulse\",\"Breathe\",\"Strobe\",\"Color Wipe\",\"Comet\",\"Theater Chase\",\"Twinkle\",\"Confetti\",\"Fire\",\"Rainbow\",\"Rainbow Fade\",\"Snake\"],"
    "\"availability_topic\":\"" + String(TOPIC_STATE_ONLINE) + "\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\","
    "\"optimistic\":false,"
    "\"device\":{"
      "\"identifiers\":[\"" + uid + "\"],"
      "\"name\":\"LED Clock\",\"model\":\"ESP32 LED 7-seg Clock\",\"manufacturer\":\"Custom\"}"
  "}";

  // ---- TEXT (custom text/number, up to 4 chars) ----
  String textPayload = "{"
    "\"name\":\"Clock Text\","
    "\"unique_id\":\"" + uid + "_text\","
    "\"command_topic\":\"" + String(TOPIC_CMD_TEXT) + "\","
    "\"state_topic\":\"" + String(TOPIC_TEXT_STATE) + "\","
    "\"max\":4,"
    "\"mode\":\"text\","
    "\"availability_topic\":\"" + String(TOPIC_STATE_ONLINE) + "\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\","
    "\"device\":{"
      "\"identifiers\":[\"" + uid + "\"],"
      "\"name\":\"LED Clock\",\"model\":\"ESP32 LED 7-seg Clock\",\"manufacturer\":\"Custom\"}"
  "}";

    mqtt.publish(DISC_LIGHT_CFG,  lightPayload.c_str(),  true);
    mqtt.publish(DISC_TEXT_CFG,   textPayload.c_str(),   true);
  }

// One-time cleanup so Home Assistant drops the temperature switch/number
// entities (and their retained state) that older firmware versions created.
void cleanupRemovedEntities() {
  for (const char* topic : DISC_REMOVED_CFGS)     mqtt.publish(topic, "", true);
  for (const char* topic : RETAINED_REMOVED_TOPICS) mqtt.publish(topic, "", true);
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
  for (int i = start; i < start + 5; i++) {
    safeSet(i, col);
  }
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

// Draws a single character (digit, a best-effort 7-segment letter, '-' or
// space) at a digit slot. Lets the custom-text mode show more than numbers.
// local segment index -> standard 7-seg label, derived from seg_on[] above:
//   local[0]=c local[1]=d local[2]=e local[3]=f local[4]=a local[5]=b local[6]=g
void drawChar(char c, int offset, const RgbColor& col)
{
  if (c >= '0' && c <= '9') { drawDigit(c - '0', offset, col); return; }

  bool a=false,b=false,cc=false,d=false,e=false,f=false,g=false;
  switch (toupper((unsigned char)c)) {
    case 'A': a=b=cc=e=f=g=true; break;
    case 'B': cc=d=e=f=g=true; break;            // lowercase-style b (avoids clash with '8')
    case 'C': a=d=e=f=true; break;
    case 'D': b=cc=d=e=g=true; break;             // lowercase-style d (avoids clash with '0')
    case 'E': a=d=e=f=g=true; break;
    case 'F': a=e=f=g=true; break;
    case 'G': a=cc=d=e=f=true; break;
    case 'H': b=cc=e=f=g=true; break;
    case 'I': b=cc=true; break;
    case 'J': b=cc=d=true; break;
    case 'L': d=e=f=true; break;
    case 'N': cc=e=g=true; break;                 // lowercase-style n
    case 'O': a=b=cc=d=e=f=true; break;
    case 'P': a=b=e=f=g=true; break;
    case 'Q': a=b=cc=f=g=true; break;
    case 'R': e=g=true; break;                    // lowercase-style r
    case 'S': a=cc=d=f=g=true; break;
    case 'T': d=e=f=g=true; break;                // lowercase-style t
    case 'U': b=cc=d=e=f=true; break;
    case 'Y': b=cc=d=f=g=true; break;
    case 'Z': a=b=d=e=g=true; break;
    case '-': g=true; break;
    case '_': d=true; break;
    case ' ': default: break;                     // blank
  }

  bool local[7] = { cc, d, e, f, a, b, g };
  for (int seg = 0; seg < 7; seg++) {
    if (local[seg]) drawSegmentBlock(offset + seg * 5, col);
  }
}

void clearAll()
{
  for (uint16_t i = 0; i < NUM_LEDS; i++) frameBuf[i] = RgbColor(0, 0, 0);
}

// returns local time components; if time not set yet, returns false
bool getLocalTimeSafe(struct tm* info)
{
  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) return false; // crude "time not set" guard
  localtime_r(&now, info);
  return true;
}



void updateEffect()
{
  uint32_t now = millis();
  uint32_t dt  = now - effectLastMs;
  effectLastMs = now;

  switch (currentEffect) {
    case EFFECT_SOLID:
      effectMul = 1.0f;
      break;

    case EFFECT_PULSE: {
      float phase = (float)(now - effectStartMs) / (float)PULSE_PERIOD_MS;
      float s = 0.5f * (sinf(phase * 2.0f * PI) + 1.0f); // 0..1
      effectMul = 0.3f + 0.7f * s;
      break;
    }

    case EFFECT_BREATHE: {
      float phase = (float)(now - effectStartMs) / (float)BREATHE_PERIOD_MS;
      float s = 0.5f * (sinf(phase * 2.0f * PI - PI / 2.0f) + 1.0f); // 0..1, starts near 0
      effectMul = 0.05f + 0.95f * s;
      break;
    }

    case EFFECT_STROBE: {
      uint32_t nowRel = (now - effectStartMs) % STROBE_PERIOD_MS;
      effectMul = (nowRel < STROBE_ON_MS) ? 1.0f : 0.0f;
      break;
    }

    case EFFECT_COLOR_WIPE:
      if (dt >= WIPE_SPEED_MS) {
        wipePos += (dt / WIPE_SPEED_MS);
        if (wipePos >= (int)NUM_LEDS) { wipePos = 0; effectStartMs = now; }
      }
      effectMul = 1.0f;
      break;

    case EFFECT_COMET: {
      float dtSec = dt / 1000.0f;
      cometPos += COMET_SPEED_PPS * dtSec;
      if (cometPos >= NUM_LEDS) cometPos -= NUM_LEDS;
      effectMul = 1.0f;
      break;
    }

    case EFFECT_THEATER_CHASE:
    case EFFECT_TWINKLE:
    case EFFECT_CONFETTI:
    case EFFECT_FIRE:
    case EFFECT_RAINBOW:
    case EFFECT_RAINBOW_FADE:
      // fully time/idx driven per-pixel in safeSet(); nothing to advance here
      effectMul = 1.0f;
      break;

    case EFFECT_SNAKE:
      // Static like Solid the rest of the time - the sweep itself is
      // triggered/drawn separately in renderClock() on a minute change.
      effectMul = 1.0f;
      break;
  }
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

void publishTextState() {
  mqtt.publish(TOPIC_TEXT_STATE, useCustomText ? customText : "", true);
}

void handleTextPayload(char* payload)
{
  // Trim whitespace before checking for "empty". We publish the (unpadded)
  // text back to the state topic, and Home Assistant's text field echoes
  // that value back verbatim if the user clears it - if we ever echoed a
  // padded/spaces-only value, "clearing" it would send back stray spaces
  // instead of a true empty string, silently getting stuck showing blanks
  // instead of reverting to the clock.
  char* start = payload;
  while (*start == ' ') start++;
  char* end = start + strlen(start);
  while (end > start && *(end - 1) == ' ') end--;
  size_t len = end - start;
  if (len > 4) len = 4;

  bool wasActive = useCustomText;

  if (len == 0) {
    useCustomText = false;
    customText[0] = '\0';
  } else {
    memcpy(customText, start, len);
    customText[len] = '\0';
    useCustomText = true;
  }

  if (useCustomText || wasActive != useCustomText) {
    startCrossfade();
  }
  publishTextState();
}

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  static char buf[64];
  unsigned int len = min(length, (unsigned int)(sizeof(buf)-1));
  memcpy(buf, payload, len); buf[len] = '\0';

  if (!strcmp(topic, TOPIC_CMD_POWER))            handlePowerPayload(buf);
  else if (!strcmp(topic, TOPIC_CMD_COLOR))       handleColorPayload(buf);
  else if (!strcmp(topic, TOPIC_CMD_BRIGHTNESS))  handleBrightnessPayload(buf);
  else if (!strcmp(topic, TOPIC_CMD_EFFECT))      handleEffectPayload(buf);
  else if (!strcmp(topic, TOPIC_CMD_TEXT))        handleTextPayload(buf);
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

      mqtt.subscribe(TOPIC_CMD_COLOR);
      mqtt.subscribe(TOPIC_CMD_BRIGHTNESS);
      mqtt.subscribe(TOPIC_CMD_EFFECT);
      mqtt.subscribe(TOPIC_CMD_TEXT);
      mqtt.subscribe(TOPIC_CMD_POWER);

      publishDiscovery();
      cleanupRemovedEntities();
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

  smR = displayColor.R; smG = displayColor.G; smB = displayColor.B;
  smBrightness = brightness;

  mqtt.setBufferSize(1024);

  // WiFi + MQTT
  connectWiFi();
  connectMQTT();

  // Time (ESP32 uses configTzTime)
  configTzTime(TZ_EU_AMS, NTP1, NTP2);

  delay(500); // let things settle
  mqtt.publish(TOPIC_STATE_ONLINE, "online", true);
  publishLightState();
  publishEffectState();
  publishTextState();
}

void renderClock()
{
  clearAll();

  struct tm lt;
  if (!getLocalTimeSafe(&lt)) {
    // time not ready: just keep blank
    commitFrame();
    return;
  }

  int hour = lt.tm_hour;
  int minute = lt.tm_min;
  int second = lt.tm_sec;

  RgbColor col((uint8_t)smR, (uint8_t)smG, (uint8_t)smB);

  // Offsets matching your ESPHome logic
  drawDigit(hour   / 10, 107, col);
  drawDigit(hour   % 10,  72, col);
  drawDigit(minute / 10,  35, col);
  drawDigit(minute % 10,   0, col);

  // Blinking colon (LEDs 70, 71)
  RgbColor colonCol = (second % 2 == 0) ? col : RgbColor(0,0,0);
  safeSet(70, colonCol);
  safeSet(71, colonCol);

  if (currentEffect == EFFECT_SNAKE) {
    int val = hour * 60 + minute;
    if (lastClockValue < 0) {
      lastClockValue = val; // first frame after boot/switch - don't animate
    } else if (val != lastClockValue) {
      lastClockValue = val;
      snakeActive = true;
      snakeStartMs = millis();
    }
    applySnakeOverlay(millis());
  }

  commitFrame();
}

void renderCustomText()
{
  clearAll();

  RgbColor col((uint8_t)smR, (uint8_t)smG, (uint8_t)smB);
  const int offsets[4] = {107, 72, 35, 0};
  size_t len = strlen(customText);
  if (len > 4) len = 4;
  size_t pad = 4 - len; // right-align, like the clock digits

  for (size_t slot = 0; slot < 4; slot++) {
    char c = (slot < pad) ? ' ' : customText[slot - pad];
    drawChar(c, offsets[slot], col);
  }

  commitFrame();
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
    updateSmoothing();
    updateEffect();
    if (useCustomText) {
      renderCustomText();
    } else {
      renderClock();
    }
  }
}
