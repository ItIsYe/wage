#include "led_ring1_module.h"

#include "config.h"
#include "led_gamma.h"
#include "types.h"

extern RuntimeConfig activeConfig;

// ---------------------------------------------------------------------
// Zustand
// ---------------------------------------------------------------------

static CRGB* leds = nullptr;
static LedMode mode = LedMode::ALL_OFF;
static bool frameDirty = true;
static uint8_t brightnessByte = 0;
static bool brightnessInitialized = false;

// Blink/Spinner-Timing
static uint32_t tickAtMs = 0;
static bool blinkFlip = false;
static uint16_t spinnerGroup = 0;

// Standby-Twinkle (ein Stern = eine logische Gruppe)
struct TwinkleStar {
  bool on;
  uint16_t hue;      // 0..65535 (FastLED CHSV hue Byte kommt aus hue>>8)
  uint8_t value;
};
static TwinkleStar stars[PIXEL_GROUPS];
static uint16_t starsOnCount = 0;
static uint32_t twinkleChangeAtMs = 0;
static uint32_t twinkleFrameAtMs = 0;

// Farben (einmalig in ring1Init gesetzt)
static CRGB colorGreen = CRGB::Black;
static CRGB colorBlue = CRGB::Black;
static CRGB colorRed = CRGB::Black;
static CRGB colorCyan = CRGB::Black;

// Diagnose-Modus
static bool diagActive = false;
static uint16_t diagPixel = 0;
static uint32_t diagNextStepMs = 0;
static constexpr uint32_t DIAG_STEP_MS = 400;

// "Warte auf Glas": abwechselnd gerade/ungerade logische Gruppen.
// Wird zur Laufzeit aus PIXEL_GROUPS berechnet statt hartcodiert, damit
// eine spaetere Aenderung von PIXEL_COUNT nicht erneut zu stillen
// Off-by-Bugs fuehrt (siehe Historie: mehrfach hartcodierte Arrays,
// die nach PIXEL_COUNT-Aenderungen nicht mehr zur Gruppenzahl passten).

// ---------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------

static inline bool ready() { return leds != nullptr; }

static inline uint8_t percentToByte(uint8_t percent) {
  if (percent >= 100) return 255;
  return (uint8_t)((uint16_t)percent * 255u / 100u);
}

static inline CRGB scaled(const CRGB& color) {
  CRGB out = color;
  out.nscale8_video(brightnessByte);
  return out;
}

static inline CRGB rgb(uint8_t r, uint8_t g, uint8_t b) { return CRGB(r, g, b); }

static inline uint8_t gamma8(uint8_t value) { return LedGamma::apply(value); }

static inline CRGB hsvGamma(uint16_t hue, uint8_t sat, uint8_t val) {
  CHSV hsv((uint8_t)(hue >> 8), sat, val);
  CRGB out;
  hsv2rgb_rainbow(hsv, out);
  out.r = gamma8(out.r);
  out.g = gamma8(out.g);
  out.b = gamma8(out.b);
  return out;
}

static inline void setBrightnessPercent(uint8_t percent) {
  const uint8_t target = percentToByte(percent);
  if (brightnessInitialized && brightnessByte == target) return;
  brightnessByte = target;
  brightnessInitialized = true;
}

static inline void applyBrightnessForMode() {
  const uint8_t percent = (mode == LedMode::STANDBY_TWINKLE)
    ? activeConfig.standbyBrightnessPercent
    : activeConfig.pixelBrightnessPercent;
  setBrightnessPercent(percent);
}

static inline void clearAll() {
  if (!ready()) return;
  fill_solid(leds, PIXEL_COUNT, CRGB::Black);
}

static inline void fillAll(const CRGB& color) {
  if (!ready()) return;
  const CRGB c = scaled(color);
  for (uint16_t i = 0; i < PIXEL_COUNT; ++i) leds[i] = c;
}

// Setzt/loescht alle physischen Pixel einer logischen Dreiergruppe.
static inline void groupSet(uint16_t groupIdx, const CRGB& color) {
  if (!ready()) return;
  const CRGB c = scaled(color);
  const uint16_t base = groupIdx * PIXEL_GROUP_SIZE;
  for (uint8_t j = 0; j < PIXEL_GROUP_SIZE; ++j) {
    if (base + j < PIXEL_COUNT) leds[base + j] = c;
  }
}

static inline uint32_t clampRange(uint32_t minValue, uint32_t maxValue) {
  return (minValue > maxValue) ? maxValue : minValue;
}

static inline uint32_t sanitizeFrameMs(uint32_t frameMs) {
  if (frameMs < 30U) return 30U;
  if (frameMs > 1000U) return 1000U;
  return frameMs;
}

static inline uint32_t randU32(uint32_t minValue, uint32_t maxValue) {
  if (minValue > maxValue) { const uint32_t t = minValue; minValue = maxValue; maxValue = t; }
  return minValue + (uint32_t)random(0, (long)(maxValue - minValue + 1U));
}

