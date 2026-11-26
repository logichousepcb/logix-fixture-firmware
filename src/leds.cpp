#include "leds.h"

#include <NeoPixelBus.h>

#include "config.h"
#include "platform.h"

// ---------------------------------------------------------------------------
// Driver: pick the best NeoPixelBus method for each output's pin at runtime.
// ESP8266 (one output):
//   GPIO2 -> UART1 hardware (doesn't block interrupts, so IR + WiFi stay happy)
//   GPIO3 -> I2S DMA (RX pin)
//   other -> bit-bang
// ESP32: RMT hardware, one channel per output (non-blocking, all four send at once).
// Bytes are always sent raw (NeoRgbFeature); color order is applied by us.
// ---------------------------------------------------------------------------
class LedDriver {
 public:
  virtual ~LedDriver() {}
  virtual void begin() = 0;
  virtual void set(uint16_t i, uint8_t a, uint8_t b, uint8_t c) = 0;
  virtual void show() = 0;
  virtual bool canShow() = 0;
};

template <typename METHOD>
class NpbDriver : public LedDriver {
 public:
  NpbDriver(uint16_t n, uint8_t pin) : bus(n, pin) {}
  void begin() override { bus.Begin(); }
  void set(uint16_t i, uint8_t a, uint8_t b, uint8_t c) override { bus.SetPixelColor(i, RgbColor(a, b, c)); }
  void show() override { bus.Show(); }
  bool canShow() override { return bus.CanShow(); }

 private:
  NeoPixelBus<NeoRgbFeature, METHOD> bus;
};

#if defined(ESP32)
class RmtDriver : public LedDriver {
 public:
  RmtDriver(uint16_t n, uint8_t pin, uint8_t channel) : bus(n, pin, (NeoBusChannel)channel) {}
  void begin() override { bus.Begin(); }
  void set(uint16_t i, uint8_t a, uint8_t b, uint8_t c) override { bus.SetPixelColor(i, RgbColor(a, b, c)); }
  void show() override { bus.Show(); }
  bool canShow() override { return bus.CanShow(); }

 private:
  NeoPixelBus<NeoRgbFeature, NeoEsp32RmtNWs2812xMethod> bus;
};
#endif

const char *const EFFECT_NAMES[] = {"Solid",         "Breathe",       "Rainbow", "Rainbow Cycle",
                                    "Color Wipe",    "Theater Chase", "Twinkle", "Comet",
                                    "Candle",        "Christmas",     "Buffalo Bills", "Buffalo Sabres",
                                    "Lake Shore",    "Buffalo Bills Solid", "Buffalo Sabres Solid",
                                    "Lake Shore Solid", "Frost", "Valentine"};

// Team / theme colors, shown as pairs of LEDs (2 of each color) repeating along the strip.
static const uint8_t BILLS[][3] = {{0, 51, 141}, {198, 12, 48}, {255, 255, 255}};  // blue, red, white
static const uint8_t SABRES[][3] = {{0, 48, 135}, {255, 184, 28}};                  // navy, gold
static const uint8_t LAKESHORE[][3] = {{255, 255, 255}, {34, 139, 34}};             // white, forest green

// Traditional multi-color C9 bulb string, repeated along the strip.
static const uint8_t XMAS[][3] = {
    {255, 8, 0},    // red
    {0, 190, 20},   // green
    {0, 40, 255},   // blue
    {255, 70, 0},   // orange
    {255, 170, 0},  // warm yellow
};
// Frost: icy theme (deep ice blue, light blue, silver, white), blended smoothly along the strip.
static const uint8_t FROST[][3] = {
    {10, 70, 200},    // deep ice blue
    {90, 180, 255},   // light blue
    {160, 180, 200},  // silver
    {255, 255, 255},  // white
    {120, 210, 255},  // pale aqua
};
static const uint8_t FROST_N = sizeof(FROST) / sizeof(FROST[0]);

// Valentine: deep red, rose, hot pink and blush, with a heartbeat pulse.
static const uint8_t VALENTINE[][3] = {
    {200, 0, 20},     // deep red
    {255, 20, 60},    // rose red
    {255, 40, 140},   // hot pink
    {255, 150, 180},  // blush
    {255, 70, 110},   // pink
};
static const uint8_t VALENTINE_N = sizeof(VALENTINE) / sizeof(VALENTINE[0]);

