#include "led_module.h"

#include <FastLED.h>

#include "config.h"
#include "types.h"

extern RuntimeConfig activeConfig;

// Ring 1 / Haupt-LED-Ring
static CRGB primaryLeds[PIXEL_COUNT];
#if RING2_ENABLED
static CRGB ring2Leds[RING2_PIXEL_COUNT];
#endif

static LedMode ledMode = LedMode::ALL_OFF;
static uint32_t ledTickMs = 0;
static bool ledFlip = false;
static uint32_t twinkleNextMs = 0;
static uint32_t standbyFrameNextMs = 0;
static uint8_t ledSpinIdx = 0;
static bool ledFrameDirty = true;


static inline CRGB scaleColor(const CRGB& color, uint8_t brightness) {
  CRGB out = color;
  out.nscale8_video(brightness);
  return out;
}

static inline CRGB rgb(uint8_t r, uint8_t g, uint8_t b) { return CRGB(r, g, b); }

static inline CRGB hsvGamma(uint16_t hue, uint8_t sat, uint8_t val) {
  CHSV hsv((uint8_t)(hue >> 8), sat, val);
  CRGB out;
  hsv2rgb_rainbow(hsv, out);
  out.r = gamma8(out.r);
  out.g = gamma8(out.g);
  out.b = gamma8(out.b);
  return out;
}
static bool standbyTwinkleOn[PIXEL_COUNT] = {};
static uint8_t standbyOnCount = 0;
static uint8_t currentBrightnessByte = 0;
static bool brightnessInitialized = false;
static uint16_t standbyHue[PIXEL_COUNT] = {};
static uint8_t standbyValue[PIXEL_COUNT] = {};
static CRGB colorGreen = CRGB::Black;
static CRGB colorBlue = CRGB::Black;
static CRGB colorRed = CRGB::Black;
static CRGB colorCyan = CRGB::Black;
#if RING2_ENABLED
static bool ring2FrameDirty = true;
static uint8_t ring2BrightnessByte = 0;
static bool ring2BrightnessInitialized = false;
static uint32_t ring2TickMs = 0;
static uint8_t ring2PulseValue = 10;
static int8_t ring2PulseStep = 5;
#endif

static inline uint8_t brightnessPercentToByte(uint8_t percent) {
  if (percent >= 100) return 255;
  return (uint8_t)((uint16_t)percent * 255u / 100u);
}

// Basis-Helfer Ring 1
static inline void pixelsClear() { fill_solid(primaryLeds, PIXEL_COUNT, CRGB::Black); }
static inline void pixelsShow() { FastLED.show(); }

static inline void setStripBrightnessPercent(uint8_t percent) {
  const uint8_t target = brightnessPercentToByte(percent);
  if (brightnessInitialized && currentBrightnessByte == target) return;
  currentBrightnessByte = target;
  brightnessInitialized = true;
}

#if RING2_ENABLED
// Basis-Helfer Ring 2
static inline void ring2SetBrightnessPercent(uint8_t percent) {
  const uint8_t target = brightnessPercentToByte(percent);
  if (ring2BrightnessInitialized && ring2BrightnessByte == target) return;
  ring2BrightnessByte = target;
  ring2BrightnessInitialized = true;
  ring2FrameDirty = true;
}

static inline void ring2Clear() { fill_solid(ring2Leds, RING2_PIXEL_COUNT, CRGB::Black); }
static inline void ring2Fill(const CRGB& color) {
  for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) ring2Leds[i] = scaleColor(color, ring2BrightnessByte);
}

