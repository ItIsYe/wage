#include "led_module.h"

#include <FastLED.h>

#include "config.h"
#include "led_ring1_module.h"
#include "led_ring2_module.h"

extern RuntimeConfig activeConfig;

static CRGB primaryLeds[PIXEL_COUNT];
static CRGB ring2Leds[RING2_PIXEL_COUNT];
static LedMode currentLedMode = LedMode::ALL_OFF;
static bool sharedStandbyActive = false;
static uint32_t sharedStandbyFrameNextMs = 0;
static uint32_t sharedStandbyChangeNextMs = 0;
static constexpr uint16_t SHARED_PIXELS = PIXEL_COUNT + RING2_PIXEL_COUNT;
static bool sharedStandbyTwinkleOn[SHARED_PIXELS] = {};
static uint16_t sharedStandbyHue[SHARED_PIXELS] = {};
static uint8_t sharedStandbyValue[SHARED_PIXELS] = {};
static uint16_t sharedStandbyOnCount = 0;

static inline uint32_t sanitizeRangeMin(uint32_t minValue, uint32_t maxValue) { return (minValue > maxValue) ? maxValue : minValue; }
static inline uint32_t sanitizeStandbyFrameMs(uint32_t frameMs) {
  if (frameMs < 30U) return 30U;
  if (frameMs > 1000U) return 1000U;
  return frameMs;
}
static inline uint32_t randomInclusiveU32(uint32_t minValue, uint32_t maxValue) {
  if (minValue > maxValue) { const uint32_t t = minValue; minValue = maxValue; maxValue = t; }
  return minValue + (uint32_t)random(0, (long)(maxValue - minValue + 1U));
}
static inline uint8_t randomInclusiveU8(uint8_t minValue, uint8_t maxValue) {
  if (minValue > maxValue) { const uint8_t t = minValue; minValue = maxValue; maxValue = t; }
  return (uint8_t)(minValue + (uint8_t)random(0, (int16_t)(maxValue - minValue + 1U)));
}
static void sharedStandbyInit(uint32_t now) {
  const uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
  const uint32_t changeMinMs = sanitizeRangeMin(activeConfig.standbyChangeMinMs, changeMaxMs);
  const uint8_t valueMax = activeConfig.standbyValueMax;
  const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
  const uint8_t onMax = activeConfig.standbyOnMax;
  const uint8_t onMin = (activeConfig.standbyOnMin > onMax) ? onMax : activeConfig.standbyOnMin;
  sharedStandbyFrameNextMs = now + sanitizeStandbyFrameMs(activeConfig.standbyFrameMs);
  sharedStandbyChangeNextMs = now + randomInclusiveU32(changeMinMs, changeMaxMs);
  sharedStandbyOnCount = 0;
  for (uint16_t i = 0; i < SHARED_PIXELS; ++i) {
    sharedStandbyTwinkleOn[i] = false;
    sharedStandbyHue[i] = (uint16_t)random(0, 65536);
    sharedStandbyValue[i] = randomInclusiveU8(valueMin, valueMax);
  }
  const uint8_t initialOn = randomInclusiveU8(onMin, onMax);
  for (uint8_t i = 0; i < initialOn; ++i) {
    const uint16_t idx = (uint16_t)random(0, SHARED_PIXELS);
    if (!sharedStandbyTwinkleOn[idx]) { sharedStandbyTwinkleOn[idx] = true; ++sharedStandbyOnCount; }
  }
}
static bool sharedStandbyService(uint32_t now) {
  bool changed = false;
  if (now >= sharedStandbyChangeNextMs) {
    const uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
    const uint32_t changeMinMs = sanitizeRangeMin(activeConfig.standbyChangeMinMs, changeMaxMs);
    const uint8_t valueMax = activeConfig.standbyValueMax;
    const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
    const uint8_t onMax = activeConfig.standbyOnMax;
    const uint8_t onMin = (activeConfig.standbyOnMin > onMax) ? onMax : activeConfig.standbyOnMin;
    sharedStandbyChangeNextMs = now + randomInclusiveU32(changeMinMs, changeMaxMs);
    const bool needMore = sharedStandbyOnCount < onMin;
    const bool needLess = sharedStandbyOnCount > onMax;
    const bool shouldToggle = !needMore && !needLess && (random(0, 100) < 45);
    if (needMore || needLess || shouldToggle) {
      for (uint16_t tries = 0; tries < SHARED_PIXELS; ++tries) {
        const uint16_t i = (uint16_t)random(0, SHARED_PIXELS);
        if (needMore && !sharedStandbyTwinkleOn[i]) { sharedStandbyTwinkleOn[i] = true; ++sharedStandbyOnCount; sharedStandbyHue[i] = (uint16_t)random(0, 65536); sharedStandbyValue[i] = randomInclusiveU8(valueMin, valueMax); changed = true; break; }
        if (needLess && sharedStandbyTwinkleOn[i]) { sharedStandbyTwinkleOn[i] = false; --sharedStandbyOnCount; changed = true; break; }
        if (!needMore && !needLess) { sharedStandbyTwinkleOn[i] = !sharedStandbyTwinkleOn[i]; if (sharedStandbyTwinkleOn[i]) { ++sharedStandbyOnCount; sharedStandbyHue[i] = (uint16_t)random(0, 65536); sharedStandbyValue[i] = randomInclusiveU8(valueMin, valueMax); } else --sharedStandbyOnCount; changed = true; break; }
      }
    }
  }
  if (now >= sharedStandbyFrameNextMs) {
    const uint8_t valueMax = activeConfig.standbyValueMax;
    const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
    sharedStandbyFrameNextMs = now + sanitizeStandbyFrameMs(activeConfig.standbyFrameMs);
    for (uint16_t i = 0; i < SHARED_PIXELS; ++i) {
      if (!sharedStandbyTwinkleOn[i]) continue;
      sharedStandbyHue[i] = (uint16_t)(sharedStandbyHue[i] + (int16_t)random(-2, 3));
      int16_t nextValue = (int16_t)sharedStandbyValue[i] + (int16_t)random(-4, 5);
      if (nextValue < valueMin) nextValue = valueMin;
      if (nextValue > valueMax) nextValue = valueMax;
      sharedStandbyValue[i] = (uint8_t)nextValue;
    }
    changed = true;
  }
  if (changed) {
    ring1ApplySharedStandby(sharedStandbyTwinkleOn, sharedStandbyHue, sharedStandbyValue, SHARED_PIXELS);
    if (RING2_ENABLED) ring2ApplySharedStandby(sharedStandbyTwinkleOn, sharedStandbyHue, sharedStandbyValue, SHARED_PIXELS);
  }
  return changed;
}