static const uint8_t XMAS_N = sizeof(XMAS) / sizeof(XMAS[0]);
const uint8_t EFFECT_COUNT = sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]);

// Solid, Breathe, Color Wipe, Theater Chase, Twinkle, Comet and Candle draw with the chosen color.
bool effectUsesColor(uint8_t e) { return e <= 1 || (e >= 4 && e <= 8); }

static const uint32_t FRAME_MS = 20;      // 50 fps
static const float FADE_MS = 400.0f;      // on/off & brightness transition
static const uint8_t MA_PER_CHANNEL = 20; // WS2812 per color at full duty

// Per-output runtime: driver, slice of the frame buffer, fade and effect timers.
struct SegRt {
  bool used = false;
  LedDriver *drv = nullptr;
  uint16_t start = 0, len = 0;  // slice of fb
  uint8_t order[3] = {1, 0, 2};
  float dispBri = 0;            // current (fading) brightness, master x output
  bool dirty = true;
  uint8_t lastEffect = 255;
  uint32_t phase = 0;
  uint16_t lastHead = 0;
  uint32_t twinkleAcc = 0;
  int16_t candleLevel = 200, candleTarget = 200;
  uint32_t candleNext = 0;
};

static SegRt segs[MAX_OUTPUTS];
static uint8_t *fb = nullptr;  // all outputs back to back, RGB at full brightness
static uint16_t total = 0;
static bool paused = false;
static uint32_t lastFrame = 0;
static uint16_t estMa = 0;
static bool limited = false;
static uint32_t identifyUntil = 0;

// The output being rendered: its pixels, LED count, settings and timers.
static uint8_t *frame = nullptr;
static uint16_t n = 0;
static const Seg *sg = nullptr;
static SegRt *rt = nullptr;

void stateChanged() {
  for (auto &s : segs) s.dirty = true;
  stateVersion++;
  stateMarkDirty();
}

void ledsIdentify() { identifyUntil = millis() + 3000; }

static void parseOrder(const String &o, uint8_t *ord) {
  for (int k = 0; k < 3; k++) {
    char c = o[k];
    ord[k] = c == 'R' ? 0 : (c == 'G' ? 1 : 2);
  }
}

void ledsUpdateOrders() {
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    if (!segs[k].used) continue;
    parseOrder(cfg.out[k].order, segs[k].order);
    segs[k].dirty = true;
  }
}

// Outputs keep their own animation clock (each can run its own speed). When the same
// effect/speed is set for all of them, give every output the first one's clock so they
// move in step; the first output carries on without a jump.
void ledsSync() {
  const SegRt &ref = segs[firstOutput()];
  for (auto &s : segs) {
    if (!s.used || &s == &ref) continue;
    s.phase = ref.phase;
    s.lastHead = ref.lastHead;
    s.twinkleAcc = ref.twinkleAcc;
  }
}

void ledsPause(bool p) { paused = p; }
uint16_t ledsEstimatedMa() { return estMa; }
bool ledsPowerLimited() { return limited; }

void ledsBegin() {
  total = totalLeds();
  fb = new uint8_t[(total ? total : 1) * 3]();
  uint16_t at = 0;
  uint8_t channel = 0;
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    SegRt &s = segs[k];
    s = SegRt();
    if (!outputUsed(k)) continue;
    s.used = true;
    s.start = at;
    s.len = cfg.out[k].count;
    at += s.len;
    uint8_t pin = cfg.out[k].pin;
#if defined(ESP32)
    s.drv = new RmtDriver(s.len, pin, channel);
    const char *method = "RMT";
#else
    if (pin == 2)
      s.drv = new NpbDriver<NeoEsp8266Uart1Ws2812xMethod>(s.len, pin);
    else if (pin == 3)
      s.drv = new NpbDriver<NeoEsp8266DmaWs2812xMethod>(s.len, pin);
    else
      s.drv = new NpbDriver<NeoEsp8266BitBangWs2812xMethod>(s.len, pin);
    const char *method = pin == 2 ? "UART1" : pin == 3 ? "DMA" : "bit-bang";
#endif
    s.drv->begin();
    parseOrder(cfg.out[k].order, s.order);
    s.dispBri = 0;  // fade in on boot
    s.drv->show();
    Serial.printf("[led] output %u: %u LEDs on GPIO%u (%s %u, %s)\n", k + 1, s.len, pin, method, channel,
                  cfg.out[k].order.c_str());
    channel++;
  }
}