static void ring2Service(uint32_t now) {
  static uint32_t lastEntryLogMs = 0;
  if (MASTER_DEBUG_LOG && millis() - lastEntryLogMs >= 2000) {
    lastEntryLogMs = millis();
    Serial.printf("[RING2 ENTRY] now=%lu boot=%u force=%u enabled=%u pin=%u\n",
                  (unsigned long)now,
                  (unsigned)RING2_BOOT_TEST,
                  (unsigned)RING2_FORCE_INDEPENDENT_TEST,
                  (unsigned)RING2_ENABLED,
                  (unsigned)RING2_PIN);
  }


  if (RING2_BOOT_TEST) return;

  static bool lastRing2Enabled = false;
  static uint8_t lastRing2PatternMode = 255;
  static bool lastRing2DebugAllOn = false;
  static uint8_t lastRing2BrightnessPercent = 255;
  static uint8_t lastRing2StandbyBrightnessPercent = 255;

  if (lastRing2Enabled != activeConfig.ring2Enabled ||
      lastRing2PatternMode != activeConfig.ring2PatternMode ||
      lastRing2DebugAllOn != activeConfig.ring2DebugAllOn ||
      lastRing2BrightnessPercent != activeConfig.ring2BrightnessPercent ||
      lastRing2StandbyBrightnessPercent != activeConfig.ring2StandbyBrightnessPercent) {
    ring2FrameDirty = true;
    if (MASTER_DEBUG_LOG) {
      Serial.printf("[RING2] pin=%u enabled=%u mode=%u debug=%u brightness=%u standbyBrightness=%u\n",
                    (unsigned)RING2_PIN,
                    (unsigned)activeConfig.ring2Enabled,
                    (unsigned)activeConfig.ring2PatternMode,
                    (unsigned)activeConfig.ring2DebugAllOn,
                    (unsigned)activeConfig.ring2BrightnessPercent,
                    (unsigned)activeConfig.ring2StandbyBrightnessPercent);
    }
    if (lastRing2PatternMode != activeConfig.ring2PatternMode && activeConfig.ring2PatternMode == 2) {
      ring2PulseValue = 10;
      ring2PulseStep = 5;
      ring2TickMs = now;
    }
    lastRing2Enabled = activeConfig.ring2Enabled;
    lastRing2PatternMode = activeConfig.ring2PatternMode;
    lastRing2DebugAllOn = activeConfig.ring2DebugAllOn;
    lastRing2BrightnessPercent = activeConfig.ring2BrightnessPercent;
    lastRing2StandbyBrightnessPercent = activeConfig.ring2StandbyBrightnessPercent;
  }

  if (!activeConfig.ring2Enabled) {
    if (ring2FrameDirty) {
      ring2Clear();
      FastLED.show();
      ring2FrameDirty = false;
    }
    return;
  }

  if (activeConfig.ring2DebugAllOn) {
    ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
    if (ring2FrameDirty) {
      ring2Fill(rgb(80, 80, 80));
      FastLED.show();
      ring2FrameDirty = false;
    }
    return;
  }

  const uint8_t mode = activeConfig.ring2PatternMode;
  if (mode == 2) {
    ring2SetBrightnessPercent(activeConfig.ring2StandbyBrightnessPercent);
  } else {
    ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
  }

  if (mode == 0) {
    if (ring2FrameDirty) {
      ring2Clear();
      FastLED.show();
      ring2FrameDirty = false;
    }
    return;
  }

  if (mode == 1) {
    if (ring2FrameDirty) {
      ring2Fill(rgb(0, 0, 64));
      FastLED.show();
      ring2FrameDirty = false;
    }
    return;
  }

  if (now - ring2TickMs >= 80) {
    ring2TickMs = now;
    const int16_t next = (int16_t)ring2PulseValue + ring2PulseStep;
    if (next >= 120 || next <= 10) ring2PulseStep = -ring2PulseStep;
    ring2PulseValue = (uint8_t)((int16_t)ring2PulseValue + ring2PulseStep);
    ring2Fill(rgb(0, 0, ring2PulseValue));
    ring2FrameDirty = true;
  }
  if (ring2FrameDirty) {
    FastLED.show();
    ring2FrameDirty = false;
  }
}
#else
static void ring2Service(uint32_t) {}
#endif

static inline void applyBrightnessForLedModeInternal() {
  const uint8_t percent = (ledMode == LedMode::STANDBY_TWINKLE)
    ? activeConfig.standbyBrightnessPercent
    : activeConfig.pixelBrightnessPercent;
  setStripBrightnessPercent(percent);
}