static inline void logRing2ServiceDispatch(uint32_t now, bool ring2ForceTestActive) {
  if (!MASTER_DEBUG_LOG || !ring2ForceTestActive) return;
  static uint32_t lastRing2SkipLogMs = 0;
  if (now - lastRing2SkipLogMs >= 2000) {
    lastRing2SkipLogMs = now;
    Serial.println("[LED FASTLED BUILD] skipping ring2Service: force test active");
  }
}

void ledsInit() {
  FastLED.addLeds<WS2812B, LED_STRIP_PIN, GRB>(primaryLeds, PIXEL_COUNT);
  ring1Init(primaryLeds);
  if (RING2_ENABLED) {
    FastLED.addLeds<WS2812B, RING2_PIN, GRB>(ring2Leds, RING2_PIXEL_COUNT);
    ring2Init(ring2Leds);
  }
  FastLED.show();
}

void ledsSetMode(LedMode m) {
  if (currentLedMode == m) return;
  currentLedMode = m;
  ring1SetMode(m, millis());
}

void ledSetState(State state) {
  if (RING2_ENABLED) ring2SetState(state);
}

void ledService(uint32_t now) {
  const bool ring2ForceTestActive = RING2_ENABLED && RING2_FORCE_INDEPENDENT_TEST;


  if (MASTER_DEBUG_LOG) {
    static uint32_t lastLedDiagMs = 0;
    if (now - lastLedDiagMs >= 2000) {
      lastLedDiagMs = now;
      Serial.printf("[LED FASTLED BUILD] service mode=%u ring2Force=%u\n",
                    (unsigned)currentLedMode,
                    (unsigned)ring2ForceTestActive);
    }
  }

  bool ring1Changed = false;
  bool ring2Changed = false;

  const bool sharedStandbyShouldRun = RING2_ENABLED && !ring2ForceTestActive && currentLedMode == LedMode::STANDBY_TWINKLE && ring2IsStandbyState();
  if (sharedStandbyShouldRun && !sharedStandbyActive) { sharedStandbyActive = true; sharedStandbyInit(now); ring1Changed = true; ring2Changed = true; }
  if (!sharedStandbyShouldRun && sharedStandbyActive) sharedStandbyActive = false;

  if (sharedStandbyActive) {
    ring1Changed = sharedStandbyService(now);
    ring2Changed = ring1Changed;
  } else if (RING2_ENABLED && !ring2ForceTestActive) {
    if (MASTER_DEBUG_LOG) {
      static uint32_t lastEarlyRing2CallLogMs = 0;
      if (now - lastEarlyRing2CallLogMs >= 1000) {
        lastEarlyRing2CallLogMs = now;
        Serial.println("[LED FASTLED BUILD] early-call ring2Service");
      }
    }
    ring2Changed = ring2Service(now);
  } else if (RING2_ENABLED) {
    ring2Changed = ring2ForceTestService(now);
    logRing2ServiceDispatch(now, true);
  }

  static bool allOnApplied = false;
  if (activeConfig.pixelDebugAllOn) {
    if (!allOnApplied) {
      ring1FillDebugAllOn();
      ring1Changed = true;
      allOnApplied = true;
    }
  } else {
    allOnApplied = false;
    ring1Changed = ring1Service(now);
  }

  if (ring1Changed || ring2Changed) {
    FastLED.show();
    if (RING2_ENABLED && ring2Changed) {
      ring2LogWrite(now);
    }
  }
}

void ledApplyBrightnessForCurrentMode() { ring1ApplyBrightnessForCurrentMode(); }

void ledMarkRing2Dirty() {
  if (RING2_ENABLED) ring2MarkDirty();
}

void ledMarkAllDirty() {
  ring1MarkDirty();
  ledMarkRing2Dirty();
}

void ledClear() {
  ring1Clear();
  if (RING2_ENABLED) ring2Clear();
}

void ledShow() { FastLED.show(); }