// --------------------------------------------------------------------------- helpers
static inline void px(uint16_t i, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t *p = frame + i * 3;
  p[0] = r;
  p[1] = g;
  p[2] = b;
}
static inline uint8_t scale8(uint8_t v, uint8_t s) { return ((uint16_t)v * (s + 1)) >> 8; }
static void fadeAll(uint8_t keep) {
  for (uint16_t i = 0; i < n * 3; i++) frame[i] = scale8(frame[i], keep);
}
static void wheel(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = h / 43;
  uint8_t t = (h - region * 43) * 6;
  uint8_t q = 255 - t;
  switch (region) {
    case 0: r = 255; g = t; b = 0; break;
    case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break;
    case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break;
    default: r = 255; g = 0; b = q; break;
  }
}
static uint8_t sinTable[256];
static bool sinReady = false;
static uint8_t sin8(uint8_t x) {
  if (!sinReady) {
    for (int i = 0; i < 256; i++) sinTable[i] = (uint8_t)(127.5f + 127.5f * sinf(i * (2.0f * PI / 256.0f)));
    sinReady = true;
  }
  return sinTable[x];
}
// Cheap per-pixel pseudo-random byte, stable for a given index.
static inline uint8_t hash8(uint16_t i, uint8_t salt) {
  uint32_t h = (i + 1) * 2654435761u ^ (salt * 40503u);
  return (h >> 16) ^ (h >> 24);
}

// Smoothly blended, cyclic palette: 256 units of pos = one trip through all colors.
static void blendPalette(const uint8_t (*colors)[3], uint8_t count, uint16_t pos, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint32_t q = (uint32_t)pos * count % ((uint32_t)count * 256);
  const uint8_t *a = colors[q >> 8], *c = colors[((q >> 8) + 1) % count];
  uint8_t t = q & 0xFF;
  r = a[0] + (((int)c[0] - a[0]) * t >> 8);
  g = a[1] + (((int)c[1] - a[1]) * t >> 8);
  b = a[2] + (((int)c[2] - a[2]) * t >> 8);
}

// Repeating pairs of colors (AA BB CC AA BB CC ...). The moving versions slowly march along the
// strip (speed 128 ~ 2 LEDs/s; speed 0 holds still); the "Solid" versions never move.
static void pairs(const uint8_t (*colors)[3], uint8_t count, uint32_t p, bool moving) {
  const uint32_t shift = (moving && sg->speed) ? p / 16 : 0;
  const uint32_t period = (uint32_t)count * 2;
  for (uint16_t i = 0; i < n; i++) {
    const uint8_t *c = colors[((i + period - shift % period) % period) / 2];
    px(i, c[0], c[1], c[2]);
  }
}