static inline void pixelsFill(const CRGB& color) {
  for (uint16_t i = 0; i < PIXEL_COUNT; ++i) primaryLeds[i] = scaleColor(color, currentBrightnessByte);
}

static inline void pixelsSet(const uint8_t* indices, uint8_t count, const CRGB& color) {
  for (uint8_t i = 0; i < count; ++i) {
    if (indices[i] < PIXEL_COUNT) primaryLeds[indices[i]] = scaleColor(color, currentBrightnessByte);
  }
}

static constexpr uint8_t ALT_PATTERN_A[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
static constexpr uint8_t ALT_PATTERN_B[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23};

static void standbyApplyOutputs() {
  pixelsClear();
  for (uint8_t i = 0; i < PIXEL_COUNT; ++i) {
    if (standbyTwinkleOn[i]) {
      primaryLeds[i] = scaleColor(hsvGamma(standbyHue[i], STANDBY_SATURATION, standbyValue[i]), currentBrightnessByte);
    }
  }
}

void ledsInit() {
  FastLED.addLeds<WS2812B, LED_STRIP_PIN, GRB>(primaryLeds, PIXEL_COUNT);
#if RING2_ENABLED
  FastLED.addLeds<WS2812B, RING2_PIN, GRB>(ring2Leds, RING2_PIXEL_COUNT);
#endif
  setStripBrightnessPercent(activeConfig.pixelBrightnessPercent);
  colorGreen = rgb(0, 90, 0);
  colorBlue = rgb(0, 0, 100);
  colorRed = rgb(100, 0, 0);
  colorCyan = rgb(0, 90, 90);
  pixelsClear();
  pixelsShow();
  ledFrameDirty = false;
  #if RING2_ENABLED
  ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);

  if (RING2_BOOT_TEST) {
    for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) {
      ring2Leds[i] = scaleColor(rgb(80, 80, 80), ring2BrightnessByte);
    }

    FastLED.show();
    ring2FrameDirty = false;

    if (MASTER_DEBUG_LOG) {
      Serial.printf(
        "[RING2] boot test GPIO=%u pixels=%u\n",
        (unsigned)RING2_PIN,
        (unsigned)RING2_PIXEL_COUNT
      );
    }
  } else {
    ring2Clear();
    FastLED.show();

    // Initialen Frame markieren: ring2Service() schreibt das Default-Pattern
    // beim nächsten Service-Lauf zuverlässig auf Ring 2.
    ring2FrameDirty = true;
  }
  #endif
}

void ledsSetMode(LedMode m) {
  if (ledMode == m) return;
  ledMode = m;
  const uint32_t now = millis();
  ledTickMs = now;
  ledFlip = false;
  if (m == LedMode::READY_GREEN_BLINK) ledFlip = true;
  ledSpinIdx = 0;
  ledFrameDirty = true;
  if (m == LedMode::STANDBY_TWINKLE) {
    standbyFrameNextMs = now + STANDBY_FRAME_MS;
    twinkleNextMs = now + random(STANDBY_CHANGE_MIN_MS, STANDBY_CHANGE_MAX_MS);
    standbyOnCount = 0;
    for (uint8_t i = 0; i < PIXEL_COUNT; i++) {
      standbyTwinkleOn[i] = false;
      standbyHue[i] = (uint16_t)random(0, 65536);
      standbyValue[i] = (uint8_t)random(STANDBY_VALUE_MIN, STANDBY_VALUE_MAX + 1);
    }
    const uint8_t initialOn = (uint8_t)random(STANDBY_ON_MIN, STANDBY_ON_MAX + 1);
    for (uint8_t i = 0; i < initialOn; ++i) {
      const uint8_t idx = (uint8_t)random(0, PIXEL_COUNT);
      if (!standbyTwinkleOn[idx]) {
        standbyTwinkleOn[idx] = true;
        ++standbyOnCount;
      }
      ledFrameDirty = true;
    }
  }
}