static inline uint8_t randU8(uint8_t minValue, uint8_t maxValue) {
  if (minValue > maxValue) { const uint8_t t = minValue; minValue = maxValue; maxValue = t; }
  return (uint8_t)(minValue + (uint8_t)random(0, (int16_t)(maxValue - minValue + 1U)));
}

// ---------------------------------------------------------------------
// "Warte auf Glas" Pattern: abwechselnd gerade/ungerade Gruppen
// ---------------------------------------------------------------------

static void applyAltGlassPattern(bool phaseA) {
  clearAll();
  for (uint16_t g = 0; g < PIXEL_GROUPS; ++g) {
    const bool isEven = (g % 2 == 0);
    if (isEven == phaseA) {
      groupSet(g, phaseA ? colorGreen : colorBlue);
    }
  }
}

// ---------------------------------------------------------------------
// Standby-Twinkle
// ---------------------------------------------------------------------

static void twinkleInit(uint32_t now) {
  const uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
  const uint32_t changeMinMs = clampRange(activeConfig.standbyChangeMinMs, changeMaxMs);
  const uint8_t valueMax = activeConfig.standbyValueMax;
  const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
  const uint8_t onMax = activeConfig.standbyOnMax;
  const uint8_t onMin = (activeConfig.standbyOnMin > onMax) ? onMax : activeConfig.standbyOnMin;

  twinkleFrameAtMs = now + sanitizeFrameMs(activeConfig.standbyFrameMs);
  twinkleChangeAtMs = now + randU32(changeMinMs, changeMaxMs);
  starsOnCount = 0;

  for (uint16_t i = 0; i < PIXEL_GROUPS; ++i) {
    stars[i].on = false;
    stars[i].hue = (uint16_t)random(0, 65536);
    stars[i].value = randU8(valueMin, valueMax);
  }
  const uint8_t initialOn = randU8(onMin, onMax);
  for (uint8_t i = 0; i < initialOn; ++i) {
    const uint16_t idx = (uint16_t)random(0, PIXEL_GROUPS);
    if (!stars[idx].on) {
      stars[idx].on = true;
      ++starsOnCount;
    }
  }
  frameDirty = true;
}

static void twinkleRender() {
  clearAll();
  for (uint16_t i = 0; i < PIXEL_GROUPS; ++i) {
    if (stars[i].on) {
      groupSet(i, hsvGamma(stars[i].hue, activeConfig.standbySaturation, stars[i].value));
    }
  }
}

static void twinkleUpdate(uint32_t now) {
  // Sterne ein-/ausschalten um die Ziel-Anzahl zu erreichen
  if (now >= twinkleChangeAtMs) {
    const uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
    const uint32_t changeMinMs = clampRange(activeConfig.standbyChangeMinMs, changeMaxMs);
    const uint8_t onMax = activeConfig.standbyOnMax;
    const uint8_t onMin = (activeConfig.standbyOnMin > onMax) ? onMax : activeConfig.standbyOnMin;
    const uint8_t valueMax = activeConfig.standbyValueMax;
    const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;

    twinkleChangeAtMs = now + randU32(changeMinMs, changeMaxMs);
    const bool needMore = starsOnCount < onMin;
    const bool needLess = starsOnCount > onMax;
    const bool shouldToggle = !needMore && !needLess && (random(0, 100) < 45);

    if (needMore || needLess || shouldToggle) {
      for (uint16_t tries = 0; tries < PIXEL_GROUPS; ++tries) {
        const uint16_t i = (uint16_t)random(0, PIXEL_GROUPS);
        if (needMore && !stars[i].on) {
          stars[i].on = true; ++starsOnCount;
          stars[i].hue = (uint16_t)random(0, 65536);
          stars[i].value = randU8(valueMin, valueMax);
          frameDirty = true; break;
        }
        if (needLess && stars[i].on) {
          stars[i].on = false; --starsOnCount;
          frameDirty = true; break;
        }
        if (!needMore && !needLess) {
          if (stars[i].on) { stars[i].on = false; --starsOnCount; }
          else { stars[i].on = true; ++starsOnCount; stars[i].hue = (uint16_t)random(0, 65536); stars[i].value = randU8(valueMin, valueMax); }
          frameDirty = true; break;
        }
      }
    }
  }

  // Sanftes Farb-/Helligkeitsdriften der aktiven Sterne
  if (now >= twinkleFrameAtMs) {
    const uint8_t valueMax = activeConfig.standbyValueMax;
    const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
    twinkleFrameAtMs = now + sanitizeFrameMs(activeConfig.standbyFrameMs);

    for (uint16_t i = 0; i < PIXEL_GROUPS; ++i) {
      if (!stars[i].on) continue;
      stars[i].hue = (uint16_t)(stars[i].hue + (int16_t)random(-2, 3));
      int16_t nextValue = (int16_t)stars[i].value + (int16_t)random(-4, 5);
      if (nextValue < valueMin) nextValue = valueMin;
      if (nextValue > valueMax) nextValue = valueMax;
      stars[i].value = (uint8_t)nextValue;
    }
    frameDirty = true;
  }

  if (frameDirty) twinkleRender();
}