// --------------------------------------------------------------------------- effects
// Renders one output: frame/n/sg/rt point at it (see ledsLoop).
static void render(uint32_t dt) {
  const uint8_t R = sg->r, G = sg->g, B = sg->b;
  if (sg->effect != rt->lastEffect) {
    memset(frame, 0, n * 3);
    rt->lastEffect = sg->effect;
    rt->phase = 0;
    rt->lastHead = 0;
  }
  // the phase advances proportional to speed; p wraps cleanly every 256 units
  rt->phase += dt * (sg->speed + 8);
  const uint32_t p = rt->phase >> 12;  // speed 128 ≈ 33 units/s

  switch (sg->effect) {
    default:
    case 0:  // Solid
      for (uint16_t i = 0; i < n; i++) px(i, R, G, B);
      break;

    case 1: {  // Breathe
      uint8_t lvl = 24 + scale8(sin8(p), 231);
      for (uint16_t i = 0; i < n; i++) px(i, scale8(R, lvl), scale8(G, lvl), scale8(B, lvl));
      break;
    }

    case 2: {  // Rainbow (whole strip)
      uint8_t r, g, b;
      wheel(p, r, g, b);
      for (uint16_t i = 0; i < n; i++) px(i, r, g, b);
      break;
    }

    case 3:  // Rainbow Cycle (spread across strip)
      for (uint16_t i = 0; i < n; i++) {
        uint8_t r, g, b;
        wheel((uint8_t)(p + (uint32_t)i * 256 / n), r, g, b);
        px(i, r, g, b);
      }
      break;

    case 4: {  // Color Wipe: fill on, then off
      uint32_t pos = (p % 128) * 2 * n / 128;
      for (uint16_t i = 0; i < n; i++) {
        bool lit = pos < n ? i < pos : i >= pos - n;
        px(i, lit ? R : 0, lit ? G : 0, lit ? B : 0);
      }
      break;
    }

    case 5: {  // Theater Chase
      uint8_t off = (p / 4) % 3;
      for (uint16_t i = 0; i < n; i++) {
        bool lit = (i + off) % 3 == 0;
        px(i, lit ? R : 0, lit ? G : 0, lit ? B : 0);
      }
      break;
    }

    case 6:  // Twinkle
      fadeAll(245);
      rt->twinkleAcc += dt * n * (sg->speed + 16);
      while (rt->twinkleAcc >= 400000) {
        rt->twinkleAcc -= 400000;
        px(random(n), R, G, B);
      }
      break;

    case 7: {  // Comet
      fadeAll(200);
      uint16_t head = (p * n / 64) % n;
      uint16_t i = rt->lastHead;
      for (uint16_t guard = 0; guard < n; guard++) {  // fill skipped pixels at high speed
        px(i, R, G, B);
        if (i == head) break;
        i = (i + 1) % n;
      }
      rt->lastHead = head;
      break;
    }

    case 8: {  // Candle flicker
      uint32_t now = millis();
      if ((int32_t)(now - rt->candleNext) >= 0) {
        rt->candleTarget = random(110, 256);
        rt->candleNext = now + random(40, 40 + (256 - sg->speed) * 2);
      }
      rt->candleLevel += (rt->candleTarget - rt->candleLevel) / 3;
      for (uint16_t i = 0; i < n; i++) {
        int lvl = rt->candleLevel - (int)random(0, 20);
        if (lvl < 0) lvl = 0;
        px(i, scale8(R, lvl), scale8(G, lvl), scale8(B, lvl));
      }
      break;
    }

    case 9: {  // Christmas: classic multi-color bulbs, each softly twinkling on its own
      for (uint16_t i = 0; i < n; i++) {
        const uint8_t *c = XMAS[i % XMAS_N];
        uint8_t lvl = 255;
        if (sg->speed > 0) {
          // every bulb gets its own rt->phase and slightly different rate
          uint8_t rate = 2 + (hash8(i, 1) & 3);
          uint8_t s = sin8((uint8_t)(p * rate / 2 + hash8(i, 2)));
          lvl = 150 + scale8(s, 105);  // 150..255: dims gently, never goes dark
        }
        px(i, scale8(c[0], lvl), scale8(c[1], lvl), scale8(c[2], lvl));
      }
      break;
    }

    case 10: pairs(BILLS, 3, p, true); break;       // Buffalo Bills: blue, red, white
    case 11: pairs(SABRES, 2, p, true); break;      // Buffalo Sabres: navy, gold
    case 12: pairs(LAKESHORE, 2, p, true); break;   // Lake Shore: white, forest green
    case 13: pairs(BILLS, 3, p, false); break;      // ... and the same three, standing still
    case 14: pairs(SABRES, 2, p, false); break;
    case 15: pairs(LAKESHORE, 2, p, false); break;

    case 16: {  // Frost: icy colors drift slowly along the strip, with white glints like ice crystals
      for (uint16_t i = 0; i < n; i++) {
        // position in the palette: spread along the strip, wobbling slowly, drifting with time
        uint16_t pos = (uint16_t)(i * 12 + p / 2 + scale8(sin8((uint8_t)(i * 5 + p / 3)), 60));
        uint8_t r, g, bl;
        blendPalette(FROST, FROST_N, pos, r, g, bl);
        // each pixel glints white briefly at its own random moment
        uint8_t s = sin8((uint8_t)(p * (3 + (hash8(i, 3) & 3)) / 2 + hash8(i, 4)));
        if (s > 248 && sg->speed > 0) {
          uint8_t w = (s - 248) * 34;  // 0..238, a short flash near the peak
          r += scale8(255 - r, w);
          g += scale8(255 - g, w);
          bl += scale8(255 - bl, w);
        }
        px(i, r, g, bl);
      }
      break;
    }

    case 17: {  // Valentine: reds and pinks drifting along the strip, pulsing with a heartbeat
      // Heartbeat: a strong beat then a softer one ("lub-dub"), then rest. ~1.2 s per beat at
      // speed 128; speed 0 holds still with no pulse.
      uint8_t lvl = 255;
      if (sg->speed > 0) {
        const uint32_t q = (rt->phase >> 8) % 640;  // 16 sub-steps per p unit
        auto bump = [](int32_t x, int32_t center, int32_t width, int32_t height) -> int32_t {
          int32_t d = x > center ? x - center : center - x;
          return d >= width ? 0 : height * (width - d) / width;
        };
        int32_t v = 140 + bump(q, 40, 50, 115) + bump(q, 130, 50, 75);
        lvl = v > 255 ? 255 : v;
      }
      for (uint16_t i = 0; i < n; i++) {
        uint16_t pos = (uint16_t)(i * 10 + p / 3);
        uint8_t r, g, b;
        blendPalette(VALENTINE, VALENTINE_N, pos, r, g, b);
        px(i, scale8(r, lvl), scale8(g, lvl), scale8(b, lvl));
      }
      break;
    }
  }
}