void ledService(uint32_t now) {
  const bool ring2ForceTestActive =
#if RING2_ENABLED
    RING2_FORCE_INDEPENDENT_TEST;
#else
    false;
#endif

#if RING2_ENABLED
  if (MASTER_DEBUG_LOG) {
    static uint32_t lastInlineEntryLogMs = 0;
    if (now - lastInlineEntryLogMs >= 2000) {
      lastInlineEntryLogMs = now;
      Serial.printf("[RING2 FASTLED ENTRY] force=%u pin=%u pixels=%u now=%lu\n",
                    (unsigned)RING2_FORCE_INDEPENDENT_TEST,
                    (unsigned)RING2_PIN,
                    (unsigned)RING2_PIXEL_COUNT,
                    (unsigned long)now);
    }
  }

  if (ring2ForceTestActive) {
    static uint32_t lastInlineWriteMs = 0;
    if (now - lastInlineWriteMs >= 250) {
      lastInlineWriteMs = now;

      ring2BrightnessByte = 255;
      ring2Clear();

      if (RING2_PIXEL_COUNT > 0) ring2Leds[0] = rgb(255, 0, 0);
      if (RING2_PIXEL_COUNT > 1) ring2Leds[1] = rgb(0, 255, 0);
      if (RING2_PIXEL_COUNT > 2) ring2Leds[2] = rgb(0, 0, 255);
      if (RING2_PIXEL_COUNT > 3) ring2Leds[3] = rgb(255, 255, 255);

      FastLED.show();

      if (MASTER_DEBUG_LOG) {
        Serial.printf("[RING2 FASTLED TEST] wrote RGBW pin=%u pixels=%u\n",
                      (unsigned)RING2_PIN,
                      (unsigned)RING2_PIXEL_COUNT);
      }
    }
  }
#endif

  if (MASTER_DEBUG_LOG) {
    static uint32_t lastLedDiagMs = 0;
    if (now - lastLedDiagMs >= 2000) {
      lastLedDiagMs = now;
      Serial.printf("[LED FASTLED BUILD] service mode=%u ring2Force=%u\n",
                    (unsigned)ledMode,
                    (unsigned)RING2_FORCE_INDEPENDENT_TEST);
    }
  }

  static bool allOnApplied = false;
  if (activeConfig.pixelDebugAllOn) {
    setStripBrightnessPercent(activeConfig.pixelBrightnessPercent);
    if (!allOnApplied) {
      pixelsFill(rgb(80, 80, 80));
      pixelsShow();
      allOnApplied = true;
    }
    if (MASTER_DEBUG_LOG && !ring2ForceTestActive) {
      static uint32_t lastRing2CallLogMs = 0;
      if (millis() - lastRing2CallLogMs >= 2000) {
        lastRing2CallLogMs = millis();
        Serial.println("[LED FASTLED BUILD] calling ring2Service");
      }
    }
#if RING2_ENABLED
    if (!ring2ForceTestActive) ring2Service(now);
#endif
    return;
  }
  allOnApplied = false;

  applyBrightnessForLedModeInternal();

  switch (ledMode) {
    case LedMode::ALL_OFF:
      if (ledFrameDirty) pixelsClear();
      break;
    case LedMode::ERROR_BLINK_RED:
      if (now - ledTickMs >= 350) {
        ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true;
      }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsFill(colorRed); }
      break;
    case LedMode::RED_SOLID:
      if (ledFrameDirty) pixelsFill(colorRed);
      break;
    case LedMode::OK_ALT_GB:
      if (now - ledTickMs >= 450) { ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsSet(ALT_PATTERN_A, sizeof(ALT_PATTERN_A), colorGreen); else pixelsSet(ALT_PATTERN_B, sizeof(ALT_PATTERN_B), colorBlue); }
      break;
    case LedMode::READY_GREEN_BLINK:
      if (now - ledTickMs >= 450) { ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsFill(colorGreen); }
      break;
    case LedMode::GLASS_GREEN_SOLID:
      if (ledFrameDirty) pixelsFill(colorGreen);
      break;
    case LedMode::TIMING_BLUE_SPINNER:
      if (now - ledTickMs >= 180) { ledTickMs = now; ledSpinIdx = (uint8_t)((ledSpinIdx + 1) % PIXEL_COUNT); ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); primaryLeds[ledSpinIdx] = scaleColor(colorBlue, currentBrightnessByte); }
      break;
    case LedMode::RESULT_FLASH_GB_ONCE:
      if (now - ledTickMs < 200) { if (ledFrameDirty) pixelsFill(colorCyan); }
      else ledsSetMode(LedMode::GLASS_GREEN_SOLID);
      break;
    case LedMode::STANDBY_TWINKLE: {
      if (now >= twinkleNextMs) {
        twinkleNextMs = now + random(STANDBY_CHANGE_MIN_MS, STANDBY_CHANGE_MAX_MS);
        const bool needMore = standbyOnCount < STANDBY_ON_MIN;
        const bool needLess = standbyOnCount > STANDBY_ON_MAX;
        const bool shouldToggle = !needMore && !needLess && (random(0, 100) < 45);
        if (needMore || needLess || shouldToggle) {
          for (uint8_t tries = 0; tries < PIXEL_COUNT; ++tries) {
            const uint8_t i = (uint8_t)random(0, PIXEL_COUNT);
            if (needMore) {
              if (!standbyTwinkleOn[i]) { standbyTwinkleOn[i] = true; ++standbyOnCount; standbyHue[i] = (uint16_t)random(0, 65536); standbyValue[i] = (uint8_t)random(STANDBY_VALUE_MIN, STANDBY_VALUE_MAX + 1); ledFrameDirty = true; break; }
            } else if (needLess) {
              if (standbyTwinkleOn[i]) { standbyTwinkleOn[i] = false; --standbyOnCount; ledFrameDirty = true; break; }
            } else if (standbyTwinkleOn[i]) {
              standbyTwinkleOn[i] = false; --standbyOnCount; ledFrameDirty = true; break;
            } else {
              standbyTwinkleOn[i] = true; ++standbyOnCount; standbyHue[i] = (uint16_t)random(0, 65536); standbyValue[i] = (uint8_t)random(STANDBY_VALUE_MIN, STANDBY_VALUE_MAX + 1); ledFrameDirty = true; break;
            }
          }
        }
      }
      if (now >= standbyFrameNextMs) {
        standbyFrameNextMs = now + STANDBY_FRAME_MS;
        for (uint8_t i = 0; i < PIXEL_COUNT; ++i) {
          if (!standbyTwinkleOn[i]) continue;
          const int16_t shift = (int16_t)random(-2, 3);
          standbyHue[i] = (uint16_t)(standbyHue[i] + shift);
          const int16_t delta = (int16_t)random(-4, 5);
          int16_t nextValue = (int16_t)standbyValue[i] + delta;
          if (nextValue < STANDBY_VALUE_MIN) nextValue = STANDBY_VALUE_MIN;
          if (nextValue > STANDBY_VALUE_MAX) nextValue = STANDBY_VALUE_MAX;
          standbyValue[i] = (uint8_t)nextValue;
        }
        ledFrameDirty = true;
      }
      if (ledFrameDirty) standbyApplyOutputs();
      break;
    }
  }

  if (ledFrameDirty) { pixelsShow(); ledFrameDirty = false; }
  if (MASTER_DEBUG_LOG && !ring2ForceTestActive) {
    static uint32_t lastRing2CallLogMs = 0;
    if (millis() - lastRing2CallLogMs >= 2000) {
      lastRing2CallLogMs = millis();
      Serial.println("[LED FASTLED BUILD] calling ring2Service");
    }
  }
#if RING2_ENABLED
  if (!ring2ForceTestActive) ring2Service(now);
#endif
}

void ledApplyBrightnessForCurrentMode() {
  applyBrightnessForLedModeInternal();
}

void ledMarkRing2Dirty() {
#if RING2_ENABLED
  ring2FrameDirty = true;
#endif
}

void ledMarkAllDirty() {
  ledFrameDirty = true;
  ledMarkRing2Dirty();
}

void ledClear() {
  pixelsClear();
#if RING2_ENABLED
  ring2Clear();
#endif
}

void ledShow() {
  pixelsShow();
}