// ---------------------------------------------------------------------
// Diagnose-Modus
// ---------------------------------------------------------------------

void ring1DiagnosticStart() {
  diagActive = true;
  diagPixel = 0;
  diagNextStepMs = 0;
  Serial.println("[RING1 DIAG] Start - jeder Pixel einzeln, ~400ms Abstand");
}

bool ring1DiagnosticActive() { return diagActive; }

bool ring1DiagnosticService(uint32_t now) {
  if (!diagActive || !ready()) return false;
  if (now < diagNextStepMs) return false;

  diagNextStepMs = now + DIAG_STEP_MS;
  clearAll();
  if (diagPixel < PIXEL_COUNT) {
    leds[diagPixel] = CRGB(60, 60, 60);
    Serial.printf("[RING1 DIAG] Pixel %u/%u an\n", (unsigned)diagPixel, (unsigned)(PIXEL_COUNT - 1));
    ++diagPixel;
  } else {
    Serial.println("[RING1 DIAG] Durchlauf komplett - Neustart bei Pixel 0");
    diagPixel = 0;
  }
  return true;
}

// ---------------------------------------------------------------------
// Oeffentliche API
// ---------------------------------------------------------------------

void ring1Init(CRGB* ledsIn) {
  LedGamma::build();
  leds = ledsIn;
  setBrightnessPercent(activeConfig.pixelBrightnessPercent);
  colorGreen = rgb(0, 90, 0);
  colorBlue = rgb(0, 0, 100);
  colorRed = rgb(100, 0, 0);
  colorCyan = rgb(0, 90, 90);
  clearAll();
  frameDirty = true;
}

void ring1SetMode(LedMode m, uint32_t now) {
  if (mode == m) return;
  mode = m;
  tickAtMs = now;
  blinkFlip = (m == LedMode::READY_GREEN_BLINK);
  spinnerGroup = 0;
  frameDirty = true;
  if (m == LedMode::STANDBY_TWINKLE) {
    twinkleInit(now);
  }
}

bool ring1Service(uint32_t now) {
  if (!ready()) return false;

  if (diagActive) {
    return ring1DiagnosticService(now);
  }

  applyBrightnessForMode();

  switch (mode) {
    case LedMode::ALL_OFF:
      if (frameDirty) clearAll();
      break;

    case LedMode::ERROR_BLINK_RED:
      if (now - tickAtMs >= 350) { tickAtMs = now; blinkFlip = !blinkFlip; frameDirty = true; }
      if (frameDirty) { clearAll(); if (blinkFlip) fillAll(colorRed); }
      break;

    case LedMode::RED_SOLID:
      if (frameDirty) fillAll(colorRed);
      break;

    case LedMode::OK_ALT_GB:
      if (now - tickAtMs >= 450) { tickAtMs = now; blinkFlip = !blinkFlip; frameDirty = true; }
      if (frameDirty) applyAltGlassPattern(blinkFlip);
      break;

    case LedMode::READY_GREEN_BLINK:
      if (now - tickAtMs >= 450) { tickAtMs = now; blinkFlip = !blinkFlip; frameDirty = true; }
      if (frameDirty) { clearAll(); if (blinkFlip) fillAll(colorGreen); }
      break;

    case LedMode::GLASS_GREEN_SOLID:
      if (frameDirty) fillAll(colorGreen);
      break;

    case LedMode::TIMING_BLUE_SPINNER:
      if (now - tickAtMs >= 180) {
        tickAtMs = now;
        spinnerGroup = (uint16_t)((spinnerGroup + 1) % PIXEL_GROUPS);
        frameDirty = true;
      }
      if (frameDirty) { clearAll(); groupSet(spinnerGroup, colorBlue); }
      break;

    case LedMode::RESULT_FLASH_GB_ONCE:
      if (now - tickAtMs < 200) {
        if (frameDirty) fillAll(colorCyan);
      } else {
        ring1SetMode(LedMode::GLASS_GREEN_SOLID, now);
      }
      break;

    case LedMode::STANDBY_TWINKLE:
      twinkleUpdate(now);
      break;
  }

  const bool dirty = frameDirty;
  frameDirty = false;
  return dirty;
}

void ring1ApplyBrightnessForCurrentMode() { applyBrightnessForMode(); }
void ring1MarkDirty() { frameDirty = true; }
void ring1Clear() { if (ready()) clearAll(); }

void ring1FillDebugAllOn() {
  if (!ready()) return;
  applyBrightnessForMode();
  fillAll(rgb(80, 80, 80));
  frameDirty = false;
}