// Apply brightness, the current limit and color order, then push every output.
// forceBri >= 0 overrides each output's brightness (identify blink).
static void output(int16_t forceBri = -1) {
  uint8_t bri[MAX_OUTPUTS] = {0};
  uint32_t ledMa = 0;  // mA drawn by the LEDs at their brightness
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    const SegRt &s = segs[k];
    if (!s.used) continue;
    bri[k] = forceBri >= 0 ? forceBri : (uint8_t)(s.dispBri + 0.5f);
    uint32_t sum = 0;  // <= 800*3*255, fits easily
    const uint8_t *p = fb + s.start * 3;
    for (uint16_t i = 0; i < s.len * 3; i++) sum += p[i];
    ledMa += (sum * bri[k] / 255) * MA_PER_CHANNEL / 255;
  }
  // One budget for the whole board: when it's exceeded every output dims by the same factor.
  uint32_t budget = 0;
  limited = false;
  if (cfg.maxMa > 0) {
    int32_t b = (int32_t)cfg.maxMa - BOARD_MA - total;  // ~1 mA idle per LED
    if (b < 20) b = 20;
    if (ledMa > (uint32_t)b) {
      budget = b;
      limited = true;
    }
  }

  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    SegRt &s = segs[k];
    if (!s.used) continue;
    uint8_t eff = limited ? (uint8_t)((uint32_t)bri[k] * budget / ledMa) : bri[k];
    const uint8_t *p = fb + s.start * 3;
    for (uint16_t i = 0; i < s.len; i++, p += 3) {
      uint8_t v[3] = {scale8(p[0], eff), scale8(p[1], eff), scale8(p[2], eff)};
      if (eff == 0) v[0] = v[1] = v[2] = 0;
      s.drv->set(i, v[s.order[0]], v[s.order[1]], v[s.order[2]]);
    }
    s.drv->show();
  }
  if (limited) ledMa = budget;
  estMa = BOARD_MA + total + ledMa;
}

void ledsLoop() {
  if (paused || !total) return;
  uint32_t now = millis();
  uint32_t dt = now - lastFrame;
  if (dt < FRAME_MS) return;
  for (auto &s : segs)
    if (s.used && !s.drv->canShow()) return;
  lastFrame = now;
  if (dt > 100) dt = 100;

  // Identify: blink white 6x over 3 s regardless of on/off, then restore.
  if ((int32_t)(identifyUntil - now) > 0) {
    bool lit = ((identifyUntil - now) / 250) % 2 == 0;
    memset(fb, lit ? 255 : 0, total * 3);
    output(160);  // still goes through the power limiter
    for (auto &s : segs) {
      s.lastEffect = 255;  // effect buffers were overwritten; restart cleanly (and in step)
      s.dirty = true;
    }
    return;
  }

  bool changed = false;
  for (uint8_t k = 0; k < MAX_OUTPUTS; k++) {
    SegRt &s = segs[k];
    if (!s.used) continue;
    const Seg &st = state.seg[k];
    float target = (state.on && st.on) ? state.bri * st.bri / 255.0f : 0;
    bool fading = s.dispBri != target;
    if (fading) {
      float step = dt * 255.0f / FADE_MS;
      s.dispBri = s.dispBri < target ? min(target, s.dispBri + step) : max(target, s.dispBri - step);
    }
    bool animated = st.effect != 0;
    if (!(fading || s.dirty || (animated && s.dispBri > 0))) continue;
    s.dirty = false;
    frame = fb + s.start * 3;
    n = s.len;
    sg = &st;
    rt = &s;
    render(dt);
    changed = true;
  }
  if (changed) output();
}
